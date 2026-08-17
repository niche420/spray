#pragma once

#include "ui/UIBackend.hpp"
#include "graphics/GraphicsTypes.hpp"

#include <vulkan/vulkan.h>

namespace spray::graphics {
    class ICommandList;
}

namespace spray::graphics::vk {
class VulkanDevice;

class VulkanUIBackend final : public ui::UIBackend {
public:
    VulkanUIBackend(VulkanDevice& device, Format swapchainColorFormat, uint32_t swapchainImageCount);
    ~VulkanUIBackend() override;

    void BeginFrame() override;
    void Render(graphics::ICommandList& cmd) override;

private:
    // Assumes dynamic rendering (VK_KHR_dynamic_rendering), matching how
    // SceneRenderer's own pipelines are built -- no VkRenderPass anywhere
    // in this engine, so ImGui's Vulkan backend is configured the same
    // way via UseDynamicRendering/PipelineRenderingCreateInfo rather than
    // a render-pass handle.
    VkFormat m_colorFormat;
};

} // namespace spray::graphics::vk