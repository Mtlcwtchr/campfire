#pragma once

#include <cstdint>
#include <vector>

#include "engine/core/geometry.hpp"
#include "engine/core/ids.hpp"

namespace game::render {

using RenderEntityId = std::uint64_t;

enum class EntityClass : std::uint8_t { Person, Animal, Building, ResourceNode, ItemStack };

constexpr RenderEntityId makeRenderEntityId(EntityClass kind, std::uint32_t index) {
    return (static_cast<RenderEntityId>(static_cast<std::uint8_t>(kind)) << 32) | index;
}

struct SpriteBatchItem {
    RenderEntityId id = 0;
    core::WorldPos position{};
    core::DefId sprite{};
    std::uint8_t layer = 0;
};
struct MeshBatchItem {
    RenderEntityId id = 0;
    core::WorldPos position{};
    core::DefId mesh{};
    core::DefId material{};
};
struct Frame {
    std::vector<SpriteBatchItem> sprites;
    std::vector<MeshBatchItem> meshes;
};

} // namespace game::render
