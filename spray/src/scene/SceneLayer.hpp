#pragma once

#include "event/Layer.hpp"
#include "Scene.hpp"
#include "graphics/Device.hpp"
#include "graphics/Viewport.hpp"
#include "graphics/Rasterizer.hpp"
#include "graphics/Pathtracer.hpp"
#include "graphics/shaders/ShaderLibrary.hpp"
#include "asset/AssetManager.hpp"

#include <entt/entt.hpp>

#include <memory>
#include <string>

namespace spray::graphics {
class ICommandList;
}

namespace spray {

// Owns the loaded Scene, its asset layer, its set of switchable viewports
// (see graphics/Viewport.hpp), and the editor panels for inspecting/
// loading scenes (outliner, inspector, content browser) plus picking which
// viewport is active. Deliberately not split into a separate UI-owning
// layer -- there's no ImGuiLayer in this app; each layer draws its own
// panels in its own OnImGuiRender, since a generic "UI layer" would
// otherwise need direct knowledge of every other layer's internals just to
// draw their panels for them.
//
// Rasterizer and PathTracer are both IViewport implementations now
// (renamed from SceneRenderer -- see Rasterizer's class comment on why),
// switched between via SetViewportMode rather than being called through
// separate hardcoded entry points. A future SplatViewer slots in the same
// way, as ViewportMode::Splat, without needing any changes here beyond
// constructing one and adding it to GetActiveViewport's switch.
class SceneLayer : public Layer {
public:
    SceneLayer(graphics::IDevice& device);
    ~SceneLayer() override;

    void OnAttach() override;
    void OnUpdate(float deltaSeconds) override;
    void OnImGuiRender() override;
    void OnEvent(event::Event& e) override;

    // Called from App::RenderFrame OUTSIDE any BeginRendering/EndRendering
    // scope -- every IViewport implementation manages its own render
    // target scope (or lack of one) internally now, and none of them can
    // be nested inside another active scope (Vulkan disallows nesting
    // dynamic-rendering scopes, and disallows ray tracing dispatch inside
    // one at all -- see Viewport.hpp's class comment). width/height size
    // the active viewport's output to match whatever panel/window it'll
    // be displayed in.
    void RenderActiveViewport(graphics::ICommandList& cmd, uint32_t width, uint32_t height);

    void SetViewportMode(graphics::ViewportMode mode) { m_activeMode = mode; }
    graphics::ViewportMode GetViewportMode() const { return m_activeMode; }

    // The currently active viewport's rendered output -- what App should
    // hand to Presenter::Blit this frame.
    graphics::TextureHandle GetActiveColorOutput() const;

    // Needed by App to construct its Presenter (see App.cpp) against the
    // same shared cache Rasterizer/PathTracer already load through.
    graphics::shaders::ShaderLibrary& GetShaderLibrary() { return *m_pShaderLibrary; }

    assets::AssetManager& GetAssetManager() { return *m_pAssets; }
    Scene& GetScene() { return *m_pScene; }

private:
    // Shared by the content browser's "Load" button and the
    // SceneLoadRequestedEvent handler -- the event path exists for future
    // triggers that don't have a direct SceneLayer reference to call this
    // on (a future drag-and-drop handler in Window/App, say); the button
    // has direct access already, so it just calls this instead of
    // round-tripping through the event system for no reason.
    void LoadScene(const std::string& path);

    graphics::IViewport& GetActiveViewport();

    void DrawOutlinerPanel();
    void DrawInspectorPanel();
    void DrawContentBrowserPanel();
    void DrawViewportModePanel();

    graphics::IDevice& m_device;

    std::unique_ptr<assets::AssetManager> m_pAssets;
    std::unique_ptr<Scene> m_pScene;
    std::unique_ptr<graphics::shaders::ShaderLibrary> m_pShaderLibrary;
    std::unique_ptr<graphics::Rasterizer> m_pRasterizer;
    std::unique_ptr<graphics::PathTracer> m_pPathTracer;
    graphics::ViewportMode m_activeMode = graphics::ViewportMode::Rasterized;

    entt::entity m_activeCamera = entt::null;

    // UI-only state -- deliberately not stored on Scene, since "what's
    // selected in the editor" isn't simulation state.
    entt::entity m_selectedEntity = entt::null;
    char m_loadPathBuffer[256] = "assets/backrooms.glb";
};

} // namespace spray