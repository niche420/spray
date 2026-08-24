#include "pch.hpp"
#include "UIManager.hpp"

#include "Swapchain.hpp"
#include "Window.hpp"

#include "graphics/Device.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>

#include <stdexcept>

namespace spray::ui {

// One factory per backend, defined in that backend's own translation unit
// (e.g. VulkanUIBackend.cpp, only compiled in when SPRAY_ENABLE_VULKAN is
// on -- see spray/CMakeLists.txt) so this file never needs to include a
// backend-specific header. Mirrors how graphics::IContext::Create
// dispatches to CreateVulkanContext()/CreateD3D12Context() the same way in
// Context.cpp, without Context.cpp itself depending on either backend.
std::unique_ptr<UIBackend> CreateVulkanUIBackend(graphics::IDevice& device, Swapchain& swapchain);

UIManager::UIManager(Window& wnd, graphics::IDevice& device, Swapchain& swapchain)
    : m_ctx(nullptr) {
    IMGUI_CHECKVERSION();
    m_ctx = ImGui::CreateContext();
    if (!m_ctx) {
        throw std::runtime_error("Failed to create ImGui context");
    }
    ImGui::StyleColorsDark();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplSDL3_InitForOther(wnd.GetSDLWindow());

    switch (device.GetBackendType()) {
    case graphics::BackendType::Vulkan:
        m_backend = CreateVulkanUIBackend(device, swapchain);
        break;
    case graphics::BackendType::D3D12:
        // No D3D12UIBackend yet -- matches the D3D12 graphics backend
        // itself not being wired up to IDevice/ICommandList currently
        // (see the namespace-mismatch note wherever D3D12Device.hpp is
        // discussed). Add a CreateD3D12UIBackend() alongside that work.
        throw std::runtime_error("UIManager: no UI backend implemented for D3D12 yet");
    }
}

UIManager::~UIManager() {
    m_backend.reset(); // must be destroyed before ImGui_ImplSDL3_Shutdown/DestroyContext
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_ctx);
    m_ctx = nullptr;
}

void UIManager::BeginFrame() {
    m_backend->BeginFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport();
}

void UIManager::EndFrame() {
    ImGui::Render(); // populates draw data; actual GPU recording happens in Render()
}

void UIManager::Render(graphics::ICommandList& cmd) {
    m_backend->Render(cmd);
}

} // namespace spray::ui