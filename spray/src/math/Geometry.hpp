#pragma once

#include <glm/glm.hpp>
#include <limits>

namespace spray {

struct AABB {
    glm::vec3 min{ std::numeric_limits<float>::max() };
    glm::vec3 max{ std::numeric_limits<float>::lowest() };

    void Encapsulate(const glm::vec3& p) {
        min = glm::min(min, p);
        max = glm::max(max, p);
    }
    void Encapsulate(const AABB& other) {
        if (!other.IsValid()) return;
        min = glm::min(min, other.min);
        max = glm::max(max, other.max);
    }
    bool IsValid() const { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    glm::vec3 Center() const { return (min + max) * 0.5f; }
    glm::vec3 Extents() const { return (max - min) * 0.5f; } // half-extents
};

} // namespace spray