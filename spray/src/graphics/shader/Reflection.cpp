#include "pch.hpp"
#include "Reflection.hpp"

#include <spirv_>

#include <stdexcept>

namespace spray::graphics::shaders {

namespace {

ShaderStage ToShaderStage(SpvReflectShaderStageFlagBits stage) {
    switch (stage) {
        case SPV_REFLECT_SHADER_STAGE_VERTEX_BIT:          return ShaderStage::Vertex;
        case SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT:        return ShaderStage::Pixel;
        case SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT:         return ShaderStage::Compute;
        case SPV_REFLECT_SHADER_STAGE_RAYGEN_BIT_KHR:      return ShaderStage::RayGen;
        case SPV_REFLECT_SHADER_STAGE_MISS_BIT_KHR:        return ShaderStage::Miss;
        case SPV_REFLECT_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return ShaderStage::ClosestHit;
        default:
            throw std::runtime_error(
                "ShaderReflection: unsupported SPIR-V shader stage (this engine only models "
                "Vertex/Pixel/Compute/RayGen/Miss/ClosestHit -- see ShaderStage's comment on "
                "AnyHit/Intersection being deliberately omitted)");
    }
}

BindingType ToBindingType(SpvReflectDescriptorType type) {
    switch (type) {
        case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:             return BindingType::UniformBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:             return BindingType::StorageBuffer;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:              return BindingType::SampledTexture;
        case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:              return BindingType::StorageTexture;
        case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:                    return BindingType::Sampler;
        case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR: return BindingType::AccelerationStructure;
        case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            // GLSL's `uniform sampler2D` compiles to this. This engine's
            // BindGroupEntry model deliberately keeps textures and
            // samplers as two separate descriptor types (matches D3D12's
            // split SRV/sampler heaps -- see D3D12GraphicsDevice's comment
            // on why), so a combined binding isn't representable yet.
            // Declare `texture2D` + a separate `sampler` uniform in GLSL
            // instead (both are core Vulkan GLSL, no extension needed)
            // until/unless combined-sampler support gets added.
            throw std::runtime_error(
                "ShaderReflection: combined image+sampler bindings aren't representable by this "
                "engine's BindGroupEntry model yet -- declare separate texture2D/sampler uniforms "
                "in the GLSL source instead of a combined sampler2D");
        default:
            throw std::runtime_error("ShaderReflection: unrecognized SPIR-V descriptor type (" +
                                      std::to_string(static_cast<int>(type)) + ")");
    }
}

} // namespace

ReflectedModule ReflectSpirv(const std::vector<uint8_t>& spirv) {
    if (spirv.empty()) {
        throw std::runtime_error("ShaderReflection: empty SPIR-V bytecode");
    }

    SpvReflectShaderModule module{};
    SpvReflectResult result = spvReflectCreateShaderModule(spirv.size(), spirv.data(), &module);
    if (result != SPV_REFLECT_RESULT_SUCCESS) {
        throw std::runtime_error("ShaderReflection: spvReflectCreateShaderModule failed (code " +
                                  std::to_string(static_cast<int>(result)) + ")");
    }

    ReflectedModule out;
    try {
        out.stage = ToShaderStage(module.shader_stage);
        out.entryPoint = module.entry_point_name ? module.entry_point_name : "main";

        uint32_t setCount = 0;
        spvReflectEnumerateDescriptorSets(&module, &setCount, nullptr);
        std::vector<SpvReflectDescriptorSet*> sets(setCount);
        spvReflectEnumerateDescriptorSets(&module, &setCount, sets.data());

        for (SpvReflectDescriptorSet* set : sets) {
            for (uint32_t i = 0; i < set->binding_count; ++i) {
                const SpvReflectDescriptorBinding* binding = set->bindings[i];
                ReflectedBinding rb;
                rb.set = set->set;
                rb.binding = binding->binding;
                rb.type = ToBindingType(binding->descriptor_type);
                out.bindings.push_back(rb);
            }
        }
    } catch (...) {
        spvReflectDestroyShaderModule(&module);
        throw;
    }

    spvReflectDestroyShaderModule(&module);
    return out;
}

} // namespace spray::graphics::shaders