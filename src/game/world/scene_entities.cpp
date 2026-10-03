#include "game/world/scene_entities.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace world::decor {

namespace {
// Which entity stands for which object, kept in the registry's context
// between publications: a republish then finds each object's entity by a hash
// lookup instead of collecting and sorting every entity first. Rebuilt from
// the ScatterRef view whenever the two disagree (a cleared registry, an
// entity destroyed by someone else, a duplicated id).
struct ScatterIndex {
    struct Slot { engine::render::Entity entity{}; std::uint32_t seen = 0; };
    std::unordered_map<std::uint64_t, Slot> byId;
    std::uint32_t publication = 0;
};
} // namespace

Published publishScatter(engine::ecs::Registry& registry, const Scatter& scatter,
                         std::uint32_t models) {
    using namespace engine::render;
    Published result;

    // What is there now, by the object it stands for. A republish is mostly
    // the same objects - a region arrived or left at the edge of the view -
    // so an object that keeps its id keeps its entity and only has its
    // components rewritten. Destroying and creating every one of them on each
    // publication was the largest single cost of the scene on the frame thread.
    auto& index = registry.ctx().contains<ScatterIndex>() ? registry.ctx().get<ScatterIndex>()
                                                          : registry.ctx().emplace<ScatterIndex>();
    const auto view = registry.view<ScatterRef>();
    if (index.byId.size() != view.size()) {
        index.byId.clear();
        index.byId.reserve(view.size());
        for (const auto entity : view) index.byId[view.get<ScatterRef>(entity).id].entity = entity;
        if (index.byId.size() != view.size()) {
            // Duplicated ids: start from nothing rather than leak the extras.
            // (Collected first: destroying while iterating a view is the one
            // thing EnTT asks you not to do.)
            std::vector<Entity> all(view.begin(), view.end());
            for (const auto entity : all) registry.destroy(entity);
            result.destroyed += all.size();
            index.byId.clear();
        }
    }
    const std::uint32_t now = ++index.publication;
    // The component pools, once: registry.get<T> finds T's pool by type on
    // every call, and three of those an object was most of a republish.
    auto& transforms = registry.storage<WorldTransform>();
    auto& tints = registry.storage<RenderTint>();
    auto& meshes = registry.storage<SmartMeshRef>();

    for (const Object& object : scatter.objects) {
        if (object.model >= models) {
            ++result.unknownModel;
            continue;
        }
        const WorldTransform transform{{float(object.x), float(object.y), float(object.z)},
                                       object.scale, object.yaw};
        const RenderTint tint{object.tint, object.phase, object.moss};
        const auto found = index.byId.find(object.id);
        if (found != index.byId.end() && found->second.seen != now && registry.valid(found->second.entity)) {
            const auto entity = found->second.entity;
            found->second.seen = now;
            transforms.get(entity) = transform;
            tints.get(entity) = tint;
            auto& mesh = meshes.get(entity);
            if (mesh.mesh != object.model) mesh = SmartMeshRef{object.model, 0, 0};
            ++result.kept;
            continue;
        }
        const auto entity = registry.create();
        transforms.emplace(entity, transform);
        meshes.emplace(entity, SmartMeshRef{object.model, 0, 0});
        tints.emplace(entity, tint);
        registry.emplace<ScatterRef>(entity, ScatterRef{object.id});
        // A second object with an id already placed this time is drawn but not
        // indexed; the size check above rebuilds the index next time.
        if (found == index.byId.end()) index.byId.emplace(object.id, ScatterIndex::Slot{entity, now});
        else if (found->second.seen != now) found->second = {entity, now};
        ++result.created;
    }
    for (auto it = index.byId.begin(); it != index.byId.end();) {
        if (it->second.seen == now) { ++it; continue; }
        if (registry.valid(it->second.entity)) { registry.destroy(it->second.entity); ++result.destroyed; }
        it = index.byId.erase(it);
    }
    return result;
}

namespace {
struct RegionIndex {
    struct Entry {
        std::shared_ptr<const Scatter> part;
        std::vector<engine::render::Entity> entities;
        std::uint32_t seen = 0;
    };
    std::unordered_map<ScatterBounds, Entry, ScatterBoundsHash> regions;
    std::uint32_t publication = 0;
};
} // namespace

Published publishParts(engine::ecs::Registry& registry, std::span<const ScatterBounds> regions,
                       std::span<const std::shared_ptr<const Scatter>> parts, std::uint32_t models) {
    using namespace engine::render;
    Published result;
    auto& index = registry.ctx().contains<RegionIndex>() ? registry.ctx().get<RegionIndex>()
                                                         : registry.ctx().emplace<RegionIndex>();
    const std::uint32_t now = ++index.publication;
    auto& transforms = registry.storage<WorldTransform>();
    auto& tints = registry.storage<RenderTint>();
    auto& meshes = registry.storage<SmartMeshRef>();
    auto& refs = registry.storage<ScatterRef>();
    const auto drop = [&](RegionIndex::Entry& entry) {
        for (const auto e : entry.entities)
            if (registry.valid(e)) { registry.destroy(e); ++result.destroyed; }
        entry.entities.clear();
    };
    for (std::size_t i = 0; i < regions.size() && i < parts.size(); ++i) {
        auto& entry = index.regions[regions[i]];
        entry.seen = now;
        if (entry.part == parts[i] && parts[i]) { result.kept += entry.entities.size(); continue; }
        drop(entry);
        entry.part = parts[i];
        if (!parts[i]) continue;
        entry.entities.reserve(parts[i]->objects.size());
        for (const Object& object : parts[i]->objects) {
            if (object.model >= models) { ++result.unknownModel; continue; }
            const auto entity = registry.create();
            transforms.emplace(entity, WorldTransform{{float(object.x), float(object.y), float(object.z)},
                                                      object.scale, object.yaw});
            meshes.emplace(entity, SmartMeshRef{object.model, 0, 0});
            tints.emplace(entity, RenderTint{object.tint, object.phase, object.moss});
            refs.emplace(entity, ScatterRef{object.id});
            entry.entities.push_back(entity);
            ++result.created;
        }
    }
    for (auto it = index.regions.begin(); it != index.regions.end();) {
        if (it->second.seen == now) { ++it; continue; }
        drop(it->second);
        it = index.regions.erase(it);
    }
    return result;
}

} // namespace world::decor
