#pragma once

#include "UIBackend.hpp"
#include "graphics/GraphicsTypes.hpp"

#include <memory>

struct ImGuiContext;

namespace spray {
class Swapchain;
class Window;
}

namespace spray::graphics {
class IDevice;
}

namespace spray::ui {

// Owns the ImGui context, the SDL3<->ImGui glue (backend-agnostic --
// ImGui_ImplSDL3_InitForOther doesn't care which graphics API renders the
// result), and one UIBackend selected by the device's backend type.
class UIManager {
public:
    // Throws if `device`'s backend has no UIBackend implementation yet
    // (currently: anything but Vulkan -- see UIManager.cpp).
    UIManager(Window& wnd, graphics::IDevice& device, Swapchain& swapchain);
    ~UIManager();

    void BeginFrame();
    void EndFrame();

    // Records this frame's ImGui draw data via the active UIBackend. Must
    // be called after EndFrame() (which is what actually populates the
    // draw data via ImGui::Render()), inside an active BeginRendering/
    // EndRendering scope.
    void Render(graphics::ICommandList& cmd);

    // Passthrough to the active backend -- see UIBackend::GetTextureID.
    // Used by SceneLayer's docked viewport panel to display an IViewport's
    // color output via ImGui::Image.
    ImTextureID GetTextureID(graphics::TextureHandle texture) { return m_backend->GetTextureID(texture); }

private:
    ImGuiContext* m_ctx;
    std::unique_ptr<UIBackend> m_backend;
};

} // namespace spray::ui