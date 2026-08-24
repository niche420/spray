#include "pch.hpp"
#include "Rasterizer.hpp"
#include "scene/Components.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>

namespace spray::graphics {

Rasterizer::Rasterizer(IDevice& device, assets::AssetManager& assets, shaders::ShaderLibrary& shaderLibrary)
    : m_device(device), m_assets(assets), m_shaders(shaderLibrary) {

    // Loaded eagerly (not lazily in EnsurePipeline) -- the bind group
    // layouts below need these shaders' reflected bindings, and the camera
    // bind group is created in this constructor, before EnsurePipeline
    // ever runs. Same pattern as PathTracer's constructor.
    m_shaders.Load("mesh.vs", m_device);
    m_shaders.Load("mesh.ps", m_device);

    // Set/binding indices come straight from Mesh.hlsl's register(bN,
    // spaceM) declarations via reflection -- see ShaderLibrary's class
    // comment for why this replaces a hand-typed BindGroupLayoutDesc here.
    m_cameraLayout = m_device.CreateBindGroupLayout(m_shaders.DeriveBindGroupLayout({ "mesh.vs", "mesh.ps" }, 0));
    m_objectLayout = m_device.CreateBindGroupLayout(m_shaders.DeriveBindGroupLayout({ "mesh.vs", "mesh.ps" }, 1));

    BufferDesc camBufDesc;
    camBufDesc.sizeBytes = sizeof(glm::mat4);
    camBufDesc.usage = BufferUsage::UniformBuffer;
    camBufDesc.hostVisible = true;
    m_cameraUniformBuffer = m_device.CreateBuffer(camBufDesc);
    m_cameraMapped = m_device.MapBuffer(m_cameraUniformBuffer);

    BindGroupDesc camBindDesc;
    camBindDesc.layout = m_cameraLayout;
    camBindDesc.entries = { BindGroupEntry::Buffer(0, m_cameraUniformBuffer) };
    m_cameraBindGroup = m_device.CreateBindGroup(camBindDesc);
}

Rasterizer::~Rasterizer() {
    // Relies on the caller destroying Rasterizer before IDevice, same
    // lifetime rule as everything else touching device-owned handles.
    // Shader modules are NOT destroyed here -- ShaderLibrary (shared,
    // owned by SceneLayer) owns their lifetime; see its InvalidateGpuCache.
    for (auto& [entity, data] : m_objectCache) {
        m_device.UnmapBuffer(data.uniformBuffer);
        m_device.DestroyBuffer(data.uniformBuffer);
        m_device.DestroyBindGroup(data.bindGroup);
    }
    m_device.UnmapBuffer(m_cameraUniformBuffer);
    m_device.DestroyBuffer(m_cameraUniformBuffer);
    m_device.DestroyBindGroup(m_cameraBindGroup);
    if (m_pipeline.IsValid()) m_device.DestroyPipeline(m_pipeline);
    m_device.DestroyBindGroupLayout(m_cameraLayout);
    m_device.DestroyBindGroupLayout(m_objectLayout);
    if (m_colorTexture.IsValid()) m_device.DestroyTexture(m_colorTexture);
    if (m_depthTexture.IsValid()) m_device.DestroyTexture(m_depthTexture);
}

void Rasterizer::SetOutputSize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return; // e.g. a minimized/zero-sized panel -- ignore rather than allocate a 0x0 texture
    if (width == m_width && height == m_height) return;
    m_width = width;
    m_height = height;
    m_outputDirty = true;
}

