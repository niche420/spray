#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace spray::graphics {

// Backs every Handle<Tag> (see Types.hpp) with a slot array + generation
// counter, so a stale handle (held across a backend switch, or used after
// Destroy*/asset removal) is detectable rather than silently indexing
// garbage.
//
// Shared across backends *and* the asset layer -- previously duplicated
// verbatim in VulkanCommon.hpp and D3D12Common.hpp (each backend had its
// own copy to avoid a cross-backend header dependency). Pulled out here
// because AssetManager needs the same scheme and a third copy-paste wasn't
// worth it. Backends still don't depend on each other's headers -- they
// both just depend on this one, which is backend-agnostic.
template <typename HandleT, typename NativeT>
class HandlePool {
public:
    HandleT Add(NativeT value) {
        for (uint32_t i = 0; i < m_slots.size(); ++i) {
            if (!m_slots[i].has_value()) {
                m_slots[i] = std::move(value);
                return HandleT{ i, m_generations[i] };
            }
        }
        m_slots.push_back(std::move(value));
        m_generations.push_back(1);
        return HandleT{ static_cast<uint32_t>(m_slots.size() - 1), 1 };
    }

    void Remove(HandleT handle) {
        if (!IsValid(handle)) return;
        m_slots[handle.index].reset();
        m_generations[handle.index]++;
    }

    bool IsValid(HandleT handle) const {
        return handle.index < m_slots.size() &&
               m_generations[handle.index] == handle.generation &&
               m_slots[handle.index].has_value();
    }

    NativeT& Get(HandleT handle) {
        if (!IsValid(handle)) throw std::runtime_error("Stale or invalid handle");
        return *m_slots[handle.index];
    }

private:
    std::vector<std::optional<NativeT>> m_slots;
    std::vector<uint32_t> m_generations;
};

} // namespace spray::graphics