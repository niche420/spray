#include "pch.hpp"
#include "VulkanDevice.hpp"
#include "VulkanCommandList.hpp"
#include "VulkanCommon.hpp"
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

namespace spray::graphics::vk {
namespace {
constexpr uint32_t kFramesInFlight = 2;

// Extensions required for raster + dynamic rendering + sync2 + full RT.
const std::vector<const char*> kRequiredDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
    VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
    VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
    VK_KHR_SPIRV_1_4_EXTENSION_NAME, // required by ray tracing pipeline on some drivers
};
} // namespace

VulkanDevice::VulkanDevice(VkInstance instance, VkPhysicalDevice physicalDevice)
    : m_instance(instance), m_physicalDevice(physicalDevice) {
    CreateLogicalDeviceAndQueue();
    LoadExtensionFunctions();

    VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = m_queueFamilyIndex;
    VK_CHECK(vkCreateCommandPool(m_device, &poolInfo, nullptr, &m_commandPool));

    // Fixed-size pool sized for a modest app; grow (or pool-per-frame-with-reset)
    // if hit VK_ERROR_OUT_OF_POOL_MEMORY under heavier descriptor usage.
    std::vector<VkDescriptorPoolSize> poolSizes = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 256 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 256 },
        { VK_DESCRIPTOR_TYPE_SAMPLER, 64 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 },
        { VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 64 },
    };
    VkDescriptorPoolCreateInfo descPoolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    descPoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descPoolInfo.maxSets = 256;
    descPoolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    descPoolInfo.pPoolSizes = poolSizes.data();
    VK_CHECK(vkCreateDescriptorPool(m_device, &descPoolInfo, nullptr, &m_descriptorPool));
}

VulkanDevice::~VulkanDevice() {
    if (m_device) {
        vkDeviceWaitIdle(m_device);
        m_liveCommandLists.clear();
        if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        if (m_commandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
}

void VulkanDevice::CreateLogicalDeviceAndQueue() {
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &familyCount, families.data());

    m_queueFamilyIndex = UINT32_MAX;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            m_queueFamilyIndex = i;
            break;
        }
    }
    if (m_queueFamilyIndex == UINT32_MAX) {
        throw std::runtime_error("No graphics-capable queue family found");
    }

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queueInfo.queueFamilyIndex = m_queueFamilyIndex;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    // Feature chain: RT pipeline -> acceleration structure -> buffer device
    // address -> dynamic rendering -> sync2 -> base features.
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };
    rtPipelineFeatures.rayTracingPipeline = VK_TRUE;

    VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    accelFeatures.accelerationStructure = VK_TRUE;
    accelFeatures.pNext = &rtPipelineFeatures;

    VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES };
    bdaFeatures.bufferDeviceAddress = VK_TRUE;
    bdaFeatures.pNext = &accelFeatures;

    VkPhysicalDeviceDynamicRenderingFeatures dynRenderFeatures{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES };
    dynRenderFeatures.dynamicRendering = VK_TRUE;
    dynRenderFeatures.pNext = &bdaFeatures;

    VkPhysicalDeviceSynchronization2Features sync2Features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
    sync2Features.synchronization2 = VK_TRUE;
    sync2Features.pNext = &dynRenderFeatures;

    VkPhysicalDeviceFeatures2 features2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    features2.pNext = &sync2Features;
    features2.features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo deviceInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    deviceInfo.pNext = &features2;
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(kRequiredDeviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = kRequiredDeviceExtensions.data();

    VK_CHECK(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_queue);
}

void VulkanDevice::LoadExtensionFunctions() {
#define LOAD(fn) fn##_ = reinterpret_cast<PFN_##fn>(vkGetDeviceProcAddr(m_device, #fn))
    LOAD(vkCmdBuildAccelerationStructuresKHR);
    LOAD(vkCmdTraceRaysKHR);
    LOAD(vkGetBufferDeviceAddressKHR);
    LOAD(vkGetAccelerationStructureDeviceAddressKHR);
    LOAD(vkGetAccelerationStructureBuildSizesKHR);
    LOAD(vkCreateAccelerationStructureKHR);
    LOAD(vkDestroyAccelerationStructureKHR);
#undef LOAD
}

// ============================================================================
// Buffers
// ============================================================================

NativeBuffer VulkanDevice::AllocateBuffer(size_t size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags memProps,
    bool needsDeviceAddress) {
    NativeBuffer buf;
    buf.size = size;

    if (needsDeviceAddress) usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(m_device, &bufferInfo, nullptr, &buf.buffer));

    VkMemoryRequirements memReq;
    vkGetBufferMemoryRequirements(m_device, buf.buffer, &memReq);

    VkMemoryAllocateFlagsInfo allocFlagsInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO };
    allocFlagsInfo.flags = needsDeviceAddress ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0;

    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.pNext = needsDeviceAddress ? &allocFlagsInfo : nullptr;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(m_physicalDevice, memReq.memoryTypeBits, memProps);
    VK_CHECK(vkAllocateMemory(m_device, &allocInfo, nullptr, &buf.memory));
    VK_CHECK(vkBindBufferMemory(m_device, buf.buffer, buf.memory, 0));

    if (needsDeviceAddress) {
        VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
        addrInfo.buffer = buf.buffer;
        buf.deviceAddress = vkGetBufferDeviceAddressKHR_(m_device, &addrInfo);
    }
    return buf;
}

BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc) {
    VkBufferUsageFlags usage = 0;
    if (HasFlag(desc.usage, BufferUsage::VertexBuffer)) usage |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (HasFlag(desc.usage, BufferUsage::IndexBuffer)) usage |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    if (HasFlag(desc.usage, BufferUsage::UniformBuffer)) usage |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (HasFlag(desc.usage, BufferUsage::StorageBuffer)) usage |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (HasFlag(desc.usage, BufferUsage::CopySrc)) usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (HasFlag(desc.usage, BufferUsage::CopyDst)) usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    // Vertex/index buffers get device-address support unconditionally since
    // they may be referenced by a BLAS build -- cheap to always allow.
    bool needsDeviceAddress = HasFlag(desc.usage, BufferUsage::VertexBuffer) ||
        HasFlag(desc.usage, BufferUsage::IndexBuffer) ||
        HasFlag(desc.usage, BufferUsage::StorageBuffer);

    // Same set of buffers (vertex/index/storage) may be handed to
    // vkCmdBuildAccelerationStructuresKHR as BLAS geometry input (see
    // AssetManager::GetOrCreateGpuBlas) -- validation requires this usage
    // flag be present on the buffer regardless of whether this particular
    // instance ever actually gets used that way, same "cheap to always
    // allow" reasoning as needsDeviceAddress above. Device address support
    // alone (VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, set below) isn't
    // sufficient -- Vulkan validates this as a separate, explicit usage bit.
    if (needsDeviceAddress) usage |= VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;

    VkMemoryPropertyFlags memProps = desc.hostVisible
        ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    NativeBuffer buf = AllocateBuffer(desc.sizeBytes, usage, memProps, needsDeviceAddress);
    return m_buffers.Add(buf);
}

void VulkanDevice::DestroyBuffer(BufferHandle handle) {
    if (!m_buffers.IsValid(handle)) return;
    NativeBuffer& buf = m_buffers.Get(handle);
    if (buf.mapped) vkUnmapMemory(m_device, buf.memory);
    vkDestroyBuffer(m_device, buf.buffer, nullptr);
    vkFreeMemory(m_device, buf.memory, nullptr);
    m_buffers.Remove(handle);
}

void* VulkanDevice::MapBuffer(BufferHandle handle) {
    NativeBuffer& buf = m_buffers.Get(handle);
    if (!buf.mapped) VK_CHECK(vkMapMemory(m_device, buf.memory, 0, buf.size, 0, &buf.mapped));
    return buf.mapped;
}

void VulkanDevice::UnmapBuffer(BufferHandle handle) {
    NativeBuffer& buf = m_buffers.Get(handle);
    if (buf.mapped) {
        vkUnmapMemory(m_device, buf.memory);
        buf.mapped = nullptr;
    }
}

// ============================================================================
// Textures
// ============================================================================

NativeTexture VulkanDevice::AllocateTexture(const TextureDesc& desc) {
    if (desc.dimension != TextureDimension::Texture2D &&
        HasFlag(desc.usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil)) {
        // Rendering into one slice of an array/cube/3D texture needs a
        // per-slice RTV/DSV (a separate view than the one this function
        // creates, which covers the whole resource) -- not supported yet.
        throw std::runtime_error(
            "RenderTarget/DepthStencil usage is only supported for Texture2D right now");
    }

    NativeTexture tex;
    tex.format = ToVkFormat(desc.format);
    tex.width = desc.width;
    tex.height = desc.height;

    VkImageUsageFlags usage = 0;
    if (HasFlag(desc.usage, TextureUsage::RenderTarget)) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (HasFlag(desc.usage, TextureUsage::DepthStencil)) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (HasFlag(desc.usage, TextureUsage::ShaderResource)) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (HasFlag(desc.usage, TextureUsage::Storage)) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (HasFlag(desc.usage, TextureUsage::CopySrc)) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (HasFlag(desc.usage, TextureUsage::CopyDst)) usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    bool is3D = desc.dimension == TextureDimension::Texture3D;
    uint32_t arrayLayers = desc.dimension == TextureDimension::TextureCube ? 6
        : desc.dimension == TextureDimension::Texture2DArray ? desc.depthOrArrayLayers
        : 1;

    VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageInfo.flags = desc.dimension == TextureDimension::TextureCube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = is3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = tex.format;
    imageInfo.extent = { desc.width, desc.height, is3D ? desc.depthOrArrayLayers : 1 };
    imageInfo.mipLevels = desc.mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.usage = usage;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(m_device, &imageInfo, nullptr, &tex.image));

    VkMemoryRequirements memReq;
    vkGetImageMemoryRequirements(m_device, tex.image, &memReq);
    VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = FindMemoryType(m_physicalDevice, memReq.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(m_device, &allocInfo, nullptr, &tex.memory));
    VK_CHECK(vkBindImageMemory(m_device, tex.image, tex.memory, 0));

    bool isDepth = HasFlag(desc.usage, TextureUsage::DepthStencil);
    VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    viewInfo.image = tex.image;
    viewInfo.viewType = ToVkImageViewType(desc.dimension);
    viewInfo.format = tex.format;
    viewInfo.subresourceRange.aspectMask = isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = desc.mipLevels;
    viewInfo.subresourceRange.layerCount = arrayLayers;
    VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &tex.view));

    return tex;
}

TextureHandle VulkanDevice::CreateTexture(const TextureDesc& desc) {
    return m_textures.Add(AllocateTexture(desc));
}

void VulkanDevice::DestroyTexture(TextureHandle handle) {
    if (!m_textures.IsValid(handle)) return;
    NativeTexture& tex = m_textures.Get(handle);
    if (!tex.isSwapchainImage) {
        vkDestroyImageView(m_device, tex.view, nullptr);
        vkDestroyImage(m_device, tex.image, nullptr);
        vkFreeMemory(m_device, tex.memory, nullptr);
    }
    else {
        vkDestroyImageView(m_device, tex.view, nullptr); // view is ours; image is swapchain-owned
    }
    m_textures.Remove(handle);
}

// ============================================================================
// Samplers
// ============================================================================

