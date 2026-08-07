#pragma once

#include "draw/IGraphicsContext.h"
#include "D3D12Common.h"

namespace draw::d3d12_backend {

class D3D12GraphicsContext final : public IGraphicsContext {
public:
    D3D12GraphicsContext();
    ~D3D12GraphicsContext() override = default;

    GraphicsBackendType GetBackendType() const override { return GraphicsBackendType::D3D12; }
    std::vector<AdapterInfo> EnumerateAdapters() override;
    std::unique_ptr<IGraphicsDevice> CreateDevice(uint32_t adapterIndex, void* windowHandle) override;

private:
    ComPtr<IDXGIFactory6> m_factory;
    std::vector<ComPtr<IDXGIAdapter1>> m_adapters;
};

} // namespace draw::d3d12_backend