#pragma once

#include "graphics/GraphicsTypes.hpp"

namespace spray::graphics {
    class IDevice;
}

namespace spray {
class Window;

// Owns a swapchain plus its paired depth buffer -- recreated together on
// resize, since both must always match the window's current size, which
// is the whole reason to bundle them into one object rather than two
// separately-managed resources. Doesn't own Window or IDevice, just reads
// the window's current size at construction and whenever Resize is called.
class Swapchain {
public:
    Swapchain(Window& window, graphics::IDevice& device);
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // Recreates both the swapchain and depth buffer at the new size.
    // Caller must have already called device.WaitIdle() -- Swapchain
    // doesn't do this itself, since App may want to batch other resize-
    // time work under a single WaitIdle rather than paying for it twice.
    // No-ops on a 0-sized request (window minimized) rather than trying to
    // create a 0x0 resource.
    void Resize(uint32_t width, uint32_t height);

    graphics::TextureHandle AcquireNextTexture();
    void Present();

    graphics::TextureHandle GetDepthTexture() const { return m_depthTexture; }
    graphics::Format GetColorFormat() const { return m_colorFormat; }
    graphics::Format GetDepthFormat() const { return m_depthFormat; }

private:
    void CreateDepthTexture(uint32_t width, uint32_t height);
    void DestroyDepthTexture();

    graphics::IDevice& m_device;
    graphics::SwapchainHandle m_swapchain;
    graphics::TextureHandle m_depthTexture;
    graphics::Format m_colorFormat = graphics::Format::BGRA8_UNorm;
    graphics::Format m_depthFormat = graphics::Format::D32_Float;
};

} // namespace spray