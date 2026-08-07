#include "D3D12CommandList.h"
#include "D3D12GraphicsDevice.h"
#include <cstring>
#include <stdexcept>

namespace draw::d3d12_backend {

D3D12CommandList::D3D12CommandList(D3D12GraphicsDevice* device, ComPtr<ID3D12CommandAllocator> allocator,
                                    ComPtr<ID3D12GraphicsCommandList4> cmdList)
    : m_device(device), m_allocator(allocator), m_cmdList(cmdList) {}

D3D12CommandList::~D3D12CommandList() {
    // Safe only because the device guarantees this object isn't destroyed
    // until its submission's fence has signaled (see D3D12GraphicsDevice::
    // WaitForFence/WaitIdle) -- ComPtr releases here (allocator, scratch
    // buffers) would otherwise free GPU-visible memory the GPU might still
    // be using.
}

// ============================================================================
// Barriers
// ============================================================================

void D3D12CommandList::TransitionTextures(const std::vector<TextureBarrier>& barriers) {
    std::vector<D3D12_RESOURCE_BARRIER> d3dBarriers;
    d3dBarriers.reserve(barriers.size());

    for (const auto& b : barriers) {
        NativeTexture& tex = m_device->GetNativeTexture(b.texture);

        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex.resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = ToD3D12ResourceState(b.before);
        barrier.Transition.StateAfter = ToD3D12ResourceState(b.after);
        d3dBarriers.push_back(barrier);

        // Track so Submit() knows which swapchain (and which of its buffers)
        // to signal the frame fence for, without the app having to say so
        // explicitly -- mirrors the Vulkan backend's approach.
        if (b.after == ResourceState::Present && tex.isSwapchainImage) {
            m_presentedSwapchain = tex.owningSwapchain;
            NativeSwapchain& sc = m_device->GetNativeSwapchain(tex.owningSwapchain);
            for (uint32_t i = 0; i < sc.imageTextures.size(); ++i) {
                if (sc.imageTextures[i] == b.texture) {
                    m_presentedImageIndex = i;
                    break;
                }
            }
        }
    }

    if (!d3dBarriers.empty()) {
        m_cmdList->ResourceBarrier(static_cast<uint32_t>(d3dBarriers.size()), d3dBarriers.data());
    }
}

// ============================================================================
// Render target binding
// ============================================================================
// No render-pass object needed -- see the comment above
// D3D12GraphicsDevice::CreateGraphicsPipeline. OMSetRenderTargets here is the
// direct equivalent of Vulkan's vkCmdBeginRendering.

void D3D12CommandList::BeginRendering(const std::vector<TextureHandle>& colorTargets,
                                       TextureHandle depthTarget) {
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> rtvs;
    rtvs.reserve(colorTargets.size());
    for (auto h : colorTargets) rtvs.push_back(m_device->GetNativeTexture(h).rtvHandle);

    bool hasDepth = depthTarget.IsValid();
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (hasDepth) dsv = m_device->GetNativeTexture(depthTarget).dsvHandle;

    m_cmdList->OMSetRenderTargets(static_cast<uint32_t>(rtvs.size()), rtvs.data(), FALSE,
                                   hasDepth ? &dsv : nullptr);
}

void D3D12CommandList::EndRendering() {
    // No-op: D3D12 has no explicit end-of-rendering scope to close --
    // OMSetRenderTargets simply gets called again for the next pass.
}

// ============================================================================
// Pipeline / resource binding
// ============================================================================

void D3D12CommandList::SetPipeline(PipelineHandle pipeline) {
    NativePipeline& p = m_device->GetNativePipeline(pipeline);
    m_boundPipeline = &p;

    if (p.isRayTracing) {
        // DXR pipelines bind through the *compute* root signature slot --
        // DispatchRays doesn't have its own root-binding mechanism.
        m_cmdList->SetPipelineState1(p.rtStateObject.Get());
        m_cmdList->SetComputeRootSignature(p.rootSignature.Get());
    } else {
        m_cmdList->SetPipelineState(p.pso.Get());
        m_cmdList->SetGraphicsRootSignature(p.rootSignature.Get());
        m_cmdList->IASetPrimitiveTopology(p.topology);
    }
}

void D3D12CommandList::SetBindGroup(uint32_t setIndex, BindGroupHandle group) {
    if (!m_boundPipeline) throw std::runtime_error("SetBindGroup called before SetPipeline");
    NativeBindGroup& bg = m_device->GetNativeBindGroup(group);
    bool isCompute = m_boundPipeline->isRayTracing; // DXR binds through the compute root signature slot

    if (bg.hasResourceHandle && setIndex < m_boundPipeline->setResourceRootParam.size()) {
        int32_t param = m_boundPipeline->setResourceRootParam[setIndex];
        if (param >= 0) {
            if (isCompute) m_cmdList->SetComputeRootDescriptorTable(param, bg.resourceGpuHandle);
            else m_cmdList->SetGraphicsRootDescriptorTable(param, bg.resourceGpuHandle);
        }
    }
    if (bg.hasSamplerHandle && setIndex < m_boundPipeline->setSamplerRootParam.size()) {
        int32_t param = m_boundPipeline->setSamplerRootParam[setIndex];
        if (param >= 0) {
            if (isCompute) m_cmdList->SetComputeRootDescriptorTable(param, bg.samplerGpuHandle);
            else m_cmdList->SetGraphicsRootDescriptorTable(param, bg.samplerGpuHandle);
        }
    }
}

void D3D12CommandList::SetVertexBuffer(uint32_t slot, BufferHandle buffer, size_t offset) {
    if (!m_boundPipeline) throw std::runtime_error("SetVertexBuffer called before SetPipeline");
    NativeBuffer& buf = m_device->GetNativeBuffer(buffer);

    D3D12_VERTEX_BUFFER_VIEW vbv{};
    vbv.BufferLocation = buf.gpuAddress + offset;
    vbv.SizeInBytes = static_cast<uint32_t>(buf.size - offset);
    vbv.StrideInBytes = m_boundPipeline->vertexStrides[slot]; // see NativePipeline's comment on why this is looked up here
    m_cmdList->IASetVertexBuffers(slot, 1, &vbv);
}

void D3D12CommandList::SetIndexBuffer(BufferHandle buffer, size_t offset, bool use32BitIndices) {
    NativeBuffer& buf = m_device->GetNativeBuffer(buffer);
    D3D12_INDEX_BUFFER_VIEW ibv{};
    ibv.BufferLocation = buf.gpuAddress + offset;
    ibv.SizeInBytes = static_cast<uint32_t>(buf.size - offset);
    ibv.Format = use32BitIndices ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    m_cmdList->IASetIndexBuffer(&ibv);
}

void D3D12CommandList::SetViewport(float x, float y, float width, float height) {
    D3D12_VIEWPORT viewport{ x, y, width, height, 0.0f, 1.0f };
    m_cmdList->RSSetViewports(1, &viewport);
}

void D3D12CommandList::SetScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) {
    D3D12_RECT rect{ x, y, x + static_cast<int32_t>(width), y + static_cast<int32_t>(height) };
    m_cmdList->RSSetScissorRects(1, &rect);
}

