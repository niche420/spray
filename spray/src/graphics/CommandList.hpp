#pragma once

#include "Context.hpp"

#include <vector>

namespace spray::graphics {
// A single recorded stream of GPU commands. Obtained from IGraphicsDevice::
// BeginCommandList(), recorded into directly, then handed back via
// IGraphicsDevice::Submit(). Not thread-safe to record into from multiple
// threads simultaneously (mirrors both VkCommandBuffer and
// ID3D12GraphicsCommandList restrictions) -- if you want multithreaded
// recording, get one ICommandList per thread from the device.
class ICommandList {
public:
    virtual ~ICommandList() = default;

    // --- Barriers ---
    virtual void TransitionTextures(const std::vector<TextureBarrier>& barriers) = 0;

    // --- Render target binding ---
    // colorTargets/depthTarget must already be in RenderTarget/DepthWrite
    // state via TransitionTextures before this call. Each attachment's
    // `clear` flag controls load behavior for that attachment specifically --
    // uncleared attachments are LOAD (preserve existing contents).
    virtual void BeginRendering(const std::vector<ColorAttachment>& colorTargets,
        const DepthAttachment& depthTarget) = 0;
    virtual void EndRendering() = 0;

    // --- Pipeline / resource binding ---
    virtual void SetPipeline(PipelineHandle pipeline) = 0;
    virtual void SetBindGroup(uint32_t setIndex, BindGroupHandle group) = 0;
    virtual void SetVertexBuffer(uint32_t slot, BufferHandle buffer, size_t offset) = 0;
    virtual void SetIndexBuffer(BufferHandle buffer, size_t offset, bool use32BitIndices) = 0;

    virtual void SetViewport(float x, float y, float width, float height) = 0;
    virtual void SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;

    // --- Draw / dispatch ---
    virtual void Draw(uint32_t vertexCount, uint32_t instanceCount,
        uint32_t firstVertex, uint32_t firstInstance) = 0;
    virtual void DrawIndexed(uint32_t indexCount, uint32_t instanceCount,
        uint32_t firstIndex, int32_t vertexOffset,
        uint32_t firstInstance) = 0;

    // --- Ray tracing ---
    // Records the actual build for a handle previously allocated via
    // IDevice::CreateBLAS/CreateTLAS. Pass the same desc used at
    // creation. Building a TLAS requires its referenced BLASes to have
    // completed building already (order build submissions accordingly, or
    // split across separate Submit calls with a WaitForFence between).
    virtual void BuildBLAS(BLASHandle handle, const BLASBuildDesc& desc) = 0;
    virtual void BuildTLAS(TLASHandle handle, const TLASBuildDesc& desc) = 0;

    // Pipeline must be a ray tracing pipeline set via SetPipeline. Bind
    // groups (including one exposing the TLAS as an AccelerationStructure
    // binding) must be set beforehand, same as a graphics draw.
    virtual void TraceRays(uint32_t width, uint32_t height, uint32_t depth) = 0;

    // --- Compute ---
    // Pipeline must be a compute pipeline set via SetPipeline. Bind groups
    // must be set beforehand, same as a graphics draw or TraceRays.
    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;

    // --- Copies ---
    virtual void CopyBufferToBuffer(BufferHandle src, size_t srcOffset,
        BufferHandle dst, size_t dstOffset,
        size_t sizeBytes) = 0;
    // mipLevel/arrayLayer select which subresource of dst to copy into --
    // pass 0 for both for a plain 2D texture. For a TextureCube, arrayLayer
    // selects the face (0-5, in +X,-X,+Y,-Y,+Z,-Z order, matching both
    // Vulkan's VK_IMAGE_VIEW_TYPE_CUBE and D3D12_SRV_DIMENSION_TEXTURECUBE
    // face ordering); for a Texture2DArray it selects the array slice.
    virtual void CopyBufferToTexture(BufferHandle src, TextureHandle dst, uint32_t mipLevel,
        uint32_t arrayLayer) = 0;
};

} // namespace spray::graphics