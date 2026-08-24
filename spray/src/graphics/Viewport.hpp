#pragma once

#include "GraphicsTypes.hpp"
#include "CommandList.hpp"
#include "scene/Scene.hpp"

#include <entt/entt.hpp>

namespace spray::graphics {

// Which renderer is currently driving the viewport -- mirrors Unreal's
// viewport-mode model (Lit/Unlit/Wireframe/etc: several renderers coexist,
// each clearly labeled for what it is, switchable at any time). Rasterized
// is fast-but-approximate (editing/navigation only -- see Rasterizer's
// class comment on why it's never treated as a stand-in for PathTraced's
// output). PathTraced is the ground-truth renderer that also produces the
// training dataset. Splat is reserved for viewing a *trained* 3DGS scene
// loaded back in -- nothing implements it yet (no splat renderer, no .ply
// loader, and nothing to view until a capture has actually been trained
// externally -- see the project's architecture notes). Adding it as an
// enum value now, with no implementation, keeps IViewport's contract
// stable for whenever that lands instead of forcing another interface
// rehaul then.
enum class ViewportMode {
    Rasterized,
    PathTraced,
    Splat,
};

// Common contract for anything that can act as the scene viewport.
// SceneLayer owns one implementation per ViewportMode and dispatches to
// whichever is currently active (see SceneLayer::SetViewportMode) -- this
// is what makes switching between them a one-line dispatch instead of the
// separate hardcoded call sites SceneLayer/App used to have (SceneRenderer
// via Render(), PathTracer via a separately-timed RenderPathTraced()).
//
// Every implementation renders into ITS OWN offscreen color texture
// (GetColorOutput()) rather than the swapchain's backbuffer directly. This
// is a deliberate, uniform contract across renderer types that are
// otherwise structurally quite different -- Rasterizer records ordinary
// draw calls inside its own BeginRendering/EndRendering scope targeting
// its own color+depth textures; PathTracer issues TraceRays with no such
// scope at all (Vulkan disallows ray tracing dispatch inside an active
// dynamic-rendering scope). Making both write to an owned offscreen
// texture instead of the swapchain sidesteps that asymmetry entirely: the
// caller (App) doesn't need to know which category of renderer it's
// driving -- it calls Render() once for whichever viewport is active, then
// displays whatever GetColorOutput() points at via Presenter::Blit. See
// App::RenderFrame for exactly how the two calls are sequenced (Render
// happens before the swapchain's own BeginRendering scope opens, since
// Rasterizer now also needs a scope of its own that can't nest inside
// another one, same restriction PathTracer already had).
class IViewport {
public:
    virtual ~IViewport() = default;

    virtual ViewportMode GetMode() const = 0;

    // Must be called before Render() to size (or resize) the output
    // texture -- typically to match the window/panel this viewport is
    // being displayed in. Cheap to call every frame with an unchanged size
    // (no-ops); each implementation only actually recreates the texture
    // when the requested size differs from what's currently allocated.
    virtual void SetOutputSize(uint32_t width, uint32_t height) = 0;

    // Renders the scene from cameraEntity into GetColorOutput(), sized per
    // the last SetOutputSize call. NOT nested inside any caller-managed
    // BeginRendering/EndRendering scope -- each implementation manages its
    // own render-target scope (or lack thereof) internally. No-ops if
    // cameraEntity is entt::null.
    virtual void Render(ICommandList& cmd, Scene& scene, entt::entity cameraEntity) = 0;

    // The color texture this viewport last rendered into. Left in
    // ResourceState::ShaderReadOnly when Render() returns -- see each
    // implementation for the exact transition -- so Presenter::Blit (or
    // anything else) can sample it directly without an extra barrier.
    virtual TextureHandle GetColorOutput() const = 0;
};

} // namespace spray::graphics