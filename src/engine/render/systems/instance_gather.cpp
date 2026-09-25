#include "engine/render/systems/instance_gather.hpp"

#include <algorithm>
#include <cmath>

namespace engine::render {
namespace {

// What one entity contributes, before anything is grouped.
struct Candidate {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
    Entity owner{};
    GatheredInstance instance;
};

bool finite(const WorldTransform& transform) {
    return std::isfinite(transform.position[0]) && std::isfinite(transform.position[1]) &&
           std::isfinite(transform.position[2]) && std::isfinite(transform.scale) &&
           std::isfinite(transform.yaw) && transform.scale > 0;
}

} // namespace

GatheredInstances gatherInstances(const ecs::Registry& registry) {
    GatheredInstances result;
    std::vector<Candidate> candidates;

    auto view = registry.view<const WorldTransform, const SmartMeshRef>();
    candidates.reserve(view.size_hint());
    for (const Entity entity : view) {
        if (const auto* visible = registry.try_get<RenderVisible>(entity); visible && !visible->value) {
            ++result.invisible;
            continue;
        }
        const auto& transform = view.template get<const WorldTransform>(entity);
        if (!finite(transform)) {
            // A rejected instance is counted rather than dropped in silence: an
            // entity whose position became a NaN is a bug upstream, and a frame
            // that quietly draws one fewer tree never says so.
            ++result.rejected;
            continue;
        }
        const auto& reference = view.template get<const SmartMeshRef>(entity);
        Candidate candidate;
        candidate.mesh = reference.mesh;
        candidate.material = reference.material;
        candidate.owner = entity;
        candidate.instance.position[0] = transform.position[0];
        candidate.instance.position[1] = transform.position[1];
        candidate.instance.position[2] = transform.position[2];
        candidate.instance.scale = transform.scale;
        candidate.instance.yaw = transform.yaw;
        candidate.instance.variant = reference.variant;
        if (const auto* tint = registry.try_get<RenderTint>(entity)) {
            candidate.instance.tint = tint->tint;
            candidate.instance.phase = tint->phase;
        }
        candidates.push_back(candidate);
    }

    // By asset, then by material, then by entity identity.
    //
    // The last key is what keeps a batch STABLE: an entity's id does not change
    // while it lives, so the order inside a batch is the same next frame, and
    // creating or destroying something unrelated does not reshuffle the trees
    // around it. That matters because a dithered or screen-door transition
    // keyed on instance position would shimmer if the positions moved under it.
    //
    // It is identity, not content: two worlds built from the same contents in a
    // different order get different ids and therefore a different order within
    // a batch. For a world whose entities are created by a deterministic
    // scatter that is the same thing; for one assembled in an arbitrary order
    // it is not, and this does not claim otherwise.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.mesh != b.mesh) return a.mesh < b.mesh;
        if (a.material != b.material) return a.material < b.material;
        return entt::to_integral(a.owner) < entt::to_integral(b.owner);
    });

    result.instances.reserve(candidates.size());
    result.owners.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        if (result.batches.empty() || result.batches.back().mesh != candidate.mesh ||
            result.batches.back().material != candidate.material)
            result.batches.push_back({candidate.mesh, candidate.material,
                                      static_cast<std::uint32_t>(result.instances.size()), 0});
        ++result.batches.back().count;
        result.instances.push_back(candidate.instance);
        result.owners.push_back(candidate.owner);
    }
    return result;
}

GatheredInstances gatherInstances(const ecs::Snapshot& snapshot) {
    return gatherInstances(snapshot.registry());
}

} // namespace engine::render
