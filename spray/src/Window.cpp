#include "pch.hpp"
#include "Window.hpp"

#ifdef SPRAY_VULKAN_ENABLED
    #include <SDL3/SDL_vulkan.h>
#endif

#include <imgui_impl_sdl3.h>

#include <stdexcept>

namespace spray {

using namespace event;

Window::Window(uint32_t width, uint32_t height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        throw std::runtime_error("SDL_Init failed: " + std::string(SDL_GetError()));
    }

    SDL_WindowFlags wndFlags = SDL_WINDOW_RESIZABLE;
#ifdef SPRAY_VULKAN_ENABLED
    wndFlags |= SDL_WINDOW_VULKAN;
#endif
    m_pWnd = SDL_CreateWindow("spray", width, height, wndFlags);
    if (!m_pWnd) {
        throw std::runtime_error("SDL_CreateWindow failed: " + std::string(SDL_GetError()));
    }
}

Window::~Window() {
    if (m_pWnd) SDL_DestroyWindow(m_pWnd);
    SDL_Quit();
}

glm::uvec2 Window::GetSize() const {
    int w, h;
    SDL_GetWindowSize(m_pWnd, &w, &h);
    return { static_cast<uint32_t>(w), static_cast<uint32_t>(h) };
}

void Window::PollEvents() {
    SDL_Event sdlEvent;
    while (SDL_PollEvent(&sdlEvent)) {
        // ImGui's SDL3 backend needs the raw event, not our translated
        // variant -- run before translation so ImGui's IO flags
        // (WantCaptureMouse/Keyboard) are already up to date by the time
        // layers see the corresponding event::Event from this same poll.
        ImGui_ImplSDL3_ProcessEvent(&sdlEvent);

        switch (sdlEvent.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: {
            m_quitRequested = true;
            Event e{ WindowCloseEvent{} };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_WINDOW_RESIZED: {
            Event e{ WindowResizeEvent{ static_cast<uint32_t>(sdlEvent.window.data1),
                                         static_cast<uint32_t>(sdlEvent.window.data2) } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_KEY_DOWN: {
            Event e{ KeyPressedEvent{ static_cast<int32_t>(sdlEvent.key.scancode), sdlEvent.key.repeat } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_KEY_UP: {
            Event e{ KeyReleasedEvent{ static_cast<int32_t>(sdlEvent.key.scancode) } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            Event e{ MouseMovedEvent{ sdlEvent.motion.x, sdlEvent.motion.y,
                                       sdlEvent.motion.xrel, sdlEvent.motion.yrel } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            Event e{ MouseButtonPressedEvent{ sdlEvent.button.button } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            Event e{ MouseButtonReleasedEvent{ sdlEvent.button.button } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            Event e{ MouseScrolledEvent{ sdlEvent.wheel.x, sdlEvent.wheel.y } };
            if (m_eventCallback) m_eventCallback(e);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace spray