SamplerHandle VulkanDevice::CreateSampler(const SamplerDesc& desc) {
    NativeSampler native;
    VkSamplerCreateInfo info{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    info.magFilter = ToVkFilter(desc.magFilter);
    info.minFilter = ToVkFilter(desc.minFilter);
    info.addressModeU = ToVkSamplerAddressMode(desc.addressModeU);
    info.addressModeV = ToVkSamplerAddressMode(desc.addressModeV);
    info.addressModeW = info.addressModeU; // no separate W wrap mode exposed yet (2D textures only so far)
    info.mipmapMode = (desc.magFilter == FilterMode::Linear || desc.minFilter == FilterMode::Linear)
        ? VK_SAMPLER_MIPMAP_MODE_LINEAR
        : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(m_device, &info, nullptr, &native.sampler));
    return m_samplers.Add(native);
}

void VulkanDevice::DestroySampler(SamplerHandle handle) {
    if (!m_samplers.IsValid(handle)) return;
    vkDestroySampler(m_device, m_samplers.Get(handle).sampler, nullptr);
    m_samplers.Remove(handle);
}

// ============================================================================
// Shader modules
// ============================================================================

ShaderModuleHandle VulkanDevice::CreateShaderModule(const ShaderModuleDesc& desc) {
    NativeShaderModule mod;
    mod.stage = desc.stage;
    mod.entryPoint = desc.entryPoint;

    VkShaderModuleCreateInfo info{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    info.codeSize = desc.bytecode.spirv.size();
    info.pCode = reinterpret_cast<const uint32_t*>(desc.bytecode.spirv.data());
    VK_CHECK(vkCreateShaderModule(m_device, &info, nullptr, &mod.module));

    return m_shaderModules.Add(mod);
}

void VulkanDevice::DestroyShaderModule(ShaderModuleHandle handle) {
    if (!m_shaderModules.IsValid(handle)) return;
    vkDestroyShaderModule(m_device, m_shaderModules.Get(handle).module, nullptr);
    m_shaderModules.Remove(handle);
}

// ============================================================================
// Bind groups
// ============================================================================

namespace {
VkDescriptorType ToVkDescriptorType(BindingType type) {
    switch (type) {
    case BindingType::UniformBuffer:          return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case BindingType::StorageBuffer:          return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case BindingType::SampledTexture:         return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    case BindingType::StorageTexture:         return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; // NEW
    case BindingType::Sampler:                return VK_DESCRIPTOR_TYPE_SAMPLER;
    case BindingType::AccelerationStructure:  return VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    }
    return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
}
} // namespace

BindGroupLayoutHandle VulkanDevice::CreateBindGroupLayout(const BindGroupLayoutDesc& desc) {
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const auto& e : desc.entries) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = e.binding;
        b.descriptorType = ToVkDescriptorType(e.type);
        b.descriptorCount = 1;
        // Ray tracing shaders can read bind-group resources from RayGen/
        // ClosestHit/Miss just like fragment shaders read from Pixel --
        // mark every relevant RT stage visible alongside the raster stage.
        VkShaderStageFlags stageFlags = ToVkShaderStage(e.visibleStage);
        stageFlags |= VK_SHADER_STAGE_RAYGEN_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR |
            VK_SHADER_STAGE_MISS_BIT_KHR;
        b.stageFlags = stageFlags;
        bindings.push_back(b);
    }

    NativeBindGroupLayout native;
    native.entries = desc.entries;

    VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &native.layout));

    return m_bindGroupLayouts.Add(native);
}

void VulkanDevice::DestroyBindGroupLayout(BindGroupLayoutHandle handle) {
    if (!m_bindGroupLayouts.IsValid(handle)) return;
    vkDestroyDescriptorSetLayout(m_device, m_bindGroupLayouts.Get(handle).layout, nullptr);
    m_bindGroupLayouts.Remove(handle);
}

