#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <set>

#include "engine/render/systems/instance_gather.hpp"

namespace {
using namespace engine::render;
using engine::ecs::Registry;
using engine::ecs::Snapshot;

Entity tree(Registry& registry, float x, float y, std::uint32_t mesh,
            std::uint32_t material = 0, std::uint32_t variant = 0) {
    const auto entity = registry.create();
    registry.emplace<WorldTransform>(entity, WorldTransform{{x, y, 0}, 1.0f, 0.0f});
    registry.emplace<SmartMeshRef>(entity, SmartMeshRef{mesh, material, variant});
    return entity;
}
}

TEST(render_gather_groups_by_asset_so_a_forest_is_a_few_draws_not_a_few_thousand) {
    // The whole reason this exists: ten thousand trees of five species must
    // leave as five batches, not as ten thousand of anything. An entity that
    // owned a renderer would make that impossible however the frame was sorted.
    Registry registry;
    for (int i = 0; i < 10000; ++i) tree(registry, float(i), float(i % 97), std::uint32_t(i % 5));
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.drawn(), std::size_t(10000));
    CHECK_EQ(gathered.batches.size(), std::size_t(5));
    // Every batch is a contiguous run, they tile the instance array exactly,
    // and no instance is in two of them.
    std::size_t covered = 0;
    for (std::size_t i = 0; i < gathered.batches.size(); ++i) {
        const auto& batch = gathered.batches[i];
        CHECK_EQ(std::size_t(batch.first), covered);
        CHECK(batch.count > 0);
        covered += batch.count;
        if (i) CHECK(gathered.batches[i - 1].mesh <= batch.mesh);
    }
    CHECK_EQ(covered, gathered.instances.size());
}

TEST(render_gather_keeps_the_entity_behind_every_instance_it_draws) {
    // The only link between a drawn thing and its own data. Whatever a later
    // system puts on a tree - species, age, damage - is reached through this id
    // and not through the renderer, which is the point of the split.
    Registry registry;
    std::vector<Entity> made;
    for (int i = 0; i < 64; ++i)
        made.push_back(tree(registry, float(i) * 3, float(i), std::uint32_t(i % 3)));
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.owners.size(), gathered.instances.size());
    CHECK_EQ(gathered.owners.size(), made.size());
    // Each owner appears once, is one of the entities made, and its instance
    // really is that entity's transform.
    std::set<std::uint32_t> seen;
    for (std::size_t i = 0; i < gathered.owners.size(); ++i) {
        const Entity owner = gathered.owners[i];
        CHECK(registry.valid(owner));
        CHECK(seen.insert(entt::to_integral(owner)).second);
        const auto& transform = registry.get<WorldTransform>(owner);
        CHECK_EQ(gathered.instances[i].position[0], transform.position[0]);
        CHECK_EQ(gathered.instances[i].position[1], transform.position[1]);
        CHECK_EQ(gathered.instances[i].variant, registry.get<SmartMeshRef>(owner).variant);
    }
}

TEST(render_gather_batches_stay_put_when_something_unrelated_is_created) {
    // What stability is for: a dithered or screen-door transition keyed on an
    // instance's place in its batch shimmers if the places move. Creating and
    // destroying entities elsewhere must not reshuffle the trees.
    Registry registry;
    std::vector<Entity> trees;
    for (int i = 0; i < 200; ++i)
        trees.push_back(tree(registry, float(i), float(i * 2), std::uint32_t(i % 4)));
    const auto before = gatherInstances(registry);

    // Something with no drawable components at all, and a drawable one of a
    // different asset: neither may move an existing tree within its batch.
    for (int i = 0; i < 50; ++i) (void)registry.create();
    tree(registry, 999, 999, 77);
    const auto after = gatherInstances(registry);

    CHECK_EQ(before.drawn() + 1, after.drawn());
    std::size_t matched = 0;
    for (std::size_t i = 0; i < before.owners.size(); ++i) {
        // Every original tree keeps its position within its own batch.
        const auto owner = before.owners[i];
        const auto found = std::find(after.owners.begin(), after.owners.end(), owner);
        CHECK(found != after.owners.end());
        CHECK_EQ(std::size_t(found - after.owners.begin()), i);
        ++matched;
    }
    CHECK_EQ(matched, before.owners.size());

    // And asking twice for the same registry is the same answer, always.
    const auto again = gatherInstances(registry);
    CHECK_EQ(again.owners, after.owners);
    CHECK_EQ(again.batches.size(), after.batches.size());
}

