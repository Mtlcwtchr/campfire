#pragma once
#include "engine/render/hemisphere_impostor.hpp"
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace engine::render {
using ImpostorPixel=std::array<std::uint8_t,4>;
struct ImpostorAtlas {
    unsigned resolution=0;
    camera::Vec3 centre{}; // metadata only; pixels describe positions relative to centre
    double side=0,error=0;
    unsigned members=0,levels=0;
    // Measured screen-plane error (metres) of showing this atlas, reprojected
    // from its nearest baked views, instead of what it was baked from - over
    // the worst directions between views. Negative: never measured (a leaf
    // baked offline); callers then use the analytic angular bound.
    double viewError=-1;
    std::vector<ImpostorPixel> colour,normal,depth; // 21 tightly packed layers
    std::vector<std::vector<ImpostorPixel>> colourMips,normalMips; // levels 1..N
    bool valid() const;
    std::size_t bytes() const;
};
struct ImpostorPlacement {
    std::shared_ptr<const ImpostorAtlas> atlas;
    camera::Vec3 centre{}; // centre in the parent's common coordinate system
    double scale=1,yaw=0,tint=1;
};
struct ImpostorBakeOptions {
    unsigned resolution=64,leafMembers=4,maxMembers=64,maxDepth=3;
};
// Alpha-weighted colour/normal mip chain down to 1x1 (packed depth has none).
void buildImpostorMips(ImpostorAtlas& atlas);
// Never drops a member to meet the budget. Missing/malformed input or cancellation
// returns null, leaving replacement ownership with the original children.
std::shared_ptr<const ImpostorAtlas> bakeRecursiveImpostor(
    std::span<const ImpostorPlacement> members,const ImpostorBakeOptions& options={},
    const std::atomic_bool* cancel=nullptr);
} // namespace engine::render
