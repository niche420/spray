#pragma once

#include "event/Events.hpp"

#include <SDL3/SDL.h>

#include <glm/vec2.hpp>

#include <functional>

namespace spray {
class Window
{
public:
	using EventCallback = std::function<void(event::Event&)>;

	Window(uint32_t width, uint32_t height);
	~Window();

	glm::uvec2 GetSize() const;
	SDL_Window* GetSDLWindow() const { return m_pWnd; }

	void SetEventCallback(EventCallback callback) { m_eventCallback = std::move(callback); }

	// Pumps SDL's event queue for this frame. Forwards each raw SDL_Event
	// to ImGui first (ImGui_ImplSDL3_ProcessEvent needs the raw event, not
	// a translated one -- the one deliberate bypass of the Event system
	// here, since ImGui's SDL backend integration doesn't go through it),
	// then translates window/input events into event::Event and invokes
	// the callback set via SetEventCallback for each.
	void PollEvents();

	bool IsOpen() const { return !m_quitRequested; }

private:
	SDL_Window* m_pWnd;
	bool m_quitRequested = false;
	EventCallback m_eventCallback;
};
}