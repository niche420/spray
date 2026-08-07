#include "pch.hpp"
#include "App.hpp"
#include "Window.hpp"
#include "graphics/Context.hpp"
#include "graphics/Device.hpp"
#include "graphics/Types.hpp"
#include "ui/UI.hpp"

#include <stdexcept>

namespace spray {
App::App() {
	m_pWnd = std::make_unique<Window>(1280, 720);
	m_pCtx = graphics::IContext::Create(graphics::BackendType::Vulkan);

	graphics::GpuRequirements reqs;
	reqs.minDedicatedVideoMemoryBytes = 4ull * 1024 * 1024 * 1024; // 4GB
	reqs.requireDiscrete = true;
	auto adapters = m_pCtx->EnumerateAdapters();
	auto adapterIndex = graphics::SelectAdapter(adapters, reqs);
	if (!adapterIndex) throw std::runtime_error("No adapter meets requirements");

	auto pDev = m_pCtx->CreateDevice(*adapterIndex);

	graphics::SwapchainDesc scDesc;
	scDesc.windowHandle = m_pWnd->GetSDLWindow();
	auto wndSize = m_pWnd->GetSize();
	scDesc.width = wndSize.x;
	scDesc.height = wndSize.y;
	graphics::SwapchainHandle swapchain = pDev->CreateSwapchain(scDesc);
}

App::~App()
{
}

int32_t App::Run() {
	return 0;
}

} // namespace rb