#pragma once

#include "graphics/Context.hpp"
#include <vulkan/vulkan.h>

namespace spray::graphics::vk {

class VulkanContext final : public IContext {
public:
    VulkanContext();
    ~VulkanContext() override;

    BackendType GetBackendType() const override { return BackendType::Vulkan; }
    std::vector<AdapterInfo> EnumerateAdapters() override;
    std::unique_ptr<IDevice> CreateDevice(uint32_t adapterIndex) override;

private:
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    std::vector<VkPhysicalDevice> m_physicalDevices;

    friend class VulkanDevice;
};

} // namespace rbr::vk