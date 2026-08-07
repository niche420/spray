#pragma once

#include "draw/GraphicsTypes.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace draw::d3d12_backend {

using Microsoft::WRL::ComPtr;

#define HR_CHECK(expr)                                                                   \
    do {                                                                                 \
        HRESULT _hr = (expr);                                                            \
        if (FAILED(_hr)) {                                                               \
            throw std::runtime_error(std::string("D3D12 call failed (" #expr "): hr=") +  \
                                      std::to_string(_hr));                              \
        }                                                                                 \
    } while (0)

// ----------------------------------------------------------------------------
// Handle pool -- identical scheme to the Vulkan backend's (slot + generation),
// duplicated rather than shared across backends to keep each backend's build
// target free of a cross-backend header dependency. Consider factoring into
// a common target if a third backend is ever added.
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

// ----------------------------------------------------------------------------
// draw:: <-> D3D12 translation
// ----------------------------------------------------------------------------
inline DXGI_FORMAT ToDxgiFormat(Format format) {
    switch (format) {
        case Format::RGBA8_UNorm:       return DXGI_FORMAT_R8G8B8A8_UNORM;
        case Format::BGRA8_UNorm:       return DXGI_FORMAT_B8G8R8A8_UNORM;
        case Format::RGBA16_Float:      return DXGI_FORMAT_R16G16B16A16_FLOAT;
        case Format::RGBA32_Float:      return DXGI_FORMAT_R32G32B32A32_FLOAT;
        case Format::R32_Float:         return DXGI_FORMAT_R32_FLOAT;
        case Format::D32_Float:         return DXGI_FORMAT_D32_FLOAT;
        case Format::D24_UNorm_S8_UInt: return DXGI_FORMAT_D24_UNORM_S8_UINT;
        default:                        return DXGI_FORMAT_UNKNOWN;
    }
}

// Unlike Vulkan (image layout transitions), D3D12 resource states also cover
// buffers, but this API only calls TransitionTextures today -- kept scoped
// to what ICommandList actually exposes.
inline D3D12_RESOURCE_STATES ToD3D12ResourceState(ResourceState state) {
    switch (state) {
        case ResourceState::Undefined:      return D3D12_RESOURCE_STATE_COMMON;
        case ResourceState::RenderTarget:   return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case ResourceState::DepthWrite:     return D3D12_RESOURCE_STATE_DEPTH_WRITE;
        case ResourceState::DepthRead:      return D3D12_RESOURCE_STATE_DEPTH_READ;
        case ResourceState::ShaderReadOnly: return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        case ResourceState::CopySrc:        return D3D12_RESOURCE_STATE_COPY_SOURCE;
        case ResourceState::CopyDst:        return D3D12_RESOURCE_STATE_COPY_DEST;
        case ResourceState::Present:        return D3D12_RESOURCE_STATE_PRESENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

inline D3D12_FILTER ToD3D12Filter(FilterMode magFilter, FilterMode minFilter) {
    bool linear = magFilter == FilterMode::Linear && minFilter == FilterMode::Linear;
    return linear ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
    // Mixed min/mag combinations (e.g. linear mag + point min) aren't
    // expressible from just FilterMode today -- add if a use case needs it.
}

inline D3D12_TEXTURE_ADDRESS_MODE ToD3D12AddressMode(AddressMode mode) {
    switch (mode) {
        case AddressMode::Repeat:         return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::ClampToEdge:    return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case AddressMode::MirroredRepeat: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

inline D3D12_BLEND ToD3D12Blend(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return D3D12_BLEND_ZERO;
        case BlendFactor::One:              return D3D12_BLEND_ONE;
        case BlendFactor::SrcAlpha:         return D3D12_BLEND_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return D3D12_BLEND_DEST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return D3D12_BLEND_INV_DEST_ALPHA;
    }
    return D3D12_BLEND_ONE;
}

inline D3D12_BLEND_OP ToD3D12BlendOp(BlendOp op) {
    switch (op) {
        case BlendOp::Add:             return D3D12_BLEND_OP_ADD;
        case BlendOp::Subtract:        return D3D12_BLEND_OP_SUBTRACT;
        case BlendOp::ReverseSubtract: return D3D12_BLEND_OP_REV_SUBTRACT;
        case BlendOp::Min:             return D3D12_BLEND_OP_MIN;
        case BlendOp::Max:             return D3D12_BLEND_OP_MAX;
    }
    return D3D12_BLEND_OP_ADD;
}

} // namespace draw::d3d12_backend