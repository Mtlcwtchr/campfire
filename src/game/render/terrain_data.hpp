#pragma once
// What the ground looks like once it is on the card.
//
// These are the vertex and instance layouts, and nothing else: no world types,
// no simulation types, no streaming. A collector somewhere else fills arrays of
// these out of whatever the world happens to be made of, and the passes draw
// them. That is the whole seam - it is why the ground can come from a patch
// cache today and from an ECS query tomorrow without a pass changing a line.
//
// The field order is the vertex attribute order in the pipeline, and the
// shader's input struct has to match both.

#include <cstdint>

namespace game {

struct TerrainVertexGpu {
    float position[3];    // world metres, z up
    float normal[3];
    float weights0[4];    // grass, dirt, sand, rock
    float weights1[2];    // marsh, snow
    float uv[2];          // world metres again: the material tiles in world space
    float waterHeight;    // where the water stands over this vertex
    float waterMotion[4]; // flow xy, river and lake weights
    float waterCover;     // how much of this vertex's own footprint is under it
    // The same point on the next level up's surface, and its normal there. The
    // vertex shader walks from here to there as the zoom crosses the level, so
    // that by the moment the coarser mesh takes over, this one is already
    // drawing exactly what it draws.
    float morphHeight;
    float morphNormal[3];
    // And where the material sits on it up there.
    //
    // A turn of the material is scaled with the level - it has to be, or a
    // coarse patch samples its texture a thousand times too fast and comes out
    // one flat colour - so two levels lay the same ground out at two sizes, and
    // that, measured, is what a change of level actually looks like. The shape
    // barely moves; the ground underfoot doubles in size. So the coordinate
    // walks across with the geometry.
    float morphUv[2];
    // What is left of the eleven climate channels this vertex used to carry.
    //
    // Foliage, desert cover, temperature, fertility, moisture, wind and
    // drainage were fifty-two bytes on every corner of every tile at every
    // level - for a field whose samples are sixty-four metres apart and which
    // never changes once the map is raised. On a four-metre mesh that is the
    // same answer written out two hundred and fifty-six times. They are three
    // pictures the vertex stage samples now (climate_field.hlsli).
    //
    // These two stayed because they are not climate. Exposure is this point
    // against its upwind skyline and openness is it against the ground around
    // it, so both are properties of the height at this level - a coarse tile
    // has a smoother skyline than a fine one and must.
    float relief[2];   // wind exposure, openness
};

struct FoliageVertexGpu {
    float corner[2];
    float uv[2];
};

struct FoliageInstanceGpu {
    float position[3];
    float scale;
    float tint[4];
    float phase;      // where in its sway this blade is, so a field is not one blade
    float variant;    // which card of the array
    float climate[4]; // thermal index, moisture, drainage, source ground height
};

} // namespace game
