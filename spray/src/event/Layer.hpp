#pragma once

#include "Events.hpp"

#include <string>
#include <utility>

namespace spray {

// A layer owns some vertical slice of app behavior (scene simulation +
// rendering, ImGui panels, a future capture/training overlay, etc.) and
// participates in the three things App::Run drives every frame: update, UI
// drawing, and event handling. LayerStack owns the actual list and
// dispatch ordering (see LayerStack.hpp).
class Layer {
public:
    explicit Layer(std::string name) : m_name(std::move(name)) {}
    virtual ~Layer() = default;

    virtual void OnAttach() {}
    virtual void OnDetach() {}
    virtual void OnUpdate(float deltaSeconds) {}
    virtual void OnImGuiRender() {}

    // Dispatched top-of-stack-first (see LayerStack::OnEvent). Set
    // e.handled = true (directly, or via event::DispatchEvent) to stop
    // the event propagating to layers further down.
    virtual void OnEvent(event::Event& e) {}

    const std::string& GetName() const { return m_name; }

private:
    std::string m_name;
};

} // namespace spray