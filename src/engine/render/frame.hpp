#pragma once
// What every pipeline and every pass is told about the frame it is working on.
//
// Assembled once by the runner and handed down. Nothing in here is fetched a
// second time by anybody: two passes that each asked the window how big it was
// could get two different answers on the frame the window is being resized, and
// half a picture would be drawn to the wrong scale.

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>

namespace engine {

class Device;
class InstanceArena;

// The constant buffer at b0, space1, that every shader in a frame reads. The
// field order is the shader's; this struct and the cbuffer in the HLSL have to
// change together, and there is no reflection keeping them honest.
//
// It is engine-shaped on purpose: a camera, a viewport, and one spare vector
// the game fills with whatever its shaders need. Putting a game's ideas in here
// would mean the engine could not draw anything else.
// How many four-float vectors an editor's overlay has (Scene::editor).
inline constexpr int kSceneEditorVectors = 35;

struct Scene {
    // World metres to clip space, row by row. The one thing every shader that
    // draws anything in the world needs, and the only thing any of them needs to
    // know about the camera.
    //
    // A matrix rather than the projection written out in the shader, which is
    // what it was: a 2:1 dimetric, by hand, in the one place every shader
    // included. Written out, the projection belongs to the shader; as a matrix
    // it belongs to the camera, and a shader draws whatever the camera says -
    // dimetric today, and a tipped or perspective view the day somebody wants
    // one, without a line changing in terrain, water, grass or sprites. That is
    // the whole of what "ready for 3d" amounts to, and it costs nothing: the
    // same sixteen numbers uploaded once a frame, against eleven multiplies a
    // vertex for the formula.
    float viewProjection[16]{};
    float camera[4]{};      // centre x, centre y, focus height, pixels per metre
    float viewport[4]{};    // width, height, seconds since start, spare
    float extra[4]{};       // the game's own four numbers
    // Which way the wind is blowing and how hard.
    //
    // In the scene rather than worked out in each shader, because grass, water
    // and anything else that moves have to agree about it: wind that blows one
    // way through the grass and another across the water is two weathers in one
    // picture. The engine does not decide what it is - the game does, once a
    // frame - it only carries it.
    float wind[4]{};        // direction x, direction y, strength, gust
    // Four numbers a game may keep per material of its ground, or per anything
    // else it has eight of. The engine neither reads nor understands them; it
    // carries them because the alternative is a second uniform block pushed
    // alongside this one for every draw.
    //
    // What the game puts here is written where it fills them in (sceneFor) and
    // where its shaders read them (terrain.hlsl).
    float table[8][4]{};
    // Game-defined weather, season and inspection parameters. Kept opaque to
    // the engine and mirrored by world.hlsli's SceneVertex/ScenePixel blocks.
    float parameters[21][4]{};
    // Up to eight spatial vegetation-density regions selected by the instance
    // hierarchy for this frame: centre x/y, radius, replacement weight. This
    // is still payload rather than engine policy; terrain is merely one
    // consumer of the game's explicit far representation.
    float vegetationDensity[8][4]{};
    float shadowSun[4]{}; // towards the light, enabled
    float shadowClip[4][4]{}; // world origin xyz, extent (zero = unavailable)
    // Distance fog: opaque at x metres (the draw distance), starting at y
    // metres, enabled when z > 0.5. Measured from the eye; perspective only.
    float fog[4]{};
    // Lighting: sun intensity, sky/ambient intensity, exposure, provided (1).
    // Zero `w` means "not set" and shaders keep their built-in look.
    float look[4]{};
    // The grade's own dials: brightness, contrast, saturation (1 = the built-in
    // look), and w = 1 when they are set.
    float grading[4]{};
    // Sky: horizon rgb + skybox enabled, zenith rgb + panorama rotation (rad).
    // Without a skybox the horizon/zenith colours are still the fog colour.
    float skyHorizon[4]{};
    float skyZenith[4]{};
    // Volumetric clouds: coverage 0..1, density, base altitude (m), ray steps (0 = off).
    float clouds[4]{};
    // Quality knobs read by shaders: terrain material blend (m), shadow
    // softness multiplier, spare, spare. Zero means the built-in default.
    float quality[4]{};
    // The camera every DECISION is made from: culling, LOD, streaming,
    // placement. Normally the drawing camera; frozen by the scene view so the
    // world can be inspected from elsewhere without being recomputed.
    // cullState: x = these are set, y = frozen (drawing camera differs).
    float cullViewProjection[16]{};
    float cullCamera[4]{};
    float cullState[4]{};
    // An editor's overlay on the world: game-defined payload like `parameters`
    // (the game's world editor draws its region grid, selection and brush from
    // it). All zero when nothing is being edited.
    float editor[kSceneEditorVectors][4]{};
    // How the ground's textures are laid, game-defined like `parameters`
    // (content/config/terrain_look.json, terrain_material.hlsli): tile size
    // multiplier, distance tiling octaves, the texels a pixel at which the
    // next octave starts, macro variation. All zero: the shader's defaults.
    float terrainLook[4]{};
    // World-anchored forcing for long water waves, independent of eye position.
    float swellWind[4]{};
};

// What CPU culling/LOD code must read instead of the drawing camera.
inline const float* cullMatrix(const Scene& s) { return s.cullState[0] > 0.5f ? s.cullViewProjection : s.viewProjection; }
inline const float* cullEye(const Scene& s) { return s.cullState[0] > 0.5f ? s.cullCamera : s.camera; }

struct Frame {
    Device* device = nullptr;
    SDL_GPUCommandBuffer* commands = nullptr;
    // Where a pass puts the copies of whatever it is drawing. One arena for the
    // whole frame: a pass appends and is told the byte offset, the pipeline
    // uploads the lot once, and every instanced draw in the frame - a crowd of
    // cards, a field of grass, a street of carts - comes out of the same buffer.
    InstanceArena* instances = nullptr;
    // Where a pass puts the arguments of an indirect draw it decided this
    // frame. Same arena, different usage: a list that changes every frame and
    // must not be a buffer created every frame.
    InstanceArena* arguments = nullptr;
    // Live only inside a stage. Null while the calculations pipeline runs,
    // which is the point: a copy in the middle of a render pass is a frame
    // waiting on itself.
    SDL_GPURenderPass* pass = nullptr;
    SDL_GPUTexture* colour = nullptr;
    SDL_GPUTexture* depth = nullptr;
    // Where the multisampled colour is folded to one sample a pixel at the end
    // of a stage. Null when there is no multisampling.
    SDL_GPUTexture* resolve = nullptr;
    // A one-sample copy of the picture as it stood before a stage that asked
    // for it (StageInfo::grabColour). Sampled, never drawn into. Its alpha is
    // the encoded view distance the opaque passes wrote (scene_depth.hlsli).
    SDL_GPUTexture* grab = nullptr;
    Uint32 width = 0, height = 0;
    std::uint64_t index = 0;
    double seconds = 0;       // since the run began
    double step = 0;          // since the last frame, or the fixed step in a fixed pipeline
    // How far the world is between the tick it has finished and the one it has
    // not started: nought just after a tick, approaching one just before the
    // next. What anything drawing a moving body interpolates by, so a pawn
    // walking at ten ticks a second is not drawn in ten places a second.
    float alpha = 0;
    Scene scene;

