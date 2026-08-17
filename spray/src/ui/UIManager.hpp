#pragma once

#include "UIBackend.hpp"
#include "graphics/GraphicsTypes.hpp"

#include <memory>

struct ImGuiContext;
struct SDL_Window;

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
    UIManager(SDL_Window* wnd, graphics::IDevice& device, graphics::Format swapchainColorFormat,
        uint32_t swapchainImageCount);
    ~UIManager();

    void BeginFrame();
    void EndFrame();

    // Records this frame's ImGui draw data via the active UIBackend. Must
    // be called after EndFrame() (which is what actually populates the
    // draw data via ImGui::Render()), inside an active BeginRendering/
    // EndRendering scope.
    void Render(graphics::ICommandList& cmd);

private:
    ImGuiContext* m_ctx;
    SDL_Window* m_sdlWindow;
    std::unique_ptr<UIBackend> m_backend;
};

} // namespace spray::ui