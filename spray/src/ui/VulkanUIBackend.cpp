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
    : m_device(device)
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

    // Owned directly rather than through IDevice::CreateSampler -- purely a
    // UI-display concern (see header comment), unrelated to any renderer's
    // own sampler set.
    VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device.GetDevice(), &samplerInfo, nullptr, &m_sampler);
}

VulkanUIBackend::~VulkanUIBackend() {
    if (m_cachedDescriptorSet) ImGui_ImplVulkan_RemoveTexture(m_cachedDescriptorSet);
    if (m_sampler) vkDestroySampler(m_device.GetDevice(), m_sampler, nullptr);
    ImGui_ImplVulkan_Shutdown();
}

void VulkanUIBackend::BeginFrame() {
    ImGui_ImplVulkan_NewFrame();
}

void VulkanUIBackend::Render(graphics::ICommandList& cmd) {
    auto& vkCmd = static_cast<graphics::vk::VulkanCommandList&>(cmd);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vkCmd.GetCommandBuffer());
}

ImTextureID VulkanUIBackend::GetTextureID(graphics::TextureHandle texture) {
    if (texture != m_cachedTexture) {
        if (m_cachedDescriptorSet) {
            ImGui_ImplVulkan_RemoveTexture(m_cachedDescriptorSet);
            m_cachedDescriptorSet = VK_NULL_HANDLE;
        }
        graphics::vk::NativeTexture& tex = m_device.GetNativeTexture(texture);
        m_cachedDescriptorSet = ImGui_ImplVulkan_AddTexture(m_sampler, tex.view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        m_cachedTexture = texture;
    }
    return reinterpret_cast<ImTextureID>(m_cachedDescriptorSet);
}

} // namespace spray::ui::vk

namespace spray::ui {
std::unique_ptr<UIBackend> CreateVulkanUIBackend(graphics::IDevice& device, Swapchain& swapchain) {
    auto& vkDevice = static_cast<graphics::vk::VulkanDevice&>(device);
    return std::make_unique<vk::VulkanUIBackend>(vkDevice, swapchain);
}
} // namespace spray::ui