    // What this frame actually asked the GPU to draw. Counted where the draws
    // are issued, so it is the truth and not an estimate - and it is the only
    // honest number available here: what a rasteriser then throws away to back
    // faces is its own business and SDL's GPU interface does not report it.
    struct Work {
        std::uint64_t triangles = 0;   // from draws whose counts are known here
        std::uint32_t draws = 0;
        std::uint32_t indirectDraws = 0;
        std::uint32_t unknownIndirectDraws = 0; // GPU-authored counts, not read back
        // What the scatter offered and what was thrown away before it cost
        // anything. Filled by whoever culls; nought means nobody reported, not
        // that nothing was culled.
        std::uint32_t instances = 0;      // survived everything and were drawn
        std::uint32_t culledFrustum = 0;  // behind the eye or off the sides
        std::uint32_t culledHorizon = 0;  // in view, with ground in front of it
        // How many ground squares were drawn at each level, finest first. The
        // one number that says whether the cut is starting coarse and refining
        // or starting fine and hoping: a view of a whole world should be a
        // handful of squares at the coarse end and nothing at the fine one.
        std::array<std::uint32_t, 12> groundByLevel{};
        std::array<std::uint64_t, 12> groundTrianglesByLevel{};
        // Triangles by whoever queued the draw - see DrawItem::author.
        std::array<std::uint64_t, 8> trianglesByAuthor{};
        std::array<std::uint32_t, 8> drawsByAuthor{};
    };
    Work* work = nullptr;
};

} // namespace engine
