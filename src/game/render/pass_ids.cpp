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
