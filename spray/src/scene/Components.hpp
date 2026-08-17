#pragma once

#include "asset/AssetTypes.hpp"

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

namespace spray {

struct Name {
    std::string value;
};

struct Transform {
    glm::vec3 position{ 0.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
    glm::vec3 scale{ 1.0f };
};

// Recomputed every frame by Scene::UpdateWorldTransforms from Transform +
// Hierarchy -- not hand-edited. Always present alongside Transform (Scene::
// CreateEntity emplaces both together).
struct WorldTransform {
    glm::mat4 matrix{ 1.0f };
};

// entt::null parent = a root entity. Always present alongside Transform
// (Scene::CreateEntity emplaces it, defaulted to no parent/no children) so
// UpdateWorldTransforms doesn't need to special-case entities that lack it.
struct Hierarchy {
    entt::entity parent = entt::null;
    std::vector<entt::entity> children;
};

struct MeshRenderer {
    assets::AssetHandle<assets::MeshTag> mesh;
    assets::AssetHandle<assets::MaterialTag> material; // may be invalid -- renderer falls back to a default material
};

struct Camera {
    float fovYRadians = glm::radians(60.0f);
    float nearPlane = 0.01f;
    float farPlane = 1000.0f;
    bool isPrimary = false; // which camera the viewport reads if a scene has several
};

struct Light {
    enum class Type { Point, Directional };
    Type type = Type::Point;
    glm::vec3 color{ 1.0f };
    float intensity = 1.0f;
};

} // namespace spray