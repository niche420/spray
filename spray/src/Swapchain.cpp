#include "pch.hpp"
#include "Swapchain.hpp"
#include "Window.hpp"
#include "graphics/Device.hpp"

namespace spray {

Swapchain::Swapchain(Window& window, graphics::IDevice& device) : m_device(device) {
    auto size = window.GetSize();

    graphics::SwapchainDesc scDesc;
    scDesc.windowHandle = window.GetSDLWindow();
    scDesc.width = size.x;
    scDesc.height = size.y;
    scDesc.format = m_colorFormat;
    m_swapchain = m_device.CreateSwapchain(scDesc);

    CreateDepthTexture(size.x, size.y);
}

Swapchain::~Swapchain() {
    DestroyDepthTexture();
    if (m_swapchain.IsValid()) {
        m_device.DestroySwapchain(m_swapchain);
        m_swapchain = {};
    }
}

void Swapchain::CreateDepthTexture(uint32_t width, uint32_t height) {
    graphics::TextureDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = m_depthFormat;
    depthDesc.usage = graphics::TextureUsage::DepthStencil;
    m_depthTexture = m_device.CreateTexture(depthDesc);
}

void Swapchain::DestroyDepthTexture() {
    if (m_depthTexture.IsValid()) {
        m_device.DestroyTexture(m_depthTexture);
        m_depthTexture = {};
    }
}

void Swapchain::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_device.ResizeSwapchain(m_swapchain, width, height);
    DestroyDepthTexture();
    CreateDepthTexture(width, height);
}

graphics::TextureHandle Swapchain::AcquireNextTexture() {
    return m_device.AcquireNextSwapchainTexture(m_swapchain);
}

void Swapchain::Present() {
    m_device.Present(m_swapchain);
}

} // namespace spray