void Rasterizer::EnsureOutputTextures() {
    if (!m_outputDirty && m_colorTexture.IsValid()) return;

    if (m_colorTexture.IsValid()) m_device.DestroyTexture(m_colorTexture);
    if (m_depthTexture.IsValid()) m_device.DestroyTexture(m_depthTexture);

    TextureDesc colorDesc;
    colorDesc.width = m_width;
    colorDesc.height = m_height;
    colorDesc.format = kColorFormat;
    colorDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    colorDesc.debugName = "Rasterizer.Color";
    m_colorTexture = m_device.CreateTexture(colorDesc);

    TextureDesc depthDesc;
    depthDesc.width = m_width;
    depthDesc.height = m_height;
    depthDesc.format = kDepthFormat;
    depthDesc.usage = TextureUsage::DepthStencil;
    depthDesc.debugName = "Rasterizer.Depth";
    m_depthTexture = m_device.CreateTexture(depthDesc);

    m_outputDirty = false;
}

void Rasterizer::EnsurePipeline() {
    if (m_pipeline.IsValid()) return;

    // Already loaded in the constructor -- this just fetches the cached
    // entry (no file read, no re-reflection).
    const auto& vs = m_shaders.Load("mesh.vs", m_device);
    const auto& ps = m_shaders.Load("mesh.ps", m_device);

    GraphicsPipelineDesc desc;
    desc.vertexShader = vs.handle;
    desc.pixelShader = ps.handle;
    desc.bindGroupLayouts = { m_cameraLayout, m_objectLayout }; // index == register space, see mesh.hlsl

    // Vertex layout is still hand-authored, deliberately -- it's a contract
    // between AssetManager's CPU-side Vertex struct and the shader, not
    // purely something the shader alone can determine. Left out of
    // ShaderLibrary's scope for that reason (see ShaderLibrary's class
    // comment).
    VertexBufferLayout vbLayout;
    vbLayout.stride = sizeof(assets::Vertex);
    vbLayout.attributes = {
        { 0, Format::RGB32_Float, offsetof(assets::Vertex, position) },
        { 1, Format::RGB32_Float, offsetof(assets::Vertex, normal) },
        { 2, Format::RG32_Float,  offsetof(assets::Vertex, uv) },
    };
    desc.vertexBuffers = { vbLayout };

    // Fixed formats now -- this pipeline always targets this class's own
    // offscreen textures (kColorFormat/kDepthFormat), never the swapchain
    // directly (see class comment).
    desc.colorTargetFormats = { kColorFormat };
    desc.depthTargetFormat = kDepthFormat;
    desc.depthStencil.depthTestEnable = true;
    desc.depthStencil.depthWriteEnable = true;

    m_pipeline = m_device.CreateGraphicsPipeline(desc);
}

Rasterizer::ObjectGpuData& Rasterizer::GetOrCreateObjectData(entt::entity e) {
    auto it = m_objectCache.find(e);
    if (it != m_objectCache.end()) return it->second;

    ObjectGpuData data;
    BufferDesc bufDesc;
    bufDesc.sizeBytes = sizeof(glm::mat4);
    bufDesc.usage = BufferUsage::UniformBuffer;
    bufDesc.hostVisible = true;
    data.uniformBuffer = m_device.CreateBuffer(bufDesc);
    data.mapped = m_device.MapBuffer(data.uniformBuffer);

    BindGroupDesc bindDesc;
    bindDesc.layout = m_objectLayout;
    bindDesc.entries = { BindGroupEntry::Buffer(0, data.uniformBuffer) };
    data.bindGroup = m_device.CreateBindGroup(bindDesc);

    return m_objectCache.emplace(e, data).first->second;
}

void Rasterizer::PruneStaleObjectData(const Scene& scene) {
    for (auto it = m_objectCache.begin(); it != m_objectCache.end();) {
        if (!scene.GetRegistry().valid(it->first)) {
            m_device.UnmapBuffer(it->second.uniformBuffer);
            m_device.DestroyBuffer(it->second.uniformBuffer);
            m_device.DestroyBindGroup(it->second.bindGroup);
            it = m_objectCache.erase(it);
        }
        else {
            ++it;
        }
    }
}

