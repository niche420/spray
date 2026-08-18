#pragma once

#include "graphics/GraphicsTypes.hpp"
#include "graphics/Device.hpp"
#include "graphics/CommandList.hpp"
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
// Deliberately a separate class from SceneRenderer rather than folded into
// it -- see SceneLayer's class comment on why "which renderer draws it" is
// kept separate from scene simulation. SceneRenderer stays the raster
// viewport path; this is the offline/capture path. Both read the same
// Scene + AssetManager, neither owns the other.
class PathTracer {
public:
    PathTracer(IDevice& device, assets::AssetManager& assets);
    ~PathTracer();

    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    // Width/height of the output texture; recreated if the requested size
    // differs from what's currently allocated. Call before Render if you
    // need a specific capture resolution (defaults to 512x512 on first
    // use).
    void SetOutputSize(uint32_t width, uint32_t height);

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
    void Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity);

    TextureHandle GetOutputTexture() const { return m_outputTexture; }
    uint32_t GetOutputWidth() const { return m_width; }
    uint32_t GetOutputHeight() const { return m_height; }

private:
    void EnsurePipeline();
    void EnsureOutputTexture();
    void RebuildTlas(ICommandList& cmd, Scene& scene);

    IDevice& m_device;
    assets::AssetManager& m_assets;

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