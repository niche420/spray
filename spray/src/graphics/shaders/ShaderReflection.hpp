#pragma once

#include "graphics/GraphicsTypes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace spray::graphics::shaders {

// One descriptor binding as seen by reflection, before it's merged with any
// other stage's view of the same set (see ShaderLibrary::
// DeriveBindGroupLayout, which is what actually does that merge).
struct ReflectedBinding {
    uint32_t set = 0;
    uint32_t binding = 0;
    BindingType type = BindingType::UniformBuffer;
};

// Everything ShaderReflection can currently pull out of one compiled
// module. SPIR-V (Vulkan) only for now -- see ShaderReflection.cpp's
// header comment for why DXIL reflection isn't implemented alongside it.
struct ReflectedModule {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entryPoint;                 // the module's actual entry point name, not hand-typed
    std::vector<ReflectedBinding> bindings; // every descriptor binding the module declares, any set
};

// Wraps SPIRV-Reflect (external/SPIRV-Reflect) to pull entry point, stage,
// and descriptor-binding information directly out of compiled SPIR-V
// bytes, rather than requiring that information to be hand-typed next to
// pipeline creation code and kept in sync with the shader source by hand.
// See ShaderLibrary's class comment for the specific bug this exists to
// make structurally impossible (PathTracer's Vulkan shaders moved from
// HLSL to GLSL, and a hand-typed entryPoint string silently stopped
// matching what was actually compiled -- reflection can't drift like that,
// since the entry point name comes from the module itself).
//
// Throws on malformed SPIR-V, an unsupported shader stage (this engine
// only models Vertex/Pixel/Compute/RayGen/Miss/ClosestHit -- see
// ShaderStage's own comment on AnyHit/Intersection being deliberately
// omitted), or a descriptor type this engine's BindGroupEntry model can't
// represent (currently: combined image+sampler -- see the "combined
// image+sampler" comment in the .cpp for the workaround).
ReflectedModule ReflectSpirv(const std::vector<uint8_t>& spirv);

} // namespace spray::graphics::shaders