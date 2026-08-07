#pragma once

#include "draw/ICommandList.h"
#include "D3D12Common.h"
#include <d3d12.h>

namespace draw::d3d12_backend {

class D3D12GraphicsDevice;

class D3D12CommandList final : public ICommandList {
public:
    D3D12CommandList(D3D12GraphicsDevice* device, ComPtr<ID3D12CommandAllocator> allocator,
                      ComPtr<ID3D12GraphicsCommandList4> cmdList);
    ~D3D12CommandList() override;

    void TransitionTextures(const std::vector<TextureBarrier>& barriers) override;
    void BeginRendering(const std::vector<TextureHandle>& colorTargets, TextureHandle depthTarget) override;
    void EndRendering() override;

    void SetPipeline(PipelineHandle pipeline) override;
    void SetBindGroup(uint32_t setIndex, BindGroupHandle group) override;
    void SetVertexBuffer(uint32_t slot, BufferHandle buffer, size_t offset) override;
    void SetIndexBuffer(BufferHandle buffer, size_t offset, bool use32BitIndices) override;
    void SetViewport(float x, float y, float width, float height) override;
    void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override;

    void Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
    void DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                      int32_t vertexOffset, uint32_t firstInstance) override;

    void CopyBufferToBuffer(BufferHandle src, size_t srcOffset, BufferHandle dst, size_t dstOffset,
                             size_t sizeBytes) override;
    void CopyBufferToTexture(BufferHandle src, TextureHandle dst) override;

    void BuildBLAS(BLASHandle handle, const BLASBuildDesc& desc) override;
    void BuildTLAS(TLASHandle handle, const TLASBuildDesc& desc) override;
    void TraceRays(uint32_t width, uint32_t height, uint32_t depth) override;

    // --- Internal, used by D3D12GraphicsDevice::Submit ---
    ID3D12GraphicsCommandList4* GetCommandList() const { return m_cmdList.Get(); }
    SwapchainHandle GetPresentedSwapchain() const { return m_presentedSwapchain; }
    uint32_t GetPresentedImageIndex() const { return m_presentedImageIndex; }

private:
    D3D12GraphicsDevice* m_device;
    ComPtr<ID3D12CommandAllocator> m_allocator; // dedicated per list, see BeginCommandList's comment
    ComPtr<ID3D12GraphicsCommandList4> m_cmdList;

    NativePipeline* m_boundPipeline = nullptr;
    SwapchainHandle m_presentedSwapchain;
    uint32_t m_presentedImageIndex = 0;

    // BLAS/TLAS build scratch (and, for TLAS, the instance upload buffer)
    // kept alive until this list's fence signals -- same lifetime rule as
    // the Vulkan backend's m_scratchBuffers.
    std::vector<NativeBuffer> m_scratchBuffers;
};

} // namespace draw::d3d12_backend