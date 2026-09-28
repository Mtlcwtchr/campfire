#pragma once
// What this game's passes and pipelines are called.
//
// The engine takes these as opaque integers: it sorts, looks up and profiles by
// them and never asks what they mean. Naming them is the game's business, and
// this is the only file that does it - which is the whole reason the engine
// stopped carrying a string on every pass.

#include "engine/pipeline/ids.hpp"

namespace game {

enum class Pass : engine::PassId {
    // Working out what the frame contains.
    Stream,        // patches of ground in and out of memory
    Collect,       // world into vertices, vertices onto the card
    Control,       // control-map generation on GPU
    // Drawing it.
    Terrain,
    Foliage,
    Water,
    Weather,
    Sprite,        // everything drawn as a card: pawns, goods, trees, markers
    Menu,
    Models,
    Sky,           // what the distance fog fades into, behind everything
    Frustum,       // the scene view's frozen cull camera
    FarTrees,      // GPU-scattered tree impostors from the object reach to the horizon
    Grade,         // the finished picture's colour: warmth, bloom, print curve, grain
};

// One render pass on the card apiece. The world is drawn into a cleared target
// with depth; the interface will go over it with the depth turned off.
enum class Stage : engine::StageId {
    World = 0,
    // Everything drawn OVER the finished opaque world: water first (it reads
    // a copy of that world - its bed through it, its shores in it), then the
    // blended cards, weather and anything else that is part of the picture.
    Surface = 1,
    // The whole picture, reread and regraded: it needs a copy of everything
    // above, which is only to be had between render passes.
    Post = 2,
    // What is being said to the person. After the grade, because it is not
    // part of the picture and must not be tinted as if it were.
    Interface = 3,
};

// Where a pass sits inside its stage. Ground and grass may be interleaved by the
// batcher - they are opaque and depth sorts them - but water is blended and has
// to come after everything it is over.
enum class Order : engine::PassOrder {
    Opaque = 0,
    // The sky: after every opaque pass (it fills only what they left empty),
    // before blended water and cards that must be over it.
    Sky = 150,
    Blended = 200,
    // Over everything, because it is a curtain rather than a thing in the world.
    Cover = 250,
    // And over that: what is being said to the person, which is not part of the
    // picture at all and is never hidden by anything in it.
    Interface = 254,
};

// The phases of a frame, in the order the runner runs them.
//
// World is the simulation and is the only one on a fixed step. Update is
// everything that looks at the world without deciding it. Prepare turns what is
// there into vertices and puts them on the card. Render draws.
enum class Phase : engine::PipelineId {
    World,
    Update,
    Prepare,
    Render,
};

const char* nameOf(Pass pass);
const char* nameOf(Phase phase);

} // namespace game
