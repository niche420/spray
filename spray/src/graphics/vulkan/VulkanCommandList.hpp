#pragma once

#include "graphics/CommandList.hpp"

#include "VulkanCommon.hpp"

#include <vulkan/vulkan.h>

namespace spray::graphics::vk {
class VulkanDevice;
struct NativePipeline;
struct NativeBuffer;

class VulkanCommandList final : public ICommandList {
public:
    VulkanCommandList(VulkanDevice* device, VkCommandBuffer cmdBuffer);
    ~VulkanCommandList() override;

    void TransitionTextures(const std::vector<TextureBarrier>& barriers) override;
    void BeginRendering(const std::vector<ColorAttachment>& colorTargets, const DepthAttachment& depthTarget) override;
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
    void CopyBufferToTexture(BufferHandle src, TextureHandle dst, uint32_t mipLevel,
        uint32_t arrayLayer) override;
    void CopyTextureToBuffer(TextureHandle src, uint32_t mipLevel, uint32_t arrayLayer,
        BufferHandle dst, size_t dstOffset) override;

    void BuildBLAS(BLASHandle handle, const BLASBuildDesc& desc) override;
    void BuildTLAS(TLASHandle handle, const TLASBuildDesc& desc) override;
    void TraceRays(uint32_t width, uint32_t height, uint32_t depth) override;
    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override;

    // --- Internal, used by VulkanGraphicsDevice::Submit ---
    VkCommandBuffer GetCommandBuffer() const { return m_cmdBuffer; }
    SwapchainHandle GetPresentedSwapchain() const { return m_presentedSwapchain; }

private:
    // Scratch buffers allocated for BLAS/TLAS builds recorded on this list.
    // Kept alive until the list's fence signals (owned here, freed in the
    // destructor -- relies on VulkanGraphicsDevice not destroying a command
    // list's owning unique_ptr until its GPU work has completed, which
    // WaitForFence/WaitIdle both guarantee before any teardown path touches it).
    VulkanDevice* m_device;
    VkCommandBuffer m_cmdBuffer;
    bool m_isRayTracingPipelineBound = false;
    NativePipeline* m_boundPipeline = nullptr;
    SwapchainHandle m_presentedSwapchain;
    std::vector<NativeBuffer> m_scratchBuffers; // one per BuildBLAS/BuildTLAS call
};
} // namespace spray::graphics::vk