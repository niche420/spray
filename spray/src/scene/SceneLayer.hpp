#pragma once

#include "event/Layer.hpp"
#include "Scene.hpp"
#include "graphics/Device.hpp"
#include "SceneRenderer.hpp"
#include "graphics/Pathtracer.hpp"
#include "asset/AssetManager.hpp"

#include <entt/entt.hpp>

#include <memory>
#include <string>

namespace spray::graphics {
class ICommandList;
}

namespace spray {

// Owns the loaded Scene, its asset layer, its GPU-side renderer, the
// active (WASD fly-)camera, and the editor panels for inspecting/loading
// scenes (outliner, inspector, content browser). Deliberately not split
// into a separate UI-owning layer -- there's no ImGuiLayer in this app;
// each layer draws its own panels in its own OnImGuiRender, since a
// generic "UI layer" would otherwise need direct knowledge of every other
// layer's internals just to draw their panels for them.
//
// Now also owns a PathTracer alongside SceneRenderer -- mirrors Unreal's
// viewport-mode model (multiple renderers, each clearly scoped to what
// it's for, not a single "the" renderer): SceneRenderer is the fast raster
// preview for spatial editing, PathTracer is the ground-truth renderer
// that also produces the training dataset. Neither is "more correct" than
// the other in a UI sense -- they're different tools. There's no mode
// switch yet (both currently just run every frame; PathTracer's output
// isn't displayed anywhere yet, see RenderPathTraced's comment), but this
// is the seam a future toggle hangs off of.
class SceneLayer : public Layer {
public:
    SceneLayer(graphics::IDevice& device, graphics::Format colorFormat, graphics::Format depthFormat);
    ~SceneLayer() override;

    void OnAttach() override;
    void OnUpdate(float deltaSeconds) override;
    void OnImGuiRender() override;
    void OnEvent(event::Event& e) override;

    // Called from App::RenderFrame inside an active BeginRendering scope --
    // deliberately not part of the Layer interface itself, since recording
    // draw commands needs the frame's ICommandList, which OnUpdate/
    // OnImGuiRender don't have access to (Layer's interface is generic
    // across layers that may have nothing to do with rendering at all).
    void Render(graphics::ICommandList& cmd, float aspectRatio);

    // Called from App::RenderFrame OUTSIDE any BeginRendering/EndRendering
    // scope -- TraceRays (recorded inside PathTracer::Render) is not valid
    // to call while a Vulkan dynamic-rendering scope is active, unlike
    // ordinary draw calls, so this can't just be folded into Render()
    // above. No-op if there's no active camera, same guard Render() uses.
    void RenderPathTraced(graphics::ICommandList& cmd);

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

    void DrawOutlinerPanel();
    void DrawInspectorPanel();
    void DrawContentBrowserPanel();

    graphics::IDevice& m_device;
    graphics::Format m_colorFormat;
    graphics::Format m_depthFormat;

    std::unique_ptr<assets::AssetManager> m_pAssets;
    std::unique_ptr<Scene> m_pScene;
    std::unique_ptr<graphics::SceneRenderer> m_pSceneRenderer;
    std::unique_ptr<graphics::PathTracer> m_pPathTracer;
    entt::entity m_activeCamera = entt::null;

    // UI-only state -- deliberately not stored on Scene, since "what's
    // selected in the editor" isn't simulation state.
    entt::entity m_selectedEntity = entt::null;
    char m_loadPathBuffer[256] = "assets/backrooms.glb";
};

} // namespace spray