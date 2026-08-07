#pragma once

#include "draw/IGraphicsDevice.h"
#include "D3D12Common.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <unordered_map>

namespace draw::d3d12_backend {

struct NativeBuffer {
    ComPtr<ID3D12Resource> resource;
    size_t size = 0;
    void* mapped = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
};

struct NativeTexture {
    ComPtr<ID3D12Resource> resource; // shared_ptr semantics: swapchain buffers keep their own ref via GetBuffer
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    bool isSwapchainImage = false;
    SwapchainHandle owningSwapchain;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle{};
    bool hasRtv = false, hasDsv = false;
};

struct NativeShaderModule {
    std::vector<uint8_t> bytecode; // DXIL
    ShaderStage stage;
    std::string entryPoint;
};

struct NativeSampler {
    D3D12_SAMPLER_DESC desc{}; // no live GPU object -- D3D12 samplers are written into a heap slot on demand (see CreateBindGroup)
};

struct NativeBindGroupLayout {
    std::vector<BindGroupLayoutEntry> entries; // order defines descriptor-table range order
};

struct NativeBindGroup {
    // A bind group may need descriptors from two different heaps -- D3D12
    // requires CBV/SRV/UAV and SAMPLER descriptors to live in separate
    // heaps, unlike Vulkan where one VkDescriptorSet can mix both. At most
    // one of each is populated depending on whether the layout has any
    // resource (non-Sampler) entries and/or any Sampler entries.
    D3D12_GPU_DESCRIPTOR_HANDLE resourceGpuHandle{};
    bool hasResourceHandle = false;
    D3D12_GPU_DESCRIPTOR_HANDLE samplerGpuHandle{};
    bool hasSamplerHandle = false;
};

struct NativePipeline {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pso;         // graphics only
    ComPtr<ID3D12StateObject> rtStateObject; // ray tracing only
    ComPtr<ID3D12StateObjectProperties> rtProps;
    bool isRayTracing = false;

    // Graphics only: D3D12_VERTEX_BUFFER_VIEW needs a stride per slot at
    // IASetVertexBuffers time, but ICommandList::SetVertexBuffer doesn't take
    // one (Vulkan doesn't need it there -- stride is baked into the pipeline's
    // VkVertexInputBindingDescription instead). Stashed here so
    // D3D12CommandList::SetVertexBuffer can look it up by slot.
    std::vector<uint32_t> vertexStrides;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // Per bind-group-layout-index (i.e. per "set"): which root parameter
    // holds that set's resource descriptor table and/or sampler descriptor
    // table, or -1 if that set has none of that kind. A set with both
    // resources and samplers consumes two consecutive root parameters (see
    // BuildRootSignature) -- SetBindGroup uses these to know which root
    // parameter(s) to bind for a given set index.
    std::vector<int32_t> setResourceRootParam;
    std::vector<int32_t> setSamplerRootParam;

    // Ray tracing only: shader binding table.
    NativeBuffer sbtBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE raygenRange{};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE missRange{};
    D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE hitRange{};
};

struct NativeAccelStruct {
    ComPtr<ID3D12Resource> backingBuffer;
    D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct NativeSwapchain {
    ComPtr<IDXGISwapChain3> swapchain;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    uint32_t bufferCount = 0;
    std::vector<TextureHandle> imageTextures;

    // D3D12 has no acquire semaphore -- GetCurrentBackBufferIndex() is
    // synchronous/immediate. Per-buffer fence values here instead track
    // "has the GPU actually finished with this buffer" so Present doesn't
    // race a buffer still being read by an earlier frame.
    ComPtr<ID3D12Fence> frameFence;
    HANDLE fenceEvent = nullptr;
    uint64_t nextFenceValue = 1;
    std::vector<uint64_t> bufferFenceValues;
};

struct NativeFence {
    ComPtr<ID3D12Fence> fence;
    uint64_t value = 0;
    HANDLE event = nullptr;
};

// Simple bump-allocated descriptor heap. Descriptors are never individually
// freed -- fine for a first pass (matches the per-resource memory allocator
// simplification in the Vulkan backend); revisit with a free-list if you
// destroy/recreate bind groups or render targets frequently at runtime.
struct DescriptorHeap {
    ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_DESCRIPTOR_HEAP_TYPE type{};
    uint32_t descriptorSize = 0;
    uint32_t capacity = 0;
    uint32_t nextFree = 0;
    bool shaderVisible = false;

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * descriptorSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(index) * descriptorSize;
        return h;
    }
    uint32_t Allocate(uint32_t count) {
        if (nextFree + count > capacity) throw std::runtime_error("Descriptor heap exhausted");
        uint32_t start = nextFree;
        nextFree += count;
        return start;
    }
};

class D3D12GraphicsDevice final : public IGraphicsDevice {
public:
    D3D12GraphicsDevice(ComPtr<IDXGIFactory6> factory, ComPtr<IDXGIAdapter1> adapter, void* windowHandle);
    ~D3D12GraphicsDevice() override;

    GraphicsBackendType GetBackendType() const override { return GraphicsBackendType::D3D12; }

    BufferHandle CreateBuffer(const BufferDesc& desc) override;
    void DestroyBuffer(BufferHandle handle) override;
    void* MapBuffer(BufferHandle handle) override;
    void UnmapBuffer(BufferHandle handle) override;

