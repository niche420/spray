#pragma once

#include "graphics/GraphicsTypes.hpp"

#include <imgui.h>

namespace spray::graphics {
    class ICommandList;
}

namespace spray::ui {

// One implementation per graphics backend (VulkanUIBackend today,
// eventually a D3D12 one). UIManager owns one polymorphically via
// std::unique_ptr<UIBackend> and never touches backend-specific headers
// (vulkan.h, d3d12.h, ...) itself -- see UIManager.cpp's factory dispatch
// for the one place backend selection actually happens. This is what lets
// App.cpp stay free of #ifdef SPRAY_VULKAN_ENABLED entirely; the only place a backend's build flag matters now is
// CMakeLists.txt (which factory function actually gets compiled/linked).
class UIBackend {
public:
    virtual ~UIBackend() = default;

    // ImGui's per-frame NewFrame equivalent for this backend (e.g.
    // ImGui_ImplVulkan_NewFrame). Called once per frame before any ImGui::
    // calls, from UIManager::BeginFrame.
    virtual void BeginFrame() = 0;

    // Records ImGui's already-rendered draw data (ImGui::Render() must
    // have been called already this frame -- see UIManager::EndFrame) into
    // `cmd`. Must be called inside an active BeginRendering/EndRendering
    // scope targeting the same color format the backend was constructed
    // with. The concrete backend is responsible for downcasting `cmd` to
    // its own native command-list type (e.g. VulkanUIBackend casts to
    // VulkanCommandList to get a raw VkCommandBuffer) -- callers just pass
    // the generic ICommandList they already have.
    virtual void Render(graphics::ICommandList& cmd) = 0;

    // Returns an ImTextureID for sampling `texture` via ImGui::Image --
    // used to draw an IViewport's offscreen output (see Viewport.hpp) as
    // a normal dockable ImGui panel instead of a fullscreen blit. The
    // concrete backend caches the underlying descriptor per texture and
    // only rebuilds it when the handle changes (e.g. the viewport's
    // output texture was recreated on resize) -- cheap to call every
    // frame with the same handle. `texture` must currently be in
    // ResourceState::ShaderReadOnly, same contract as IViewport::
    // GetColorOutput().
    virtual ImTextureID GetTextureID(graphics::TextureHandle texture) = 0;
};

} // namespace spray::ui