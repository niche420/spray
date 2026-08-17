#pragma once

#include "graphics/GraphicsTypes.hpp"
#include "math/Geometry.hpp"

#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace spray::assets {

struct MeshTag {};
struct MaterialTag {};

// Reuses graphics::Handle's slot+generation scheme. Deliberately a distinct
// type from graphics::BufferHandle/TextureHandle: asset handles are meant
// to outlive a graphics::IDevice (e.g. across a backend switch), while GPU
// handles don't -- AssetManager owns the CPU-side data these point to and
// translates to GPU handles lazily/on demand (see AssetManager::
// GetOrCreateGpuMesh), rather than the two being conflated.
template <typename Tag>
using AssetHandle = graphics::Handle<Tag>;

struct Vertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// CPU-side, backend-agnostic. GPU buffers are created lazily by
// AssetManager, not stored here.
struct MeshAsset {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    // Local-space bounds, computed once by AssetManager::AddMesh (not set
    // by importers) -- used by Scene::ComputeWorldBounds so the capture rig
    // can size its orbit radius without re-scanning vertex data every call.
    AABB localBounds;
};

struct MaterialAsset {
    glm::vec4 baseColorFactor{1.0f};
    std::string baseColorTexturePath; // empty = untextured; see GltfImporter's note on texture loading
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
};

} // namespace spray::assets