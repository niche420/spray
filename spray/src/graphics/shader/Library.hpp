#pragma once

#include "Reflection.hpp"
#include "graphics/GraphicsTypes.hpp"
#include "graphics/Device.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace spray::graphics::shaders {

// Owns compiled shader modules for the app's whole lifetime (per active
// IDevice -- see InvalidateGpuCache, same contract as
// assets::AssetManager::InvalidateGpuCache), loaded by the stem name every
// spray_compile_* CMake call already produces (e.g. "capture.raygen" ->
// shaders/compiled/capture.raygen.spv/.dxil -- see spray/CMakeLists.txt).
//
// Replaces two things that used to be duplicated by hand in both
// SceneRenderer.cpp and Pathtracer.cpp:
//   1. The ReadFileBytes/LoadCompiledShader file-loading helper.
//   2. Hand-typed ShaderModuleDesc::entryPoint strings and
//      BindGroupLayoutDesc entries authored next to each pipeline's
//      creation code, with nothing to catch drift between what a shader
//      actually declares and what the C++ claims it declares.
//
// (2) is the more important half: PathTracer's Vulkan shaders moved from
// HLSL to GLSL, and a hand-typed entryPoint value ("RayGenMain") silently
// stopped matching what was actually compiled (GLSL always compiles to an
// entry point literally named "main") -- vkCreateRayTracingPipelinesKHR
// failed with VK_ERROR_INITIALIZATION_FAILED as a result. Deriving both
// the entry point and the bind group layout from the compiled module via
// reflection (see ShaderReflection.hpp) makes that whole class of bug
// structurally impossible: there's no second, hand-authored copy of that
// information left to drift out of sync.
//
// DXIL reflection isn't implemented yet (D3D12 is secondary priority right
// now and its ray tracing pipeline path has its own separate known gaps --
// see D3D12Device.hpp's namespace-mismatch note). Load() requires SPIR-V
// bytecode to be present and reflects that; a shader with only DXIL
// bytecode (a hypothetical D3D12-only build) isn't a supported input to
// ShaderLibrary right now -- see Load()'s implementation.
class ShaderLibrary {
public:
    struct LoadedShader {
        ShaderModuleHandle handle;
        ReflectedModule reflection; // stage/entryPoint/bindings, straight from the compiled module
    };

    // Loads (or returns the already-cached) shader module for `stem`
    // against `device` -- e.g. Load("capture.raygen", device). Reads
    // shaders/compiled/<stem>.spv (required -- see class comment) and
    // shaders/compiled/<stem>.dxil (optional, loaded and stored but not
    // reflected). Cache is keyed by stem only, not by device -- callers
    // must InvalidateGpuCache() before loading against a different device
    // (e.g. after a backend switch), same as AssetManager's GPU cache.
    const LoadedShader& Load(const std::string& stem, IDevice& device);

    // Merges the descriptor bindings of every already-Load()ed shader in
    // `stems` that declares set `setIndex` into one BindGroupLayoutDesc.
    // Replaces hand-authoring a BindGroupLayoutDesc next to pipeline
    // creation -- see class comment. Throws if two stages disagree about a
    // binding's type (a real authoring error worth catching loudly here,
    // not silently picking one and hoping).
    //
    // Each resulting entry's visibleStage is a best-effort, NON-
    // AUTHORITATIVE hint only -- both backends already grant every binding
    // broad visibility internally regardless of this field (see
    // VulkanDevice::CreateBindGroupLayout unconditionally OR-ing in
    // RayGen|ClosestHit|Miss visibility, and D3D12's BuildRootSignature
    // using SHADER_VISIBILITY_ALL) -- so it doesn't currently gate
    // anything at runtime; it's descriptive only. Worth knowing if
    // GraphicsTypes.hpp's binding-visibility model ever becomes more
    // granular.
    BindGroupLayoutDesc DeriveBindGroupLayout(const std::vector<std::string>& stems, uint32_t setIndex) const;

    // Must be called (against the still-live device) before that device is
    // destroyed -- same contract as AssetManager::InvalidateGpuCache. Safe
    // to call after the renderers that Load()ed these shaders have already
    // been destroyed (shader modules aren't needed after pipeline
    // creation completes, on either backend).
    void InvalidateGpuCache(IDevice& device);

private:
    std::unordered_map<std::string, LoadedShader> m_cache;
};

} // namespace spray::graphics::shaders