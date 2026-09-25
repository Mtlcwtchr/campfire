#include "game/ecs/render/extraction.hpp"
#include "game/ecs/entity.hpp"
#include "game/ecs/registry.hpp"

#include <algorithm>

namespace sim::ecs::render {
namespace {
RenderEntityId renderId(const entt::registry& registry, entt::entity entity) {
    if (registry.all_of<const Identity>(entity)) {
        const auto& identity = registry.get<const Identity>(entity);
        return game::render::makeRenderEntityId(
                static_cast<game::render::EntityClass>(identity.kind), identity.legacyIndex);
    }
    return static_cast<RenderEntityId>(entt::to_entity(entity));
}
} // namespace

Frame extract(const entt::registry& registry) {
    Frame frame;
    auto sprites = registry.view<const Position, const SpriteRender>();
    frame.sprites.reserve(sprites.size_hint());
    for (const entt::entity entity : sprites) {
        const auto& render = sprites.get<const SpriteRender>(entity);
        if (!render.visible) continue;
        frame.sprites.push_back({renderId(registry, entity), sprites.get<const Position>(entity).value,
                                 render.sprite, render.layer});
    }
    std::stable_sort(frame.sprites.begin(), frame.sprites.end(),
                     [](const SpriteBatchItem& a, const SpriteBatchItem& b) {
                         if (a.layer != b.layer) return a.layer < b.layer;
                          return a.id < b.id;
                     });

    auto meshes = registry.view<const Position, const MeshRender>();
    frame.meshes.reserve(meshes.size_hint());
    for (const entt::entity entity : meshes) {
        const auto& render = meshes.get<const MeshRender>(entity);
        if (!render.visible) continue;
        frame.meshes.push_back({renderId(registry, entity), meshes.get<const Position>(entity).value,
                                render.mesh, render.material});
    }
    std::stable_sort(frame.meshes.begin(), frame.meshes.end(),
                     [](const MeshBatchItem& a, const MeshBatchItem& b) {
                          return a.id < b.id;
                     });
    return frame;
}

Frame extract(const ::sim::ecs::EcsWorld& world) {
    return extract(world.readRegistry());
}

} // namespace sim::ecs::render