// ============================================================================
// Draw
// ============================================================================

void D3D12CommandList::Draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                             uint32_t firstInstance) {
    m_cmdList->DrawInstanced(vertexCount, instanceCount, firstVertex, firstInstance);
}

void D3D12CommandList::DrawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t firstInstance) {
    m_cmdList->DrawIndexedInstanced(indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

// ============================================================================
// Copies
// ============================================================================

void D3D12CommandList::CopyBufferToBuffer(BufferHandle src, size_t srcOffset, BufferHandle dst,
                                           size_t dstOffset, size_t sizeBytes) {
    NativeBuffer& s = m_device->GetNativeBuffer(src);
    NativeBuffer& d = m_device->GetNativeBuffer(dst);
    m_cmdList->CopyBufferRegion(d.resource.Get(), dstOffset, s.resource.Get(), srcOffset, sizeBytes);
}

void D3D12CommandList::CopyBufferToTexture(BufferHandle src, TextureHandle dst) {
    NativeBuffer& s = m_device->GetNativeBuffer(src);
    NativeTexture& d = m_device->GetNativeTexture(dst);

    D3D12_RESOURCE_DESC dstDesc = d.resource->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT numRows = 0;
    UINT64 rowSizeInBytes = 0, totalBytes = 0;
    ID3D12Device* device = nullptr;
    d.resource->GetDevice(IID_PPV_ARGS(&device));
    device->GetCopyableFootprints(&dstDesc, 0, 1, 0, &footprint, &numRows, &rowSizeInBytes, &totalBytes);
    device->Release();

    D3D12_TEXTURE_COPY_LOCATION dstLoc{};
    dstLoc.pResource = d.resource.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dstLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION srcLoc{};
    srcLoc.pResource = s.resource.Get();
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    srcLoc.PlacedFootprint = footprint;

    // Caller must have transitioned dst to CopyDst beforehand -- mirrors
    // every other command here in not issuing its own barriers.
    m_cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
}

// ============================================================================
// Ray tracing
// ============================================================================

void D3D12CommandList::BuildBLAS(BLASHandle handle, const BLASBuildDesc& desc) {
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    for (const auto& g : desc.geometries) {
        uint32_t primCount = 0;
        geometries.push_back(m_device->ToGeometry(g, primCount));
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = static_cast<uint32_t>(geometries.size());
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.pGeometryDescs = geometries.data();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    m_device->GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    NativeBuffer scratch = m_device->AllocateBuffer(prebuild.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_scratchBuffers.push_back(scratch); // kept alive until this list's fence signals

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.DestAccelerationStructureData = m_device->GetNativeBLAS(handle).address;
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = scratch.gpuAddress;

    m_cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
}

void D3D12CommandList::BuildTLAS(TLASHandle handle, const TLASBuildDesc& desc) {
    // Same ordering rule as the Vulkan backend: a UAV barrier here only
    // covers a BLAS built earlier *in this same command list*. A BLAS built
    // in an already-submitted list needs that submission's fence waited on
    // before this list is submitted, not a barrier within this one.
    D3D12_RESOURCE_BARRIER uavBarrier{};
    uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uavBarrier.UAV.pResource = nullptr; // null = applies to all UAV accesses, adequate for a first pass
    m_cmdList->ResourceBarrier(1, &uavBarrier);

    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
    instances.reserve(desc.instances.size());
    for (const auto& inst : desc.instances) {
        NativeAccelStruct& blas = m_device->GetNativeBLAS(inst.blas);
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        std::memcpy(d.Transform, inst.transform, sizeof(float) * 12);
        d.InstanceID = inst.instanceID;
        d.InstanceMask = inst.mask;
        d.InstanceContributionToHitGroupIndex = inst.hitGroupIndex;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_FRONT_COUNTERCLOCKWISE;
        d.AccelerationStructure = blas.address;
        instances.push_back(d);
    }

    size_t instanceBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instances.size();
    NativeBuffer instanceBuffer = m_device->AllocateBuffer(instanceBufferSize, D3D12_HEAP_TYPE_UPLOAD,
                                                             D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, 0 };
    instanceBuffer.resource->Map(0, &readRange, &mapped);
    std::memcpy(mapped, instances.data(), instanceBufferSize);
    instanceBuffer.resource->Unmap(0, nullptr);
    m_scratchBuffers.push_back(instanceBuffer);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.NumDescs = static_cast<uint32_t>(instances.size());
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.InstanceDescs = instanceBuffer.gpuAddress;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO prebuild{};
    m_device->GetDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &prebuild);

    NativeBuffer scratch = m_device->AllocateBuffer(prebuild.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    m_scratchBuffers.push_back(scratch);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
    buildDesc.DestAccelerationStructureData = m_device->GetNativeTLAS(handle).address;
    buildDesc.Inputs = inputs;
    buildDesc.ScratchAccelerationStructureData = scratch.gpuAddress;

    m_cmdList->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
}

void D3D12CommandList::TraceRays(uint32_t width, uint32_t height, uint32_t depth) {
    if (!m_boundPipeline || !m_boundPipeline->isRayTracing) {
        throw std::runtime_error("TraceRays called without a bound ray tracing pipeline");
    }

    D3D12_DISPATCH_RAYS_DESC dispatchDesc{};
    dispatchDesc.RayGenerationShaderRecord = m_boundPipeline->raygenRange;
    dispatchDesc.MissShaderTable = m_boundPipeline->missRange;
    dispatchDesc.HitGroupTable = m_boundPipeline->hitRange;
    dispatchDesc.Width = width;
    dispatchDesc.Height = height;
    dispatchDesc.Depth = depth;

    m_cmdList->DispatchRays(&dispatchDesc);
}

} // namespace draw::d3d12_backend