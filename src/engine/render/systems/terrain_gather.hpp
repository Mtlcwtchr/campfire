#pragma once
// The system that turns a cut of the ground into what a frame draws.
//
// The ground arrives as a set of squares at mixed levels - whatever the
// streaming quadtree settled on this frame. Four passes want it: the surface,
// the water on it, the grass growing out of it, and the trees that fade into
// impostors over it. Today each of them walks the cut again and re-derives the
// same numbers, which is three answers too many and, worse, three places for
// them to disagree.
//
// So the walk happens once. What comes out is the same cut, ordered, grouped by
// level, measured against the screen, and with the water squares listed. Every
// pass reads it; none of them re-derives anything.
//
// Two things this deliberately does NOT do:
//
// - It does not cull. The cut it is handed was already chosen against the
//   camera by whoever built it; culling again would be a second opinion about
//   the same question, and the two would disagree on the frame where only one
//   of them ran.
// - It does not know where heights come from, what a page is, or what a buffer
//   is. A patch carries the caller's own index and nothing else, so the
//   resources stay with whoever owns them.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/systems/level_select.hpp"

namespace engine::render {

// One square of ground, as the renderer needs to think about it.
struct TerrainPatch {
    // The corner it starts at and the side it spans, in world metres.
    double originX = 0, originY = 0, metres = 1;
    // The height range this square actually occupies. Its middle is where the
    // patch is measured against the screen, because measuring a mountain at sea
    // level says a summit is as far away as the water beside it.
    float lowZ = 0, highZ = 0;
    // 0 is the finest. Levels are what the cut is grouped by, because a level
    // is what a change of pipeline state usually follows.
    std::uint32_t level = 0;
    // How far along the way to the next level up this square stands, for the
    // vertex stage to walk. Carried, never interpreted.
    float morph = 0;
    bool water = false;
    // The caller's own index for this patch: which buffers it lives in, which
    // page it came from, whatever the caller keeps beside it. Carried, never
    // read - that is what keeps this header free of everything.
    std::uint32_t source = 0;
};

// A run of patches at one level, which is what one set of pipeline state covers.
struct TerrainRun {
    std::uint32_t level = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

struct GatheredTerrain {
    // Coarsest level first, and inside a level in a fixed sweep. The cut's own
    // order is an artefact of the quadtree walk, and two machines that walked
    // it differently would submit different frames for the same view.
    std::vector<TerrainPatch> patches;
    std::vector<TerrainRun> levels;
    // Parallel to `patches`: how many pixels one metre covers at this patch's
    // middle. The grass and the tree impostors both ask this, and asking it
    // twice is how they came to disagree about where the crossover was.
    std::vector<float> pixelsPerMetre;
    // Indices into `patches` of the squares that carry water, in the same
    // order. A list rather than a flag to re-test, because the water pass wants
    // to iterate them and nothing else.
    std::vector<std::uint32_t> water;
    // How much ground is covered, in square metres, and how coarse it is on
    // average: a cut that is spending its budget far from the camera says so
    // here rather than in a frame time.
    double area = 0;
    // Squares with no extent, or one that is not a finite number. Counted
    // rather than drawn, and counted rather than ignored.
    std::size_t rejected = 0;

    [[nodiscard]] std::size_t drawn() const { return patches.size(); }
};

[[nodiscard]] GatheredTerrain gatherTerrain(std::span<const TerrainPatch> cut,
                                            const ScreenScale& screen);

} // namespace engine::render