BindGroupHandle VulkanDevice::CreateBindGroup(const BindGroupDesc& desc) {
    NativeBindGroupLayout& layout = m_bindGroupLayouts.Get(desc.layout);

    VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout.layout;

    NativeBindGroup native;
    VK_CHECK(vkAllocateDescriptorSets(m_device, &allocInfo, &native.set));

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> asInfos;
    // Reserve so pointers taken into these vectors below stay valid --
    // resizing after that point would invalidate them.
    bufferInfos.reserve(desc.entries.size());
    imageInfos.reserve(desc.entries.size());
    asInfos.reserve(desc.entries.size());

    for (const auto& entry : desc.entries) {
        const BindGroupLayoutEntry* layoutEntry = nullptr;
        for (const auto& e : layout.entries) {
            if (e.binding == entry.binding) { layoutEntry = &e; break; }
        }
        if (!layoutEntry) continue;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = native.set;
        write.dstBinding = entry.binding;
        write.descriptorCount = 1;
        write.descriptorType = ToVkDescriptorType(layoutEntry->type);

        switch (layoutEntry->type) {
        case BindingType::UniformBuffer:
        case BindingType::StorageBuffer: {
            NativeBuffer& buf = m_buffers.Get(entry.buffer);
            bufferInfos.push_back({ buf.buffer, 0, VK_WHOLE_SIZE });
            write.pBufferInfo = &bufferInfos.back();
            break;
        }
        case BindingType::SampledTexture: {
            NativeTexture& tex = m_textures.Get(entry.texture);
            VkDescriptorImageInfo imgInfo{};
            imgInfo.imageView = tex.view;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfos.push_back(imgInfo);
            write.pImageInfo = &imageInfos.back();
            break;
        }
        case BindingType::StorageTexture: {
            NativeTexture& tex = m_textures.Get(entry.texture);
            VkDescriptorImageInfo imgInfo{};
            imgInfo.imageView = tex.view;
            imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            imageInfos.push_back(imgInfo);
            write.pImageInfo = &imageInfos.back();
            break;
        }
        case BindingType::Sampler: {
            NativeSampler& sampler = m_samplers.Get(entry.sampler);
            VkDescriptorImageInfo imgInfo{};
            imgInfo.sampler = sampler.sampler;
            imageInfos.push_back(imgInfo);
            write.pImageInfo = &imageInfos.back();
            break;
        }
        case BindingType::AccelerationStructure: {
            NativeAccelStruct& tlas = m_tlases.Get(entry.accelerationStructure);
            VkWriteDescriptorSetAccelerationStructureKHR asInfo{
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR };
            asInfo.accelerationStructureCount = 1;
            asInfo.pAccelerationStructures = &tlas.accelStruct;
            asInfos.push_back(asInfo);
            write.pNext = &asInfos.back();
            break;
        }
        }
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(m_device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return m_bindGroups.Add(native);
}

void VulkanDevice::DestroyBindGroup(BindGroupHandle handle) {
    if (!m_bindGroups.IsValid(handle)) return;
    vkFreeDescriptorSets(m_device, m_descriptorPool, 1, &m_bindGroups.Get(handle).set);
    m_bindGroups.Remove(handle);
}

// ============================================================================
// Graphics pipeline (dynamic rendering, no VkRenderPass/VkFramebuffer)
// ============================================================================

PipelineHandle VulkanDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) {
    std::vector<VkDescriptorSetLayout> setLayouts;
    for (auto h : desc.bindGroupLayouts) setLayouts.push_back(m_bindGroupLayouts.Get(h).layout);

    NativePipeline native;
    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    VK_CHECK(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &native.layout));

    NativeShaderModule& vs = m_shaderModules.Get(desc.vertexShader);
    NativeShaderModule& ps = m_shaderModules.Get(desc.pixelShader);
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_VERTEX_BIT, vs.module, vs.entryPoint.c_str(), nullptr },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
          VK_SHADER_STAGE_FRAGMENT_BIT, ps.module, ps.entryPoint.c_str(), nullptr },
    };

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t b = 0; b < desc.vertexBuffers.size(); ++b) {
        const auto& vb = desc.vertexBuffers[b];
        bindings.push_back({ b, vb.stride, VK_VERTEX_INPUT_RATE_VERTEX });
        for (const auto& attr : vb.attributes) {
            attributes.push_back({ attr.location, b, ToVkFormat(attr.format), attr.offset });
        }
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    vertexInput.pVertexBindingDescriptions = bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    inputAssembly.topology = desc.topology == PrimitiveTopology::TriangleList ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
        : desc.topology == PrimitiveTopology::LineList ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
        : VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rasterizer.polygonMode = desc.rasterizer.wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = desc.rasterizer.cullMode == CullMode::None ? VK_CULL_MODE_NONE
        : desc.rasterizer.cullMode == CullMode::Front ? VK_CULL_MODE_FRONT_BIT
        : VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    depthStencil.depthTestEnable = desc.depthStencil.depthTestEnable;
    depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnable;
    depthStencil.depthCompareOp = desc.depthStencil.depthCompareOp == CompareOp::Less ? VK_COMPARE_OP_LESS
        : desc.depthStencil.depthCompareOp == CompareOp::LessEqual ? VK_COMPARE_OP_LESS_OR_EQUAL
        : desc.depthStencil.depthCompareOp == CompareOp::Greater ? VK_COMPARE_OP_GREATER
        : desc.depthStencil.depthCompareOp == CompareOp::GreaterEqual ? VK_COMPARE_OP_GREATER_OR_EQUAL
        : desc.depthStencil.depthCompareOp == CompareOp::Equal ? VK_COMPARE_OP_EQUAL
        : desc.depthStencil.depthCompareOp == CompareOp::Always ? VK_COMPARE_OP_ALWAYS
        : VK_COMPARE_OP_NEVER;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = desc.blendState.blendEnable;
    if (desc.blendState.blendEnable) {
        colorBlendAttachment.srcColorBlendFactor = ToVkBlendFactor(desc.blendState.srcColorFactor);
        colorBlendAttachment.dstColorBlendFactor = ToVkBlendFactor(desc.blendState.dstColorFactor);
        colorBlendAttachment.colorBlendOp = ToVkBlendOp(desc.blendState.colorBlendOp);
        colorBlendAttachment.srcAlphaBlendFactor = ToVkBlendFactor(desc.blendState.srcAlphaFactor);
        colorBlendAttachment.dstAlphaBlendFactor = ToVkBlendFactor(desc.blendState.dstAlphaFactor);
        colorBlendAttachment.alphaBlendOp = ToVkBlendOp(desc.blendState.alphaBlendOp);
    }
    // Vulkan requires VkPipelineColorBlendStateCreateInfo::attachmentCount to
    // equal the pipeline's color attachment count (colorFormats.size() below)
    // -- one entry per target, all identical, since BlendState is a single
    // pipeline-wide setting in this API (matches D3D12's IndependentBlendEnable
    // = FALSE behavior, where RenderTarget[0] applies to every target).
    std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
        desc.colorTargetFormats.size(), colorBlendAttachment);

    VkPipelineColorBlendStateCreateInfo colorBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    colorBlend.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
    colorBlend.pAttachments = colorBlendAttachments.data();

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    std::vector<VkFormat> colorFormats;
    for (auto f : desc.colorTargetFormats) colorFormats.push_back(ToVkFormat(f));

    VkPipelineRenderingCreateInfo renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    renderingInfo.colorAttachmentCount = static_cast<uint32_t>(colorFormats.size());
    renderingInfo.pColorAttachmentFormats = colorFormats.data();
    renderingInfo.depthAttachmentFormat = ToVkFormat(desc.depthTargetFormat);

    VkGraphicsPipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pipelineInfo.pNext = &renderingInfo; // dynamic rendering: no VkRenderPass here
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = native.layout;

    VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &native.pipeline));

    return m_pipelines.Add(native);
}

