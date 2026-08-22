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
};

} // namespace spray::ui::vk