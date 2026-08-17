#pragma once

#include "scene/Scene.hpp"
#include "AssetManager.hpp"

#include <filesystem>

namespace spray::assets {

// Populates `scene` with entities mirroring the glTF node graph (Hierarchy +
// Transform + MeshRenderer per node), and registers referenced meshes and
// materials into `assets`. Sets scene.name/scene.sourcePath on success.
// Returns false (leaving `outError` populated) on failure; `scene` may be
// partially populated in that case.
bool ImportGltf(const std::filesystem::path& path, Scene& scene, AssetManager& assets, std::string& outError);

} // namespace spray::assets