// ============================================================================
// Ray tracing pipeline + shader binding table
// ============================================================================

PipelineHandle VulkanDevice::CreateRayTracingPipeline(const RayTracingPipelineDesc& desc) {
    std::vector<VkDescriptorSetLayout> setLayouts;
    for (auto h : desc.bindGroupLayouts) setLayouts.push_back(m_bindGroupLayouts.Get(h).layout);

    NativePipeline native;
    native.isRayTracing = true;

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    VK_CHECK(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &native.layout));

    std::vector<VkPipelineShaderStageCreateInfo> stages;
    for (auto h : desc.shaderModules) {
        NativeShaderModule& mod = m_shaderModules.Get(h);
        VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage.stage = ToVkShaderStage(mod.stage);
        stage.module = mod.module;
        stage.pName = mod.entryPoint.c_str();
        stages.push_back(stage);
    }

    std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups;
    for (const auto& g : desc.shaderGroups) {
        VkRayTracingShaderGroupCreateInfoKHR group{ VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        group.generalShader = VK_SHADER_UNUSED_KHR;
        group.closestHitShader = VK_SHADER_UNUSED_KHR;
        group.anyHitShader = VK_SHADER_UNUSED_KHR;
        group.intersectionShader = VK_SHADER_UNUSED_KHR;
        if (g.type == ShaderGroupType::General) {
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
            group.generalShader = g.generalShaderIndex;
        }
        else {
            group.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            group.closestHitShader = g.closestHitShaderIndex;
        }
        groups.push_back(group);
    }

    VkRayTracingPipelineCreateInfoKHR pipelineInfo{ VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR };
    pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
    pipelineInfo.pStages = stages.data();
    pipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
    pipelineInfo.pGroups = groups.data();
    pipelineInfo.maxPipelineRayRecursionDepth = desc.maxRecursionDepth;
    pipelineInfo.layout = native.layout;

    auto createRTPipelines = reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(
        vkGetDeviceProcAddr(m_device, "vkCreateRayTracingPipelinesKHR"));
    VK_CHECK(createRTPipelines(m_device, VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &native.pipeline));

    // --- Build the shader binding table ---
    // Group order (raygen, then misses, then hit groups) must match how
    // TLASInstanceDesc::hitGroupIndex is interpreted by the caller.
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 props2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &rtProps;
    vkGetPhysicalDeviceProperties2(m_physicalDevice, &props2);

    uint32_t handleSize = rtProps.shaderGroupHandleSize;
    uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;
    auto alignUp = [](uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); };
    uint32_t handleSizeAligned = alignUp(handleSize, handleAlignment);

    uint32_t raygenCount = 0, missCount = 0, hitCount = 0;
    for (const auto& g : desc.shaderGroups) {
        if (g.type == ShaderGroupType::General) {
            ShaderStage s = m_shaderModules.Get(desc.shaderModules[g.generalShaderIndex]).stage;
            (s == ShaderStage::RayGen) ? raygenCount++ : missCount++;
        }
        else {
            hitCount++;
        }
    }

    uint32_t handleCount = static_cast<uint32_t>(groups.size());
    std::vector<uint8_t> handles(handleCount * handleSize);
    auto getHandles = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        vkGetDeviceProcAddr(m_device, "vkGetRayTracingShaderGroupHandlesKHR"));
    VK_CHECK(getHandles(m_device, native.pipeline, 0, handleCount,
        static_cast<size_t>(handles.size()), handles.data()));

    uint32_t raygenRegionSize = alignUp(raygenCount * handleSizeAligned, baseAlignment);
    uint32_t missRegionSize = alignUp(missCount * handleSizeAligned, baseAlignment);
    uint32_t hitRegionSize = alignUp(hitCount * handleSizeAligned, baseAlignment);
    uint32_t sbtSize = raygenRegionSize + missRegionSize + hitRegionSize;

    native.sbtBuffer = AllocateBuffer(
        sbtSize,
        VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        /*needsDeviceAddress=*/true);

    // sbtBuffer is owned directly by the pipeline (not via the buffer
    // pool/BufferHandle), so it's mapped directly here rather than through
    // MapBuffer.
    {
        uint8_t* sbtMapped = nullptr;
        VK_CHECK(vkMapMemory(m_device, native.sbtBuffer.memory, 0, sbtSize, 0,
            reinterpret_cast<void**>(&sbtMapped)));

        // Layout: raygen (exactly one, at region start), then misses, then hit groups.
        uint32_t offset = 0;
        for (uint32_t i = 0; i < groups.size(); ++i) {
            const auto& g = desc.shaderGroups[i];
            if (g.type == ShaderGroupType::General &&
                m_shaderModules.Get(desc.shaderModules[g.generalShaderIndex]).stage == ShaderStage::RayGen) {
                std::memcpy(sbtMapped + offset, handles.data() + i * handleSize, handleSize);
                offset += handleSizeAligned;
            }
        }
        offset = raygenRegionSize;
        for (uint32_t i = 0; i < groups.size(); ++i) {
            const auto& g = desc.shaderGroups[i];
            if (g.type == ShaderGroupType::General &&
                m_shaderModules.Get(desc.shaderModules[g.generalShaderIndex]).stage == ShaderStage::Miss) {
                std::memcpy(sbtMapped + offset, handles.data() + i * handleSize, handleSize);
                offset += handleSizeAligned;
            }
        }
        offset = raygenRegionSize + missRegionSize;
        for (uint32_t i = 0; i < groups.size(); ++i) {
            if (desc.shaderGroups[i].type == ShaderGroupType::TrianglesHitGroup) {
                std::memcpy(sbtMapped + offset, handles.data() + i * handleSize, handleSize);
                offset += handleSizeAligned;
            }
        }
        vkUnmapMemory(m_device, native.sbtBuffer.memory);
    }

    VkBufferDeviceAddressInfo addrInfo{ VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO };
    addrInfo.buffer = native.sbtBuffer.buffer;
    VkDeviceAddress sbtAddress = vkGetBufferDeviceAddressKHR_(m_device, &addrInfo);

    // Raygen region is special-cased: the spec requires its `size` equal
    // its `stride` (exactly one raygen record per TraceRays call, no
    // array of them) -- raygenRegionSize below is the *buffer space*
    // reserved for this region (padded up to baseAlignment so the miss
    // region that follows starts at a valid offset), which is NOT the
    // same value and must not be reported as this region's `size`.
    native.raygenRegion = { sbtAddress, handleSizeAligned, handleSizeAligned };
    native.missRegion = { sbtAddress + raygenRegionSize, handleSizeAligned, missRegionSize };
    native.hitRegion = { sbtAddress + raygenRegionSize + missRegionSize, handleSizeAligned, hitRegionSize };

    return m_pipelines.Add(native);
}

