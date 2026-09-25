#include "framework.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

#include "engine/render/systems/level_select.hpp"

namespace {
using namespace engine::render;
using engine::ecs::Registry;

// The chain measured for CommonTree_1: full mesh, then a quarter, a tenth and a
// twenty-fifth of its triangles, with the 95th-percentile distance from the
// imported surface in model metres. Height 13.338 m.
constexpr std::array<float, 4> kTree{0.0f, 0.121f, 0.260f, 0.546f};
constexpr double kTreeExtent = 13.338;

Entity tree(Registry& registry, float x, float depth, float scale = 1, std::uint32_t mesh = 0) {
    const auto entity = registry.create();
    registry.emplace<WorldTransform>(entity, WorldTransform{{x, 0, depth}, scale, 0});
    registry.emplace<SmartMeshRef>(entity, SmartMeshRef{mesh, 0, 0});
    return entity;
}

// A perspective screen where depth is z and one metre at depth d is focal/d.
ScreenScale perspective(double focal = 900) {
    ScreenScale screen;
    screen.rowX[0] = 1;
    screen.rowY[1] = 1;
    screen.rowW[2] = 1;
    screen.focal = focal;
    return screen;
}
}

TEST(level_select_picks_the_coarsest_representation_that_still_fits_the_allowance) {
    // The rule itself, at the sizes a forest actually shows. Anything coarser
    // than the one chosen must fail the allowance, or the choice was timid.
    for (const double pixels : {1200.0, 300.0, 120.0, 60.0, 25.0}) {
        const auto level = levelFor(kTree, pixels, kTreeExtent);
        CHECK(level < kTree.size());
        CHECK(kTree[level] * pixels / kTreeExtent <= kLevelPixelError + 1e-9);
        if (level + 1 < kTree.size())
            CHECK(kTree[level + 1] * pixels / kTreeExtent > kLevelPixelError);
    }
    // Filling the screen is the model itself; about to become an impostor is
    // the coarsest the chain has.
    CHECK_EQ(levelFor(kTree, 1200, kTreeExtent), std::size_t(0));
    CHECK_EQ(levelFor(kTree, 25, kTreeExtent), kTree.size() - 1);
    // And the choice never goes backwards as a thing gets smaller.
    std::size_t previous = 0;
    for (int p = 2000; p > 0; --p) {
        const auto level = levelFor(kTree, p * 0.5, kTreeExtent);
        CHECK(level >= previous);
        previous = level;
    }
}

TEST(level_select_refuses_a_chain_that_is_not_ordered_or_not_measured) {
    const std::array<float, 3> ordered{0.0f, 0.1f, 0.4f};
    const std::array<float, 3> backwards{0.0f, 0.4f, 0.1f};
    // An unordered chain would let a coarser level win on a larger object, so
    // the walk stops where the ordering does rather than trusting the rest.
    CHECK_EQ(levelFor(backwards, 100, 10.0), std::size_t(0));
    CHECK_EQ(levelFor(backwards, 50, 10.0), std::size_t(1));
    CHECK_EQ(levelFor(ordered, 50, 10.0), std::size_t(2));
    const std::array<float, 2> broken{0.0f, std::numeric_limits<float>::quiet_NaN()};
    CHECK_EQ(levelFor(broken, 20, 10.0), std::size_t(0));
    CHECK_EQ(levelFor({}, 20, 10.0), std::size_t(0));
    CHECK_EQ(levelFor(ordered, std::numeric_limits<double>::infinity(), 10.0), std::size_t(0));
    CHECK_EQ(levelFor(ordered, 20, 0.0), std::size_t(0));
    CHECK_EQ(levelFor(ordered, -5, 10.0), std::size_t(0));
    // A tighter allowance may only ask for more detail, never less.
    for (int p = 1; p < 400; ++p)
        CHECK(levelFor(ordered, p, 10.0, 1.0) <= levelFor(ordered, p, 10.0, 4.0));
}

TEST(level_select_splits_one_asset_into_one_batch_per_level_it_actually_uses) {
    // What the system is for: a hundred trees spread from close to far leave as
    // a few batches - one per level in use - not as a hundred draws and not as
    // one batch drawn at whichever level the first tree wanted.
    Registry registry;
    for (int i = 0; i < 100; ++i) tree(registry, float(i), 40.0f + float(i) * 30.0f);
    const auto gathered = gatherInstances(registry);
    const MeshDescription levels[]{{kTree, kTreeExtent, 0}};
    const auto selected = selectLevels(gathered, levels, perspective());

    CHECK_EQ(selected.drawn(), std::size_t(100));
    CHECK(selected.batches.size() > 1);
    CHECK(selected.batches.size() <= kTree.size());
    // Contiguous, tiling, and ordered by level within the asset.
    std::size_t covered = 0;
    for (std::size_t i = 0; i < selected.batches.size(); ++i) {
        const auto& batch = selected.batches[i];
        CHECK_EQ(std::size_t(batch.first), covered);
        CHECK(batch.count > 0);
        covered += batch.count;
        if (i) CHECK(selected.batches[i - 1].level < batch.level);
    }
    CHECK_EQ(covered, selected.instances.size());
    // Every instance really is in the batch its own size asks for.
    for (const auto& batch : selected.batches)
        for (std::uint32_t i = 0; i < batch.count; ++i) {
            const auto& instance = selected.instances[batch.first + i];
            const double pixels = kTreeExtent * instance.scale * 900.0 / instance.position[2];
            CHECK_EQ(std::size_t(batch.level), levelFor(kTree, pixels, kTreeExtent));
        }
}

