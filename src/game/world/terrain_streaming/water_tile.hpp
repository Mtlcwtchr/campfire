#pragma once
// The water over one page, derived rather than sampled.
//
// This is the product that ends the per-vertex water decision. Today a
// renderer asks `HeightField::waterLevelAt` at a point, that call rebuilds a
// procedural reach from the coarse cell under it, and the answer depends on
// which channel happened to be nearest to that one point. Two vertices a few
// metres apart could pick different channels and get different levels, and a
// lake could stand at two heights with a step where they met.
//
// A WaterTile is decided once for a whole page, from two things that a camera
// cannot move: the persistent hydrology graph, which owns identity and head,
// and the page's own H_sim, which owns where the ground is. It needs no
// HeightField and no macro map - a cached BaseTile and the graph are enough.
//
// Sea level is zero everywhere in the world, which is what makes it a sea
// level, so the ocean needs no footprint here either: below zero and unclaimed
// is the sea.
//
// Transient by default. It is cheap enough to rebuild beside a page and it
// depends on nothing that persists differently from the graph.

#include <cstdint>
#include <functional>
#include <vector>

#include "engine/core/fixed.hpp"
#include "game/world/terrain_streaming/base_tile.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"

namespace world::streaming {

// How far outside a page a reach can still put water in it. The legacy
// carving draws a valley no wider than this, so nothing beyond it can flood a
// sample inside the page; it also sets how many neighbouring index pages a
// worker has to read.
inline constexpr std::int32_t kWaterHaloMetres = 800;

// Signed shore distance is stored in decimetres, which reaches 3.2 km either
// way - past any valley this world draws - in half the bytes of metres in
// fixed point.
inline constexpr std::int32_t kShoreDecimetresPerMetre = 10;

struct WaterTile {
    TileKey key{};
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t padding = 0;
    std::int32_t sampleMetres = 4;
    // The same range the page's H_sim uses, so a surface and a ground can be
    // subtracted without leaving quantised space.
    core::Fixed elevationMin{};
    core::Fixed elevationMax{};

    // Hydraulic head, quantised like H_sim and extended onto dry banks for
    // interpolation. A nonzero encoded height does not imply any water.
    std::vector<std::uint16_t> surfaceQuantized;
    // Standing water that owns the sample: the ocean, a lake, or none. A
    // sample wet from a river alone carries no body, because a river is a
    // reach and not a body.
    std::vector<std::uint16_t> waterBodyId;
    // The reach whose water reaches the sample, or the nearest one when it is
    // dry. Zero when no reach is near enough to matter.
    std::vector<std::uint32_t> riverId;
    // To the nearest reach's bank: negative inside the wet channel, zero at
    // the water's edge, positive across the flood plain.
    std::vector<std::int16_t> shoreDecimetres;
    // The share of the sample's own footprint that is under water, read off
    // the padded lattice rather than by taking more samples of the field.
    std::vector<std::uint8_t> coverage;
    // Unit tangent of the reach, downstream, in hundred-and-twenty-sevenths.
    std::vector<std::int8_t> flowX;
    std::vector<std::int8_t> flowY;
    // How much of this water is the sea's rather than its river's, in
    // two-hundred-and-fifty-fifths: nought up a river, rising over its last
    // stretch before the coast, so a river hands over to the sea instead of
    // butting into it (see CarvedSample::estuary). Not part of valid(): a page
    // without it is a page from before it existed, and reads as all river.
    std::vector<std::uint8_t> estuary;

    [[nodiscard]] std::size_t sampleCount() const {
        return (static_cast<std::size_t>(width) + padding * 2u) *
               (static_cast<std::size_t>(height) + padding * 2u);
    }
    [[nodiscard]] bool valid() const {
        const auto count = sampleCount();
        return width > 0 && height > 0 && sampleMetres > 0 &&
               surfaceQuantized.size() == count && waterBodyId.size() == count &&
               riverId.size() == count && shoreDecimetres.size() == count &&
               coverage.size() == count && flowX.size() == count && flowY.size() == count;
    }
    [[nodiscard]] bool wet(std::size_t at) const {
        return at < waterBodyId.size() &&
               (waterBodyId[at] != kInvalidWaterBodyId ||
                (at < coverage.size() && coverage[at] > 0));
    }
};

// Every reach whose water could reach inside this page, by ID. Reads the
// graph's page index over the page grown by the halo, so a worker touches a
// bounded number of index pages and never walks the world.
std::vector<RiverId> reachesAround(const HydrologyGraph& graph, TileKey key,
                                   std::int32_t haloMetres = kWaterHaloMetres);

// A water tile is made by BaseTileBaker::bakePage, in the same sweep as the
// ground it stands on. It used to be derived here from a finished BaseTile,
// which meant the water was read from the graph over ground the old carving
// had cut: two models, and they disagreed by thousands of samples on a lake
// page. One sweep is the whole point.

} // namespace world::streaming