TEST(render_gather_reads_visibility_rather_than_deciding_it) {
    // Culling belongs to whichever system owns it. If the gather also decided,
    // the two would disagree on any frame where only one of them ran, and the
    // disagreement would look like flickering rather than like a bug.
    Registry registry;
    const auto shown = tree(registry, 0, 0, 1);
    const auto hidden = tree(registry, 10, 0, 1);
    registry.emplace<RenderVisible>(hidden, RenderVisible{false});
    registry.emplace<RenderVisible>(shown, RenderVisible{true});
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.drawn(), std::size_t(1));
    CHECK_EQ(gathered.invisible, std::size_t(1));
    CHECK_EQ(gathered.owners.front(), shown);
    // An entity with no opinion is drawn: absence of a component is not a no.
    const auto quiet = tree(registry, 20, 0, 1);
    CHECK_EQ(gatherInstances(registry).drawn(), std::size_t(2));
    CHECK(gatherInstances(registry).owners.back() == quiet ||
          gatherInstances(registry).owners.front() == quiet);
}

TEST(render_gather_needs_both_a_place_and_an_asset_and_says_when_one_is_broken) {
    Registry registry;
    // A transform with no asset, and an asset with no transform: neither is a
    // drawable thing, and neither is an error worth counting.
    const auto placeless = registry.create();
    registry.emplace<SmartMeshRef>(placeless, SmartMeshRef{1, 0, 0});
    const auto assetless = registry.create();
    registry.emplace<WorldTransform>(assetless, WorldTransform{});
    CHECK_EQ(gatherInstances(registry).drawn(), std::size_t(0));
    CHECK_EQ(gatherInstances(registry).rejected, std::size_t(0));

    // A position that became a NaN upstream is counted, not silently dropped:
    // a frame that quietly draws one fewer tree never says so.
    const auto broken = tree(registry, std::nanf(""), 0, 1);
    (void)broken;
    const auto negative = tree(registry, 1, 1, 1);
    registry.get<WorldTransform>(negative).scale = -1;
    const auto huge = tree(registry, 2, 2, 1);
    registry.get<WorldTransform>(huge).yaw = std::numeric_limits<float>::infinity();
    const auto good = tree(registry, 3, 3, 1);
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.drawn(), std::size_t(1));
    CHECK_EQ(gathered.rejected, std::size_t(3));
    CHECK_EQ(gathered.owners.front(), good);
}

TEST(render_gather_carries_per_instance_shading_without_a_component_per_entity) {
    Registry registry;
    const auto plain = tree(registry, 0, 0, 2);
    const auto tinted = tree(registry, 1, 0, 2);
    registry.emplace<RenderTint>(tinted, RenderTint{0.5f, 2.5f});
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.drawn(), std::size_t(2));
    for (std::size_t i = 0; i < gathered.owners.size(); ++i) {
        const bool isTinted = gathered.owners[i] == tinted;
        CHECK_EQ(gathered.instances[i].tint, isTinted ? 0.5f : 1.0f);
        CHECK_EQ(gathered.instances[i].phase, isTinted ? 2.5f : 0.0f);
    }
    CHECK(plain != tinted);
}

TEST(render_gather_separates_material_from_asset) {
    // One mesh drawn with two materials is two batches, because a batch is one
    // binding of both. Merging them would draw half the trees with the wrong
    // material and cost nothing to notice until a screenshot.
    Registry registry;
    for (int i = 0; i < 8; ++i) tree(registry, float(i), 0, 3, std::uint32_t(i % 2));
    const auto gathered = gatherInstances(registry);
    CHECK_EQ(gathered.batches.size(), std::size_t(2));
    CHECK_EQ(gathered.batches[0].mesh, gathered.batches[1].mesh);
    CHECK(gathered.batches[0].material != gathered.batches[1].material);
    CHECK_EQ(gathered.batches[0].count + gathered.batches[1].count, 8u);
}

TEST(render_gather_of_an_empty_world_is_empty_rather_than_undefined) {
    Registry registry;
    const auto gathered = gatherInstances(registry);
    CHECK(gathered.batches.empty());
    CHECK(gathered.instances.empty());
    CHECK(gathered.owners.empty());
    CHECK_EQ(gathered.drawn(), std::size_t(0));
    // And through the read-only boundary the rest of this ECS uses.
    CHECK_EQ(gatherInstances(Snapshot{registry}).drawn(), std::size_t(0));
}
