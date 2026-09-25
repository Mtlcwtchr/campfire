#pragma once
// What the player currently has selected, and how a click turns into one.
//
// Everything in the world is inspectable: a person, a building, a standing tree,
// a pile of grain, or the bare ground itself. A click never comes back empty -
// at worst it selects the tile, which still has terrain, fertility, pollution and
// whatever zones cover it.

#include <cstdint>
#include <vector>

#include "game/simulation/world.hpp"

namespace client {

enum class SelectionKind : std::uint8_t { None, Person, Animal, Building, Resource, Stack, Tile };

struct Selection {
    SelectionKind kind = SelectionKind::None;
    core::TilePos tile;

    core::PersonId person;
    core::AnimalId animal;
    core::BuildingId building;
    core::ResourceNodeId node;
    core::ItemStackId stack;
    // Batch slots are recycled, so a stored handle needs its generation to prove
    // it still means the batch the player clicked (see DECISIONS.md D16).
    std::uint32_t stackGeneration = 0;

    bool valid() const { return kind != SelectionKind::None; }
    friend bool operator==(const Selection& a, const Selection& b);
};

// Everything worth inspecting on a tile, in the order a player expects to reach
// it: whoever is standing there, then what is built there, then what grows there,
// then what is piled there, then the ground.
std::vector<Selection> candidatesAt(const sim::World& w, core::TilePos tile);

// A click. Clicking the same tile again walks through whatever else is stacked on
// it, so a person standing on a stockpile tile never hides the pile underneath.
Selection pickAt(const sim::World& w, core::TilePos tile, const Selection& current);

// Selections outlive the things they point at: people die, sites are abandoned,
// batches are eaten. Returns false when the selection no longer refers to
// anything, so the caller can clear it.
bool stillExists(const sim::World& w, const Selection& sel);

} // namespace client
