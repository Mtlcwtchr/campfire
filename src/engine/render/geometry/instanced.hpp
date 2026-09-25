#pragma once
// A shape on the card, drawn once or a hundred thousand times.
//
// This is the only way anything in this engine draws a thing that there may be
// more than one of, and it does not know the word "sprite". A shape is vertices
// and indices; a draw is that shape plus a run of instances out of the frame's
// arena. A quad with a picture on it and a cart with eight hundred triangles go
// down the same path, in the same buffer, through the same queue and the same
// sort - the difference between them is the mesh and the shader, and nothing
// else in the engine or in the game changes when one becomes the other.
//
// That is the point of writing it this way rather than as a sprite renderer.
// A sprite renderer is a second drawing path that has to be kept in step with
// the first, and the day the game wants a real mesh for a cart it either grows a
// third path or the sprite path grows a special case. Here the sprite *is* the
// general case with the simplest possible mesh.

#include <cstdint>

#include "engine/render/device.hpp"
#include "engine/render/mesh.hpp"
#include "engine/geometry/smart_sprite_mesh.hpp"
#include "engine/render/draw_queue.hpp"
#include "engine/render/geometry/instances.hpp"

namespace engine {

// Vertices and indices, uploaded once and drawn for the life of the program.
// Whatever the vertex layout is, it is the pipeline's business and not this
// struct's - which is what lets one type describe a quad and a cart.
using Geometry = Mesh; // source compatibility; one resource type, not another backend

// The mesh a card is: two triangles, a unit either side of the anchor and a unit
// up from it, so scaling makes a thing taller rather than moving it off the
// ground. Corner runs -1..1 across and 0..1 up; uv runs 0..1 with v flipped,
// because a picture's first row is its top.
//
// Vertex layout: two floats of corner, two of uv. A shader that wants a real
// mesh instead declares its own layout and loads its own vertices; nothing else
// about the draw changes.
using QuadVertex = SmartSpriteMesh::Vertex;
Geometry unitQuad(Device& device);

// One draw: this shape, these instances. The instances live in the frame's
// arena and are addressed by the byte offset it handed out.
DrawItem instanced(const Geometry& shape, const InstanceArena& arena, std::uint32_t at,
                   std::uint32_t count);

// A run of one shape's indices, for a mesh whose index buffer holds more than
// one level of detail. The vertices, the binding and the pipeline are the same
// for every level, so the only thing a coarser one changes is this range.
DrawItem instanced(const Geometry& shape, const InstanceArena& arena, std::uint32_t at,
                   std::uint32_t count, IndexRange range);

} // namespace engine
