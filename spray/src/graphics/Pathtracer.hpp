#pragma once

#include "graphics/GraphicsTypes.hpp"
#include "graphics/Device.hpp"
#include "graphics/CommandList.hpp"
#include "graphics/shaders/ShaderLibrary.hpp"
#include "graphics/Viewport.hpp"
#include "scene/Scene.hpp"
#include "asset/AssetManager.hpp"

#include <glm/glm.hpp>

namespace spray::graphics {

// Traces a Scene's MeshRenderer entities from a given camera entity into an
// internal storage texture, for dataset capture (this is a dataset
// generator feeding an external PyTorch/CUDA training pipeline, not an
// in-engine differentiable pass -- see the project's architectural notes).
// One TLAS instance per MeshRenderer, rebuilt every call (unlike BLASes,
// which are cached per-mesh in AssetManager and only built once) since
// object transforms can change frame to frame.
//
// Deliberately a separate class from Rasterizer rather than folded into
// it -- "which renderer draws it" is kept separate from scene simulation.
// Rasterizer stays the fast/approximate interactive viewport path; this is
// the ground-truth path (used both for live interactive viewing AND, later,
// driven repeatedly by a CaptureRig for offline dataset capture -- those are
// two different callers of the same underlying renderer, not two different
// renderers). Both Rasterizer and PathTracer read the same Scene +
// AssetManager, neither owns the other.
// Shader loading and bind-group-layout authorship go through ShaderLibrary
// now (shared with Rasterizer, owned by SceneLayer) instead of a
// hand-duplicated file-loading helper and hand-typed entryPoint/
// BindGroupLayoutDesc values -- see ShaderLibrary's class comment for why
// (this class specifically is what surfaced the bug that motivated it: a
// hand-typed entryPoint value silently stopped matching the compiled
// shader when the Vulkan-side shaders moved from HLSL to GLSL).
class PathTracer final : public IViewport {
public:
    PathTracer(IDevice& device, assets::AssetManager& assets, shaders::ShaderLibrary& shaderLibrary);
    ~PathTracer() override;

    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    ViewportMode GetMode() const override { return ViewportMode::PathTraced; }

    // Width/height of the output texture; recreated if the requested size
    // differs from what's currently allocated. Call before Render if you
    // need a specific resolution -- when driven as the interactive
    // viewport this tracks the viewport panel's size (same as Rasterizer);
    // when driven later by a CaptureRig for offline capture, the rig will
    // call this with the dataset's target resolution instead, temporarily
    // overriding whatever the interactive viewport had set (capture and
    // live viewing can't run at once anyway, since both go through this
    // same PathTracer instance -- see the class comment). Defaults to
    // 512x512 on first use.
    void SetOutputSize(uint32_t width, uint32_t height) override;

    // Builds/rebuilds the TLAS from the scene's current MeshRenderer +
    // WorldTransform state, records any BLAS builds AssetManager reports as
    // still pending (see AssetManager::NeedsBlasBuild), then traces from
    // cameraEntity into the output texture. Caller owns submission/fencing
    // as usual -- this only records into cmd.
    //
    // BuildBLAS/BuildTLAS/TraceRays are all recorded into the same cmd, so
    // in-order command list execution guarantees any BLAS/TLAS builds this
    // call records complete before the TraceRays in the same call reads
    // them -- see TLASBuildDesc's comment on this ordering requirement in
    // general (it matters across command lists/submissions too, which this
    // single-list recording sidesteps for the common case).
    void Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity) override;

    TextureHandle GetColorOutput() const override { return m_outputTexture; }
    uint32_t GetOutputWidth() const { return m_width; }
    uint32_t GetOutputHeight() const { return m_height; }

private:
    void EnsurePipeline();
    void EnsureOutputTexture();
    void RebuildTlas(ICommandList& cmd, Scene& scene);

    IDevice& m_device;
    assets::AssetManager& m_assets;
    shaders::ShaderLibrary& m_shaders;

    PipelineHandle m_pipeline;            // ray tracing pipeline
    BindGroupLayoutHandle m_sceneLayout;  // set 0: camera UBO + output image + TLAS
    BindGroupHandle m_sceneBindGroup;

    BufferHandle m_cameraUniformBuffer;   // inverse-view + inverse-proj, for primary ray generation
    void* m_cameraMapped = nullptr;

    TextureHandle m_outputTexture;
    uint32_t m_width = 512, m_height = 512;
    bool m_outputDirty = true; // forces (re)creation + bind group rebuild on first Render / after SetOutputSize

    TLASHandle m_tlas;
};

} // namespace spray::graphics