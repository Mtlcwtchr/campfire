#pragma once
// Choosing how much geometry an instance is worth, and splitting a frame by it.
//
// The rule lives here rather than in a game, and rather than twice, because the
// GPU cluster culler applies the same one: a representation is drawn when its
// own projected error fits the allowance and the coarser one's does not. A CPU
// selection and a GPU selection that disagreed would show as objects changing
// detail when the frame merely changed how it was submitted.
//
// This is the CPU half. It takes what the gather produced, asks the rule once
// per instance, and regroups the batches so each one is a single (mesh,
// material, level) - which is exactly what one instanced draw of one level of a
// chain covers. The owning entity still travels beside every instance.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/render/level_of_detail.hpp"
#include "engine/render/systems/instance_gather.hpp"

namespace engine::render {

// How much room one mesh asset needs around its anchor, in model units. The
// instance's own scale multiplies both, so a sapling is neither culled nor
// measured with a grown tree's reach.
struct MeshBounds {
    // Contains the whole model when centred `rise` above the anchor.
    double radius = 0;
    // World up is +z here, as the rest of the renderer keeps it. A model that
    // stands on its anchor has its middle half its height above it.
    double rise = 0;
};

// What one mesh asset is: its chain of measured errors, how large it is, and
// the room it takes. One description rather than one per system, because the
// cull and the selection have to agree about where a thing is - a cull that
// measured the trunk while the selection measured the crown would keep and
// then mis-size the same tree.
//
// The renderer owns these; a system is handed them rather than looking them up,
// so the same selection can be run against a test catalogue.
struct MeshDescription {
    std::span<const float> errors;
    double extent = 1;
    // Levels the renderer can actually draw right now. A chain whose finer
    // pages have not arrived selects the coarsest level it HAS rather than one
    // it does not, which is the same fallback the terrain pages already use.
    std::size_t resident = 0;
    MeshBounds bounds;
};

// Where the camera is and how it turns metres into pixels.
struct ScreenScale {
    // Rows 0, 1 and 3 of the view-projection, row-major, as the rest of the
    // renderer keeps them.
    float rowX[4]{};
    float rowY[4]{};
    float rowW[4]{};
    // Zero focal says the frame is orthographic and `scale` already is pixels
    // per metre; otherwise pixels per metre is focal over depth.
    double focal = 0;
    double scale = 1;
    double allowance = kLevelPixelError;

    // How many pixels one metre covers at this point, and how deep it is.
    [[nodiscard]] double pixelsPerMetreAt(const float position[3], double& depth) const;
};

// A batch after selection: one asset, one material, one level.
struct LevelBatch {
    std::uint32_t mesh = 0;
    std::uint32_t material = 0;
    std::uint32_t level = 0;
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

struct SelectedLevels {
    std::vector<LevelBatch> batches;
    std::vector<GatheredInstance> instances;
    std::vector<Entity> owners;
    // Instances the camera cannot see at all. Counted rather than merged into
    // the coarsest level, because "behind the eye" and "very far away" are
    // different answers and a renderer that confuses them draws the horizon
    // twice.
    std::size_t behind = 0;
    [[nodiscard]] std::size_t drawn() const { return instances.size(); }
};

// `levels` is indexed by the mesh id the gather emitted. A mesh with no entry,
// or an empty chain, draws at level zero - an asset nobody described is not an
// asset nobody draws.
[[nodiscard]] SelectedLevels selectLevels(const GatheredInstances& gathered,
                                          std::span<const MeshDescription> assets,
                                          const ScreenScale& screen);

} // namespace engine::render
