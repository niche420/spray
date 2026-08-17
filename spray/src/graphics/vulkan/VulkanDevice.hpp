#pragma once

#include "graphics/Device.hpp"
#include "graphics/GraphicsTypes.hpp"

#include "VulkanCommon.hpp"

#include <vulkan/vulkan.h>

#include <unordered_map>

namespace spray::graphics::vk {
struct NativeBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t size = 0;
    void* mapped = nullptr;
    VkDeviceAddress deviceAddress = 0; // populated if created with shader-device-address usage (needed for AS builds)
};
 
struct NativeTexture {
    VkImage image = VK_NULL_HANDLE;       // VK_NULL_HANDLE for swapchain-owned views before first acquire
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE; // VK_NULL_HANDLE for swapchain images (swapchain owns their memory)
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool isSwapchainImage = false;
    SwapchainHandle owningSwapchain; // valid only if isSwapchainImage
};
 
struct NativeShaderModule {
    VkShaderModule module = VK_NULL_HANDLE;
    ShaderStage stage;
    std::string entryPoint;
};
 
struct NativeSampler {
    VkSampler sampler = VK_NULL_HANDLE;
};
 
struct NativeBindGroupLayout {
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    std::vector<BindGroupLayoutEntry> entries;
};
 
struct NativeBindGroup {
    VkDescriptorSet set = VK_NULL_HANDLE;
};
 
struct NativePipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    bool isRayTracing = false;
    bool isCompute = false;
 
    // Ray tracing only: shader binding table.
    NativeBuffer sbtBuffer;
    VkStridedDeviceAddressRegionKHR raygenRegion{};
    VkStridedDeviceAddressRegionKHR missRegion{};
    VkStridedDeviceAddressRegionKHR hitRegion{};
    VkStridedDeviceAddressRegionKHR callableRegion{}; // unused, kept zeroed
};
 
struct NativeAccelStruct {
    VkAccelerationStructureKHR accelStruct = VK_NULL_HANDLE;
    NativeBuffer backingBuffer;
    VkDeviceAddress address = 0;
    // Scratch is allocated per-build call and freed after; not stored here.
};
 
struct NativeSwapchain {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    std::vector<TextureHandle> imageTextures; // one NativeTexture (view-only) per swapchain image
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    uint32_t currentImageIndex = 0;
    uint32_t frameIndex = 0; // rotates through imageAvailableSemaphores independent of image index
};
 
struct NativeFence {
    VkFence fence = VK_NULL_HANDLE;
};
 
class VulkanDevice final : public IDevice {
public:
    VulkanDevice(VkInstance instance, VkPhysicalDevice physicalDevice);
    ~VulkanDevice() override;
 
    BackendType GetBackendType() const override { return BackendType::Vulkan; }
 
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
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override;
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
 
    // --- Internal accessors used by VulkanCommandList ---
    VkDevice GetDevice() const { return m_device; }
    VkPhysicalDevice GetPhysicalDevice() const { return m_physicalDevice; }
    NativeBuffer& GetNativeBuffer(BufferHandle h) { return m_buffers.Get(h); }
    NativeTexture& GetNativeTexture(TextureHandle h) { return m_textures.Get(h); }
    NativeBindGroup& GetNativeBindGroup(BindGroupHandle h) { return m_bindGroups.Get(h); }
    NativePipeline& GetNativePipeline(PipelineHandle h) { return m_pipelines.Get(h); }
    NativeAccelStruct& GetNativeBLAS(BLASHandle h) { return m_blases.Get(h); }
    NativeAccelStruct& GetNativeTLAS(TLASHandle h) { return m_tlases.Get(h); }

    // --- Native handle accessors, used only by ui::UIManager's ImGui
    // Vulkan-backend init (ImGui_ImplVulkan_InitInfo needs raw Vulkan
    // objects that IDevice deliberately doesn't expose to app/scene code).
    // Not for use outside that one call site -- reach for IDevice's
    // backend-agnostic API everywhere else.
    VkInstance GetInstance() const { return m_instance; }
    VkQueue GetQueue() const { return m_queue; }
    uint32_t GetQueueFamilyIndex() const { return m_queueFamilyIndex; }
    VkDescriptorPool GetDescriptorPool() const { return m_descriptorPool; }
 
    // Extension function pointers, loaded once and shared with VulkanCommandList.
    PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR_ = nullptr;
    PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR_ = nullptr;
    PFN_vkGetBufferDeviceAddressKHR vkGetBufferDeviceAddressKHR_ = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR_ = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR_ = nullptr;
    PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR_ = nullptr;
    PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR_ = nullptr;
 
private:
    friend class VulkanCommandList;
 
    void CreateLogicalDeviceAndQueue();
    void LoadExtensionFunctions();
    NativeBuffer AllocateBuffer(size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memProps,
                                 bool needsDeviceAddress = false);
    NativeTexture AllocateTexture(const TextureDesc& desc);
    VkAccelerationStructureGeometryKHR ToGeometry(const BLASGeometryDesc& geom,
                                                   uint32_t& outPrimitiveCount);
 
    VkInstance m_instance;
    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamilyIndex = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    void* m_windowHandle = nullptr;
 
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
 
    // Command lists handed out by BeginCommandList are owned here so the
    // app's raw ICommandList* stays valid until Submit consumes it.
    std::vector<std::unique_ptr<class VulkanCommandList>> m_liveCommandLists;
 
    // After Submit, a command list moves here (keyed by its raw VkFence)
    // instead of being destroyed immediately -- it holds BLAS/TLAS build
    // scratch buffers the GPU may still be reading until the fence signals.
    // Freed in WaitForFence (per-fence) and WaitIdle (all of them, since a
    // full device-idle guarantees every pending submission has completed).
    std::unordered_map<VkFence, std::unique_ptr<class VulkanCommandList>> m_pendingCommandLists;
};
} // namespace spray::graphics::vk