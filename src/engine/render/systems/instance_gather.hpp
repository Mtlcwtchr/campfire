#pragma once
// The system that turns entities into what a renderer draws.
//
// One walk of the registry produces flat arrays grouped by (mesh, material),
// plus the entity that owns every instance. That grouping is what makes the
// draw instanced at all: the renderer binds one asset and one material for a
// whole batch, so a forest costs a handful of draws rather than one per tree.
//
// Three properties are worth stating because they are what the tests hold:
//
// - The result depends on the registry's contents, not on the order entities
//   were created in. Two machines that built the same world in different orders
//   have to draw the same frame.
// - `owners[i]` is the entity behind `instances[i]`. That is the only link
//   between a drawn thing and its own data, and it is deliberately the only
//   one: nothing here knows what the data means.
// - Nothing is culled here. Visibility is another system's answer, read from a
//   component; a gather that also decided visibility would disagree with it on
//   any frame where only one of them ran.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/render/components.hpp"
#include "engine/ecs/registry.hpp"
#include "engine/ecs/snapshot.hpp"

namespace engine::render {

// One instance as the renderer wants it, with no pointers and no ownership.
struct GatheredInstance {
    float position[3]{0, 0, 0};
    float scale = 1;
    float yaw = 0;
    float tint = 1;
    float phase = 0;
    std::uint32_t variant = 0;
    float surface = 0;
};

// A run of instances that share an asset and a material, which is exactly what
// one instanced draw covers.
struct InstanceBatch {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

struct GatheredInstances {
    std::vector<InstanceBatch> batches;
    std::vector<GatheredInstance> instances;
    // Parallel to `instances`. The link from a drawn thing back to its entity.
    std::vector<Entity> owners;
    // Entities that had the components but were not drawn, and why.
    std::size_t invisible = 0;
    std::size_t rejected = 0;   // a transform that is not a finite number

    [[nodiscard]] std::size_t drawn() const { return instances.size(); }
};

// Reads; never writes. A snapshot rather than a registry reference because that
// is the boundary the rest of this ECS already draws around presentation.
[[nodiscard]] GatheredInstances gatherInstances(const ecs::Registry& registry);
[[nodiscard]] GatheredInstances gatherInstances(const ecs::Snapshot& snapshot);

} // namespace engine::render
