#include "pch.hpp"
#include "PathTracer.hpp"
#include "scene/Components.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace spray::graphics {

namespace {

// Same helper SceneRenderer::LoadCompiledShader uses -- duplicated rather
// than shared for now since it's a 10-line file-read; pull into a common
// ShaderLoading.hpp if a third renderer ends up needing it too.
std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

ShaderBytecode LoadCompiledShader(const std::string& dxilPath, const std::string& spirvPath) {
    ShaderBytecode bc;
    bc.dxil = ReadFileBytes(dxilPath);
    bc.spirv = ReadFileBytes(spirvPath);
    if (bc.spirv.empty() && bc.dxil.empty()) {
        throw std::runtime_error("PathTracer: no compiled shader found at '" + spirvPath +
            "' or '" + dxilPath + "' -- did the shader build step run?");
    }
    return bc;
}

// Mirrors SceneRenderer's camera UBO layout convention (view-proj packed as
// one matrix there); path tracing needs the inverses separately to
// reconstruct a world-space ray per pixel in the raygen shader, so this is
// its own layout rather than reusing SceneRenderer's.
struct CameraUniforms {
    glm::mat4 invView;
    glm::mat4 invProj;
};

} // namespace

PathTracer::PathTracer(IDevice& device, assets::AssetManager& assets)
    : m_device(device), m_assets(assets) {

    BindGroupLayoutDesc layoutDesc;
    layoutDesc.entries = {
        { 0, BindingType::UniformBuffer,         ShaderStage::RayGen },
        { 1, BindingType::StorageTexture,        ShaderStage::RayGen },
        { 2, BindingType::AccelerationStructure, ShaderStage::RayGen },
    };
    m_sceneLayout = m_device.CreateBindGroupLayout(layoutDesc);

    BufferDesc camBufDesc;
    camBufDesc.sizeBytes = sizeof(CameraUniforms);
    camBufDesc.usage = BufferUsage::UniformBuffer;
    camBufDesc.hostVisible = true;
    m_cameraUniformBuffer = m_device.CreateBuffer(camBufDesc);
    m_cameraMapped = m_device.MapBuffer(m_cameraUniformBuffer);

    // m_sceneBindGroup deliberately NOT created here -- it references
    // m_outputTexture and m_tlas, neither of which exist yet. Built lazily
    // in Render once both are valid (see EnsureOutputTexture/RebuildTlas),
    // same "create on first use" pattern SceneRenderer uses for its
    // pipeline via EnsurePipeline.
}

PathTracer::~PathTracer() {
    // Same lifetime contract as SceneRenderer: caller destroys this before
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

    ShaderModuleDesc raygenDesc;
    raygenDesc.stage = ShaderStage::RayGen;
    raygenDesc.entryPoint = "RayGenMain";
    raygenDesc.bytecode = LoadCompiledShader("shaders/compiled/capture.raygen.dxil",
                                              "shaders/compiled/capture.raygen.spv");
    ShaderModuleHandle raygen = m_device.CreateShaderModule(raygenDesc);

    ShaderModuleDesc missDesc;
    missDesc.stage = ShaderStage::Miss;
    missDesc.entryPoint = "MissMain";
    missDesc.bytecode = LoadCompiledShader("shaders/compiled/capture.miss.dxil",
                                            "shaders/compiled/capture.miss.spv");
    ShaderModuleHandle miss = m_device.CreateShaderModule(missDesc);

    ShaderModuleDesc chitDesc;
    chitDesc.stage = ShaderStage::ClosestHit;
    chitDesc.entryPoint = "ClosestHitMain";
    chitDesc.bytecode = LoadCompiledShader("shaders/compiled/capture.chit.dxil",
                                            "shaders/compiled/capture.chit.spv");
    ShaderModuleHandle chit = m_device.CreateShaderModule(chitDesc);

    RayTracingPipelineDesc desc;
    desc.shaderModules = { raygen, miss, chit };
    desc.shaderGroups = {
        { ShaderGroupType::General, /*generalShaderIndex=*/0 },                          // raygen
        { ShaderGroupType::General, /*generalShaderIndex=*/1 },                          // miss
        { ShaderGroupType::TrianglesHitGroup, UINT32_MAX, /*closestHitShaderIndex=*/2 },  // hit group 0
    };
    desc.maxRecursionDepth = 1; // primary rays only for now -- no reflection/GI bounce yet
    desc.bindGroupLayouts = { m_sceneLayout };

    m_pipeline = m_device.CreateRayTracingPipeline(desc);

    m_device.DestroyShaderModule(raygen);
    m_device.DestroyShaderModule(miss);
    m_device.DestroyShaderModule(chit);
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
        // one implicitly). SceneRenderer already does this for entities it
        // draws, but PathTracer may run on a mesh the raster path hasn't
        // touched yet this session.
        auto& gpuMesh = m_assets.GetOrCreateGpuMesh(meshRenderer.mesh, m_device);
        if (gpuMesh.indexCount == 0) continue; // same skip SceneRenderer applies

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

    cmd.TransitionTextures({
        { m_outputTexture, ResourceState::Undefined, ResourceState::General }
    });

    cmd.SetPipeline(m_pipeline);
    cmd.SetBindGroup(0, m_sceneBindGroup);
    cmd.TraceRays(m_width, m_height, 1);
}

} // namespace spray::graphics