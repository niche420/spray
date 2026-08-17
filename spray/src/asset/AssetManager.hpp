#pragma once

#include "AssetTypes.hpp"
#include "graphics/Device.hpp"
#include "graphics/HandlePool.hpp"

#include <unordered_map>

namespace spray::assets {

// Owns CPU-side asset data for the lifetime of the app. GPU-side handles
// (per currently-active graphics::IDevice) are a separate, invalidatable
// cache -- on backend switch, call InvalidateGpuCache() against the
// *old* device before it's destroyed, then let GetOrCreateGpuMesh
// re-upload from the CPU data here against the new one. GPU resources
// never migrate directly between devices.
class AssetManager {
public:
    AssetHandle<MeshTag> AddMesh(MeshAsset asset);
    AssetHandle<MaterialTag> AddMaterial(MaterialAsset asset);

    MeshAsset& GetMesh(AssetHandle<MeshTag> h);
    MaterialAsset& GetMaterial(AssetHandle<MaterialTag> h);

    struct GpuMesh {
        graphics::BufferHandle vertexBuffer;
        graphics::BufferHandle indexBuffer;
        uint32_t indexCount = 0;
    };

    // Lazily creates (and caches) GPU buffers for this mesh on `device`.
    // Cache is keyed by asset index only, not the full handle (index +
    // generation) -- fine while assets are never destroyed/recreated at
    // runtime; revisit if that changes, since a reused index with a bumped
    // generation would then collide with a stale cache entry.
    GpuMesh& GetOrCreateGpuMesh(AssetHandle<MeshTag> h, graphics::IDevice& device);

    // Must be called (against the still-live device) before that device is
    // destroyed -- e.g. right before a backend switch tears the old IDevice
    // down. GPU handles cached here are meaningless once their creating
    // device is gone.
    void InvalidateGpuCache(graphics::IDevice& device);

private:
    graphics::HandlePool<AssetHandle<MeshTag>, MeshAsset> m_meshes;
    graphics::HandlePool<AssetHandle<MaterialTag>, MaterialAsset> m_materials;
    std::unordered_map<uint32_t, GpuMesh> m_gpuMeshCache; // keyed by AssetHandle<MeshTag>::index
};

} // namespace spray::assets