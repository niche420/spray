#pragma once

#include "graphics/GraphicsTypes.hpp"
#include "graphics/Device.hpp"
#include "graphics/CommandList.hpp"
#include "scene/Scene.hpp"
#include "asset/AssetManager.hpp"

#include <glm/glm.hpp>

#include <unordered_map>

namespace spray::graphics {

// Rasterizes a Scene's MeshRenderer entities from a given camera entity.
// Owns the graphics pipeline plus a per-object GPU uniform-buffer/bind-group
// cache -- one draw call per MeshRenderer, no batching/instancing yet.
class SceneRenderer {
public:
    SceneRenderer(IDevice& device, assets::AssetManager& assets);
    ~SceneRenderer();

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;

    // colorTargetFormat/depthTargetFormat must match what the caller passed
    // to ICommandList::BeginRendering this frame -- the pipeline bakes
    // these in (D3D12's PSO especially: RTVFormats/DSVFormat), so a
    // mismatch is a wrong-pipeline bug, not just a warning. Pass
    // Format::Unknown for depthTargetFormat if not rendering with a depth
    // buffer this call.
    void Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity, float aspectRatio,
        Format colorTargetFormat, Format depthTargetFormat);

private:
    struct ObjectGpuData {
        BufferHandle uniformBuffer; // host-visible, holds one float4x4 (object's world matrix)
        BindGroupHandle bindGroup;
        void* mapped = nullptr;
    };

    void EnsurePipeline(Format colorFormat, Format depthFormat);
    ObjectGpuData& GetOrCreateObjectData(entt::entity e);

    // Drops cache entries for entities the scene no longer has (destroyed
    // since last frame), freeing their GPU resources. Doesn't currently
    // handle "MeshRenderer removed from a still-alive entity" -- narrower
    // case, left for later.
    void PruneStaleObjectData(const Scene& scene);

    IDevice& m_device;
    assets::AssetManager& m_assets;

    PipelineHandle m_pipeline;
    BindGroupLayoutHandle m_cameraLayout;
    BindGroupLayoutHandle m_objectLayout;

    BufferHandle m_cameraUniformBuffer;
    void* m_cameraMapped = nullptr;
    BindGroupHandle m_cameraBindGroup;

    std::unordered_map<entt::entity, ObjectGpuData> m_objectCache;

    Format m_builtColorFormat = Format::Unknown;
    Format m_builtDepthFormat = Format::Unknown;
};

} // namespace spray::graphics