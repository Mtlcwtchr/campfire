#pragma once
// What an entity says about being drawn, and nothing about how.
//
// The rule this exists to enforce: an entity carries a HANDLE, never a
// renderer. A forest is a hundred thousand trees; if each one owned a mesh, a
// material and a draw, the frame would be a hundred thousand of everything and
// the geometry would be resident a hundred thousand times. Each tree carries
// where it stands and which asset it is - two numbers and a transform - and one
// instanced renderer resolves those once for the whole batch.
//
// The link back is the entity id. The gather keeps the owner of every instance
// it emits, so anything that wants the tree's own data - its species, its age,
// whatever a later system puts on it - goes through the id rather than through
// the renderer. Nothing in this header knows what a tree is.
#include <cstdint>

#include <entt/entity/registry.hpp>

namespace engine::render {

// The engine's entity handle, spelled here so a render system never has to
// reach into the ECS headers for one name.
using Entity = entt::entity;

// Where a drawable thing stands. Three dimensions and a turn: what an instanced
// renderer needs, in the units the world is measured in.
struct WorldTransform {
    float position[3]{0, 0, 0};
    float scale = 1;
    float yaw = 0;
};

// WHICH geometry and material. Ids into the renderer's catalogue, resolved once
// per batch rather than once per entity.
struct SmartMeshRef {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
    // Which of a shared asset's variations this instance is: a species tint, an
    // impostor azimuth, a palette row. The renderer reads it; the gather only
    // carries it.
    std::uint32_t variant = 0;
};

// Per-instance shading the gather passes straight through. Separate from the
// transform because a system that animates colour should not touch position.
struct RenderTint {
    float tint = 1;
    float phase = 0;
};

// Decided by a visibility system, read by the gather. The gather never culls:
// two systems that both decide what is visible would disagree on a frame where
// one of them ran and the other did not.
struct RenderVisible {
    bool value = true;
};

} // namespace engine::render
