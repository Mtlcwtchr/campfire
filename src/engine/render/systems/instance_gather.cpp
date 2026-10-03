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
            candidate.instance.surface = tint->surface;
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
    // The order as one 64-bit key, sorted by radix (linear: a republish of a
    // few hundred thousand instances spent most of its time in a comparison
    // sort), then the candidates read once in that order.
    std::vector<std::uint32_t> order(candidates.size());
    for (std::uint32_t i = 0; i < order.size(); ++i) order[i] = i;
    bool narrow = true;
    std::vector<std::uint64_t> keys(candidates.size());
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const auto& c = candidates[i];
        narrow = narrow && c.mesh < 0x10000u && c.material < 0x10000u;
        keys[i] = (std::uint64_t(c.mesh & 0xFFFFu) << 48) | (std::uint64_t(c.material & 0xFFFFu) << 32) |
                  std::uint64_t(std::uint32_t(entt::to_integral(c.owner)));
    }
    if (narrow) {
        std::vector<std::uint32_t> scratch(order.size());
        std::vector<std::uint32_t> count(0x10000);
        for (int pass = 0; pass < 4; ++pass) {
            const int shift = pass * 16;
            std::fill(count.begin(), count.end(), 0u);
            for (const auto i : order) ++count[(keys[i] >> shift) & 0xFFFFu];
            // Every key the same here: nothing to move.
            if (std::any_of(count.begin(), count.end(), [&](std::uint32_t n) { return n == order.size(); })) continue;
            std::uint32_t at = 0;
            for (auto& n : count) { const auto here = n; n = at; at += here; }
            for (const auto i : order) scratch[count[(keys[i] >> shift) & 0xFFFFu]++] = i;
            order.swap(scratch);
        }
    } else {
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            const Candidate& x = candidates[a];
            const Candidate& y = candidates[b];
            if (x.mesh != y.mesh) return x.mesh < y.mesh;
            if (x.material != y.material) return x.material < y.material;
            return entt::to_integral(x.owner) < entt::to_integral(y.owner);
        });
    }

    result.instances.reserve(candidates.size());
    result.owners.reserve(candidates.size());
    for (const auto i : order) {
        const Candidate& candidate = candidates[i];
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
