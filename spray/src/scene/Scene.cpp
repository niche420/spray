#include "pch.hpp"
#include "Scene.hpp"
#include "asset/AssetManager.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

namespace spray {

entt::entity Scene::CreateEntity(const std::string& name, entt::entity parent) {
    entt::entity e = registry.create();
    registry.emplace<Name>(e, name);
    registry.emplace<Transform>(e);
    registry.emplace<WorldTransform>(e);
    registry.emplace<Hierarchy>(e);
    if (parent != entt::null) {
        Reparent(e, parent);
    }
    return e;
}

void Scene::Reparent(entt::entity e, entt::entity newParent) {
    Hierarchy& h = registry.get_or_emplace<Hierarchy>(e);

    if (h.parent != entt::null && registry.valid(h.parent)) {
        auto& oldSiblings = registry.get<Hierarchy>(h.parent).children;
        oldSiblings.erase(std::remove(oldSiblings.begin(), oldSiblings.end(), e), oldSiblings.end());
    }

    h.parent = newParent;

    if (newParent != entt::null) {
        registry.get_or_emplace<Hierarchy>(newParent).children.push_back(e);
    }
}

void Scene::DestroyEntity(entt::entity e) {
    if (!registry.valid(e)) return;

    if (Hierarchy* h = registry.try_get<Hierarchy>(e)) {
        // Copy: the recursive DestroyEntity calls below mutate sibling
        // vectors elsewhere in the registry, but never this vector while
        // we're iterating it, so a copy (rather than a reference) just
        // keeps that invariant obviously true rather than relying on it.
        auto children = h->children;
        for (entt::entity child : children) DestroyEntity(child);

        if (h->parent != entt::null && registry.valid(h->parent)) {
            auto& siblings = registry.get<Hierarchy>(h->parent).children;
            siblings.erase(std::remove(siblings.begin(), siblings.end(), e), siblings.end());
        }
    }

    registry.destroy(e);
}

void Scene::UpdateWorldTransformRecursive(entt::entity e, const glm::mat4& parentWorld) {
    Transform& local = registry.get<Transform>(e);
    glm::mat4 localMatrix = glm::translate(glm::mat4(1.0f), local.position) *
        glm::mat4_cast(local.rotation) *
        glm::scale(glm::mat4(1.0f), local.scale);
    glm::mat4 world = parentWorld * localMatrix;
    registry.get_or_emplace<WorldTransform>(e).matrix = world;

    if (Hierarchy* h = registry.try_get<Hierarchy>(e)) {
        for (entt::entity child : h->children) {
            UpdateWorldTransformRecursive(child, world);
        }
    }
}

void Scene::UpdateWorldTransforms() {
    // Roots = entities with a Transform but no parent (either no Hierarchy
    // component at all, or one whose parent == entt::null). Every entity
    // Scene::CreateEntity makes has both Transform and Hierarchy, but this
    // stays defensive against entities constructed by hand elsewhere.
    for (auto entity : registry.view<Transform>()) {
        Hierarchy* h = registry.try_get<Hierarchy>(entity);
        if (!h || h->parent == entt::null) {
            UpdateWorldTransformRecursive(entity, glm::mat4(1.0f));
        }
    }
}

AABB Scene::ComputeWorldBounds(assets::AssetManager& assets) const {
    AABB bounds;
    auto view = registry.view<const MeshRenderer, const WorldTransform>();
    for (auto [entity, mesh, world] : view.each()) {
        if (!mesh.mesh.IsValid()) continue;
        const assets::MeshAsset& asset = assets.GetMesh(mesh.mesh);
        const AABB& local = asset.localBounds;
        if (!local.IsValid()) continue;

        // Transform all 8 corners rather than just center/extents -- a
        // rotated box's world-space AABB isn't obtainable by transforming
        // each local axis independently.
        glm::vec3 corners[8] = {
            { local.min.x, local.min.y, local.min.z }, { local.max.x, local.min.y, local.min.z },
            { local.min.x, local.max.y, local.min.z }, { local.max.x, local.max.y, local.min.z },
            { local.min.x, local.min.y, local.max.z }, { local.max.x, local.min.y, local.max.z },
            { local.min.x, local.max.y, local.max.z }, { local.max.x, local.max.y, local.max.z },
        };
        for (const auto& c : corners) {
            bounds.Encapsulate(glm::vec3(world.matrix * glm::vec4(c, 1.0f)));
        }
    }
    return bounds;
}

} // namespace spray