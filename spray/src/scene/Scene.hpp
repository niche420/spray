#pragma once

#include "Components.hpp"
#include "math/Geometry.hpp"

#include <entt/entt.hpp>

#include <filesystem>
#include <string>

namespace spray::assets {
class AssetManager;
}

namespace spray {

class Scene {
public:
    // parent == entt::null (the default) creates a root entity.
    entt::entity CreateEntity(const std::string& name, entt::entity parent = entt::null);

    // Destroys the whole subtree rooted at e (its children, recursively --
    // Hierarchy::children holds raw entt::entity values with no
    // generation-checked access, so leaving a child pointing at a destroyed
    // parent-less entity would dangle).
    void DestroyEntity(entt::entity e);

    void Reparent(entt::entity e, entt::entity newParent);

    // Recomputes WorldTransform for every entity from Transform + Hierarchy.
    // No dirty-flag tracking -- full re-walk every call. Cheap enough at the
    // scene sizes this app targets (single imported objects, not
    // open-world); revisit if that assumption stops holding.
    void UpdateWorldTransforms();

    // Requires up-to-date WorldTransform data (call UpdateWorldTransforms
    // first this frame/before calling this). Uses each MeshAsset's
    // precomputed local-space bounds transformed by world matrix --
    // intended for sizing the capture rig's orbit radius around the loaded
    // scene. Returns an invalid AABB (AABB::IsValid() == false) if the
    // scene has no MeshRenderer entities with valid mesh handles.
    AABB ComputeWorldBounds(assets::AssetManager& assets) const;

    entt::registry& GetRegistry() { return registry; }
    const entt::registry& GetRegistry() const { return registry; }

    std::string name;
    std::filesystem::path sourcePath; // empty if not yet saved/loaded from a file

private:
    void UpdateWorldTransformRecursive(entt::entity e, const glm::mat4& parentWorld);

    entt::registry registry;
};

} // namespace spray