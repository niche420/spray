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

    // Lazily allocates (and caches) a BLAS for this mesh's GPU buffers.
    // Requires GetOrCreateGpuMesh to have already been called for this
    // handle against the same device (uses its cached vertex/index buffers
    // directly rather than re-creating them) -- callers doing path tracing
    // still need the raster vertex/index buffers too (e.g. a viewport
    // preview), so this doesn't duplicate that upload. Throws if no GPU
    // mesh is cached yet for this handle, rather than silently creating one
    // as a side effect.
    //
    // Like AssetManager itself, this only *allocates* the BLAS (queries
    // prebuild size, creates the backing buffer) -- it does not build it.
    // The caller must still record the actual build via
    // ICommandList::BuildBLAS using the BLASBuildDesc returned by
    // GetGpuBlasBuildDesc, exactly once per handle (see NeedsBlasBuild/
    // MarkBlasBuilt below), before the BLAS is usable in a TLAS build or
    // TraceRays call. Mirrors the two-step Create*/Build* split
    // GraphicsTypes.hpp documents for BLASHandle/TLASHandle generally.
    graphics::BLASHandle GetOrCreateGpuBlas(AssetHandle<MeshTag> h, graphics::IDevice& device);

    // The exact BLASBuildDesc GetOrCreateGpuBlas used to allocate this
    // mesh's BLAS -- pass this to ICommandList::BuildBLAS to actually build
    // it. Kept here (rather than reconstructed by the caller) so the
    // geometry description used for allocation and the one used for the
    // actual build can never drift apart. Only valid after
    // GetOrCreateGpuBlas has been called for this handle.
    const graphics::BLASBuildDesc& GetGpuBlasBuildDesc(AssetHandle<MeshTag> h) const;

    // True if GetOrCreateGpuBlas has been called for this handle but the
    // returned BLASHandle hasn't been built yet this device-lifetime (i.e.
    // BuildBLAS hasn't been recorded for it, or InvalidateGpuCache reset
    // everything since). Callers building a TLAS use this to know which
    // BLASes still need a BuildBLAS call this frame vs. which are already
    // resident from a previous one.
    bool NeedsBlasBuild(AssetHandle<MeshTag> h) const;
    void MarkBlasBuilt(AssetHandle<MeshTag> h);

    // Must be called (against the still-live device) before that device is
    // destroyed -- e.g. right before a backend switch tears the old IDevice
    // down. GPU handles cached here are meaningless once their creating
    // device is gone.
    void InvalidateGpuCache(graphics::IDevice& device);

private:
    graphics::HandlePool<AssetHandle<MeshTag>, MeshAsset> m_meshes;
    graphics::HandlePool<AssetHandle<MaterialTag>, MaterialAsset> m_materials;
    std::unordered_map<uint32_t, GpuMesh> m_gpuMeshCache; // keyed by AssetHandle<MeshTag>::index

    // Mirrors m_gpuMeshCache's keying (asset index, not full handle -- same
    // "never destroyed/recreated at runtime" caveat applies). `buildDesc` is
    // retained so GetGpuBlasBuildDesc can hand back the exact desc used at
    // allocation time; `built` tracks status per the NeedsBlasBuild/
    // MarkBlasBuilt contract above.
    struct GpuBlasEntry {
        graphics::BLASHandle handle;
        graphics::BLASBuildDesc buildDesc;
        bool built = false;
    };
    std::unordered_map<uint32_t, GpuBlasEntry> m_gpuBlasCache;
};

} // namespace spray::assets