void Rasterizer::Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity) {
    if (cameraEntity == entt::null) return;

    EnsureOutputTextures();
    EnsurePipeline();
    PruneStaleObjectData(scene);

    // "before = Undefined" every frame even though after frame 1 the real
    // prior state is ShaderReadOnly (this class's own tail transition,
    // below) -- same simplification App::RenderFrame's own comment already
    // documents for the swapchain depth texture: safe here specifically
    // because BeginRendering's clear=true below discards old contents
    // regardless. No persistent per-resource state tracker exists in this
    // engine yet; every call site manually specifies before/after.
    cmd.TransitionTextures({
        { m_colorTexture, ResourceState::Undefined, ResourceState::RenderTarget },
        { m_depthTexture, ResourceState::Undefined, ResourceState::DepthWrite },
    });

    ColorAttachment colorAttachment;
    colorAttachment.texture = m_colorTexture;
    colorAttachment.clear = true;
    colorAttachment.clearColor[0] = 0.05f;
    colorAttachment.clearColor[1] = 0.05f;
    colorAttachment.clearColor[2] = 0.08f;
    colorAttachment.clearColor[3] = 1.0f;

    DepthAttachment depthAttachment;
    depthAttachment.texture = m_depthTexture;
    depthAttachment.clear = true;
    depthAttachment.clearDepth = 1.0f;

    cmd.BeginRendering({ colorAttachment }, depthAttachment);

    auto& registry = scene.GetRegistry();
    const Camera& cam = registry.get<Camera>(cameraEntity);
    const WorldTransform& camWorld = registry.get<WorldTransform>(cameraEntity);

    float aspect = m_height > 0 ? static_cast<float>(m_width) / static_cast<float>(m_height) : 1.0f;
    glm::mat4 view = glm::inverse(camWorld.matrix);
    glm::mat4 proj = glm::perspective(cam.fovYRadians, aspect, cam.nearPlane, cam.farPlane);
    glm::mat4 viewProj = proj * view;
    std::memcpy(m_cameraMapped, &viewProj, sizeof(glm::mat4));

    cmd.SetPipeline(m_pipeline);
    cmd.SetBindGroup(0, m_cameraBindGroup);

    // Both pipelines in this engine declare viewport/scissor as dynamic
    // state (see VulkanDevice::CreateGraphicsPipeline's dynamicState array)
    // -- Vulkan requires these be set before any draw call once declared
    // dynamic. Neither was being set anywhere in the app before this;
    // that's a real pre-existing gap, not something new about switching to
    // an owned offscreen texture -- fixed here while touching this code,
    // and it now matters more directly since this renders at its own
    // size, not implicitly the swapchain's.
    cmd.SetViewport(0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height));
    cmd.SetScissor(0, 0, m_width, m_height);

    auto meshView = registry.view<MeshRenderer, WorldTransform>();
    for (auto [entity, mesh, world] : meshView.each()) {
        if (!mesh.mesh.IsValid()) continue;

        ObjectGpuData& obj = GetOrCreateObjectData(entity);
        std::memcpy(obj.mapped, &world.matrix, sizeof(glm::mat4));

        auto& gpuMesh = m_assets.GetOrCreateGpuMesh(mesh.mesh, m_device);
        if (gpuMesh.indexCount == 0) continue; // e.g. a primitive with no valid indices, see GltfImporter

        cmd.SetBindGroup(1, obj.bindGroup);
        cmd.SetVertexBuffer(0, gpuMesh.vertexBuffer, 0);
        cmd.SetIndexBuffer(gpuMesh.indexBuffer, 0, /*use32BitIndices=*/true);
        cmd.DrawIndexed(gpuMesh.indexCount, 1, 0, 0, 0);
    }

    cmd.EndRendering();

    // Left in ShaderReadOnly so Presenter::Blit (or an ImGui::Image, if
    // that replaces Presenter later) can sample this directly -- see
    // IViewport::GetColorOutput's documented contract.
    cmd.TransitionTextures({
        { m_colorTexture, ResourceState::RenderTarget, ResourceState::ShaderReadOnly },
    });
}

} // namespace spray::graphics