    TextureHandle CreateTexture(const TextureDesc& desc) override;
    void DestroyTexture(TextureHandle handle) override;

    SamplerHandle CreateSampler(const SamplerDesc& desc) override;
    void DestroySampler(SamplerHandle handle) override;

    ShaderModuleHandle CreateShaderModule(const ShaderModuleDesc& desc) override;
    void DestroyShaderModule(ShaderModuleHandle handle) override;

    BindGroupLayoutHandle CreateBindGroupLayout(const BindGroupLayoutDesc& desc) override;
    void DestroyBindGroupLayout(BindGroupLayoutHandle handle) override;

    BindGroupHandle CreateBindGroup(const BindGroupDesc& desc) override;
    void DestroyBindGroup(BindGroupHandle handle) override;

    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
    PipelineHandle CreateRayTracingPipeline(const RayTracingPipelineDesc& desc) override;
    void DestroyPipeline(PipelineHandle handle) override;

    BLASHandle CreateBLAS(const BLASBuildDesc& desc) override;
    void DestroyBLAS(BLASHandle handle) override;
    TLASHandle CreateTLAS(const TLASBuildDesc& desc) override;
    void DestroyTLAS(TLASHandle handle) override;

    SwapchainHandle CreateSwapchain(const SwapchainDesc& desc) override;
    void DestroySwapchain(SwapchainHandle handle) override;
    void ResizeSwapchain(SwapchainHandle handle, uint32_t width, uint32_t height) override;
    TextureHandle AcquireNextSwapchainTexture(SwapchainHandle handle) override;
    void Present(SwapchainHandle handle) override;

    ICommandList* BeginCommandList() override;
    FenceHandle Submit(ICommandList* commandList) override;
    void WaitForFence(FenceHandle fence) override;
    void WaitIdle() override;

    // --- Internal accessors used by D3D12CommandList ---
    ID3D12Device5* GetDevice() const { return m_device.Get(); }
    ID3D12CommandQueue* GetQueue() const { return m_queue.Get(); }
    NativeBuffer& GetNativeBuffer(BufferHandle h) { return m_buffers.Get(h); }
    NativeTexture& GetNativeTexture(TextureHandle h) { return m_textures.Get(h); }
    NativeBindGroup& GetNativeBindGroup(BindGroupHandle h) { return m_bindGroups.Get(h); }
    NativePipeline& GetNativePipeline(PipelineHandle h) { return m_pipelines.Get(h); }
    NativeAccelStruct& GetNativeBLAS(BLASHandle h) { return m_blases.Get(h); }
    NativeAccelStruct& GetNativeTLAS(TLASHandle h) { return m_tlases.Get(h); }
    NativeSwapchain& GetNativeSwapchain(SwapchainHandle h) { return m_swapchains.Get(h); }
    DescriptorHeap& GetCbvSrvUavHeap() { return m_cbvSrvUavHeap; }

private:
    friend class D3D12CommandList;

    struct RootSignatureInfo {
        ComPtr<ID3D12RootSignature> rootSignature;
        std::vector<int32_t> setResourceParam; // -1 if that set has no resource (non-Sampler) entries
        std::vector<int32_t> setSamplerParam;  // -1 if that set has no Sampler entries
    };

    NativeBuffer AllocateBuffer(size_t size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES initialState,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    NativeTexture AllocateTexture(const TextureDesc& desc);
    RootSignatureInfo BuildRootSignature(const std::vector<BindGroupLayoutHandle>& layouts);
    D3D12_RAYTRACING_GEOMETRY_DESC ToGeometry(const BLASGeometryDesc& geom, uint32_t& outPrimitiveCount);

    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<ID3D12Device5> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    void* m_windowHandle = nullptr;

    DescriptorHeap m_rtvHeap;
    DescriptorHeap m_dsvHeap;
    DescriptorHeap m_cbvSrvUavHeap; // shader-visible; backs all bind-group resource (non-Sampler) descriptors
    DescriptorHeap m_samplerHeap;   // shader-visible; separate from m_cbvSrvUavHeap -- D3D12 requires this split

    HandlePool<BufferHandle, NativeBuffer> m_buffers;
    HandlePool<TextureHandle, NativeTexture> m_textures;
    HandlePool<SamplerHandle, NativeSampler> m_samplers;
    HandlePool<ShaderModuleHandle, NativeShaderModule> m_shaderModules;
    HandlePool<BindGroupLayoutHandle, NativeBindGroupLayout> m_bindGroupLayouts;
    HandlePool<BindGroupHandle, NativeBindGroup> m_bindGroups;
    HandlePool<PipelineHandle, NativePipeline> m_pipelines;
    HandlePool<BLASHandle, NativeAccelStruct> m_blases;
    HandlePool<TLASHandle, NativeAccelStruct> m_tlases;
    HandlePool<SwapchainHandle, NativeSwapchain> m_swapchains;
    HandlePool<FenceHandle, NativeFence> m_fences;

    std::vector<std::unique_ptr<class D3D12CommandList>> m_liveCommandLists;
    std::unordered_map<ID3D12Fence*, std::pair<uint64_t, std::unique_ptr<class D3D12CommandList>>> m_pendingCommandLists;
};

} // namespace draw::d3d12_backend