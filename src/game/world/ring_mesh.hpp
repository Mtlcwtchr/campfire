#pragma once
// What is left of the annulus: the foliage it scattered, and the height bounds
// a view is culled against.
//
// The mesh itself has gone. It was an annulus around a snapshot of the camera,
// which meant no vertex of one level stood where a vertex of the level above
// stood - so a change of level had to be a dissolve rather than a morph - and
// that a step sideways renamed every piece of ground, so the whole view was
// built again from nothing. Square tiles named by the ground they cover
// replace it: see tile_mesh.hpp.
//
// The foliage scatter stays because it still runs on the processor, walking
// the triangles of a finished mesh. It belongs on the card, from a density
// field, and that is a change of its own.

#include <functional>
#include <vector>

#include "game/world/terrain_mesh.hpp"
#include "game/world/terrain_adaptive.hpp"

namespace generation { struct WorldMapData; }

namespace world {

inline constexpr int kGrassCell = 2, kGrassRegion = 64, kGrassWindow = 256;
inline constexpr std::size_t kGrassCandidateBudget = 65536;
// Cards a fine root draws near the eye (PageGrassVS): a patch of turf, the
// extra ones scattered within its cell. Read from the quad, not the root.
inline constexpr int kTurfCards = 4;
inline constexpr float kTurfReach = 0.8f;   // metres off the root, at most
// Barycentric heights on actual rendered triangles, including stitched edges.
// The shader follows the same source/parent/stage morph as the terrain.
struct PageGrassRoot {
    float position[3];
    float parentHeight, priorHeight, priorParentHeight, upright;
    // Which run of candidates this belongs to, so one draw can cover them all.
    float run = 0;
    bool operator==(const PageGrassRoot&) const = default;
};
std::vector<PageGrassRoot> buildPageGrass(const terrain::AdaptiveMesh& mesh,
    double originX, double originY, int windowX, int windowY,
    int cellMetres=kGrassCell, int halfWindowMetres=kGrassWindow);

// Conservative bounds for terrain AND its water surface in a world rectangle.
// Includes interpolation, detail, channel and coordinate-warp margins.
std::pair<double, double> ringHeightBounds(const generation::WorldMapData& world,
                                          core::WorldPos min, core::WorldPos max,
                                          std::int32_t lod);

// Presentation data built by a worker; independent of the GPU API.
struct RingFoliage {
    float position[3];
    float scale;
    float tint[4];
    float phase;
    float variant;
    float climate[4]; // thermal index, moisture, drainage, source ground height
};
std::vector<RingFoliage> buildRingFoliage(const TerrainMesh& mesh,
                                        const std::vector<core::Fixed>& waterLevel,
                                        const std::function<bool()>& cancelled = {});

} // namespace world
