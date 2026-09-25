#pragma once
#include <cstdint>
#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "engine/core/ids.hpp"
namespace sim::ecs {
struct SpriteRender { core::DefId sprite{}; std::uint8_t layer = 0; bool visible = true; };
struct MeshRender { core::DefId mesh{}; core::DefId material{}; bool visible = true; };
struct RenderBounds { core::WorldPos centre{}; core::Fixed radius{}; };
struct RenderDirty {};
using Renderable = SpriteRender;
} // namespace sim::ecs
