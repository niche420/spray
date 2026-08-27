#pragma once

#include "graphics/GraphicsTypes.hpp"
#include "event/Events.hpp"
#include "event/LayerStack.hpp"

#include <memory>
#include <optional>

namespace spray::graphics {
	class IContext;
	class IDevice;
	class ICommandList;
}
namespace spray::ui {
	class UIManager;
}

namespace spray {
class Window;
class Swapchain;
class SceneLayer;

class App {
public:
	App();
	~App();

	int32_t Run();

private:
	void HandleResize(uint32_t width, uint32_t height);

	// Finishes a frame already opened by Run() -- acquires the swapchain
	// image, blits/renders ImGui into it, submits, presents. `cmd` was
	// obtained earlier in Run() (before OnImGuiRender), and already has
	// the active viewport's render recorded into it -- see Run()'s comment
	// on why that ordering matters.
	void RenderFrame(graphics::ICommandList* cmd);

	// Window's event callback target. Order: Input (unconditionally, see
	// Input's class comment) -> ImGui's want-capture check (blocks mouse/
	// key events from reaching layers if an ImGui widget wants them --
	// there's no ImGuiLayer to do this via stack position, since UIManager
	// is a plain member, not a Layer; see App.cpp for why that's fine) ->
	// App's own concerns (resize) -> m_layerStack.
	void OnEvent(event::Event& e);

	std::unique_ptr<Window> m_pWnd;
	std::unique_ptr<graphics::IContext> m_pCtx;
	std::unique_ptr<graphics::IDevice> m_pDevice;
	// Declared after m_pDevice, before m_layerStack: destructor order is
	// reverse-declaration-order, so this tears down after every layer
	// (which may still reference it right up to App::~App's WaitIdle) but
	// before the device itself goes away.
	std::unique_ptr<Swapchain> m_pSwapchain;
	std::unique_ptr<ui::UIManager> m_pUI;

	event::LayerStack m_layerStack;
	// Non-owning -- owned by m_layerStack (pushed in App::App). Kept
	// separately because RenderFrame needs to call SceneLayer::
	// RenderActiveViewport directly, outside the swapchain's
	// BeginRendering scope, which isn't something the generic Layer
	// interface exposes.
	SceneLayer* m_pSceneLayer = nullptr;

	// Naive per-frame sync: wait on the previous frame's fence before
	// recording the next one. Serializes CPU/GPU (no real double-buffering
	// of frame resources yet) -- correct and simple; revisit with proper
	// frames-in-flight once this is an actual bottleneck.
	std::optional<graphics::FenceHandle> m_previousFrameFence;
};
}