#pragma once
// A sprite, as the card sees it: one quad and a list of where to put copies of
// it.
//
// This is the whole of "a sprite is a mesh renderer whose mesh is a quad". The
// geometry is four corners, uploaded once for the life of the program; every
// pawn, barrel, tree and marker in a frame is an entry in a list, and the shader
// turns each entry into a card of the right size, in the right place, showing
// the right picture. There is no draw call per object and no buffer per object.
//
// The mesh is engine::unitQuad and the draw is engine::instanced - the same
// mechanism a real model would go through. What is game-shaped is only this
// struct: what one copy of a thing has to say about itself.
//
// Two ways to stand, because a 2.5d game needs both:
//
//   upright   the card faces the eye and keeps its size on screen. What a pawn,
//             a tree or a wall is: art drawn from the side, anchored at the
//             point on the ground where the thing stands.
//   laid      the card lies in the world's x/y plane and turns about the up
//             axis. What a field, a road, a shadow or a decal is - and the one
//             that stays correct if the camera is ever tipped, because it is
//             real geometry rather than a screen-space card.

#include <cstdint>

namespace game {

struct SpriteInstanceGpu {
    float position[3];   // world metres, z is height above the sea
    float size[2];       // metres across and metres tall
    float rotation;      // radians about the up axis; only a laid card turns
    float tint[4];       // multiplied into the picture, alpha included
    float uv[4];         // the piece of the page: u0, v0, u1, v1
    float page;          // which layer of the sprite array
    float mode;          // 0 upright, 1 laid on the ground
};

static_assert(sizeof(SpriteInstanceGpu) == 16 * sizeof(float),
              "the instance layout is written out in sprite.hlsl and in the pass's attribute "
              "list; a hole in the middle of it draws a crowd out of the wrong fields");

// How a batch of them is drawn.
enum class SpriteBlend : std::uint8_t {
    // Cut out and written into the depth buffer, in any order. What a crowd
    // wants: nothing has to be sorted against anything, so the whole crowd is
    // one draw whatever order it arrives in.
    Cutout,
    // Blended, sorted by the queue, and not written into the depth buffer. For
    // the few things that need a soft edge - smoke, a selection ring, a fade -
    // and never for a crowd, because a thousand blended cards in the wrong order
    // is a thousand wrong pixels.
    Soft,
};

} // namespace game
