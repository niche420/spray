#include "pch.hpp"
#include "AssetManager.hpp"
#include "math/Geometry.hpp"

#include <cstring>
#include <stdexcept>

namespace spray::assets {

AssetHandle<MeshTag> AssetManager::AddMesh(MeshAsset asset) {
    AABB bounds;
    for (const auto& v : asset.vertices) bounds.Encapsulate(v.position);
    asset.localBounds = bounds;
    return m_meshes.Add(std::move(asset));
}

AssetHandle<MaterialTag> AssetManager::AddMaterial(MaterialAsset asset) {
    return m_materials.Add(std::move(asset));
}

MeshAsset& AssetManager::GetMesh(AssetHandle<MeshTag> h) {
    return m_meshes.Get(h);
}

MaterialAsset& AssetManager::GetMaterial(AssetHandle<MaterialTag> h) {
    return m_materials.Get(h);
}

AssetManager::GpuMesh& AssetManager::GetOrCreateGpuMesh(AssetHandle<MeshTag> h, graphics::IDevice& device) {
    auto it = m_gpuMeshCache.find(h.index);
    if (it != m_gpuMeshCache.end()) return it->second;

    MeshAsset& asset = GetMesh(h);

    // Simplification: direct host-visible upload, no staging buffer +
    // device-local copy. Fine for the asset sizes this app targets so far
    // (single imported Sketchfab-scale objects); revisit with a staging
    // path if mesh sizes grow enough for upload-heap bandwidth/residency
    // to matter.
    graphics::BufferDesc vbDesc;
    vbDesc.sizeBytes = asset.vertices.size() * sizeof(Vertex);
    vbDesc.usage = graphics::BufferUsage::VertexBuffer;
    vbDesc.hostVisible = true;
    graphics::BufferHandle vb = device.CreateBuffer(vbDesc);
    void* vbMapped = device.MapBuffer(vb);
    std::memcpy(vbMapped, asset.vertices.data(), vbDesc.sizeBytes);
    device.UnmapBuffer(vb);

    graphics::BufferDesc ibDesc;
    ibDesc.sizeBytes = asset.indices.size() * sizeof(uint32_t);
    ibDesc.usage = graphics::BufferUsage::IndexBuffer;
    ibDesc.hostVisible = true;
    graphics::BufferHandle ib = device.CreateBuffer(ibDesc);
    void* ibMapped = device.MapBuffer(ib);
    std::memcpy(ibMapped, asset.indices.data(), ibDesc.sizeBytes);
    device.UnmapBuffer(ib);

    GpuMesh mesh;
    mesh.vertexBuffer = vb;
    mesh.indexBuffer = ib;
    mesh.indexCount = static_cast<uint32_t>(asset.indices.size());

    return m_gpuMeshCache.emplace(h.index, mesh).first->second;
}

graphics::BLASHandle AssetManager::GetOrCreateGpuBlas(AssetHandle<MeshTag> h, graphics::IDevice& device) {
    auto it = m_gpuBlasCache.find(h.index);
    if (it != m_gpuBlasCache.end()) return it->second.handle;

    // Requires the GPU mesh to already exist -- see header comment.
    // Deliberately not calling GetOrCreateGpuMesh(h, device) here to create
    // it on demand: that would silently upload a mesh a caller only wanted
    // for ray tracing and never intended to rasterize, which is a
    // surprising side effect for a "GetOrCreate the BLAS" call to have.
    // Fail loudly instead.
    auto meshIt = m_gpuMeshCache.find(h.index);
    if (meshIt == m_gpuMeshCache.end()) {
        throw std::runtime_error("GetOrCreateGpuBlas: no GPU mesh cached for this handle -- "
                                  "call GetOrCreateGpuMesh first");
    }
    const GpuMesh& gpuMesh = meshIt->second;
    const MeshAsset& asset = GetMesh(h);

    graphics::BLASGeometryDesc geom;
    geom.vertexBuffer = gpuMesh.vertexBuffer;
    geom.vertexStride = sizeof(Vertex);
    geom.vertexFormat = graphics::Format::RGB32_Float; // matches Vertex::position's layout
    geom.vertexCount = static_cast<uint32_t>(asset.vertices.size());
    geom.indexBuffer = gpuMesh.indexBuffer;
    geom.indexCount = gpuMesh.indexCount;
    geom.use32BitIndices = true; // GltfImporter always widens indices to u32, see its comment
    geom.opaque = true; // no any-hit shader support yet, see ShaderStage's comment

    GpuBlasEntry entry;
    entry.buildDesc.geometries = { geom };
    entry.handle = device.CreateBLAS(entry.buildDesc);
    entry.built = false;

    return m_gpuBlasCache.emplace(h.index, std::move(entry)).first->second.handle;
}

const graphics::BLASBuildDesc& AssetManager::GetGpuBlasBuildDesc(AssetHandle<MeshTag> h) const {
    auto it = m_gpuBlasCache.find(h.index);
    if (it == m_gpuBlasCache.end()) {
        throw std::runtime_error("GetGpuBlasBuildDesc: no BLAS cached for this handle -- "
                                  "call GetOrCreateGpuBlas first");
    }
    return it->second.buildDesc;
}

bool AssetManager::NeedsBlasBuild(AssetHandle<MeshTag> h) const {
    auto it = m_gpuBlasCache.find(h.index);
    return it != m_gpuBlasCache.end() && !it->second.built;
}

void AssetManager::MarkBlasBuilt(AssetHandle<MeshTag> h) {
    auto it = m_gpuBlasCache.find(h.index);
    if (it != m_gpuBlasCache.end()) it->second.built = true;
}

void AssetManager::InvalidateGpuCache(graphics::IDevice& device) {
    for (auto& [index, mesh] : m_gpuMeshCache) {
        device.DestroyBuffer(mesh.vertexBuffer);
        device.DestroyBuffer(mesh.indexBuffer);
    }
    m_gpuMeshCache.clear();

    for (auto& [index, blas] : m_gpuBlasCache) {
        device.DestroyBLAS(blas.handle);
    }
    m_gpuBlasCache.clear();
}

} // namespace spray::assets