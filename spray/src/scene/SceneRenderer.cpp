#include "pch.hpp"
#include "SceneRenderer.hpp"
#include "Components.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <cstring>
#include <fstream>
#include <vector>

namespace spray::graphics {

namespace {

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

// Loads both DXIL and SPIR-V blobs unconditionally; whichever one is empty
// (e.g. DXIL, since the D3D12 backend doesn't build against this yet) is
// simply unused -- VulkanDevice::CreateShaderModule only ever reads
// bytecode.spirv, D3D12GraphicsDevice::CreateShaderModule only reads
// bytecode.dxil. See CMakeLists.txt for where these files get produced.
ShaderBytecode LoadCompiledShader(const std::string& dxilPath, const std::string& spirvPath) {
    ShaderBytecode bc;
    bc.dxil = ReadFileBytes(dxilPath);
    bc.spirv = ReadFileBytes(spirvPath);
    if (bc.spirv.empty() && bc.dxil.empty()) {
        throw std::runtime_error("SceneRenderer: no compiled shader found at '" + spirvPath +
            "' or '" + dxilPath + "' -- did the shader build step run?");
    }
    return bc;
}

} // namespace

SceneRenderer::SceneRenderer(IDevice& device, assets::AssetManager& assets)
    : m_device(device), m_assets(assets) {

    BindGroupLayoutDesc cameraLayoutDesc;
    cameraLayoutDesc.entries = { { 0, BindingType::UniformBuffer, ShaderStage::Vertex } };
    m_cameraLayout = m_device.CreateBindGroupLayout(cameraLayoutDesc);

    BindGroupLayoutDesc objectLayoutDesc;
    objectLayoutDesc.entries = { { 0, BindingType::UniformBuffer, ShaderStage::Vertex } };
    m_objectLayout = m_device.CreateBindGroupLayout(objectLayoutDesc);

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

SceneRenderer::~SceneRenderer() {
    // Relies on the caller destroying SceneRenderer before IDevice (same
    // lifetime rule as everything else in the app touching device-owned
    // handles) -- m_device is a plain reference, not owned here.
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
}

void SceneRenderer::EnsurePipeline(Format colorFormat, Format depthFormat) {
    if (m_pipeline.IsValid() && colorFormat == m_builtColorFormat && depthFormat == m_builtDepthFormat) return;
    if (m_pipeline.IsValid()) m_device.DestroyPipeline(m_pipeline);

    ShaderModuleDesc vsDesc;
    vsDesc.stage = ShaderStage::Vertex;
    vsDesc.entryPoint = "VSMain";
    vsDesc.bytecode = LoadCompiledShader("shaders/compiled/mesh.vs.dxil", "shaders/compiled/mesh.vs.spv");
    ShaderModuleHandle vs = m_device.CreateShaderModule(vsDesc);

    ShaderModuleDesc psDesc;
    psDesc.stage = ShaderStage::Pixel;
    psDesc.entryPoint = "PSMain";
    psDesc.bytecode = LoadCompiledShader("shaders/compiled/mesh.ps.dxil", "shaders/compiled/mesh.ps.spv");
    ShaderModuleHandle ps = m_device.CreateShaderModule(psDesc);

    GraphicsPipelineDesc desc;
    desc.vertexShader = vs;
    desc.pixelShader = ps;
    desc.bindGroupLayouts = { m_cameraLayout, m_objectLayout }; // index == register space, see mesh.hlsl

    VertexBufferLayout vbLayout;
    vbLayout.stride = sizeof(assets::Vertex);
    vbLayout.attributes = {
        { 0, Format::RGB32_Float, offsetof(assets::Vertex, position) },
        { 1, Format::RGB32_Float, offsetof(assets::Vertex, normal) },
        { 2, Format::RG32_Float,  offsetof(assets::Vertex, uv) },
    };
    desc.vertexBuffers = { vbLayout };

    desc.colorTargetFormats = { colorFormat };
    desc.depthTargetFormat = depthFormat;
    desc.depthStencil.depthTestEnable = (depthFormat != Format::Unknown);
    desc.depthStencil.depthWriteEnable = (depthFormat != Format::Unknown);

    m_pipeline = m_device.CreateGraphicsPipeline(desc);
    m_builtColorFormat = colorFormat;
    m_builtDepthFormat = depthFormat;

    m_device.DestroyShaderModule(vs);
    m_device.DestroyShaderModule(ps);
}

SceneRenderer::ObjectGpuData& SceneRenderer::GetOrCreateObjectData(entt::entity e) {
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

void SceneRenderer::PruneStaleObjectData(const Scene& scene) {
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

void SceneRenderer::Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity, float aspectRatio,
    Format colorTargetFormat, Format depthTargetFormat) {
    EnsurePipeline(colorTargetFormat, depthTargetFormat);
    PruneStaleObjectData(scene);

    auto& registry = scene.GetRegistry();
    const Camera& cam = registry.get<Camera>(cameraEntity);
    const WorldTransform& camWorld = registry.get<WorldTransform>(cameraEntity);

    glm::mat4 view = glm::inverse(camWorld.matrix);
    glm::mat4 proj = glm::perspective(cam.fovYRadians, aspectRatio, cam.nearPlane, cam.farPlane);
    glm::mat4 viewProj = proj * view;
    std::memcpy(m_cameraMapped, &viewProj, sizeof(glm::mat4));

    cmd.SetPipeline(m_pipeline);
    cmd.SetBindGroup(0, m_cameraBindGroup);

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
}

} // namespace spray::graphics