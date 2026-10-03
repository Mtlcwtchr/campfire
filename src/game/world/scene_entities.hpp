#pragma once
// Turning a scatter into entities, once, when the scatter changes.
//
// The scatter is what the world decided: a flat list of objects with positions
// the generator derived. Entities are what the renderer talks to. Publishing
// them is a copy, and doing it here - rather than letting the pass walk the
// scatter every frame - is what puts a tree's identity somewhere a later system
// can hang data on it.
//
// A tree carries a transform, which asset it is, its shading, and the scatter
// id it came from. It does not carry a mesh, a material or a renderer: see
// engine/render/components.hpp for why that is the whole point.
#include <cstdint>
#include <memory>
#include <span>

#include "engine/ecs/registry.hpp"
#include "engine/render/components.hpp"
#include "game/world/scene_scatter.hpp"

namespace world::decor {

// The link from a drawn instance back to the object's own data.
//
// Today that data is the scatter entry itself, and the id is the stable,
// position-derived one the generator produced - so it survives a region being
// unloaded and rebuilt. When trees grow data of their own, this is the handle
// it hangs off; nothing in the renderer has to learn what a tree is.
struct ScatterRef {
    std::uint64_t id = 0;
};

// Makes the scatter entities in `registry` the objects of `scatter`: an object
// whose id is already there keeps its entity (updated in place), the rest are
// created, and entities of objects no longer present are destroyed.
//
// Only entities carrying a ScatterRef are touched: a registry shared with the
// rest of the game keeps whatever else lives in it. Objects naming a model
// beyond `models` are dropped and counted rather than drawn as model zero,
// because a tree that silently became a different species is harder to notice
// than one that is missing.
struct Published {
    std::size_t created = 0;
    std::size_t destroyed = 0;
    std::size_t kept = 0;       // same id as before: its entity updated in place
    std::size_t unknownModel = 0;
};
Published publishScatter(engine::ecs::Registry& registry, const Scatter& scatter,
                         std::uint32_t models);

// The same, region by region, for a placement that publishes parts
// (ScenePlacementSnapshot::parts): a region whose part is the same pointer as
// last time keeps its entities untouched, a changed one has its entities
// replaced, a region gone has them destroyed. A publication costs what
// changed, not what is in view. Entities made here are kept apart from
// publishScatter's (the two are not mixed in one registry).
Published publishParts(engine::ecs::Registry& registry, std::span<const ScatterBounds> regions,
                       std::span<const std::shared_ptr<const Scatter>> parts, std::uint32_t models);

} // namespace world::decor
