#include "framework.hpp"

#include <array>
#include <cmath>
#include <set>

#include "engine/render/systems/frustum_cull.hpp"

namespace {
using namespace engine::render;
using engine::ecs::Registry;

constexpr std::array<float, 4> kTree{0.0f, 0.121f, 0.260f, 0.546f};
constexpr double kTreeExtent = 13.338;
// CommonTree_1: 4.6 m across, 13.3 m tall, standing on its anchor.
const MeshDescription kTreeAsset{kTree, kTreeExtent, 0,
                                 {std::sqrt(4.6 * 4.6 + 13.338 * 13.338) * 0.6, 13.338 * 0.5}};

Entity at(Registry& registry, double x, double y, double z, float scale = 1, std::uint32_t mesh = 0) {
    const auto entity = registry.create();
    registry.emplace<WorldTransform>(entity,
                                     WorldTransform{{float(x), float(y), float(z)}, scale, 0});
    registry.emplace<SmartMeshRef>(entity, SmartMeshRef{mesh, 0, 0});
    return entity;
}

// A camera at the origin looking down +y, with z up - the world axes the rest
// of the renderer uses. Half-angle `fov` horizontally and vertically.
ScreenScale camera(double fov = 0.7854, double focal = 900) {
    const double cot = 1.0 / std::tan(fov);
    ScreenScale screen;
    screen.rowX[0] = float(cot);   // clip x from world x
    screen.rowY[2] = float(cot);   // clip y from world z
    screen.rowW[1] = 1;            // depth is world y
    screen.focal = focal;
    return screen;
}
}

TEST(frustum_cull_keeps_what_is_in_front_and_drops_the_rest_of_the_circle) {
    // A ring of trees around the camera. A 45-degree half-angle sees a quarter
    // of them; the count is what says the planes are the planes and not a box.
    Registry registry;
    constexpr int kCount = 360;
    for (int i = 0; i < kCount; ++i) {
        // Half a step off the axes, so no tree sits exactly on a plane and
        // the count is a fact rather than a rounding.
        const double a = (i + 0.5) * 6.283185307179586 / kCount;
        at(registry, std::sin(a) * 500, std::cos(a) * 500, 0);
    }
    const auto gathered = gatherInstances(registry);
    const MeshDescription assets[]{kTreeAsset};
    const auto culled = cullToFrustum(gathered, assets, camera());

    CHECK_EQ(culled.kept.drawn() + culled.removed(), std::size_t(kCount));
    CHECK_EQ(culled.behind, std::size_t(kCount / 2));   // exactly the half behind the eye
    // A quarter of the circle, plus the sliver the bounding spheres add at each
    // edge. Never fewer, or something visible was thrown away.
    CHECK(culled.kept.drawn() >= std::size_t(kCount / 4));
    CHECK(culled.kept.drawn() <= std::size_t(kCount / 4) + 6);
}

TEST(frustum_cull_never_drops_a_model_whose_bulk_reaches_into_the_frame) {
    // The bug this replaces: culling a projected point against a widened screen
    // box divides by the anchor's depth, so at the edge of a wide field of view
    // a tree whose trunk is just outside disappears with its crown still on
    // screen. Walk the edge and check nothing that touches the frustum is lost.
    Registry registry;
    std::vector<Entity> made;
    const double reach = kTreeAsset.bounds.radius;
    for (int i = 0; i < 200; ++i) {
        const double depth = 30 + i * 5.0;
        // Exactly on the left plane for a 45-degree half-angle: x = -depth.
        made.push_back(at(registry, -depth, depth, 0));
    }
    const auto gathered = gatherInstances(registry);
    const MeshDescription assets[]{kTreeAsset};
    const auto culled = cullToFrustum(gathered, assets, camera());
    CHECK_EQ(culled.kept.drawn(), made.size());   // all of them straddle the plane

    // And a whole radius beyond it, every one is gone.
    Registry outside;
    for (int i = 0; i < 200; ++i) {
        const double depth = 30 + i * 5.0;
        at(outside, -(depth + reach * 2), depth, 0);
    }
    const auto far = cullToFrustum(gatherInstances(outside), assets, camera());
    CHECK_EQ(far.kept.drawn(), std::size_t(0));
    CHECK_EQ(far.outside, std::size_t(200));
    CHECK_EQ(far.behind, std::size_t(0));
}

TEST(frustum_cull_scales_a_models_reach_by_the_instance_that_wears_it) {
    // A sapling is not culled with a grown tree's radius. Put both a hair
    // outside the plane by exactly the big one's reach: the big one survives,
    // the small one does not.
    Registry registry;
    const double depth = 200, reach = kTreeAsset.bounds.radius;
    const auto big = at(registry, -(depth + reach * 0.5), depth, 0, 1.0f);
    const auto small = at(registry, -(depth + reach * 0.5), depth, 0, 0.1f);
    CHECK(big != small);
    const auto gathered = gatherInstances(registry);
    const MeshDescription assets[]{kTreeAsset};
    const auto culled = cullToFrustum(gathered, assets, camera());
    CHECK_EQ(culled.kept.drawn(), std::size_t(1));
    CHECK_EQ(culled.outside, std::size_t(1));
    CHECK_EQ(culled.kept.instances.front().scale, 1.0f);
}

