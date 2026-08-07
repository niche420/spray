#include "pch.hpp"
#include "VulkanCommandList.hpp"
#include "VulkanDevice.hpp"

#include <cstring>
#include <stdexcept>

namespace spray::graphics::vk {

VulkanCommandList::VulkanCommandList(VulkanDevice* device, VkCommandBuffer cmdBuffer)
    : m_device(device), m_cmdBuffer(cmdBuffer) 
{
}

VulkanCommandList::~VulkanCommandList() {
    // Safe only because the device guarantees this object is not destroyed
    // until its submission's fence has signaled (see WaitForFence/WaitIdle) --
    // otherwise this would free memory/objects the GPU might still be using.
    for (auto& buf : m_scratchBuffers) {
        vkDestroyBuffer(m_device->GetDevice(), buf.buffer, nullptr);
        vkFreeMemory(m_device->GetDevice(), buf.memory, nullptr);
    }
    vkFreeCommandBuffers(m_device->GetDevice(), m_device->m_commandPool, 1, &m_cmdBuffer);
}

// ============================================================================
// Barriers
// ============================================================================

void VulkanCommandList::TransitionTextures(const std::vector<TextureBarrier>& barriers) {
    std::vector<VkImageMemoryBarrier2> vkBarriers;
    vkBarriers.reserve(barriers.size());

    for (const auto& b : barriers) {
        NativeTexture& tex = m_device->GetNativeTexture(b.texture);

        VkImageMemoryBarrier2 barrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        ToVkSync(b.before, barrier.srcStageMask, barrier.srcAccessMask);
        ToVkSync(b.after, barrier.dstStageMask, barrier.dstAccessMask);
        barrier.oldLayout = ToVkImageLayout(b.before);
        barrier.newLayout = ToVkImageLayout(b.after);
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = tex.image;

        bool isDepth = b.after == ResourceState::DepthWrite || b.after == ResourceState::DepthRead ||
            b.before == ResourceState::DepthWrite || b.before == ResourceState::DepthRead;
        barrier.subresourceRange = { static_cast<VkImageAspectFlags>(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                      0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS };
        vkBarriers.push_back(barrier);

        // Track so Submit() knows which swapchain's present semaphore to
        // signal, without the app having to say so explicitly.
        if (b.after == ResourceState::Present && tex.isSwapchainImage) {
            m_presentedSwapchain = tex.owningSwapchain;
        }
    }

    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.imageMemoryBarrierCount = static_cast<uint32_t>(vkBarriers.size());
    depInfo.pImageMemoryBarriers = vkBarriers.data();
    vkCmdPipelineBarrier2(m_cmdBuffer, &depInfo);
}

// ============================================================================
// Dynamic rendering
// ============================================================================

void VulkanCommandList::BeginRendering(const std::vector<ColorAttachment>& colorTargets,
    const DepthAttachment& depthTarget) {
    std::vector<VkRenderingAttachmentInfo> colorAttachments;
    uint32_t width = 0, height = 0;

    for (const auto& ct : colorTargets) {
        NativeTexture& tex = m_device->GetNativeTexture(ct.texture);
        width = tex.width;
        height = tex.height;

        VkRenderingAttachmentInfo attach{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
        attach.imageView = tex.view;
        attach.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attach.loadOp = ct.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        attach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (ct.clear) {
            attach.clearValue.color = { { ct.clearColor[0], ct.clearColor[1], ct.clearColor[2], ct.clearColor[3] } };
        }
        colorAttachments.push_back(attach);
    }

    VkRenderingAttachmentInfo depthAttachment{ VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    bool hasDepth = depthTarget.texture.IsValid();
    if (hasDepth) {
        NativeTexture& tex = m_device->GetNativeTexture(depthTarget.texture);
        width = tex.width;
        height = tex.height;
        depthAttachment.imageView = tex.view;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = depthTarget.clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (depthTarget.clear) {
            depthAttachment.clearValue.depthStencil = { depthTarget.clearDepth, depthTarget.clearStencil };
        }
    }

    VkRenderingInfo renderInfo{ VK_STRUCTURE_TYPE_RENDERING_INFO };
    renderInfo.renderArea = { { 0, 0 }, { width, height } };
    renderInfo.layerCount = 1;
    renderInfo.colorAttachmentCount = static_cast<uint32_t>(colorAttachments.size());
    renderInfo.pColorAttachments = colorAttachments.data();
    renderInfo.pDepthAttachment = hasDepth ? &depthAttachment : nullptr;

    vkCmdBeginRendering(m_cmdBuffer, &renderInfo);
}

void VulkanCommandList::EndRendering() {
    vkCmdEndRendering(m_cmdBuffer);
}

// ============================================================================
// Pipeline / resource binding
// ============================================================================

namespace {
    VkPipelineBindPoint ToVkBindPoint(const NativePipeline& p) {
        if (p.isRayTracing) return VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR;
        if (p.isCompute) return VK_PIPELINE_BIND_POINT_COMPUTE;
        return VK_PIPELINE_BIND_POINT_GRAPHICS;
    }
} // namespace

void VulkanCommandList::SetPipeline(PipelineHandle pipeline) {
    NativePipeline& p = m_device->GetNativePipeline(pipeline);
    m_boundPipeline = &p;
    vkCmdBindPipeline(m_cmdBuffer, ToVkBindPoint(p), p.pipeline);
}

void VulkanCommandList::SetBindGroup(uint32_t setIndex, BindGroupHandle group) {
    if (!m_boundPipeline) throw std::runtime_error("SetBindGroup called before SetPipeline");
    NativeBindGroup& bg = m_device->GetNativeBindGroup(group);
    vkCmdBindDescriptorSets(m_cmdBuffer, ToVkBindPoint(*m_boundPipeline), m_boundPipeline->layout, setIndex, 1,
        &bg.set, 0, nullptr);
}

void VulkanCommandList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, size_t offset) {
    NativeBuffer& buf = m_device->GetNativeBuffer(buffer);
    VkDeviceSize vkOffset = offset;
    vkCmdBindVertexBuffers(m_cmdBuffer, slot, 1, &buf.buffer, &vkOffset);
}

void VulkanCommandList::SetIndexBuffer(BufferHandle buffer, size_t offset, bool use32BitIndices) {
    NativeBuffer& buf = m_device->GetNativeBuffer(buffer);
    vkCmdBindIndexBuffer(m_cmdBuffer, buf.buffer, offset,
        use32BitIndices ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
}

void VulkanCommandList::SetViewport(float x, float y, float width, float height) {
    // Flipped Y so NDC/coordinate conventions match D3D12 rather than
    // Vulkan's default top-left-negative-Y -- keeps app-side vertex data
    // and matrices identical across backends.
    VkViewport viewport{ x, y + height, width, -height, 0.0f, 1.0f };
    vkCmdSetViewport(m_cmdBuffer, 0, 1, &viewport);
}

void VulkanCommandList::SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    VkRect2D scissor{ { x, y }, { width, height } };
    vkCmdSetScissor(m_cmdBuffer, 0, 1, &scissor);
}

// ============================================================================
// Draw
// ============================================================================

void VulkanCommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
    uint32_t firstInstance) {
    vkCmdDraw(m_cmdBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VulkanCommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
    int32_t vertexOffset, uint32_t firstInstance) {
    vkCmdDrawIndexed(m_cmdBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// ============================================================================
// Copies
// ============================================================================

void VulkanCommandList::CopyBufferToBuffer(BufferHandle src, size_t srcOffset, BufferHandle dst,
    size_t dstOffset, size_t sizeBytes) {
    NativeBuffer& s = m_device->GetNativeBuffer(src);
    NativeBuffer& d = m_device->GetNativeBuffer(dst);
    VkBufferCopy region{ srcOffset, dstOffset, sizeBytes };
    vkCmdCopyBuffer(m_cmdBuffer, s.buffer, d.buffer, 1, &region);
}

void VulkanCommandList::CopyBufferToTexture(BufferHandle src, TextureHandle dst, uint32_t mipLevel,
    uint32_t arrayLayer) {
    NativeBuffer& s = m_device->GetNativeBuffer(src);
    NativeTexture& d = m_device->GetNativeTexture(dst);

    // Caller must have transitioned dst to CopyDst beforehand -- this
    // mirrors CopyBufferToBuffer/every other command here in not issuing
    // its own barriers.
    VkBufferImageCopy region{};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, arrayLayer, 1 };
    region.imageExtent = { std::max(1u, d.width >> mipLevel), std::max(1u, d.height >> mipLevel), 1 };
    vkCmdCopyBufferToImage(m_cmdBuffer, s.buffer, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

// ============================================================================
// Ray tracing
// ============================================================================

void VulkanCommandList::BuildBLAS(BLASHandle handle, const BLASBuildDesc& desc) {
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<uint32_t> primitiveCounts;
    for (const auto& g : desc.geometries) {
        uint32_t primCount = 0;
        geometries.push_back(m_device->ToGeometry(g, primCount));
        primitiveCounts.push_back(primCount);
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
    buildInfo.pGeometries = geometries.data();
    buildInfo.dstAccelerationStructure = m_device->GetNativeBLAS(handle).accelStruct;

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    m_device->vkGetAccelerationStructureBuildSizesKHR_(
        m_device->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
        primitiveCounts.data(), &sizeInfo);

    // Scratch is per-build, kept alive in m_scratchBuffers until this command
    // list's fence signals (freed in the destructor) since the GPU reads it
    // for the duration of the build.
    NativeBuffer scratch = m_device->AllocateBuffer(sizeInfo.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        /*needsDeviceAddress=*/true);
    buildInfo.scratchData.deviceAddress = scratch.deviceAddress;
    m_scratchBuffers.push_back(scratch);

    std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
    for (uint32_t c : primitiveCounts) ranges.push_back({ c, 0, 0, 0 });
    const VkAccelerationStructureBuildRangeInfoKHR* pRanges = ranges.data();

    m_device->vkCmdBuildAccelerationStructuresKHR_(m_cmdBuffer, 1, &buildInfo, &pRanges);
}

void VulkanCommandList::BuildTLAS(TLASHandle handle, const TLASBuildDesc& desc) {
    // TLAS build reads the BLASes it references -- if any were built earlier
    // in this same command list, a build must complete before this one reads
    // it. If they were built in a prior, already-submitted command list, no
    // barrier is needed here (that's covered by ordering the submissions and
    // waiting on the earlier fence before this list's submission).
    VkMemoryBarrier2 asBarrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    asBarrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    asBarrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    asBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    asBarrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
    VkDependencyInfo depInfo{ VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &asBarrier;
    vkCmdPipelineBarrier2(m_cmdBuffer, &depInfo);

    std::vector<VkAccelerationStructureInstanceKHR> vkInstances;
    vkInstances.reserve(desc.instances.size());
    for (const auto& inst : desc.instances) {
        NativeAccelStruct& blas = m_device->GetNativeBLAS(inst.blas);
        VkAccelerationStructureInstanceKHR vkInst{};
        std::memcpy(&vkInst.transform, inst.transform, sizeof(float) * 12);
        vkInst.instanceCustomIndex = inst.instanceID;
        vkInst.mask = inst.mask;
        vkInst.instanceShaderBindingTableRecordOffset = inst.hitGroupIndex;
        vkInst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        vkInst.accelerationStructureReference = blas.address;
        vkInstances.push_back(vkInst);
    }

    size_t instanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR) * vkInstances.size();
    NativeBuffer instanceBuffer = m_device->AllocateBuffer(
        instanceBufferSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        /*needsDeviceAddress=*/true);
    void* mapped = nullptr;
    VK_CHECK(vkMapMemory(m_device->GetDevice(), instanceBuffer.memory, 0, instanceBufferSize, 0, &mapped));
    std::memcpy(mapped, vkInstances.data(), instanceBufferSize);
    vkUnmapMemory(m_device->GetDevice(), instanceBuffer.memory);
    m_scratchBuffers.push_back(instanceBuffer); // must outlive the build, same lifetime rule as scratch

    VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.data.deviceAddress = instanceBuffer.deviceAddress;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;
    buildInfo.dstAccelerationStructure = m_device->GetNativeTLAS(handle).accelStruct;

    uint32_t instanceCount = static_cast<uint32_t>(vkInstances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    m_device->vkGetAccelerationStructureBuildSizesKHR_(
        m_device->GetDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
        &instanceCount, &sizeInfo);

    NativeBuffer scratch = m_device->AllocateBuffer(sizeInfo.buildScratchSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        /*needsDeviceAddress=*/true);
    buildInfo.scratchData.deviceAddress = scratch.deviceAddress;
    m_scratchBuffers.push_back(scratch);

    VkAccelerationStructureBuildRangeInfoKHR range{ instanceCount, 0, 0, 0 };
    const VkAccelerationStructureBuildRangeInfoKHR* pRange = &range;
    m_device->vkCmdBuildAccelerationStructuresKHR_(m_cmdBuffer, 1, &buildInfo, &pRange);
}

void VulkanCommandList::TraceRays(uint32_t width, uint32_t height, uint32_t depth) {
    if (!m_boundPipeline || !m_boundPipeline->isRayTracing) {
        throw std::runtime_error("TraceRays called without a bound ray tracing pipeline");
    }
    VkStridedDeviceAddressRegionKHR callableRegion{}; // no callable shaders supported yet
    m_device->vkCmdTraceRaysKHR_(m_cmdBuffer, &m_boundPipeline->raygenRegion, &m_boundPipeline->missRegion,
        &m_boundPipeline->hitRegion, &callableRegion, width, height, depth);
}

void VulkanCommandList::Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    if (!m_boundPipeline || !m_boundPipeline->isCompute) {
        throw std::runtime_error("Dispatch called without a bound compute pipeline");
    }
    vkCmdDispatch(m_cmdBuffer, groupCountX, groupCountY, groupCountZ);
}

} // namespace spray::graphics::vk