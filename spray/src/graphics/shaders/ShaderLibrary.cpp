#include "pch.hpp"
#include "ShaderLibrary.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace spray::graphics::shaders {

namespace {

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    size_t size = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    return data;
}

} // namespace

const ShaderLibrary::LoadedShader& ShaderLibrary::Load(const std::string& stem, IDevice& device) {
    auto it = m_cache.find(stem);
    if (it != m_cache.end()) return it->second;

    if (device.GetBackendType() != BackendType::Vulkan) {
        // DXIL reflection isn't implemented (see class comment) -- there's
        // nothing ShaderLibrary can do for a D3D12 device yet. The old
        // duplicated LoadCompiledShader helpers this replaced used to load
        // both .spv and .dxil unconditionally "just in case"; that's
        // pointless here specifically, since nothing on this path can
        // derive an entry point or bind group layout from DXIL -- failing
        // loudly now is more honest than reading a file that goes nowhere.
        throw std::runtime_error("ShaderLibrary: '" + stem + "' -- only Vulkan devices are "
                                  "supported right now (DXIL reflection isn't implemented yet; "
                                  "see class comment)");
    }

    ShaderBytecode bytecode;
    bytecode.spirv = ReadFileBytes("shaders/compiled/" + stem + ".spv");
    if (bytecode.spirv.empty()) {
        throw std::runtime_error("ShaderLibrary: no SPIR-V found for '" + stem + "' at "
                                  "shaders/compiled/" + stem + ".spv -- did the shader build step "
                                  "run?");
    }
    // bytecode.dxil intentionally left empty -- nothing on this path reads
    // it (see the backend check above), so there's no reason to read that
    // file too.

    ReflectedModule reflection = ReflectSpirv(bytecode.spirv);

    ShaderModuleDesc desc;
    desc.stage = reflection.stage;
    desc.entryPoint = reflection.entryPoint;
    desc.bytecode = bytecode;
    desc.debugName = stem;

    LoadedShader loaded;
    loaded.handle = device.CreateShaderModule(desc);
    loaded.reflection = std::move(reflection);

    return m_cache.emplace(stem, std::move(loaded)).first->second;
}

BindGroupLayoutDesc ShaderLibrary::DeriveBindGroupLayout(const std::vector<std::string>& stems,
                                                          uint32_t setIndex) const {
    std::unordered_map<uint32_t, BindGroupLayoutEntry> merged; // keyed by binding number

    for (const auto& stem : stems) {
        auto it = m_cache.find(stem);
        if (it == m_cache.end()) {
            throw std::runtime_error("ShaderLibrary::DeriveBindGroupLayout: '" + stem +
                                      "' hasn't been Load()ed yet -- call Load() for every stage "
                                      "before deriving a layout from them");
        }

        for (const auto& rb : it->second.reflection.bindings) {
            if (rb.set != setIndex) continue;

            auto existing = merged.find(rb.binding);
            if (existing != merged.end()) {
                if (existing->second.type != rb.type) {
                    throw std::runtime_error(
                        "ShaderLibrary::DeriveBindGroupLayout: set " + std::to_string(setIndex) +
                        " binding " + std::to_string(rb.binding) + " has conflicting types across "
                        "stages ('" + stem + "' disagrees with an earlier stage in this call) -- "
                        "check the shader sources agree on this binding's declared type");
                }
                continue; // same binding, same type, declared by another stage too -- fine
            }

            BindGroupLayoutEntry entry;
            entry.binding = rb.binding;
            entry.type = rb.type;
            // Non-authoritative -- see the header comment on
            // DeriveBindGroupLayout. Whichever stage declared this binding
            // first in iteration order is as good a value as any.
            entry.visibleStage = it->second.reflection.stage;
            merged.emplace(rb.binding, entry);
        }
    }

    BindGroupLayoutDesc out;
    out.entries.reserve(merged.size());
    for (auto& [binding, entry] : merged) out.entries.push_back(entry);

    // Sort by binding number for determinism -- doesn't affect correctness
    // (both backends look entries up by ::binding, not position in this
    // vector) but makes the generated layout reproducible/diffable rather
    // than depending on unordered_map iteration order.
    std::sort(out.entries.begin(), out.entries.end(),
              [](const BindGroupLayoutEntry& a, const BindGroupLayoutEntry& b) { return a.binding < b.binding; });

    return out;
}

void ShaderLibrary::InvalidateGpuCache(IDevice& device) {
    for (auto& [stem, loaded] : m_cache) {
        device.DestroyShaderModule(loaded.handle);
    }
    m_cache.clear();
}

} // namespace spray::graphics::shaders