// ============================================================================
// Compute pipeline
// ============================================================================

PipelineHandle VulkanDevice::CreateComputePipeline(const ComputePipelineDesc& desc) {
    std::vector<VkDescriptorSetLayout> setLayouts;
    for (auto h : desc.bindGroupLayouts) setLayouts.push_back(m_bindGroupLayouts.Get(h).layout);

    NativePipeline native;
    native.isCompute = true;

    VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
    layoutInfo.pSetLayouts = setLayouts.data();
    VK_CHECK(vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &native.layout));

    NativeShaderModule& cs = m_shaderModules.Get(desc.computeShader);
    VkPipelineShaderStageCreateInfo stage{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = cs.module;
    stage.pName = cs.entryPoint.c_str();

    VkComputePipelineCreateInfo pipelineInfo{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.stage = stage;
    pipelineInfo.layout = native.layout;

    VK_CHECK(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &native.pipeline));

    return m_pipelines.Add(native);
}

void VulkanDevice::DestroyPipeline(PipelineHandle handle) {
    if (!m_pipelines.IsValid(handle)) return;
    NativePipeline& p = m_pipelines.Get(handle);
    if (p.isRayTracing && p.sbtBuffer.buffer) {
        vkDestroyBuffer(m_device, p.sbtBuffer.buffer, nullptr);
        vkFreeMemory(m_device, p.sbtBuffer.memory, nullptr);
    }
    vkDestroyPipeline(m_device, p.pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, p.layout, nullptr);
    m_pipelines.Remove(handle);
}

// ============================================================================
// Acceleration structures
// ============================================================================

VkAccelerationStructureGeometryKHR VulkanDevice::ToGeometry(const BLASGeometryDesc& geom,
    uint32_t& outPrimitiveCount) {
    NativeBuffer& vbuf = m_buffers.Get(geom.vertexBuffer);
    NativeBuffer& ibuf = m_buffers.Get(geom.indexBuffer);

    VkAccelerationStructureGeometryTrianglesDataKHR triangles{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR };
    triangles.vertexFormat = ToVkFormat(geom.vertexFormat);
    triangles.vertexData.deviceAddress = vbuf.deviceAddress;
    triangles.vertexStride = geom.vertexStride;
    triangles.maxVertex = geom.vertexCount - 1;
    triangles.indexType = geom.use32BitIndices ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    triangles.indexData.deviceAddress = ibuf.deviceAddress;

    VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.geometry.triangles = triangles;
    geometry.flags = geom.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;

    outPrimitiveCount = geom.indexCount / 3;
    return geometry;
}

BLASHandle VulkanDevice::CreateBLAS(const BLASBuildDesc& desc) {
    std::vector<VkAccelerationStructureGeometryKHR> geometries;
    std::vector<uint32_t> primitiveCounts;
    for (const auto& g : desc.geometries) {
        uint32_t primCount = 0;
        geometries.push_back(ToGeometry(g, primCount));
        primitiveCounts.push_back(primCount);
    }

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = static_cast<uint32_t>(geometries.size());
    buildInfo.pGeometries = geometries.data();

    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR_(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, primitiveCounts.data(), &sizeInfo);

    NativeAccelStruct native;
    // needsDeviceAddress=true -- the AS backing buffer itself must be
    // created with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT for
    // vkGetAccelerationStructureDeviceAddressKHR to accept it; the storage
    // usage bit alone (below) isn't sufficient, same distinction as
    // CreateBuffer's needsDeviceAddress vs. the AS-build-input usage flag.
    native.backingBuffer = AllocateBuffer(sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        /*needsDeviceAddress=*/true);

    VkAccelerationStructureCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    createInfo.buffer = native.backingBuffer.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR_(m_device, &createInfo, nullptr, &native.accelStruct));

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    addrInfo.accelerationStructure = native.accelStruct;
    native.address = vkGetAccelerationStructureDeviceAddressKHR_(m_device, &addrInfo);

    // Build-scratch buffer sizing (sizeInfo.buildScratchSize) is needed again
    // at actual-build time in VulkanCommandList::BuildBLAS -- not allocated
    // here since Create* only reserves the AS's own storage, not scratch.
    return m_blases.Add(native);
}

void VulkanDevice::DestroyBLAS(BLASHandle handle) {
    if (!m_blases.IsValid(handle)) return;
    NativeAccelStruct& blas = m_blases.Get(handle);
    vkDestroyAccelerationStructureKHR_(m_device, blas.accelStruct, nullptr);
    vkDestroyBuffer(m_device, blas.backingBuffer.buffer, nullptr);
    vkFreeMemory(m_device, blas.backingBuffer.memory, nullptr);
    m_blases.Remove(handle);
}

