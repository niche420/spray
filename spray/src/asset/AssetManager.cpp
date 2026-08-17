#include "pch.hpp"
#include "AssetManager.hpp"
#include "math/Geometry.hpp"

#include <cstring>

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

void AssetManager::InvalidateGpuCache(graphics::IDevice& device) {
    for (auto& [index, mesh] : m_gpuMeshCache) {
        device.DestroyBuffer(mesh.vertexBuffer);
        device.DestroyBuffer(mesh.indexBuffer);
    }
    m_gpuMeshCache.clear();
}

} // namespace spray::assets