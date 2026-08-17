#include "pch.hpp"
#include "VulkanUIBackend.hpp"
#include "graphics/vulkan/VulkanDevice.hpp"
#include "graphics/vulkan/VulkanCommandList.hpp"
#include "graphics/vulkan/VulkanCommon.hpp"
#include "graphics/CommandList.hpp"
#include "graphics/Device.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace spray::graphics::vk {

VulkanUIBackend::VulkanUIBackend(VulkanDevice& device, Format swapchainColorFormat, uint32_t swapchainImageCount)
    : m_colorFormat(ToVkFormat(swapchainColorFormat)) {

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = device.GetInstance();
    initInfo.PhysicalDevice = device.GetPhysicalDevice();
    initInfo.Device = device.GetDevice();
    initInfo.QueueFamily = device.GetQueueFamilyIndex();
    initInfo.Queue = device.GetQueue();
    // Reuses VulkanDevice's own descriptor pool (created with
    // VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT and a
    // COMBINED_IMAGE_SAMPLER allowance -- see VulkanDevice.cpp) rather than
    // creating a second pool just for ImGui's font atlas descriptor.
    initInfo.DescriptorPool = device.GetDescriptorPool();
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = swapchainImageCount;
    initInfo.UseDynamicRendering = true;

    ImGui_ImplVulkan_Init(&initInfo);
}

VulkanUIBackend::~VulkanUIBackend() {
    ImGui_ImplVulkan_Shutdown();
}

void VulkanUIBackend::BeginFrame() {
    ImGui_ImplVulkan_NewFrame();
}

void VulkanUIBackend::Render(graphics::ICommandList& cmd) {
    auto& vkCmd = static_cast<VulkanCommandList&>(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd.GetCommandBuffer());
}

} // namespace spray::graphics::vk

namespace spray::ui {

    // Definition of the factory UIManager.cpp declares and calls -- lives here
    // (rather than in UIManager.cpp) specifically so UIManager.cpp never needs
    // to include VulkanDevice.hpp or vulkan.h. Same split as graphics::
    // CreateVulkanContext() living in VulkanContext.cpp rather than Context.cpp.
    std::unique_ptr<UIBackend> CreateVulkanUIBackend(graphics::IDevice& device, graphics::Format swapchainColorFormat,
        uint32_t swapchainImageCount) {
        auto& vkDevice = static_cast<graphics::vk::VulkanDevice&>(device);
        return std::make_unique<graphics::vk::VulkanUIBackend>(vkDevice, swapchainColorFormat, swapchainImageCount);
    }

} // namespace spray::ui