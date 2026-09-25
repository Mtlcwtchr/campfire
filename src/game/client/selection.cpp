#include "game/client/selection.hpp"

#include <algorithm>

namespace client {

bool operator==(const Selection& a, const Selection& b) {
    if (a.kind != b.kind) return false;
    switch (a.kind) {
        case SelectionKind::None:     return true;
        case SelectionKind::Person:   return a.person == b.person;
        case SelectionKind::Animal:   return a.animal == b.animal;
        case SelectionKind::Building: return a.building == b.building;
        case SelectionKind::Resource: return a.node == b.node;
        case SelectionKind::Stack:    return a.stack == b.stack && a.stackGeneration == b.stackGeneration;
        case SelectionKind::Tile:     return a.tile == b.tile;
    }
    return false;
}

std::vector<Selection> candidatesAt(const sim::World& w, core::TilePos tile) {
    std::vector<Selection> out;
    if (!w.map().inBounds(tile)) return out;

    // People. A pawn's body is continuous, so anyone whose position falls on this
    // tile counts, ordered by id to keep repeated clicks predictable.
    for (const auto& p : w.people()) {
        if (!p.alive) continue;
        if (!(core::toTile(p.pos) == tile)) continue;
        Selection s;
        s.kind = SelectionKind::Person;
        s.person = p.id;
        s.tile = tile;
        out.push_back(s);
    }

    // Buildings are multi-tile, so this asks the footprint rather than the origin.
    for (const auto& b : w.buildings()) {
        if (!b.alive) continue;
        if (!b.covers(w.db(), tile)) continue;
        Selection s;
        s.kind = SelectionKind::Building;
        s.building = b.id;
        s.tile = tile;
        out.push_back(s);
    }

    const auto& t = w.map().at(tile);
    if (t.node.valid() && w.node(t.node).alive) {
        Selection s;
        s.kind = SelectionKind::Resource;
        s.node = t.node;
        s.tile = tile;
        out.push_back(s);
    }

    // Batches on the ground here, plus anything stored in a building that covers
    // this tile - clicking a storage pit should show what is inside it.
    for (const auto& st : w.stacks()) {
        if (!st.alive || st.count <= 0) continue;
        bool here = false;
        if (st.where == sim::StackWhere::Ground) {
            here = st.tile == tile;
        } else if (st.where == sim::StackWhere::InBuilding && st.building.valid()) {
            const auto& b = w.building(st.building);
            here = b.alive && b.covers(w.db(), tile);
        }
        if (!here) continue;
        Selection s;
        s.kind = SelectionKind::Stack;
        s.stack = st.id;
        s.stackGeneration = st.generation;
        s.tile = tile;
        out.push_back(s);
    }

    // Livestock standing here.
    for (const auto& a : w.animals()) {
        if (!a.alive || !(core::toTile(a.pos) == tile)) continue;
        Selection s;
        s.kind = SelectionKind::Animal;
        s.animal = a.id;
        s.tile = tile;
        out.push_back(s);
    }

    // The ground is always the last thing under everything else.
    Selection ground;
    ground.kind = SelectionKind::Tile;
    ground.tile = tile;
    out.push_back(ground);
    return out;
}

Selection pickAt(const sim::World& w, core::TilePos tile, const Selection& current) {
    const auto candidates = candidatesAt(w, tile);
    if (candidates.empty()) return {};

    // Clicking the same spot again moves to the next thing there.
    auto it = std::find(candidates.begin(), candidates.end(), current);
    if (it != candidates.end()) {
        ++it;
        return it == candidates.end() ? candidates.front() : *it;
    }
    return candidates.front();
}

bool stillExists(const sim::World& w, const Selection& sel) {
    switch (sel.kind) {
        case SelectionKind::None:
            return false;
        case SelectionKind::Person:
            return sel.person.valid() && sel.person.value < w.people().size() &&
                   w.person(sel.person).alive;
        case SelectionKind::Animal:
            return sel.animal.valid() && sel.animal.value < w.animals().size() &&
                   w.animal(sel.animal).alive;
        case SelectionKind::Building:
            return sel.building.valid() && sel.building.value < w.buildings().size() &&
                   w.building(sel.building).alive;
        case SelectionKind::Resource:
            return sel.node.valid() && sel.node.value < w.nodes().size() && w.node(sel.node).alive;
        case SelectionKind::Stack:
            return w.stackStillIs(sel.stack, sel.stackGeneration) && w.stack(sel.stack).count > 0;
        case SelectionKind::Tile:
            return w.map().inBounds(sel.tile);
    }
    return false;
}

} // namespace client
