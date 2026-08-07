#pragma once

#include <SDL3/SDL.h>

#include <glm/vec2.hpp>

namespace spray {
class Window 
{
public:
	Window(uint32_t width, uint32_t height);

	glm::uvec2 GetSize() const;
	SDL_Window* GetSDLWindow() const { return m_pWnd; }

private:
	SDL_Window* m_pWnd;
};
}