TEST(level_select_keeps_the_owning_entity_with_every_instance_it_moves) {
    // Selection reorders. If the owners did not travel with the instances, the
    // link back to an entity's own data would silently point at a neighbour -
    // which is worse than losing it, because it still looks like an answer.
    Registry registry;
    std::vector<Entity> made;
    for (int i = 0; i < 60; ++i) made.push_back(tree(registry, float(i), 50.0f + float(i) * 45.0f));
    const auto gathered = gatherInstances(registry);
    const MeshDescription levels[]{{kTree, kTreeExtent, 0}};
    const auto selected = selectLevels(gathered, levels, perspective());

    CHECK_EQ(selected.owners.size(), selected.instances.size());
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0; i < selected.owners.size(); ++i) {
        const Entity owner = selected.owners[i];
        CHECK(seen.insert(entt::to_integral(owner)).second);
        const auto& transform = registry.get<WorldTransform>(owner);
        CHECK_EQ(selected.instances[i].position[0], transform.position[0]);
        CHECK_EQ(selected.instances[i].position[2], transform.position[2]);
    }
    CHECK_EQ(seen.size(), made.size());
}

TEST(level_select_reads_screen_size_rather_than_distance) {
    // A sapling close by and a giant far off that cover the same pixels get the
    // same triangles. Selecting on distance would give the near one the full
    // mesh for being near, which is the whole mistake this replaces.
    Registry registry;
    const auto small = tree(registry, 0, 100, 0.25f);
    const auto large = tree(registry, 10, 400, 1.0f);
    const auto gathered = gatherInstances(registry);
    const MeshDescription levels[]{{kTree, kTreeExtent, 0}};
    const auto selected = selectLevels(gathered, levels, perspective());

    CHECK_EQ(selected.drawn(), std::size_t(2));
    CHECK_EQ(selected.batches.size(), std::size_t(1));   // same level, one batch
    CHECK(small != large);
}

TEST(level_select_never_asks_for_a_level_that_is_not_resident) {
    // A chain whose finer pages have not arrived draws the coarsest it HAS.
    // Asking for one it does not draws nothing at all, and a hole in a forest
    // is a worse answer than a coarser tree.
    Registry registry;
    for (int i = 0; i < 20; ++i) tree(registry, float(i), 60.0f);
    const auto gathered = gatherInstances(registry);
    const MeshDescription all[]{{kTree, kTreeExtent, 0}};
    const MeshDescription onlyCoarse[]{{kTree, kTreeExtent, 2}};   // levels 0 and 1 only
    const auto everything = selectLevels(gathered, all, perspective());
    const auto limited = selectLevels(gathered, onlyCoarse, perspective());

    CHECK_EQ(everything.drawn(), limited.drawn());
    for (const auto& batch : limited.batches) CHECK(batch.level <= 1);
    CHECK(everything.batches.front().level >= limited.batches.front().level);
}

TEST(level_select_counts_what_is_behind_the_eye_instead_of_drawing_it_coarsely) {
    Registry registry;
    tree(registry, 0, 200);      // in front
    tree(registry, 0, -50);      // behind
    tree(registry, 0, 0);        // exactly on the eye
    const auto gathered = gatherInstances(registry);
    const MeshDescription levels[]{{kTree, kTreeExtent, 0}};
    const auto selected = selectLevels(gathered, levels, perspective());
    CHECK_EQ(selected.drawn(), std::size_t(1));
    CHECK_EQ(selected.behind, std::size_t(2));

    // An orthographic frame has no eye to be behind, and says so by carrying a
    // scale instead of a focal length.
    ScreenScale flat;
    flat.rowW[2] = 1;
    flat.focal = 0;
    flat.scale = 4;
    const auto everything = selectLevels(gathered, levels, flat);
    CHECK_EQ(everything.drawn(), std::size_t(3));
    CHECK_EQ(everything.behind, std::size_t(0));
}

TEST(level_select_draws_an_asset_nobody_described_rather_than_dropping_it) {
    // A mesh id with no entry in the catalogue is a content bug, not a reason
    // to leave a hole. It draws at level zero, which is the one every chain has.
    Registry registry;
    tree(registry, 0, 100, 1, 7);      // no description for mesh 7
    tree(registry, 1, 100, 1, 0);
    const auto gathered = gatherInstances(registry);
    const MeshDescription levels[]{{kTree, kTreeExtent, 0}};
    const auto selected = selectLevels(gathered, levels, perspective());
    CHECK_EQ(selected.drawn(), std::size_t(2));
    for (const auto& batch : selected.batches)
        if (batch.mesh == 7) CHECK_EQ(batch.level, 0u);
    // An empty chain is the same answer.
    const MeshDescription empty[]{{{}, kTreeExtent, 0}};
    CHECK_EQ(selectLevels(gathered, empty, perspective()).drawn(), std::size_t(2));
}
