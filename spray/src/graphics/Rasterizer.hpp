#pragma once

#include "GraphicsTypes.hpp"
#include "Device.hpp"
#include "CommandList.hpp"
#include "Viewport.hpp"
#include "shaders/ShaderLibrary.hpp"
#include "scene/Scene.hpp"
#include "asset/AssetManager.hpp"

#include <glm/glm.hpp>

#include <unordered_map>

namespace spray::graphics {

// Rasterizes a Scene's MeshRenderer entities -- the fast, approximate
// viewport mode used for spatial editing/navigation. Renamed from
// "SceneRenderer": that name described what it touches (a Scene), not
// what it does (rasterize) -- misleading once PathTracer and (eventually)
// a splat viewer are equally valid ways to render the same Scene. Call it
// what it is.
//
// Deliberately NOT a stand-in for PathTracer's output -- the two
// intentionally look different (flat placeholder shading here vs. real
// light transport there). Do not use this viewport to judge what a
// capture will look like; use PathTraced mode for that. See IViewport's
// class comment for the architectural reasoning.
//
// Implements IViewport: renders into its own offscreen color+depth
// textures now, rather than directly into whatever the caller had bound
// via the swapchain (the old coupling that made switching viewports
// require separate hardcoded call sites per renderer -- see
// SceneLayer::RenderActiveViewport).
class Rasterizer final : public IViewport {
public:
    Rasterizer(IDevice& device, assets::AssetManager& assets, shaders::ShaderLibrary& shaderLibrary);
    ~Rasterizer() override;

    Rasterizer(const Rasterizer&) = delete;
    Rasterizer& operator=(const Rasterizer&) = delete;

    ViewportMode GetMode() const override { return ViewportMode::Rasterized; }
    void SetOutputSize(uint32_t width, uint32_t height) override;
    void Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity) override;
    TextureHandle GetColorOutput() const override { return m_colorTexture; }

private:
    struct ObjectGpuData {
        BufferHandle uniformBuffer; // host-visible, holds one float4x4 (object's world matrix)
        BindGroupHandle bindGroup;
        void* mapped = nullptr;
    };

    void EnsurePipeline();
    void EnsureOutputTextures();
    ObjectGpuData& GetOrCreateObjectData(entt::entity e);

    // Drops cache entries for entities the scene no longer has (destroyed
    // since last frame), freeing their GPU resources. Doesn't currently
    // handle "MeshRenderer removed from a still-alive entity" -- narrower
    // case, left for later.
    void PruneStaleObjectData(const Scene& scene);

    IDevice& m_device;
    assets::AssetManager& m_assets;
    shaders::ShaderLibrary& m_shaders;

    PipelineHandle m_pipeline;
    BindGroupLayoutHandle m_cameraLayout;
    BindGroupLayoutHandle m_objectLayout;

    BufferHandle m_cameraUniformBuffer;
    void* m_cameraMapped = nullptr;
    BindGroupHandle m_cameraBindGroup;

    std::unordered_map<entt::entity, ObjectGpuData> m_objectCache;

    // Fixed, chosen once -- no longer swapchain-dependent (this renders
    // into its own texture now, not the swapchain's attachments, so it no
    // longer needs to match whatever format the swapchain happens to be).
    static constexpr Format kColorFormat = Format::RGBA8_UNorm;
    static constexpr Format kDepthFormat = Format::D32_Float;

    TextureHandle m_colorTexture;
    TextureHandle m_depthTexture;
    uint32_t m_width = 1280, m_height = 720;
    bool m_outputDirty = true; // forces (re)creation on first Render / after SetOutputSize
};

} // namespace spray::graphics