TEST(frustum_cull_hands_the_selection_batches_it_can_use_unchanged) {
    // The output is a gather, so it feeds straight into the level selection.
    // Batches stay grouped, stay contiguous, and an emptied one disappears
    // rather than being submitted as a draw of nothing.
    Registry registry;
    for (int i = 0; i < 40; ++i) at(registry, float(i), 300, 0, 1, 0);          // mesh 0: in front
    for (int i = 0; i < 40; ++i) at(registry, float(i), -300, 0, 1, 1);         // mesh 1: all behind
    for (int i = 0; i < 40; ++i) at(registry, float(i), 600, 0, 1, 2);          // mesh 2: in front
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.batches.size(), std::size_t(3));
    const MeshDescription assets[]{kTreeAsset, kTreeAsset, kTreeAsset};
    const auto culled = cullToFrustum(gathered, assets, camera());

    CHECK_EQ(culled.behind, std::size_t(40));
    CHECK_EQ(culled.kept.batches.size(), std::size_t(2));   // mesh 1 left nothing
    std::size_t covered = 0;
    for (const auto& batch : culled.kept.batches) {
        CHECK(batch.mesh != 1u);
        CHECK(batch.count > 0);
        CHECK_EQ(std::size_t(batch.first), covered);
        covered += batch.count;
    }
    CHECK_EQ(covered, culled.kept.instances.size());
    CHECK_EQ(culled.kept.owners.size(), culled.kept.instances.size());

    // And the selection takes it: every survivor is placed, none invented.
    
    const auto selected = selectLevels(culled.kept, assets, camera());
    CHECK_EQ(selected.drawn(), culled.kept.drawn());
    CHECK_EQ(selected.behind, std::size_t(0));   // the cull already answered that
}

TEST(frustum_cull_keeps_the_owning_entity_with_every_instance_it_keeps) {
    Registry registry;
    std::vector<Entity> front;
    for (int i = 0; i < 50; ++i) {
        front.push_back(at(registry, float(i) * 3, 400, 0));
        at(registry, float(i) * 3, -400, 0);      // a behind-the-eye twin for each
    }
    const auto gathered = gatherInstances(registry);
    const MeshDescription assets[]{kTreeAsset};
    const auto culled = cullToFrustum(gathered, assets, camera());
    CHECK_EQ(culled.kept.drawn(), front.size());
    std::set<std::uint32_t> kept;
    for (std::size_t i = 0; i < culled.kept.owners.size(); ++i) {
        const Entity owner = culled.kept.owners[i];
        CHECK(kept.insert(entt::to_integral(owner)).second);
        const auto& transform = registry.get<WorldTransform>(owner);
        CHECK_EQ(culled.kept.instances[i].position[1], transform.position[1]);
        CHECK(transform.position[1] > 0);
    }
    for (const Entity owner : front) CHECK(kept.count(entt::to_integral(owner)) == 1);
}

TEST(frustum_cull_treats_an_undescribed_asset_as_a_point_rather_than_as_everywhere) {
    // A mesh id with no bounds is a content bug. It is culled where it stands,
    // which is wrong by at most its own size; giving it an infinite radius
    // would make it survive every frame and cost a draw forever.
    Registry registry;
    at(registry, -1000, 100, 0, 1, 5);   // far outside, no description
    at(registry, 0, 100, 0, 1, 5);       // dead centre
    const auto culled = cullToFrustum(gatherInstances(registry), {}, camera());
    CHECK_EQ(culled.kept.drawn(), std::size_t(1));
    CHECK_EQ(culled.outside, std::size_t(1));
    CHECK_EQ(culled.kept.instances.front().position[0], 0.0f);
}

TEST(frustum_cull_leaves_an_orthographic_frame_with_no_eye_to_be_behind) {
    // An overview draws everything between its planes, including what a
    // perspective frame would call behind it. The matrix says so: its depth row
    // is constant, so the near test passes for every point.
    Registry registry;
    for (int i = -19; i <= 19; ++i) at(registry, 0, double(i) * 100, 0);
    ScreenScale flat;
    flat.rowX[0] = 0.0005f;     // a 4 km wide overview
    flat.rowY[1] = 0.0005f;
    flat.rowW[3] = 1;           // constant depth: orthographic
    flat.focal = 0;
    flat.scale = 0.45;
    const auto culled = cullToFrustum(gatherInstances(registry), {}, flat);
    CHECK_EQ(culled.behind, std::size_t(0));
    CHECK_EQ(culled.kept.drawn(), std::size_t(39));   // 1.9 km of a 2 km half-extent

    // Narrow it and the ends go, from the sides rather than from the eye.
    flat.rowY[1] = 0.0018f;     // 1.1 km tall, so no tree lands on the plane
    const auto narrow = cullToFrustum(gatherInstances(registry), {}, flat);
    CHECK_EQ(narrow.behind, std::size_t(0));
    CHECK_EQ(narrow.kept.drawn(), std::size_t(11));
    CHECK_EQ(narrow.outside, std::size_t(28));
}

TEST(frustum_cull_costs_the_frame_nothing_it_does_not_have_to) {
    // Culling before selection is the point of the order: the selection, and
    // the budget it feeds, only ever see what is drawn. A forest behind the
    // camera must not reach them at all.
    Registry registry;
    for (int i = 0; i < 2000; ++i) at(registry, double(i % 40) * 4, -200 - double(i), 0);
    for (int i = 0; i < 100; ++i) at(registry, double(i % 10) * 4, 250 + double(i), 0);
    const auto gathered = gatherInstances(registry);
    const MeshDescription assets[]{kTreeAsset};
    const auto culled = cullToFrustum(gathered, assets, camera());
    CHECK_EQ(gathered.drawn(), std::size_t(2100));
    CHECK_EQ(culled.kept.drawn(), std::size_t(100));
    CHECK_EQ(selectLevels(culled.kept, assets, camera()).drawn(), std::size_t(100));
}
