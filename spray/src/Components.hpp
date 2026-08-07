#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

namespace spray {
struct Name {
    std::string value;
};

struct Transform {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct MeshRenderer {
    uint32_t meshHandle = 0;
    uint32_t materialHandle = 0;
};

struct Light {
    enum class Type { Point, Directional };
    Type type = Type::Point;
    glm::vec3 color{1.0f};
    float intensity = 1.0f;
};
}