#include "pch.hpp"
#include "LayerStack.hpp"

#include <algorithm>

namespace spray::event {

LayerStack::~LayerStack() {
    for (auto& layer : m_layers) layer->OnDetach();
}

void LayerStack::PushLayer(std::unique_ptr<Layer> layer) {
    layer->OnAttach();
    m_layers.insert(m_layers.begin() + m_layerInsertIndex, std::move(layer));
    ++m_layerInsertIndex;
}

void LayerStack::PushOverlay(std::unique_ptr<Layer> overlay) {
    overlay->OnAttach();
    m_layers.push_back(std::move(overlay)); // overlays always go at the end
}

void LayerStack::OnUpdate(float deltaSeconds) {
    for (auto& layer : m_layers) layer->OnUpdate(deltaSeconds);
}

void LayerStack::OnImGuiRender() {
    for (auto& layer : m_layers) layer->OnImGuiRender();
}

void LayerStack::OnEvent(Event& e) {
    for (auto it = m_layers.rbegin(); it != m_layers.rend(); ++it) {
        if (e.handled) break;
        (*it)->OnEvent(e);
    }
}

} // namespace spray