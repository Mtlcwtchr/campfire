#pragma once
// A procedural object's name, worked out rather than stored.
//
// A tree the generator placed is never saved - the generator places it again
// every time the page is scattered - so the only way to say "this tree was
// cut" is to name it by where the generator put it, in terms that do not move
// when anything else does:
//
//   ObjectID = Hash(WorldSeed, Stage, StableDomain, PlacementPage, LocalCandidate)
//
// The page is the 128 m square the candidate lattice is numbered in, and the
// candidate is its slot in that page - not a position, which is a double and
// depends on whatever decided the jitter, and not a sequence number in a
// scatter, which depends on what else happened to be scattered with it. So the
// id is the same whether a page was scattered alone, in a batch or as the edge
// of a larger rectangle, in any session, on any machine.
//
// The stage keeps two generators that share a lattice apart: removing a tree
// must not remove the rock that another stage put on the same slot.
//
// The domain is the version of the LATTICE, not of the rules. Changing which
// candidates become trees, or how tall, leaves every id where it was and every
// removal still pointing at its slot. Changing where candidates are, or how
// they are numbered, is a new domain: every saved removal then names nothing,
// which is a migration and is reported as one when a world is opened.
#include <cstdint>

#include "engine/core/rng.hpp"

namespace world {

enum class PlacementStage : std::uint16_t {
    Decor = 1,           // trees, bushes, rocks, deadwood (scene_scatter)
    Cliff = 2,           // terrain-supported outcrop lattice (16 m cells)
    Undergrowth = 3,     // small clumps (4 m cells), separate from trees/removals
    Planted = 0x8001,    // placed by a person: added objects of the delta
};

// The decor lattice: one candidate per 8 m cell, numbered within 128 m pages.
inline constexpr std::uint32_t kDecorStableDomain = 1;
inline constexpr std::int64_t kPlacementPageMetres = 128;

constexpr std::uint64_t objectId(std::uint64_t worldSeed, PlacementStage stage, std::uint32_t domain,
                                 std::int64_t pageX, std::int64_t pageY, std::uint32_t localIndex) {
    std::uint64_t h = core::splitmix64(worldSeed ^ 0x6f626a6563746964ULL);   // "objectid"
    h = core::splitmix64(h ^ (std::uint64_t(stage) << 32 | domain));
    h = core::splitmix64(h ^ std::uint64_t(pageX));
    h = core::splitmix64(h ^ std::uint64_t(pageY));
    h = core::splitmix64(h ^ localIndex);
    // Nought means "no object" wherever an id is optional.
    return h ? h : 1;
}

} // namespace world

