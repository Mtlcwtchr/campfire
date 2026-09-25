#include "framework.hpp"

#include <set>

#include "engine/render/systems/frustum_cull.hpp"
#include "game/world/scene_entities.hpp"

namespace {
using namespace world::decor;
using engine::ecs::Registry;
using engine::render::RenderTint;
using engine::render::SmartMeshRef;
using engine::render::WorldTransform;

// A hand-made scatter: real generation needs a world, and what is under test
// here is the crossing from one to the other, not what is scattered.
Scatter made(std::size_t count, std::uint32_t models = 5) {
    Scatter scatter;
    for (std::size_t i = 0; i < count; ++i)
        scatter.objects.push_back({100 + i, double(i) * 3.5, double(i) * -2.25, 12.0 + double(i),
                                   0.5f + float(i % 4) * 0.25f, float(i) * 0.1f, float(i) * 0.01f,
                                   0.8f + float(i % 3) * 0.1f, std::uint32_t(i % models)});
    return scatter;
}

// Somebody else's entity in the same registry, to prove republishing leaves it.
struct Actor {
    int who = 0;
};
}

TEST(scene_entities_publishes_every_object_with_what_it_takes_to_draw_it) {
    Registry registry;
    const auto scatter = made(400);
    const auto published = publishScatter(registry, scatter, 5);

    CHECK_EQ(published.created, std::size_t(400));
    CHECK_EQ(published.destroyed, std::size_t(0));
    CHECK_EQ(published.unknownModel, std::size_t(0));
    CHECK_EQ(registry.view<ScatterRef>().size(), std::size_t(400));

    std::set<std::uint64_t> seen;
    for (const auto entity : registry.view<ScatterRef>()) {
        const auto id = registry.get<ScatterRef>(entity).id;
        CHECK(seen.insert(id).second);
        const auto& object = scatter.objects[id - 100];
        const auto& transform = registry.get<WorldTransform>(entity);
        CHECK_EQ(double(transform.position[0]), object.x);
        CHECK_EQ(double(transform.position[1]), object.y);
        CHECK_EQ(double(transform.position[2]), object.z);
        CHECK_EQ(transform.scale, object.scale);
        CHECK_EQ(transform.yaw, object.yaw);
        CHECK_EQ(registry.get<SmartMeshRef>(entity).mesh, object.model);
        CHECK_EQ(registry.get<RenderTint>(entity).tint, object.tint);
        CHECK_EQ(registry.get<RenderTint>(entity).phase, object.phase);
    }
    CHECK_EQ(seen.size(), std::size_t(400));
}

TEST(scene_entities_carries_no_renderer_only_a_handle_to_one) {
    // The rule the whole component split exists for. A tree names an asset; it
    // does not own geometry, a material or a draw. This is a compile-time fact,
    // so the test states it as one and measures the consequence: what a tree
    // costs in the registry is a transform, two ids and its shading.
    static_assert(sizeof(SmartMeshRef) == 12, "a mesh reference is three ids");
    static_assert(std::is_trivially_copyable_v<WorldTransform>);
    static_assert(std::is_trivially_copyable_v<SmartMeshRef>);
    static_assert(std::is_trivially_copyable_v<ScatterRef>);
    CHECK(sizeof(WorldTransform) + sizeof(SmartMeshRef) + sizeof(RenderTint) + sizeof(ScatterRef) <=
          std::size_t(64));
}

TEST(scene_entities_replaces_the_previous_region_and_leaves_everything_else) {
    // A region change republishes. The trees of the old one have to go - a
    // forest that accumulated every region the camera ever visited would drift
    // upward forever - and nothing that is not a tree may go with them.
    Registry registry;
    std::vector<engine::render::Entity> actors;
    for (int i = 0; i < 7; ++i) {
        const auto entity = registry.create();
        registry.emplace<Actor>(entity, Actor{i});
        registry.emplace<WorldTransform>(entity, WorldTransform{});
        actors.push_back(entity);
    }
    publishScatter(registry, made(300), 5);
    const auto again = publishScatter(registry, made(120), 5);

    CHECK_EQ(again.destroyed, std::size_t(300));
    CHECK_EQ(again.created, std::size_t(120));
    CHECK_EQ(registry.view<ScatterRef>().size(), std::size_t(120));
    CHECK_EQ(registry.view<Actor>().size(), std::size_t(7));
    for (const auto entity : actors) CHECK(registry.valid(entity));
    // And the trees really are the new ones, not the first 120 of the old.
    for (const auto entity : registry.view<ScatterRef>())
        CHECK(registry.get<ScatterRef>(entity).id < 100 + 120);
}

TEST(scene_entities_drops_an_object_naming_an_asset_that_is_not_there) {
    // Drawing it as model zero would turn a content mistake into a pine that
    // quietly became an oak, which is harder to notice than a missing tree.
    Registry registry;
    auto scatter = made(50, 5);
    scatter.objects[10].model = 9;
    scatter.objects[11].model = 5;
    const auto published = publishScatter(registry, scatter, 5);
    CHECK_EQ(published.unknownModel, std::size_t(2));
    CHECK_EQ(published.created, std::size_t(48));
    for (const auto entity : registry.view<ScatterRef>())
        CHECK(registry.get<SmartMeshRef>(entity).mesh < 5u);
}

TEST(scene_entities_feed_the_render_systems_without_a_walk_of_their_own) {
    // The point of publishing at all: from here the frame is gather, cull,
    // select - three passes over flat arrays - and no pass ever looks at a
    // decor::Object again.
    Registry registry;
    publishScatter(registry, made(900), 5);
    const auto gathered = engine::render::gatherInstances(registry);
    CHECK_EQ(gathered.drawn(), std::size_t(900));
    CHECK_EQ(gathered.batches.size(), std::size_t(5));   // one per model in the catalogue

    const float errors[]{0.0f, 0.121f, 0.260f, 0.546f};
    std::vector<engine::render::MeshDescription> assets(5);
    for (auto& asset : assets) asset = {errors, 13.338, 0, {8.5, 6.7}};

    engine::render::ScreenScale screen;
    screen.rowX[0] = 2.4f;
    screen.rowY[2] = 2.4f;
    screen.rowW[1] = 1;
    screen.focal = 900;
    const auto culled = engine::render::cullToFrustum(gathered, assets, screen);
    const auto selected = engine::render::selectLevels(culled.kept, assets, screen);
    CHECK_EQ(selected.drawn(), culled.kept.drawn());
    CHECK(culled.removed() > 0);   // the scatter runs off to -y, behind the eye

    // Every owner is still a tree of this scatter, which is the link a later
    // system will read a tree's own data through.
    for (const auto owner : selected.owners) CHECK(registry.all_of<ScatterRef>(owner));
}
