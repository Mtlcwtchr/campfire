#pragma once
// Dropping what the camera cannot see, between the gather and the selection.
//
// The gather deliberately does not cull: it reads a visibility component and
// leaves the decision to whoever owns it. This is that owner for the one kind
// of visibility no game has an opinion about - whether a thing is inside the
// view frustum at all.
//
// It sits between the gather and the level selection because the order matters
// in both directions. Culling first means the selection never pays for an
// object nobody sees, and - more importantly - the budget that the selection
// feeds is computed over what is actually drawn. Culling after selection would
// let a forest behind the camera decide how coarsely the forest in front of it
// is drawn.
//
// The test is the exact one: an instance is out when its bounding sphere lies
// wholly outside a clip plane. Comparing a projected point against a widened
// screen box - which is the shortcut this replaces - forgets that the depth it
// divides by moves across the sphere too, and so clips things at the edge of a
// wide field of view.
#include <cstddef>
#include <span>

#include "engine/render/systems/horizon_cull.hpp"
#include "engine/render/systems/instance_gather.hpp"
#include "engine/render/systems/level_select.hpp"

namespace engine::render {

struct CulledInstances {
    // What survived, in the same shape the gather produced, so the selection
    // takes it unchanged.
    GatheredInstances kept;
    std::size_t behind = 0;   // failed the near plane
    std::size_t outside = 0;  // failed one of the four side planes
    std::size_t hidden = 0;   // inside the view, but with ground in front of it

    [[nodiscard]] std::size_t removed() const { return behind + outside + hidden; }
};

// `assets` is indexed by mesh id - the same catalogue the level selection
// reads, so the two systems cannot disagree about a model's shape. A mesh with
// no entry is treated as a point, which keeps an undescribed asset visible
// where it is rather than everywhere.
// `horizon`, when given, is the skyline the ground makes from the eye - see
// horizon_cull.hpp. Passing nothing culls on the frustum alone, which is what
// this did before there was anything to ask about occlusion.
[[nodiscard]] CulledInstances cullToFrustum(const GatheredInstances& gathered,
                                            std::span<const MeshDescription> assets,
                                            const ScreenScale& screen,
                                            const Horizon* horizon = nullptr);

} // namespace engine::render
