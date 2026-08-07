#pragma once
#include <entt/entt.hpp>
#include "components.hpp"

namespace spray {
class Scene {
public:
    entt::entity createEntity(const std::string& name) {
        entt::entity e = registry.create();
        registry.emplace<Name>(e, name);
        registry.emplace<Transform>(e);
        return e;
    }

    template<typename T, typename... Args>
    T& addComponent(entt::entity e, Args&&... args) {
        return registry.emplace<T>(e, std::forward<Args>(args)...);
    }

    template<typename T>
    T* getComponent(entt::entity e) {
        return registry.try_get<T>(e);
    }

    entt::registry& getRegistry() { return registry; }

private:
    entt::registry registry;
};
}