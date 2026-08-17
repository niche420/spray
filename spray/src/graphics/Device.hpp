#pragma once

#include "GraphicsTypes.hpp"

struct SDL_Window;

namespace spray::graphics {

class ICommandList;

struct SwapchainDesc {
    SDL_Window* windowHandle = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    Format format = Format::BGRA8_UNorm;
    uint32_t bufferCount = 2;
};

// Represents a GPU device created against one physical adapter, on one
// backend. Owns all resources created through it -- none of it is valid on
// any other IDevice instance, including one created for the same
// adapter after a backend switch. Resource *handles* are only meaningful
// against the device that created them.
class IDevice {
public:
    virtual ~IDevice() = default;

    virtual BackendType GetBackendType() const = 0;

    // --- Resource creation ---
    virtual BufferHandle CreateBuffer(const BufferDesc& desc) = 0;
    virtual void DestroyBuffer(BufferHandle handle) = 0;
    virtual void* MapBuffer(BufferHandle handle) = 0;   // only valid if hostVisible
    virtual void UnmapBuffer(BufferHandle handle) = 0;

    virtual TextureHandle CreateTexture(const TextureDesc& desc) = 0;
    virtual void DestroyTexture(TextureHandle handle) = 0;

    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) = 0;
    virtual void DestroySampler(SamplerHandle handle) = 0;

    virtual ShaderModuleHandle CreateShaderModule(const ShaderModuleDesc& desc) = 0;
    virtual void DestroyShaderModule(ShaderModuleHandle handle) = 0;

    virtual BindGroupLayoutHandle CreateBindGroupLayout(const BindGroupLayoutDesc& desc) = 0;
    virtual void DestroyBindGroupLayout(BindGroupLayoutHandle handle) = 0;

    virtual BindGroupHandle CreateBindGroup(const BindGroupDesc& desc) = 0;
    virtual void DestroyBindGroup(BindGroupHandle handle) = 0;

    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
    // Same PipelineHandle type covers both -- the backend tags which kind
    // internally, so DestroyPipeline works for either without the app
    // needing to remember which factory it came from.
    virtual PipelineHandle CreateRayTracingPipeline(const RayTracingPipelineDesc& desc) = 0;
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) = 0;
    virtual void DestroyPipeline(PipelineHandle handle) = 0;

    // --- Acceleration structures ---
    // Allocates the backing buffer sized for this build; does not build.
    // Record the actual build via ICommandList::BuildBLAS/BuildTLAS using
    // the same desc, then ensure it completes (WaitForFence or WaitIdle)
    // before use in a TraceRays call or a TLAS build that references it.
    virtual BLASHandle CreateBLAS(const BLASBuildDesc& desc) = 0;
    virtual void DestroyBLAS(BLASHandle handle) = 0;
    virtual TLASHandle CreateTLAS(const TLASBuildDesc& desc) = 0;
    virtual void DestroyTLAS(TLASHandle handle) = 0;

    // --- Swapchain ---
    virtual SwapchainHandle CreateSwapchain(const SwapchainDesc& desc) = 0;
    virtual void DestroySwapchain(SwapchainHandle handle) = 0;
    virtual void ResizeSwapchain(SwapchainHandle handle, uint32_t width, uint32_t height) = 0;
    // Returns the texture to render into this frame. Backend handles the
    // underlying acquire semantics (vkAcquireNextImageKHR / DXGI back buffer
    // index) internally -- caller just gets a TextureHandle to transition
    // and bind like any other render target.
    virtual TextureHandle AcquireNextSwapchainTexture(SwapchainHandle handle) = 0;
    // Texture passed to AcquireNextSwapchainTexture must be transitioned to
    // ResourceState::Present before calling this.
    virtual void Present(SwapchainHandle handle) = 0;

    // --- Command recording / submission ---
    virtual ICommandList* BeginCommandList() = 0;
    // Submits and returns a fence signaled on GPU completion. Caller is not
    // required to wait on it -- pass it to WaitForFence only if you need to
    // know completion (e.g. before reading back a mapped buffer).
    virtual FenceHandle Submit(ICommandList* commandList) = 0;
    virtual void WaitForFence(FenceHandle fence) = 0;

    // Blocks until all submitted GPU work has completed. Required before
    // destroying resources still referenced by in-flight command lists, and
    // before tearing the device down (e.g. on backend switch).
    virtual void WaitIdle() = 0;
};

} // namespace spray::graphics