#pragma once

#include "graphics/Types.hpp"

#include <vulkan/vulkan.h>

#include <optional>
#include <stdexcept>
#include <vector>

namespace spray::graphics::vk {
#define VK_CHECK(expr)                                                                   \
    do {                                                                                 \
        VkResult _result = (expr);                                                       \
        if (_result != VK_SUCCESS) {                                                     \
            throw std::runtime_error(std::string("Vulkan call failed (" #expr "): ") +    \
                                      std::to_string(_result));                          \
        }                                                                                 \
    } while (0)

// ----------------------------------------------------------------------------
// Handle pool: backs every draw::Handle<Tag> with a slot array + generation
// counter, so a stale handle (e.g. held across a backend switch, or used
// after Destroy*) is detectable rather than silently indexing garbage.
// ----------------------------------------------------------------------------
template <typename HandleT, typename NativeT>
class HandlePool {
public:
    HandleT Add(NativeT value) {
        for (uint32_t i = 0; i < m_slots.size(); ++i) {
            if (!m_slots[i].has_value()) {
                m_slots[i] = std::move(value);
                return HandleT{ i, m_generations[i] };
            }
        }
        m_slots.push_back(std::move(value));
        m_generations.push_back(1);
        return HandleT{ static_cast<uint32_t>(m_slots.size() - 1), 1 };
    }

    void Remove(HandleT handle) {
        if (!IsValid(handle)) return;
        m_slots[handle.index].reset();
        m_generations[handle.index]++;
    }

    bool IsValid(HandleT handle) const {
        return handle.index < m_slots.size() &&
               m_generations[handle.index] == handle.generation &&
               m_slots[handle.index].has_value();
    }

    NativeT& Get(HandleT handle) {
        if (!IsValid(handle)) throw std::runtime_error("Stale or invalid handle");
        return *m_slots[handle.index];
    }

private:
    std::vector<std::optional<NativeT>> m_slots;
    std::vector<uint32_t> m_generations;
};

inline VkFormat ToVkFormat(Format format) {
    switch (format) {
    case Format::RGBA8_UNorm:       return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8_UNorm:       return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::RGBA16_Float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA32_Float:      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R32_Float:         return VK_FORMAT_R32_SFLOAT;
    case Format::D32_Float:         return VK_FORMAT_D32_SFLOAT;
    case Format::D24_UNorm_S8_UInt: return VK_FORMAT_D24_UNORM_S8_UINT;
    default:                        return VK_FORMAT_UNDEFINED;
    }
}

inline VkImageViewType ToVkImageViewType(TextureDimension dim) {
    switch (dim) {
    case TextureDimension::Texture2D:      return VK_IMAGE_VIEW_TYPE_2D;
    case TextureDimension::Texture2DArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureDimension::TextureCube:    return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureDimension::Texture3D:      return VK_IMAGE_VIEW_TYPE_3D;
    }
    return VK_IMAGE_VIEW_TYPE_2D;
}

inline VkImageLayout ToVkImageLayout(ResourceState state) {
    switch (state) {
    case ResourceState::Undefined:      return VK_IMAGE_LAYOUT_UNDEFINED;
    case ResourceState::RenderTarget:   return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthWrite:     return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case ResourceState::DepthRead:      return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case ResourceState::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case ResourceState::CopySrc:        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case ResourceState::CopyDst:        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case ResourceState::Present:        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

// Conservative but correct stage/access masks per state. A hand-tuned
// renderer would narrow these per call site; fine as a first pass since
// sync2 barriers are cheap to get right this way and expensive to get
// wrong the other way.
inline void ToVkSync(ResourceState state, VkPipelineStageFlags2& stage, VkAccessFlags2& access) {
    switch (state) {
    case ResourceState::Undefined:
        stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; access = 0; return;
    case ResourceState::RenderTarget:
        stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        access = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        return;
    case ResourceState::DepthWrite:
        stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        return;
    case ResourceState::DepthRead:
        stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        access = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        return;
    case ResourceState::ShaderReadOnly:
        stage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR;
        access = VK_ACCESS_2_SHADER_READ_BIT;
        return;
    case ResourceState::CopySrc:
        stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT; access = VK_ACCESS_2_TRANSFER_READ_BIT; return;
    case ResourceState::CopyDst:
        stage = VK_PIPELINE_STAGE_2_TRANSFER_BIT; access = VK_ACCESS_2_TRANSFER_WRITE_BIT; return;
    case ResourceState::Present:
        stage = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT; access = 0; return;
    }
}

inline VkShaderStageFlagBits ToVkShaderStage(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:     return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Pixel:      return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute:    return VK_SHADER_STAGE_COMPUTE_BIT;
    case ShaderStage::RayGen:     return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    case ShaderStage::Miss:       return VK_SHADER_STAGE_MISS_BIT_KHR;
    case ShaderStage::ClosestHit: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
    }
    return VK_SHADER_STAGE_ALL;
}

inline VkFilter ToVkFilter(FilterMode mode) {
    return mode == FilterMode::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

inline VkSamplerAddressMode ToVkSamplerAddressMode(AddressMode mode) {
    switch (mode) {
    case AddressMode::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

inline VkBlendFactor ToVkBlendFactor(BlendFactor f) {
    switch (f) {
    case BlendFactor::Zero:             return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One:              return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcAlpha:         return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DstAlpha:         return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::OneMinusDstAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    }
    return VK_BLEND_FACTOR_ONE;
}

inline VkBlendOp ToVkBlendOp(BlendOp op) {
    switch (op) {
    case BlendOp::Add:             return VK_BLEND_OP_ADD;
    case BlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
    case BlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case BlendOp::Min:             return VK_BLEND_OP_MIN;
    case BlendOp::Max:             return VK_BLEND_OP_MAX;
    }
    return VK_BLEND_OP_ADD;
}

// ----------------------------------------------------------------------------
// Trivial device-local memory allocator: one VkDeviceMemory per resource.
// Real code should replace this with a suballocator (e.g. VMA) -- Vulkan
// guarantees only a few thousand live allocations on most drivers, and
// per-resource allocation burns that budget fast. Kept simple here so the
// resource-creation code above it is legible; swap CreateBuffer/CreateTexture's
// AllocateAndBind calls for VMA equivalents when that becomes a problem.
// ----------------------------------------------------------------------------
inline uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
    VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("No suitable Vulkan memory type found");
}
} // namespace ray::vk