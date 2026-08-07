#include "pch.hpp"
#include "Context.hpp"

namespace spray::graphics {

std::unique_ptr<IContext> CreateVulkanContext();
std::unique_ptr<IContext> CreateD3D12Context();

std::unique_ptr<IContext> IContext::Create(BackendType type) {
    switch (type) {
#ifdef SPRAY_VULKAN_ENABLED
        case BackendType::Vulkan:
            return CreateVulkanContext();
#endif
#ifdef SPRAY_D3D12_ENABLED
        case BackendType::D3D12:
            return CreateD3D12Context();
#endif
    }
    return nullptr;
}

std::optional<uint32_t> SelectAdapter(const std::vector<AdapterInfo>& adapters, const GpuRequirements& reqs)
{
    std::optional<uint32_t> best;
    size_t bestMemory = 0;

    for (const auto& a : adapters) {
        if (a.dedicatedVideoMemoryBytes < reqs.minDedicatedVideoMemoryBytes) continue;
        if (reqs.requireDiscrete && a.isIntegrated) continue;

        if (a.dedicatedVideoMemoryBytes > bestMemory) {
            bestMemory = a.dedicatedVideoMemoryBytes;
            best = a.index;
        }
    }
    return best;
}

} // namespace spray::graphics