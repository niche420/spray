#pragma once

#include "Events.hpp"

#include <glm/vec2.hpp>

#include <unordered_set>

namespace spray::event {

class Input {
public:
    static void OnEvent(const Event& e);

    static bool IsKeyDown(int32_t scancode);
    static bool IsMouseButtonDown(int32_t button);

    // Accumulated motion since the last call, then reset. Call once per
    // frame from whatever's driving mouse-look; calling it more than once
    // in the same frame will split the frame's motion across the calls.
    static glm::vec2 ConsumeMouseDelta();

private:
    static inline std::unordered_set<int32_t> s_keysDown;
    static inline std::unordered_set<int32_t> s_mouseButtonsDown;
    static inline glm::vec2 s_accumulatedMouseDelta{0.0f};
};

} // namespace spray::event