#include "pch.hpp"
#include "VulkanUIBackend.hpp"

#include "Swapchain.hpp"

#include "graphics/vulkan/VulkanDevice.hpp"
#include "graphics/vulkan/VulkanCommandList.hpp"
#include "graphics/vulkan/VulkanCommon.hpp"
#include "graphics/CommandList.hpp"
#include "graphics/Device.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>

namespace spray::ui::vk {

VulkanUIBackend::VulkanUIBackend(graphics::vk::VulkanDevice& device, Swapchain& swapchain)
{
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance = device.GetInstance();
    initInfo.PhysicalDevice = device.GetPhysicalDevice();
    initInfo.Device = device.GetDevice();
    initInfo.QueueFamily = device.GetQueueFamilyIndex();
    initInfo.Queue = device.GetQueue();
    initInfo.PipelineInfoMain.RenderPass = nullptr;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    const VkFormat colorFormat = graphics::vk::ToVkFormat(swapchain.GetColorFormat());
    const VkFormat depthFormat = graphics::vk::ToVkFormat(swapchain.GetDepthFormat());
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = depthFormat;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
    // Reuses VulkanDevice's own descriptor pool (created with
    // VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT and a
    // COMBINED_IMAGE_SAMPLER allowance -- see VulkanDevice.cpp) rather than
    // creating a second pool just for ImGui's font atlas descriptor.
    initInfo.DescriptorPool = device.GetDescriptorPool();
    initInfo.MinImageCount = 2;
    initInfo.ImageCount = swapchain.GetBufferCount();
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
    auto& vkCmd = static_cast<graphics::vk::VulkanCommandList&>(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd.GetCommandBuffer());
}

} // namespace spray::ui::vk

namespace spray::ui {
std::unique_ptr<UIBackend> CreateVulkanUIBackend(graphics::IDevice& device, Swapchain& swapchain) {
    auto& vkDevice = static_cast<graphics::vk::VulkanDevice&>(device);
    return std::make_unique<vk::VulkanUIBackend>(vkDevice, swapchain);
}
} // namespace spray::ui