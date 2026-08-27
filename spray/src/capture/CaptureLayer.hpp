#pragma once

#include "event/Layer.hpp"
#include "graphics/Device.hpp"
#include "math/Geometry.hpp"

#include <string>

namespace spray {
class SceneLayer;
}

namespace spray {

// Owns the offline dataset-capture workflow: orbits a camera around the
// currently loaded scene, renders each pose through the shared PathTracer
// (ground-truth renderer -- see PathTracer's class comment on why this is
// the one capture uses, never Rasterizer), and writes each frame plus its
// camera pose to disk. This is the first half of the project's stated
// pipeline (Vulkan path tracer -> posed multi-view dataset -> external
// PyTorch/CUDA 3DGS training -> splat viewer); the training step itself
// happens entirely outside this app.
//
// KNOWN FIDELITY LIMITATION: PathTracer's closest-hit shader
// (shaders/vulkan/Capture.rchit) currently has no lighting model at all --
// it outputs raw barycentric coordinates as a placeholder (see that
// file's header comment). Captures produced by this layer right now are
// NOT usable ground truth for training -- they confirm the capture
// pipeline (poses, readback, file I/O) works end to end, nothing more.
// Wiring real per-instance vertex/material data into the hit shader (and
// a real BRDF) is a prerequisite before this layer's output is useful.
//
// Deliberately a separate Layer from SceneLayer (see SceneLayer's class
// comment: "captured datasets and training jobs will get their own
// Layer"), but reaches into SceneLayer directly for the Scene,
// AssetManager, and -- most importantly -- the *same* PathTracer instance
// SceneLayer's interactive PathTraced viewport mode uses. Capture and live
// PathTraced viewing can't run at once as a result (both would be driving
// the same PathTracer's output texture/TLAS), which is an accepted
// limitation for now: this layer temporarily takes over PathTracer's
// output size for the duration of a capture run; the next interactive
// frame's own SetOutputSize call (see SceneLayer::RenderActiveViewport)
// restores it automatically, so nothing needs to be undone explicitly here.
//
// Capture is synchronous and blocking (one GPU submission + fence wait per
// view, see TextureReadback.hpp's own comment on why that's fine for an
// offline tool) -- runs to completion inside a single button-press call,
// no progress callback or cancellation yet. Acceptable for the view counts
// a capture session needs (tens, not thousands); revisit if that changes.
class CaptureLayer : public Layer {
public:
    CaptureLayer(graphics::IDevice& device, SceneLayer& sceneLayer);

    void OnImGuiRender() override;

private:
    struct CaptureSettings {
        uint32_t viewCount = 32;
        uint32_t resolution = 512;
        // Multiplies the scene's bounding-sphere radius (see
        // Scene::ComputeWorldBounds) to place the orbit -- 1.0 would put
        // the camera exactly on the bounding sphere's surface, too close
        // to frame the whole object; > 1 backs off.
        float orbitRadiusMultiplier = 2.2f;
        char outputDir[256] = "capture_output";
    };

    // Runs a full capture: computes the orbit, renders + reads back +
    // writes every view. Blocks until finished (see class comment).
    // Populates m_statusMessage with a result summary or an error.
    void RunCapture();

    void DrawCapturePanel();

    graphics::IDevice& m_device;
    SceneLayer& m_sceneLayer;

    CaptureSettings m_settings;
    std::string m_statusMessage;
    bool m_lastCaptureFailed = false;
};

} // namespace spray