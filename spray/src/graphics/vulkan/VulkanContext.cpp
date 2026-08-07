#include "pch.hpp"
#include "VulkanContext.hpp"
#include "VulkanDevice.hpp"
#include "VulkanCommon.hpp"

#include <cstring>

namespace spray::graphics {
std::unique_ptr<IContext> CreateVulkanContext()
{
    return std::make_unique<vk::VulkanContext>();
}
}

namespace spray::graphics::vk {

namespace {
#ifdef _DEBUG
constexpr bool kEnableValidation = true;
#else
constexpr bool kEnableValidation = false;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void*) {
    fprintf(stderr, "[vulkan] %s\n", pCallbackData->pMessage);
    return VK_FALSE;
}
} // namespace

VulkanContext::VulkanContext() {
    VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = {
        VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
        "VK_KHR_win32_surface",
#endif
    };
    std::vector<const char*> layers;
    if (kEnableValidation) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    createInfo.ppEnabledLayerNames = layers.data();

    VK_CHECK(vkCreateInstance(&createInfo, nullptr, &m_instance));

    if (kEnableValidation) {
        auto createDebugUtils = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
        if (createDebugUtils) {
            VkDebugUtilsMessengerCreateInfoEXT dbgInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
            dbgInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                       VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            dbgInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            dbgInfo.pfnUserCallback = DebugCallback;
            createDebugUtils(m_instance, &dbgInfo, nullptr, &m_debugMessenger);
        }
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    m_physicalDevices.resize(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, m_physicalDevices.data());
}

VulkanContext::~VulkanContext() {
    if (m_debugMessenger) {
        auto destroyDebugUtils = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroyDebugUtils) destroyDebugUtils(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
}

std::vector<AdapterInfo> VulkanContext::EnumerateAdapters() {
    std::vector<AdapterInfo> result;
    for (uint32_t i = 0; i < m_physicalDevices.size(); ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physicalDevices[i], &props);

        VkPhysicalDeviceMemoryProperties memProps;
        vkGetPhysicalDeviceMemoryProperties(m_physicalDevices[i], &memProps);

        size_t dedicatedMemory = 0;
        for (uint32_t h = 0; h < memProps.memoryHeapCount; ++h) {
            if (memProps.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                dedicatedMemory += memProps.memoryHeaps[h].size;
            }
        }

        AdapterInfo info;
        info.index = i;
        info.name = props.deviceName;
        info.vendorId = props.vendorID;
        info.deviceId = props.deviceID;
        info.dedicatedVideoMemoryBytes = dedicatedMemory;
        info.isIntegrated = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
        result.push_back(info);
    }
    return result;
}

std::unique_ptr<IDevice> VulkanContext::CreateDevice(uint32_t adapterIndex) {
    if (adapterIndex >= m_physicalDevices.size()) {
        throw std::runtime_error("Adapter index out of range");
    }
    return std::make_unique<VulkanDevice>(m_instance, m_physicalDevices[adapterIndex]);
}

} // namespace spray::graphics::vk