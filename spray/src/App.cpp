#include "pch.hpp"
#include "App.hpp"
#include "Window.hpp"
#include "Swapchain.hpp"
#include "event/Input.hpp"
#include "graphics/Context.hpp"
#include "graphics/Device.hpp"
#include "graphics/CommandList.hpp"
#include "scene/SceneLayer.hpp"
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

	auto sceneLayer = std::make_unique<SceneLayer>(*m_pDevice, m_pSwapchain->GetColorFormat(),
		m_pSwapchain->GetDepthFormat());
	m_pSceneLayer = sceneLayer.get();
	m_layerStack.PushLayer(std::move(sceneLayer));

	m_pUI = std::make_unique<ui::UIManager>(m_pWnd->GetSDLWindow(), *m_pDevice, m_pSwapchain->GetColorFormat(),
		/*swapchainImageCount=*/2);
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

void App::RenderFrame() {
	if (m_previousFrameFence) m_pDevice->WaitForFence(*m_previousFrameFence);

	graphics::TextureHandle backBuffer = m_pSwapchain->AcquireNextTexture();
	graphics::ICommandList* cmd = m_pDevice->BeginCommandList();

	// PathTracer's Render (TraceRays + its own BLAS/TLAS builds) is
	// recorded here, deliberately BEFORE the raster BeginRendering scope
	// opens below -- TraceRays is not valid to record inside an active
	// Vulkan dynamic-rendering scope, unlike an ordinary draw call, so this
	// can't be folded into the raster pass the way UI rendering is. Its
	// output isn't displayed anywhere yet (see SceneLayer::RenderPathTraced's
	// comment) -- this call exists right now purely to exercise the whole
	// pipeline end to end every frame.
	m_pSceneLayer->RenderPathTraced(*cmd);

	// NOTE: depth texture's "before" state is hardcoded to Undefined every
	// frame, even though after frame 1 its real state is DepthWrite from
	// the previous frame. No persistent per-resource state tracker exists
	// in this engine (every call site manually specifies before/after) --
	// transitioning from Undefined is safe here specifically because the
	// depth attachment is cleared every frframe regardless (clear=true below
	// discards old contents anyway), but this pattern would be wrong for a
	// resource whose contents need to persist across frames.
	cmd->TransitionTextures({
		{ backBuffer, graphics::ResourceState::Undefined, graphics::ResourceState::RenderTarget },
		{ m_pSwapchain->GetDepthTexture(), graphics::ResourceState::Undefined, graphics::ResourceState::DepthWrite },
		});

	graphics::ColorAttachment colorAttachment;
	colorAttachment.texture = backBuffer;
	colorAttachment.clear = true;
	colorAttachment.clearColor[0] = 0.05f;
	colorAttachment.clearColor[1] = 0.05f;
	colorAttachment.clearColor[2] = 0.08f;
	colorAttachment.clearColor[3] = 1.0f;

	graphics::DepthAttachment depthAttachment;
	depthAttachment.texture = m_pSwapchain->GetDepthTexture();
	depthAttachment.clear = true;
	depthAttachment.clearDepth = 1.0f;

	cmd->BeginRendering({ colorAttachment }, depthAttachment);

	auto size = m_pWnd->GetSize();
	float aspect = size.y > 0 ? static_cast<float>(size.x) / static_cast<float>(size.y) : 1.0f;
	m_pSceneLayer->Render(*cmd, aspect);
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

		// BeginFrame/EndFrame bracket the whole update+UI pass, not just
		// OnImGuiRender -- NewFrame must run before any ImGui:: call this
		// frame (including any a layer's OnUpdate might make), and
		// ImGui::Render() must run after every layer's OnImGuiRender has
		// finished issuing widget calls.
		m_pUI->BeginFrame();
		m_layerStack.OnUpdate(dt);
		m_layerStack.OnImGuiRender();
		m_pUI->EndFrame();

		RenderFrame();
	}

	return 0;
}

} // namespace spray