TLASHandle VulkanDevice::CreateTLAS(const TLASBuildDesc& desc) {
    VkAccelerationStructureGeometryKHR geometry{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR };
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;

    VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
    buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    buildInfo.geometryCount = 1;
    buildInfo.pGeometries = &geometry;

    uint32_t instanceCount = static_cast<uint32_t>(desc.instances.size());
    VkAccelerationStructureBuildSizesInfoKHR sizeInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    vkGetAccelerationStructureBuildSizesKHR_(m_device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
        &buildInfo, &instanceCount, &sizeInfo);

    NativeAccelStruct native;
    // See CreateBLAS's comment on why needsDeviceAddress=true is required here.
    native.backingBuffer = AllocateBuffer(sizeInfo.accelerationStructureSize,
        VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        /*needsDeviceAddress=*/true);

    VkAccelerationStructureCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR };
    createInfo.buffer = native.backingBuffer.buffer;
    createInfo.size = sizeInfo.accelerationStructureSize;
    createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    VK_CHECK(vkCreateAccelerationStructureKHR_(m_device, &createInfo, nullptr, &native.accelStruct));

    VkAccelerationStructureDeviceAddressInfoKHR addrInfo{
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR };
    addrInfo.accelerationStructure = native.accelStruct;
    native.address = vkGetAccelerationStructureDeviceAddressKHR_(m_device, &addrInfo);

    return m_tlases.Add(native);
}

void VulkanDevice::DestroyTLAS(TLASHandle handle) {
    if (!m_tlases.IsValid(handle)) return;
    NativeAccelStruct& tlas = m_tlases.Get(handle);
    vkDestroyAccelerationStructureKHR_(m_device, tlas.accelStruct, nullptr);
    vkDestroyBuffer(m_device, tlas.backingBuffer.buffer, nullptr);
    vkFreeMemory(m_device, tlas.backingBuffer.memory, nullptr);
    m_tlases.Remove(handle);
}

// ============================================================================
// Swapchain
// ============================================================================

SwapchainHandle VulkanDevice::CreateSwapchain(const SwapchainDesc& desc) {
    NativeSwapchain native;
    native.format = ToVkFormat(desc.format);
    native.width = desc.width;
    native.height = desc.height;
    SDL_Vulkan_CreateSurface(desc.windowHandle, m_instance, nullptr, &native.surface);

    VkSwapchainCreateInfoKHR swapchainInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapchainInfo.surface = native.surface;
    swapchainInfo.minImageCount = desc.bufferCount;
    swapchainInfo.imageFormat = native.format;
    swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainInfo.imageExtent = { desc.width, desc.height };
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync'd; swap for MAILBOX if you want uncapped+no-tear
    swapchainInfo.clipped = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(m_device, &swapchainInfo, nullptr, &native.swapchain));

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(m_device, native.swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(m_device, native.swapchain, &imageCount, images.data());

    SwapchainHandle handle = m_swapchains.Add(native);
    NativeSwapchain& sc = m_swapchains.Get(handle);

    for (VkImage image : images) {
        NativeTexture tex;
        tex.image = image;
        tex.format = native.format;
        tex.width = desc.width;
        tex.height = desc.height;
        tex.isSwapchainImage = true;
        tex.owningSwapchain = handle;

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = native.format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &tex.view));

        sc.imageTextures.push_back(m_textures.Add(tex));
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo semInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore s1, s2;
        VK_CHECK(vkCreateSemaphore(m_device, &semInfo, nullptr, &s1));
        VK_CHECK(vkCreateSemaphore(m_device, &semInfo, nullptr, &s2));
        sc.imageAvailableSemaphores.push_back(s1);
        sc.renderFinishedSemaphores.push_back(s2);
    }

    return handle;
}

void VulkanDevice::DestroySwapchain(SwapchainHandle handle) {
    if (!m_swapchains.IsValid(handle)) return;
    NativeSwapchain& sc = m_swapchains.Get(handle);
    vkDeviceWaitIdle(m_device);
    for (auto t : sc.imageTextures) DestroyTexture(t);
    for (auto s : sc.imageAvailableSemaphores) vkDestroySemaphore(m_device, s, nullptr);
    for (auto s : sc.renderFinishedSemaphores) vkDestroySemaphore(m_device, s, nullptr);
    vkDestroySwapchainKHR(m_device, sc.swapchain, nullptr);
    vkDestroySurfaceKHR(m_instance, sc.surface, nullptr);
    m_swapchains.Remove(handle);
}

