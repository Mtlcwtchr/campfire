#pragma once

#include <cstddef>
#include <entt/entity/registry.hpp>

namespace engine::ecs {

// Read-only access is the first safe boundary. The later snapshot builder will
// copy hot component pools into compact arrays; systems already depend on this
// interface rather than on mutable registry access.
class Snapshot {
public:
    explicit Snapshot(const entt::registry& registry) : registry_(&registry) {}

    const entt::registry& registry() const { return *registry_; }

    template <typename... Components>
    auto view() const { return registry_->view<Components...>(); }

    template <typename... Components>
    decltype(auto) get(entt::entity entity) const {
        return registry_->get<Components...>(entity);
    }

    template <typename... Components>
    bool all_of(entt::entity entity) const {
        return registry_->all_of<Components...>(entity);
    }

    template <typename... Components>
    std::size_t size() const {
        return registry_->view<Components...>().size_hint();
    }

    bool valid(entt::entity entity) const { return registry_->valid(entity); }

private:
    const entt::registry* registry_;
};

} // namespace engine::ecs
