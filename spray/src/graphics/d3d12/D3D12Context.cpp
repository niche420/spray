#include "D3D12GraphicsContext.h"
#include "D3D12GraphicsDevice.h"
#include <d3d12sdklayers.h>
#include <cstdlib>

namespace rbr {
std::unique_ptr<IContext> CreateD3D12Context()
{
    return std::make_unique<D3D12Context>();
}
}

namespace draw::d3d12_backend {

namespace {
#ifdef _DEBUG
constexpr bool kEnableDebugLayer = true;
#else
constexpr bool kEnableDebugLayer = false;
#endif

// DXGI has no direct discrete/integrated adapter flag on most Windows
// versions (unlike Vulkan's VkPhysicalDeviceType) -- approximate it from
// dedicated VRAM instead. Good enough to distinguish "has a real GPU" from
// "Intel/AMD APU sharing system memory" for GpuSelector's purposes; not a
// hardware-verified classification.
constexpr size_t kIntegratedMemoryThreshold = 512ull * 1024 * 1024;
} // namespace

D3D12GraphicsContext::D3D12GraphicsContext() {
    UINT factoryFlags = 0;
    if (kEnableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
        }
        factoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    HR_CHECK(CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&m_factory)));

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; m_factory->EnumAdapterByGpuPreference(
                          i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)) !=
                      DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue; // skip the WARP software adapter

        // Confirm it actually supports the D3D12 feature level this API
        // targets before listing it -- EnumAdapterByGpuPreference doesn't
        // guarantee that on its own.
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                         __uuidof(ID3D12Device), nullptr))) {
            m_adapters.push_back(adapter);
        }
        adapter.Reset();
    }
}

std::vector<AdapterInfo> D3D12GraphicsContext::EnumerateAdapters() {
    std::vector<AdapterInfo> result;
    for (uint32_t i = 0; i < m_adapters.size(); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        m_adapters[i]->GetDesc1(&desc);

        AdapterInfo info;
        info.index = i;

        // DXGI_ADAPTER_DESC1::Description is a wide string; narrow it for
        // the shared AdapterInfo::name field.
        char narrowName[128];
        size_t converted = 0;
        wcstombs_s(&converted, narrowName, sizeof(narrowName), desc.Description, _TRUNCATE);
        info.name = narrowName;

        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.dedicatedVideoMemoryBytes = desc.DedicatedVideoMemory;
        info.isIntegrated = desc.DedicatedVideoMemory < kIntegratedMemoryThreshold;
        result.push_back(info);
    }
    return result;
}

std::unique_ptr<IGraphicsDevice> D3D12GraphicsContext::CreateDevice(uint32_t adapterIndex,
                                                                     void* windowHandle) {
    if (adapterIndex >= m_adapters.size()) {
        throw std::runtime_error("Adapter index out of range");
    }
    return std::make_unique<D3D12GraphicsDevice>(m_factory, m_adapters[adapterIndex], windowHandle);
}

} // namespace draw::d3d12_backend