#pragma once

#include "ui/UIBackend.hpp"
#include "graphics/GraphicsTypes.hpp"

#include <vulkan/vulkan.h>

namespace spray {
class Swapchain;
}

namespace spray::graphics {
class ICommandList;

namespace vk {
class VulkanDevice;
}
}

namespace spray::ui::vk {

class VulkanUIBackend final : public UIBackend {
public:
    VulkanUIBackend(graphics::vk::VulkanDevice& device, Swapchain& swapchain);
    ~VulkanUIBackend() override;

    void BeginFrame() override;
    void Render(graphics::ICommandList& cmd) override;
    ImTextureID GetTextureID(graphics::TextureHandle texture) override;

private:
    graphics::vk::VulkanDevice& m_device;

    // Single linear/clamp sampler for all viewport-image display -- not
    // meant to reflect any renderer's own filtering choices, just readable
    // ImGui preview. Owned directly (not through IDevice::CreateSampler)
    // since it's purely a UI concern, unrelated to any renderer's sampler
    // set.
    VkSampler m_sampler = VK_NULL_HANDLE;

    // Cache of one -- rebuilt only when the requested handle changes (see
    // GetTextureID). A viewport's output texture handle is stable frame to
    // frame outside of a resize/mode switch, so this avoids calling
    // ImGui_ImplVulkan_AddTexture/_RemoveTexture every frame for no reason.
    graphics::TextureHandle m_cachedTexture;
    VkDescriptorSet m_cachedDescriptorSet = VK_NULL_HANDLE;
};

} // namespace spray::ui::vk