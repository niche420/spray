#pragma once

#include "Events.hpp"
#include "Layer.hpp"

#include <memory>
#include <vector>

namespace spray::event {

class LayerStack {
public:
    ~LayerStack();

    template<typename T>
    T* Find()
    {
        for (auto& layer : m_layers)
        {
            if (auto* result = dynamic_cast<T*>(layer.get()))
                return result;
        }

        return nullptr;
    }

    void PushLayer(std::unique_ptr<Layer> layer);
    void PushOverlay(std::unique_ptr<Layer> overlay);

    void OnUpdate(float deltaSeconds);
    void OnImGuiRender();
    void OnEvent(Event& e);

private:
    std::vector<std::unique_ptr<Layer>> m_layers;
    size_t m_layerInsertIndex = 0; // ordinary layers go before this index, overlays after
};

} // namespace spray::event