void VulkanDevice::ResizeSwapchain(SwapchainHandle handle, uint32_t width, uint32_t height) {
    SwapchainDesc desc;
    NativeSwapchain& old = m_swapchains.Get(handle);
    desc.width = width;
    desc.height = height;
    desc.format = Format::BGRA8_UNorm; // NativeSwapchain doesn't retain rb::render::Format; assumes default -- store it if you support other swapchain formats
    desc.bufferCount = static_cast<uint32_t>(old.imageTextures.size());
    desc.windowHandle = nullptr; // surface is reused below, not recreated

    vkDeviceWaitIdle(m_device);
    for (auto t : old.imageTextures) DestroyTexture(t);
    old.imageTextures.clear();

    VkSwapchainCreateInfoKHR swapchainInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swapchainInfo.surface = old.surface;
    swapchainInfo.minImageCount = desc.bufferCount;
    swapchainInfo.imageFormat = old.format;
    swapchainInfo.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swapchainInfo.imageExtent = { width, height };
    swapchainInfo.imageArrayLayers = 1;
    swapchainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    swapchainInfo.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    swapchainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchainInfo.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchainInfo.clipped = VK_TRUE;
    swapchainInfo.oldSwapchain = old.swapchain;
    VkSwapchainKHR newSwapchain;
    VK_CHECK(vkCreateSwapchainKHR(m_device, &swapchainInfo, nullptr, &newSwapchain));
    vkDestroySwapchainKHR(m_device, old.swapchain, nullptr);
    old.swapchain = newSwapchain;
    old.width = width;
    old.height = height;

    uint32_t imageCount = 0;
    vkGetSwapchainImagesKHR(m_device, old.swapchain, &imageCount, nullptr);
    std::vector<VkImage> images(imageCount);
    vkGetSwapchainImagesKHR(m_device, old.swapchain, &imageCount, images.data());
    for (VkImage image : images) {
        NativeTexture tex;
        tex.image = image;
        tex.format = old.format;
        tex.width = width;
        tex.height = height;
        tex.isSwapchainImage = true;
        tex.owningSwapchain = handle;
        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = old.format;
        viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VK_CHECK(vkCreateImageView(m_device, &viewInfo, nullptr, &tex.view));
        old.imageTextures.push_back(m_textures.Add(tex));
    }
}

TextureHandle VulkanDevice::AcquireNextSwapchainTexture(SwapchainHandle handle) {
    NativeSwapchain& sc = m_swapchains.Get(handle);
    VkSemaphore acquireSem = sc.imageAvailableSemaphores[sc.frameIndex];
    VK_CHECK(vkAcquireNextImageKHR(m_device, sc.swapchain, UINT64_MAX, acquireSem, VK_NULL_HANDLE,
        &sc.currentImageIndex));
    return sc.imageTextures[sc.currentImageIndex];
}

void VulkanDevice::Present(SwapchainHandle handle) {
    NativeSwapchain& sc = m_swapchains.Get(handle);
    VkSemaphore renderFinished = sc.renderFinishedSemaphores[sc.currentImageIndex];

    VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &sc.swapchain;
    presentInfo.pImageIndices = &sc.currentImageIndex;
    VK_CHECK(vkQueuePresentKHR(m_queue, &presentInfo));

    sc.frameIndex = (sc.frameIndex + 1) % kFramesInFlight;
}

// ============================================================================
// Command recording / submission
// ============================================================================
// Simplification: assumes a single swapchain acquire per Submit when
// presenting -- fine for a single-window app, insufficient for
// multi-swapchain rendering without extending this to track per-swapchain
// pending semaphores explicitly.

ICommandList* VulkanDevice::BeginCommandList() {
    VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmdBuffer;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &allocInfo, &cmdBuffer));

    VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmdBuffer, &beginInfo));

    auto list = std::make_unique<VulkanCommandList>(this, cmdBuffer);
    ICommandList* ptr = list.get();
    m_liveCommandLists.push_back(std::move(list));
    return ptr;
}

FenceHandle VulkanDevice::Submit(ICommandList* commandList) {
    auto* vkList = static_cast<VulkanCommandList*>(commandList);
    VkCommandBuffer cmdBuffer = vkList->GetCommandBuffer();
    VK_CHECK(vkEndCommandBuffer(cmdBuffer));

    VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    NativeFence nativeFence;
    VK_CHECK(vkCreateFence(m_device, &fenceInfo, nullptr, &nativeFence.fence));

    VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;

    // If this command list transitioned a swapchain image to Present, wire
    // up the matching acquire/render-finished semaphores automatically.
    SwapchainHandle usedSwapchain = vkList->GetPresentedSwapchain();
    VkSemaphore waitSem = VK_NULL_HANDLE, signalSem = VK_NULL_HANDLE;
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    if (usedSwapchain.IsValid() && m_swapchains.IsValid(usedSwapchain)) {
        NativeSwapchain& sc = m_swapchains.Get(usedSwapchain);
        waitSem = sc.imageAvailableSemaphores[sc.frameIndex];
        signalSem = sc.renderFinishedSemaphores[sc.currentImageIndex];
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSem;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSem;
    }

    VK_CHECK(vkQueueSubmit(m_queue, 1, &submitInfo, nativeFence.fence));

    // Move to the pending map rather than destroying now -- this command
    // list's scratch buffers (if any BLAS/TLAS build was recorded) may
    // still be read by the GPU until nativeFence.fence signals.
    auto it = std::find_if(m_liveCommandLists.begin(), m_liveCommandLists.end(),
        [&](auto& l) { return l.get() == commandList; });
    if (it != m_liveCommandLists.end()) {
        m_pendingCommandLists[nativeFence.fence] = std::move(*it);
        m_liveCommandLists.erase(it);
    }

    return m_fences.Add(nativeFence);
}

void VulkanDevice::WaitForFence(FenceHandle fence) {
    if (!m_fences.IsValid(fence)) return;
    NativeFence& f = m_fences.Get(fence);
    VK_CHECK(vkWaitForFences(m_device, 1, &f.fence, VK_TRUE, UINT64_MAX));

    m_pendingCommandLists.erase(f.fence); // safe to free scratch buffers/cmd buffer now

    vkDestroyFence(m_device, f.fence, nullptr);
    m_fences.Remove(fence);
}

void VulkanDevice::WaitIdle() {
    vkDeviceWaitIdle(m_device);
    // Every pending submission has necessarily completed -- safe to reclaim
    // all of them. Their VkFence objects are intentionally left alone here;
    // they're still owned by m_fences and get destroyed individually via a
    // (now-instant) WaitForFence call, or leak until device teardown if the
    // caller never calls WaitForFence for a given Submit. Call WaitForFence
    // for fences you don't otherwise need to check, rather than relying on
    // WaitIdle to reclaim them.
    m_pendingCommandLists.clear();
}
} // namespace ray::vk