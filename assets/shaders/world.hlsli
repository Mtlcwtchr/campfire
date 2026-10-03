// What every shader that draws the world needs: where the eye is, and how a
// point in the world becomes a point on the screen.
//
// One copy, included by the rest. The field order here is the Scene struct's in
// engine/render/frame.hpp, and the two have to change together - there is no
// reflection keeping them honest.
#ifndef WORLD_HLSLI
#define WORLD_HLSLI

// The same three vectors, twice, because the two stages do not share uniforms.
//
// SDL_GPU gives the vertex stage and the fragment stage their own sets of
// constant buffers - space1 and space3 - and pushing to one does not fill the
// other. Declaring it once at space1 and reading it from a pixel shader
// compiles, binds nothing, and returns zeros: the vertex stage had the camera
// and the projection worked, while every pixel shader in the world divided the
// texture coordinate by a metres-per-turn of nought. That is why the ground has
// been a flat wash since the day it moved onto the card, and why nothing done to
// the material textures changed it.
//
// One struct on the C++ side still fills both; only the binding differs.
// Row major, because that is the order the sixteen numbers are written in on
// the other side of the wire. HLSL packs a matrix in a constant buffer column by
// column unless told otherwise, and a transposed projection is not a wrong
// picture, it is no picture at all.
cbuffer SceneVertex : register(b0, space1)
{
    row_major float4x4 viewProjection;   // world metres to clip space
    float4 camera;      // centre x, centre y, focus height, pixels per metre
    float4 viewport;    // width, height, seconds since start, metres to a turn
    float4 extra;       // depth centre, one over the depth span, foliage show, spare
    float4 wind;        // direction x, direction y, strength, gust
    float4 table[8];    // four numbers a material of the ground keeps
    float4 parameters[21];
    float4 vegetationDensity[8]; // x/y/radius/weight, selected far instance regions
    float4 shadowSun;
    float4 shadowClip[4];
    float4 fog;         // opaque distance, start distance, enabled, spare
    float4 look;        // sun, ambient, exposure, provided
    float4 grading;     // brightness, contrast, saturation, provided
    float4 skyHorizon;  // rgb, skybox enabled
    float4 skyZenith;   // rgb, panorama rotation
    float4 clouds;      // coverage, density, base altitude, steps
    float4 quality;     // terrain blend metres, shadow softness, spare, spare
    row_major float4x4 cullViewProjection;
    float4 cullCamera;
    float4 cullState;
    float4 editor[35];   // world editor overlay (editor_overlay.hlsli); kSceneEditorVectors
    float4 terrainLook;  // tile size, distance octaves, octave start, macro variation (terrain_material.hlsli)
    float4 swellWind;    // world-anchored swell direction xy, strength, gust
};

cbuffer ScenePixel : register(b0, space3)
{
    row_major float4x4 viewProjectionPS;
    float4 cameraPS;
    float4 viewportPS;
    float4 extraPS;
    float4 windPS;
    float4 tablePS[8];
    float4 parametersPS[21];
    float4 vegetationDensityPS[8];
    float4 shadowSunPS;
    float4 shadowClipPS[4];
    float4 fogPS;
    float4 lookPS;
    float4 gradingPS;
    float4 skyHorizonPS;
    float4 skyZenithPS;
    float4 cloudsPS;
    float4 qualityPS;
    row_major float4x4 cullViewProjectionPS;
    float4 cullCameraPS;
    float4 cullStatePS;
    float4 editorPS[35];
    float4 terrainLookPS;
    float4 swellWindPS;
};

// Where a point of the world lands on the screen.
//
// One matrix multiply, and the camera decides what the matrix is (Camera::
// viewProjection). This used to be the 2:1 dimetric written out by hand here,
// which meant every shader that drew anything was a shader that knew what
// projection the game used - and the day the view tips over or gains
// perspective, all of them are wrong together.
float4 project(float3 p)
{
    // Keep depth linear through rasterization. Clamping a far skirt vertex to
    // 1 here bends the whole triangle's interpolated depth towards the camera:
    // a buried seam wall then passes the depth test in front of a river valley.
    // Device::makePipeline enables hardware depth clamp (not depth clipping),
    // which preserves distant ground without corrupting the triangle plane.
    return mul(viewProjection, float4(p, 1.0));
}

// The same, pushed away from the eye.
//
// For ground that is only standing in until the sharp level is cut. It has to
// write depth - otherwise nothing can be drawn over it that needs sorting, and
// water and grass have to be left off it entirely, which is what made a change
// of level look like the whole world reloading. But its height is an average, so
// in a valley it sits above the real ground and would win the depth test against
// the very thing meant to replace it. Pushed back by a good forty metres of
// world height, it loses to anything real in the same place and still sorts
// correctly against other backdrops.
float4 projectBehind(float3 p)
{
    float4 at = project(p);
    at.z -= 80.0 * extra.y;   // reversed depth: farther is smaller
    return at;
}

#endif
