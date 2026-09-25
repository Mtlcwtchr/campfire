#include "game/world/scene_entities.hpp"

#include <cmath>
#include <vector>

namespace world::decor {

Published publishScatter(engine::ecs::Registry& registry, const Scatter& scatter,
                         std::uint32_t models) {
    using namespace engine::render;
    Published result;

    // Collected before destroying, because destroying while iterating a view is
    // the one thing EnTT asks you not to do.
    std::vector<engine::render::Entity> previous;
    {
        const auto view = registry.view<ScatterRef>();
        previous.assign(view.begin(), view.end());
    }
    for (const auto entity : previous) registry.destroy(entity);
    result.destroyed = previous.size();

    for (const Object& object : scatter.objects) {
        if (object.model >= models) {
            ++result.unknownModel;
            continue;
        }
        const auto entity = registry.create();
        registry.emplace<WorldTransform>(
                entity, WorldTransform{{float(object.x), float(object.y), float(object.z)},
                                       object.scale, object.yaw});
        registry.emplace<SmartMeshRef>(entity, SmartMeshRef{object.model, 0, 0});
        registry.emplace<RenderTint>(entity, RenderTint{object.tint, object.phase});
        registry.emplace<ScatterRef>(entity, ScatterRef{object.id});
        ++result.created;
    }
    return result;
}

} // namespace world::decor
