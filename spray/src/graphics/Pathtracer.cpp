#include "pch.hpp"
#include "PathTracer.hpp"
#include "scene/Components.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <stdexcept>

namespace spray::graphics {

namespace {

// Mirrors Rasterizer's camera UBO layout convention (view-proj packed as
// one matrix there); path tracing needs the inverses separately to
// reconstruct a world-space ray per pixel in the raygen shader, so this is
// its own layout rather than reusing Rasterizer's.
struct CameraUniforms {
    glm::mat4 invView;
    glm::mat4 invProj;
};

} // namespace

PathTracer::PathTracer(IDevice& device, assets::AssetManager& assets, shaders::ShaderLibrary& shaderLibrary)
    : m_device(device), m_assets(assets), m_shaders(shaderLibrary) {

    // Loaded eagerly (not lazily in EnsurePipeline as before) for the same
    // reason as Rasterizer's constructor: the bind group layout below
    // needs these shaders' reflected bindings, and that layout is needed
    // now, before EnsurePipeline ever runs (deferred until first Render,
    // unlike Rasterizer it doesn't even wait on a format -- see
    // EnsurePipeline's own comment -- but keeping shader loading in the
    // constructor for both renderers keeps the pattern consistent).
    m_shaders.Load("capture.raygen", m_device);
    m_shaders.Load("capture.miss", m_device);
    m_shaders.Load("capture.chit", m_device);

    // Set/binding indices come straight from Capture.rgen/rmiss/rchit's
    // layout(set=0, binding=N) declarations via reflection -- this is
    // exactly the BindGroupLayoutDesc that used to be hand-typed here (and
    // exactly the kind of hand-typed value that drifted out of sync with
    // the shader source once -- see ShaderLibrary's class comment).
    m_sceneLayout = m_device.CreateBindGroupLayout(
        m_shaders.DeriveBindGroupLayout({ "capture.raygen", "capture.miss", "capture.chit" }, 0));

    BufferDesc camBufDesc;
    camBufDesc.sizeBytes = sizeof(CameraUniforms);
    camBufDesc.usage = BufferUsage::UniformBuffer;
    camBufDesc.hostVisible = true;
    m_cameraUniformBuffer = m_device.CreateBuffer(camBufDesc);
    m_cameraMapped = m_device.MapBuffer(m_cameraUniformBuffer);

    // m_sceneBindGroup deliberately NOT created here -- it references
    // m_outputTexture and m_tlas, neither of which exist yet. Built lazily
    // in Render once both are valid (see EnsureOutputTexture/RebuildTlas),
    // same "create on first use" pattern Rasterizer uses for its
    // pipeline via EnsurePipeline.
}

PathTracer::~PathTracer() {
    // Same lifetime contract as Rasterizer: caller destroys this before
    // IDevice (m_device is a plain reference).
    m_device.UnmapBuffer(m_cameraUniformBuffer);
    m_device.DestroyBuffer(m_cameraUniformBuffer);
    if (m_sceneBindGroup.IsValid()) m_device.DestroyBindGroup(m_sceneBindGroup);
    if (m_outputTexture.IsValid()) m_device.DestroyTexture(m_outputTexture);
    if (m_tlas.IsValid()) m_device.DestroyTLAS(m_tlas);
    if (m_pipeline.IsValid()) m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyBindGroupLayout(m_sceneLayout);
}

void PathTracer::SetOutputSize(uint32_t width, uint32_t height) {
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    m_outputDirty = true;
}

void PathTracer::EnsurePipeline() {
    if (m_pipeline.IsValid()) return;

    // Already loaded in the constructor -- entry points come from
    // reflection now (see ShaderLibrary::Load), not a hand-typed string.
    // This is exactly the fix for the entryPoint bug that used to live
    // here: these shaders compile from GLSL (shaders/vulkan/Capture.*),
    // and GLSL always compiles its one required entry function to a
    // SPIR-V OpEntryPoint literally named "main" -- unlike HLSL-via-dxc,
    // which preserves whatever name you compiled with. A hand-typed
    // "RayGenMain"/"MissMain"/"ClosestHitMain" here (left over from when
    // these shaders were HLSL) silently pointed at an entry point that no
    // longer existed in the compiled module once the source moved to
    // GLSL, and vkCreateRayTracingPipelinesKHR failed with
    // VK_ERROR_INITIALIZATION_FAILED as a result. Reflection reads the
    // real entry point out of the module directly, so this can't drift
    // out of sync again.
    const auto& raygen = m_shaders.Load("capture.raygen", m_device);
    const auto& miss = m_shaders.Load("capture.miss", m_device);
    const auto& chit = m_shaders.Load("capture.chit", m_device);

    RayTracingPipelineDesc desc;
    desc.shaderModules = { raygen.handle, miss.handle, chit.handle };
    desc.shaderGroups = {
        { ShaderGroupType::General, /*generalShaderIndex=*/0 },                          // raygen
        { ShaderGroupType::General, /*generalShaderIndex=*/1 },                          // miss
        { ShaderGroupType::TrianglesHitGroup, UINT32_MAX, /*closestHitShaderIndex=*/2 },  // hit group 0
    };
    desc.maxRecursionDepth = 1; // primary rays only for now -- no reflection/GI bounce yet
    desc.bindGroupLayouts = { m_sceneLayout };

    m_pipeline = m_device.CreateRayTracingPipeline(desc);

    // No DestroyShaderModule calls here -- ShaderLibrary (shared, owned by
    // SceneLayer) owns shader module lifetime now; see its
    // InvalidateGpuCache.
}

void PathTracer::EnsureOutputTexture() {
    if (!m_outputDirty && m_outputTexture.IsValid()) return;

    if (m_outputTexture.IsValid()) m_device.DestroyTexture(m_outputTexture);

    TextureDesc desc;
    desc.width = m_width;
    desc.height = m_height;
    desc.format = Format::RGBA32_Float; // linear HDR-ish output; tonemap/convert at readback time
    desc.usage = TextureUsage::Storage | TextureUsage::ShaderResource | TextureUsage::CopySrc;
    desc.debugName = "PathTracer.Output";
    m_outputTexture = m_device.CreateTexture(desc);

    // Bind group references the old texture handle -- invalidate so Render
    // rebuilds it below.
    if (m_sceneBindGroup.IsValid()) {
        m_device.DestroyBindGroup(m_sceneBindGroup);
        m_sceneBindGroup = {};
    }

    m_outputDirty = false;
}

void PathTracer::RebuildTlas(ICommandList& cmd, Scene& scene) {
    auto& registry = scene.GetRegistry();

    std::vector<TLASInstanceDesc> instances;
    auto meshView = registry.view<MeshRenderer, WorldTransform>();
    for (auto [entity, meshRenderer, world] : meshView.each()) {
        if (!meshRenderer.mesh.IsValid()) continue;

        // Ensure the raster GPU mesh exists first -- GetOrCreateGpuBlas
        // requires it (see AssetManager's comment on why it doesn't create
        // one implicitly). Rasterizer already does this for entities it
        // draws, but PathTracer may run on a mesh the raster path hasn't
        // touched yet this session.
        auto& gpuMesh = m_assets.GetOrCreateGpuMesh(meshRenderer.mesh, m_device);
        if (gpuMesh.indexCount == 0) continue; // same skip Rasterizer applies

        BLASHandle blas = m_assets.GetOrCreateGpuBlas(meshRenderer.mesh, m_device);
        if (m_assets.NeedsBlasBuild(meshRenderer.mesh)) {
            // Use the exact desc AssetManager allocated the BLAS with --
            // never reconstruct this locally (see GetGpuBlasBuildDesc's
            // header comment on why: allocation and build must agree).
            cmd.BuildBLAS(blas, m_assets.GetGpuBlasBuildDesc(meshRenderer.mesh));
            m_assets.MarkBlasBuilt(meshRenderer.mesh);
        }

        // Row-major 3x4 from WorldTransform's column-major glm::mat4 -- glm
        // stores column-major, TLASInstanceDesc::transform wants row-major
        // 3x4 (see its comment: matches both VkTransformMatrixKHR and
        // D3D12_RAYTRACING_INSTANCE_DESC).
        const glm::mat4& m = world.matrix;
        TLASInstanceDesc inst;
        inst.blas = blas;
        inst.transform[0]  = m[0][0]; inst.transform[1]  = m[1][0]; inst.transform[2]  = m[2][0]; inst.transform[3]  = m[3][0];
        inst.transform[4]  = m[0][1]; inst.transform[5]  = m[1][1]; inst.transform[6]  = m[2][1]; inst.transform[7]  = m[3][1];
        inst.transform[8]  = m[0][2]; inst.transform[9]  = m[1][2]; inst.transform[10] = m[2][2]; inst.transform[11] = m[3][2];
        inst.instanceID = static_cast<uint32_t>(entity);
        instances.push_back(inst);
    }

    if (!m_tlas.IsValid()) {
        TLASBuildDesc allocDesc;
        allocDesc.instances = instances; // sizing only needs instance count, see TLASBuildDesc's build-vs-allocate split
        m_tlas = m_device.CreateTLAS(allocDesc);
    }
    // NOTE: if the instance count grows past what m_tlas was originally
    // allocated for (objects added to the scene between captures), this
    // needs to destroy+recreate m_tlas here instead of reusing it -- not
    // handled yet, since the capture rig this feeds is expected to orbit a
    // static scene per capture session. Revisit if that assumption changes.

    TLASBuildDesc buildDesc;
    buildDesc.instances = instances;
    cmd.BuildTLAS(m_tlas, buildDesc);

    if (m_sceneBindGroup.IsValid()) {
        m_device.DestroyBindGroup(m_sceneBindGroup);
        m_sceneBindGroup = {};
    }
}

void PathTracer::Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity) {
    if (cameraEntity == entt::null) return;

    EnsurePipeline();
    EnsureOutputTexture();
    RebuildTlas(cmd, scene);

    if (!m_sceneBindGroup.IsValid()) {
        BindGroupDesc bgDesc;
        bgDesc.layout = m_sceneLayout;
        bgDesc.entries = {
            BindGroupEntry::Buffer(0, m_cameraUniformBuffer),
            // BindGroupEntry::Texture doubles as the StorageTexture entry
            // point too -- both SampledTexture and StorageTexture layout
            // entries read entry.texture; only the layout's declared
            // BindingType changes which descriptor gets written (see
            // CreateBindGroup in both backends).
            BindGroupEntry::Texture(1, m_outputTexture),
            BindGroupEntry::AccelStruct(2, m_tlas),
        };
        m_sceneBindGroup = m_device.CreateBindGroup(bgDesc);
    }

    auto& registry = scene.GetRegistry();
    const Camera& cam = registry.get<Camera>(cameraEntity);
    const WorldTransform& camWorld = registry.get<WorldTransform>(cameraEntity);

    CameraUniforms camUniforms;
    camUniforms.invView = camWorld.matrix; // camWorld already IS the inverse of the raster path's view matrix
    float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    glm::mat4 proj = glm::perspective(cam.fovYRadians, aspect, cam.nearPlane, cam.farPlane);
    camUniforms.invProj = glm::inverse(proj);
    std::memcpy(m_cameraMapped, &camUniforms, sizeof(CameraUniforms));

    // "before = Undefined" every frame even though after frame 1 the real
    // prior state is ShaderReadOnly (this function's own tail transition,
    // below) -- same simplification Rasterizer::Render and App::
    // RenderFrame's swapchain-depth comment both use: the whole image gets
    // overwritten by TraceRays regardless (every pixel imageStore'd), so a
    // stale "before" state is harmless here. No persistent per-resource
    // state tracker exists in this engine yet.
    cmd.TransitionTextures({
        { m_outputTexture, ResourceState::Undefined, ResourceState::General }
    });

    cmd.SetPipeline(m_pipeline);
    cmd.SetBindGroup(0, m_sceneBindGroup);
    cmd.TraceRays(m_width, m_height, 1);

    // Left in ShaderReadOnly so Presenter::Blit (or an ImGui::Image, if
    // that replaces Presenter later) can sample this directly -- matches
    // Rasterizer's tail transition, both satisfying IViewport::
    // GetColorOutput's documented contract.
    cmd.TransitionTextures({
        { m_outputTexture, ResourceState::General, ResourceState::ShaderReadOnly },
    });
}

} // namespace spray::graphics