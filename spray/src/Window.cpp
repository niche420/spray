#include "pch.hpp"
#include "Window.hpp"

#ifdef SPRAY_VULKAN_ENABLED
#include <SDL3/SDL_vulkan.h>
#endif

#include <stdexcept>
#include <iostream>

namespace spray {
Window::Window(uint32_t width, uint32_t height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
    }

    m_pWnd = SDL_CreateWindow(
        "spray",
        width, height,
#ifdef SPRAY_VULKAN_ENABLED
        SDL_WINDOW_VULKAN
#endif
    );

    if (!m_pWnd) {
        throw std::runtime_error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
    }
}

glm::uvec2 Window::GetSize() const {
    int w, h;
    SDL_GetWindowSize(m_pWnd, &w, &h);
    return { w, h };
}

} // namespace spray