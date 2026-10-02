#include "game/render/pass_ids.hpp"

namespace game {

const char* nameOf(Pass pass) {
    switch (pass) {
        case Pass::Stream:  return "stream";
        case Pass::Collect: return "collect";
        case Pass::Control: return "control";
        case Pass::Terrain: return "terrain";
        case Pass::Foliage: return "foliage";
        case Pass::Water:   return "water";
        case Pass::Weather: return "weather";
        case Pass::Sprite:   return "sprite";
        case Pass::Menu:     return "menu";
        case Pass::Models:   return "models";
        case Pass::Sky:      return "sky";
        case Pass::Frustum:  return "frustum";
        case Pass::FarTrees: return "far-trees";
        case Pass::Grade:    return "grade";
        case Pass::Hold:     return "hold";
        case Pass::Sketch:   return "sketch";
        case Pass::Highlight: return "highlight";
    }
    return "?";
}

const char* nameOf(Phase phase) {
    switch (phase) {
        case Phase::World:   return "world";
        case Phase::Update:  return "update";
        case Phase::Prepare: return "prepare";
        case Phase::Render:  return "render";
    }
    return "?";
}

} // namespace game
