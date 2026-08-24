#pragma once

#include "GraphicsTypes.hpp"
#include "Device.hpp"
#include "CommandList.hpp"
#include "shaders/ShaderLibrary.hpp"

namespace spray::graphics {

// Draws a fullscreen triangle sampling a source color texture into whatever
// render target is currently bound -- call this inside an already-open
// BeginRendering scope. This is what actually gets an IViewport's offscreen
// output (see Viewport.hpp) onto the swapchain: every viewport renders into
// its own texture rather than the swapchain directly, so something has to
// bridge "whichever viewport is active" to "what actually gets presented",
// and this is that bridge.
//
// No tonemapping/color-grading yet -- straight copy. PathTracer's output in
// particular is linear HDR-ish (see PathTracer::EnsureOutputTexture's
// comment); add a tonemap step here once that starts looking wrong on
// screen, rather than baking it into PathTracer itself -- Presenter is the
// one place all viewport output already funnels through regardless of
// which renderer produced it, so it's the natural home for that later.
class Presenter {
public:
    // targetColorFormat must match whatever color attachment format Blit()
    // will actually be called while rendering into (the swapchain's, in
    // App::RenderFrame's case) -- baked into the pipeline at construction,
    // same reason every other GraphicsPipelineDesc in this engine needs
    // its target formats up front.
    Presenter(IDevice& device, shaders::ShaderLibrary& shaderLibrary, Format targetColorFormat);
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    // Must be called inside an active BeginRendering scope targeting a
    // color attachment with the format passed to the constructor.
    // destWidth/destHeight size the viewport/scissor for that attachment
    // (this class doesn't know the destination's size any other way).
    // `source` must be in ResourceState::ShaderReadOnly -- true of any
    // IViewport's GetColorOutput() per its documented contract.
    void Blit(ICommandList& cmd, TextureHandle source, uint32_t destWidth, uint32_t destHeight);

private:
    IDevice& m_device;
    shaders::ShaderLibrary& m_shaders;

    PipelineHandle m_pipeline;
    BindGroupLayoutHandle m_layout;
    SamplerHandle m_sampler;

    // Rebuilt only when `source` changes from the last Blit() call -- bind
    // groups are cheap to create but not free, and the active viewport's
    // texture handle is usually stable frame to frame (only changes on a
    // resize or a viewport-mode switch).
    BindGroupHandle m_bindGroup;
    TextureHandle m_boundTexture;
};

} // namespace spray::graphics