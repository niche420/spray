#include "pch.hpp"
#include "Input.hpp"

namespace spray::event {

void Input::OnEvent(const Event& e) {
    if (auto* ev = std::get_if<KeyPressedEvent>(&e.data)) {
        s_keysDown.insert(ev->scancode);
    } else if (auto* ev = std::get_if<KeyReleasedEvent>(&e.data)) {
        s_keysDown.erase(ev->scancode);
    } else if (auto* ev = std::get_if<MouseButtonPressedEvent>(&e.data)) {
        s_mouseButtonsDown.insert(ev->button);
    } else if (auto* ev = std::get_if<MouseButtonReleasedEvent>(&e.data)) {
        s_mouseButtonsDown.erase(ev->button);
    } else if (auto* ev = std::get_if<MouseMovedEvent>(&e.data)) {
        s_accumulatedMouseDelta += glm::vec2(ev->dx, ev->dy);
    }
}

bool Input::IsKeyDown(int32_t scancode) {
    return s_keysDown.contains(scancode);
}

bool Input::IsMouseButtonDown(int32_t button) {
    return s_mouseButtonsDown.contains(button);
}

glm::vec2 Input::ConsumeMouseDelta() {
    glm::vec2 d = s_accumulatedMouseDelta;
    s_accumulatedMouseDelta = glm::vec2(0.0f);
    return d;
}

} // namespace spray::event