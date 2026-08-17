#pragma once

#include <cstdint>
#include <string>
#include <variant>

namespace spray::event {

// Window / input events -- sourced from Window::PollEvents translating raw

struct WindowResizeEvent { uint32_t width; uint32_t height; };
struct WindowCloseEvent {};

struct KeyPressedEvent { int32_t scancode; bool repeat; };
struct KeyReleasedEvent { int32_t scancode; };

struct MouseMovedEvent { float x; float y; float dx; float dy; };
struct MouseButtonPressedEvent { int32_t button; };
struct MouseButtonReleasedEvent { int32_t button; };
struct MouseScrolledEvent { float deltaX; float deltaY; };

// App-level events

struct SceneLoadRequestedEvent { std::string path; };
struct SceneLoadedEvent { std::string name; bool success; std::string error; };

using EventVariant = std::variant<
    WindowResizeEvent, WindowCloseEvent,
    KeyPressedEvent, KeyReleasedEvent,
    MouseMovedEvent, MouseButtonPressedEvent, MouseButtonReleasedEvent, MouseScrolledEvent,
    SceneLoadRequestedEvent, SceneLoadedEvent
>;

struct Event {
    EventVariant data;
    bool handled = false;

    template <typename T>
    bool Is() const { return std::holds_alternative<T>(data); }

    template <typename T>
    const T& Get() const { return std::get<T>(data); }
};

template <typename T, typename Fn>
void DispatchEvent(Event& e, Fn&& fn) {
    if (e.handled) return;
    if (auto* concrete = std::get_if<T>(&e.data)) {
        if (fn(*concrete)) e.handled = true;
    }
}

} // namespace spray::events