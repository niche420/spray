#include "pch.hpp"
#include "App.hpp"
#include "Window.hpp"
#include "Swapchain.hpp"
#include "event/Input.hpp"
#include "graphics/Context.hpp"
#include "graphics/Device.hpp"
#include "graphics/CommandList.hpp"
#include "scene/SceneLayer.hpp"
#include "capture/CaptureLayer.hpp"
#include "ui/UIManager.hpp"

#include <imgui.h>

#include <chrono>
#include <stdexcept>

namespace spray {

using namespace event;

App::App() {
	m_pWnd = std::make_unique<Window>(1280, 720);
	m_pWnd->SetEventCallback([this](Event& e) { OnEvent(e); });

	m_pCtx = graphics::IContext::Create(graphics::BackendType::Vulkan);
	if (!m_pCtx) throw std::runtime_error("Failed to create graphics context (Vulkan backend not built in?)");

	graphics::GpuRequirements reqs;
	reqs.minDedicatedVideoMemoryBytes = 2ull * 1024 * 1024 * 1024; // 2GB -- skip weak integrated GPUs without hard-failing modest discrete cards
	reqs.requireDiscrete = true;
	auto adapters = m_pCtx->EnumerateAdapters();
	auto adapterIndex = graphics::SelectAdapter(adapters, reqs);
	if (!adapterIndex) throw std::runtime_error("No adapter meets requirements");

	m_pDevice = m_pCtx->CreateDevice(*adapterIndex);
	m_pSwapchain = std::make_unique<Swapchain>(*m_pWnd, *m_pDevice);

	// No longer takes color/depth format -- SceneLayer's viewports each own
	// their own offscreen textures with fixed formats now, independent of
	// whatever the swapchain happens to use (see Rasterizer's class
	// comment on why that coupling went away).
	auto sceneLayer = std::make_unique<SceneLayer>(*m_pDevice);
	m_pSceneLayer = sceneLayer.get();
	m_layerStack.PushLayer(std::move(sceneLayer));

	// CaptureLayer drives SceneLayer's own PathTracer instance offline
	// (see CaptureLayer's class comment) -- pushed after SceneLayer since
	// it needs a live SceneLayer& to reach into. Order relative to
	// SceneLayer in the stack doesn't otherwise matter: it only draws its
	// own ImGui panel and has no OnUpdate/OnEvent behavior to interleave.
	auto captureLayer = std::make_unique<CaptureLayer>(*m_pDevice, *m_pSceneLayer);
	m_layerStack.PushLayer(std::move(captureLayer));

	m_pUI = std::make_unique<ui::UIManager>(*m_pWnd, *m_pDevice, *m_pSwapchain);

	// The active viewport is now drawn as a docked ImGui::Image panel (see
	// SceneLayer::DrawViewportPanel) rather than a fullscreen Presenter
	// blit, so SceneLayer needs a way to turn a TextureHandle into an
	// ImTextureID -- that's UIManager's job (it owns the backend-specific
	// descriptor machinery).
	m_pSceneLayer->SetUIManager(*m_pUI);
}

App::~App() {
	if (m_pDevice) {
		m_pDevice->WaitIdle(); // GPU work must be done before anything below tears down
		// m_layerStack, m_pUI, m_pSwapchain are all declared after
		// m_pDevice in App.hpp, so ordinary reverse-declaration-order
		// destruction already tears them down (in that order) before
		// m_pDevice -- nothing else needed here.
	}
}

void App::HandleResize(uint32_t width, uint32_t height) {
	if (width == 0 || height == 0) return; // minimized
	m_pDevice->WaitIdle();
	m_pSwapchain->Resize(width, height);
}

void App::OnEvent(event::Event& e) {
	// Input sees every raw event unconditionally, before anything gets a
	// chance to mark it handled -- see Input's class comment for why.
	Input::OnEvent(e);

	// ImGui gets first refusal on mouse/keyboard input. This is what an
	// ImGui overlay layer sitting atop the LayerStack would otherwise be
	// for -- but since UIManager is a plain member here (not a Layer;
	// there's no ImGuiLayer, see App.hpp), the check just happens directly
	// here, before events reach m_layerStack, achieving the same "don't
	// move the scene camera from a click that landed on an ImGui panel"
	// result without needing a stack member whose only job was event
	// ordering.
	ImGuiIO& io = ImGui::GetIO();
	bool wantsMouse = io.WantCaptureMouse;
	bool wantsKeyboard = io.WantCaptureKeyboard;
	DispatchEvent<MouseMovedEvent>(e, [&](const auto&) { return wantsMouse; });
	DispatchEvent<MouseButtonPressedEvent>(e, [&](const auto&) { return wantsMouse; });
	DispatchEvent<MouseButtonReleasedEvent>(e, [&](const auto&) { return wantsMouse; });
	DispatchEvent<MouseScrolledEvent>(e, [&](const auto&) { return wantsMouse; });
	DispatchEvent<KeyPressedEvent>(e, [&](const auto&) { return wantsKeyboard; });
	DispatchEvent<KeyReleasedEvent>(e, [&](const auto&) { return wantsKeyboard; });

	// App owns swapchain lifetime, so it handles resize directly rather
	// than leaving it to a layer. Deliberately doesn't mark the event
	// handled afterward -- layers might reasonably still want to know a
	// resize happened (e.g. a future "viewport size" readout).
	DispatchEvent<WindowResizeEvent>(e, [this](const auto& ev) {
		HandleResize(ev.width, ev.height);
		return false;
		});

	m_layerStack.OnEvent(e);
}

void App::RenderFrame(graphics::ICommandList* cmd) {
	graphics::TextureHandle backBuffer = m_pSwapchain->AcquireNextTexture();

	cmd->TransitionTextures({
		{ backBuffer, graphics::ResourceState::Undefined, graphics::ResourceState::RenderTarget },
		});

	graphics::ColorAttachment colorAttachment;
	colorAttachment.texture = backBuffer;
	// Cleared now -- areas of the dockspace outside any panel (or between
	// panels) need an actual clear rather than being left to whatever the
	// swapchain image last held, now that nothing draws a fullscreen quad
	// over the whole backbuffer first (the viewport itself is just one
	// docked ImGui::Image among possibly several panels).
	colorAttachment.clear = true;
	colorAttachment.clearColor[0] = 0.05f;
	colorAttachment.clearColor[1] = 0.05f;
	colorAttachment.clearColor[2] = 0.08f;
	colorAttachment.clearColor[3] = 1.0f;

	// No depth attachment -- ImGui doesn't do any depth testing, and the
	// swapchain's own depth texture isn't used by anything in this scope
	// (Rasterizer/PathTracer each own their own).
	graphics::DepthAttachment depthAttachment;

	cmd->BeginRendering({ colorAttachment }, depthAttachment);

	m_pUI->Render(*cmd);

	cmd->EndRendering();
	cmd->TransitionTextures({
		{ backBuffer, graphics::ResourceState::RenderTarget, graphics::ResourceState::Present }
		});

	m_previousFrameFence = m_pDevice->Submit(cmd);
	m_pSwapchain->Present();
}

int32_t App::Run() {
	auto lastTime = std::chrono::steady_clock::now();

	while (m_pWnd->IsOpen()) {
		auto now = std::chrono::steady_clock::now();
		float dt = std::chrono::duration<float>(now - lastTime).count();
		lastTime = now;

		m_pWnd->PollEvents(); // dispatches into App::OnEvent -> Input + ImGui capture check + LayerStack

		// Wait for the PREVIOUS frame's GPU work to finish before anything
		// below touches GPU resources.
		if (m_previousFrameFence) m_pDevice->WaitForFence(*m_previousFrameFence);

		m_pUI->BeginFrame();
		m_layerStack.OnUpdate(dt);

		// Render the active viewport BEFORE OnImGuiRender runs. This is the
		// critical ordering fix: SceneLayer::DrawViewportPanel (called from
		// OnImGuiRender) reads the active viewport's CURRENT color texture
		// and hands it to UIManager::GetTextureID, which may destroy/
		// replace a cached ImGui descriptor set if the texture handle
		// changed since last frame. RenderActiveViewport is what can
		// actually resize/destroy/recreate that texture (Rasterizer::
		// EnsureOutputTextures, when the viewport panel's reported size
		// changes). With the old ordering (viewport render happening
		// later, inside RenderFrame), a same-frame resize could destroy
		// the exact VkImageView an ImGui draw command had already been
		// queued against a few lines earlier in OnImGuiRender -- a
		// same-frame GPU use-after-free, not just a one-frame-stale
		// display. Rendering the viewport first means any resize/recreate
		// is already settled by the time DrawViewportPanel reads the
		// handle, so what ImGui queues this frame is guaranteed to still
		// be valid when m_pUI->Render actually records against it below.
		graphics::ICommandList* cmd = m_pDevice->BeginCommandList();
		auto viewportSize = m_pSceneLayer->GetViewportPanelSize();
		m_pSceneLayer->RenderActiveViewport(*cmd, viewportSize.x, viewportSize.y);

		m_layerStack.OnImGuiRender();
		m_pUI->EndFrame();

		RenderFrame(cmd);
	}

	return 0;
}

} // namespace spray