#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <limits>
#include <vector>

#include "engine/geometry/region_mass.hpp"

namespace {
using namespace engine::geometry;

// A tree: a connected trunk, and leaf cards that touch nothing. The cards are
// what `cardsOf` must find - two triangles, their own four vertices - and the
// trunk is the solid part that has to end up inside the mass rather than bare
// underneath it.
struct Model {
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    std::vector<float> layers;
};

Model tree(float height, float crown, int leaves, float leafSize = 0.6f, float trunkLayer = 0,
           float leafLayer = 1) {
    Model model;
    const auto vertex = [&](float x, float y, float z, float layer) {
        model.positions.insert(model.positions.end(), {x, y, z});
        model.layers.push_back(layer);
        return std::uint32_t(model.layers.size() - 1);
    };
    // Trunk: a square prism, every face sharing its corners, so it is one
    // connected component of twelve triangles and never mistaken for a card.
    const float r = 0.18f;
    std::uint32_t ring[2][4];
    for (int level = 0; level < 2; ++level)
        for (int corner = 0; corner < 4; ++corner) {
            const float x = corner == 0 || corner == 3 ? -r : r;
            const float y = corner < 2 ? -r : r;
            ring[level][corner] = vertex(x, y, level ? height : 0.0f, trunkLayer);
        }
    for (int corner = 0; corner < 4; ++corner) {
        const int next = (corner + 1) % 4;
        model.indices.insert(model.indices.end(),
            {ring[0][corner], ring[0][next], ring[1][corner]});
        model.indices.insert(model.indices.end(),
            {ring[1][corner], ring[0][next], ring[1][next]});
    }
    model.indices.insert(model.indices.end(), {ring[1][0], ring[1][1], ring[1][2]});
    model.indices.insert(model.indices.end(), {ring[1][0], ring[1][2], ring[1][3]});
    // Leaves: separate quads through the crown, on a deterministic spiral.
    for (int leaf = 0; leaf < leaves; ++leaf) {
        const double t = double(leaf) / std::max(1, leaves - 1);
        const double angle = double(leaf) * 2.39996;
        const float cx = float(std::cos(angle) * crown * (0.35 + 0.65 * std::sin(t * 3.14159)));
        const float cy = float(std::sin(angle) * crown * (0.35 + 0.65 * std::sin(t * 3.14159)));
        const float cz = float(height * (0.45 + 0.5 * t));
        const float h = leafSize * 0.5f;
        const auto a = vertex(cx - h, cy - h, cz, leafLayer);
        const auto b = vertex(cx + h, cy - h, cz + h, leafLayer);
        const auto c = vertex(cx + h, cy + h, cz, leafLayer);
        const auto d = vertex(cx - h, cy + h, cz - h, leafLayer);
        model.indices.insert(model.indices.end(), {a, b, c});
        model.indices.insert(model.indices.end(), {a, c, d});
    }
    return model;
}

RegionSource sourceOf(const Model& model) {
    return {model.positions, model.indices, model.layers, 0};
}

std::vector<RegionMember> grid(int side, float spacing, std::uint32_t model = 0) {
    std::vector<RegionMember> members;
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x) {
            RegionMember member;
            member.model = model;
            member.position[0] = float(x) * spacing;
            member.position[1] = float(y) * spacing;
            member.scale = 0.9f + 0.05f * float((x + 2 * y) % 5);
            member.yaw = float((x + y) % 4) * 0.7f;
            members.push_back(member);
        }
    return members;
}

bool sameGeometry(const RegionMass& a, const RegionMass& b) {
    if (a.positions != b.positions || a.indices != b.indices ||
        a.clusters.size() != b.clusters.size()) return false;
    for (std::size_t i = 0; i < a.clusters.size(); ++i)
        if (a.clusters[i].indices.first != b.clusters[i].indices.first ||
            a.clusters[i].indices.count != b.clusters[i].indices.count ||
            a.clusters[i].error != b.clusters[i].error ||
            a.clusters[i].parentError != b.clusters[i].parentError) return false;
    return true;
}
} // namespace

TEST(region_mass_is_built_from_the_members_that_are_actually_there) {
    const auto model = tree(6, 2, 90);
    const RegionSource sources[]{sourceOf(model)};
    RegionMassOptions options;
    options.cellMetres = 1.0;
    const auto members = grid(3, 7.5f);
    const auto baked = bakeRegionMass(members, sources, options);
    CHECK(!baked.empty());
    CHECK_EQ(baked.members, members.size());
    CHECK(baked.triangles() > 0);

    // Every member is inside what the aggregate claims to cover. A shape whose
    // bound does not contain its own members cannot replace them.
    for (const auto& member : members) {
        double distance = 0;
        for (int axis = 0; axis < 3; ++axis) {
            const double d = double(member.position[axis]) - baked.centre[axis];
            distance += d * d;
        }
        CHECK(std::sqrt(distance) <= baked.radius + 1e-3);
    }

    // The whole point: move one member and the geometry follows it. The 3x3
    // grove asset this replaces could not - it was the same nine trees at the
    // same nine places whatever the region held.
    auto moved = members;
    moved.back().position[0] += 9.0f;
    moved.back().position[1] += 9.0f;
    const auto elsewhere = bakeRegionMass(moved, sources, options);
    CHECK(!elsewhere.empty());
    CHECK(!sameGeometry(baked, elsewhere));
    CHECK(elsewhere.radius > baked.radius);

    // And so does what the members ARE: a region of low bushes is not a region
    // of tall trees scaled down to fit the same box.
    const auto bush = tree(1.6f, 1.1f, 60, 0.4f);
    const RegionSource other[]{sourceOf(bush)};
    const auto bushes = bakeRegionMass(members, other, options);
    CHECK(!bushes.empty());
    CHECK(!sameGeometry(baked, bushes));
    CHECK(bushes.radius < baked.radius);
}

TEST(region_mass_is_deterministic_and_independent_of_where_the_region_sits) {
    const auto model = tree(6, 2, 70);
    const RegionSource sources[]{sourceOf(model)};
    RegionMassOptions options;
    options.cellMetres = 1.2;
    const auto members = grid(3, 8.0f);
    const auto first = bakeRegionMass(members, sources, options);
    const auto again = bakeRegionMass(members, sources, options);
    CHECK(!first.empty());
    CHECK(sameGeometry(first, again));
    CHECK_EQ(first.cellMetres, again.cellMetres);

    // The SAME shape, wherever the region is. Not the same triangles: the
    // simplifier's quadrics are evaluated in floats, so an arrangement far from
    // the origin can collapse an edge its twin at the origin keeps. What must
    // hold is that the bake reads its input as a local arrangement - a bake
    // done in world coordinates loses its detail to the coordinate itself, and
    // that shows up here as a different size and a different amount of surface.
    auto shifted = members;
    const double shiftX = 512, shiftY = -384; // whole cells at this size
    for (auto& member : shifted) {
        member.position[0] += float(shiftX);
        member.position[1] += float(shiftY);
    }
    const auto moved = bakeRegionMass(shifted, sources, options);
    CHECK(!moved.empty());
    CHECK_EQ(moved.cellMetres, first.cellMetres);
    CHECK(std::abs(double(moved.radius) - first.radius) < first.radius * 0.02);
    CHECK(std::abs(double(moved.centre[0]) - shiftX - first.centre[0]) < 0.05);
    CHECK(std::abs(double(moved.centre[1]) - shiftY - first.centre[1]) < 0.05);
    CHECK(std::abs(double(moved.centre[2]) - first.centre[2]) < 0.05);
    const double ratio = double(moved.finestTriangles) / double(first.finestTriangles);
    CHECK(ratio > 0.9 && ratio < 1.1);
}

TEST(region_mass_error_admits_what_the_reconstruction_already_lost) {
    const auto model = tree(6, 2, 80);
    const RegionSource sources[]{sourceOf(model)};
    RegionMassOptions options;
    options.cellMetres = 1.5;
    const auto baked = bakeRegionMass(grid(3, 7.0f), sources, options);
    CHECK(!baked.empty());
    const float floorError = float(2.0 * baked.cellMetres);
    for (const auto& cluster : baked.clusters) {
        // No cluster may claim to be a free replacement for the members: the
        // surface moved by a cell before a single triangle was simplified.
        CHECK(cluster.error >= floorError);
        // A cut needs a strict interval at every level, including the roots.
        CHECK(cluster.parentError > cluster.error);
        CHECK(std::isfinite(cluster.error));
    }
    // Coarser cells cannot claim a finer error than finer cells.
    auto coarse = options;
    coarse.cellMetres = 3.0;
    const auto rough = bakeRegionMass(grid(3, 7.0f), sources, coarse);
    CHECK(!rough.empty());
    const auto finest = [](const RegionMass& mass) {
        float best = std::numeric_limits<float>::infinity();
        for (const auto& cluster : mass.clusters) best = std::min(best, cluster.error);
        return best;
    };
    CHECK(finest(rough) > finest(baked));
    CHECK(rough.finestTriangles < baked.finestTriangles);
}

TEST(region_mass_refuses_a_bake_it_cannot_cover_instead_of_dropping_members) {
    const auto model = tree(6, 2, 60);
    const RegionSource sources[]{sourceOf(model)};
    RegionMassOptions options;
    options.cellMetres = 1.2;
    auto members = grid(3, 7.0f);

    auto unknown = members;
    unknown[4].model = 7; // no such source
    CHECK(bakeRegionMass(unknown, sources, options).empty());

    for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(), 0.0f, -1.0f}) {
        auto broken = members;
        broken[2].scale = bad;
        CHECK(bakeRegionMass(broken, sources, options).empty());
    }
    auto nowhere = members;
    nowhere[1].position[1] = std::numeric_limits<float>::infinity();
    CHECK(bakeRegionMass(nowhere, sources, options).empty());
    auto spun = members;
    spun[0].yaw = std::numeric_limits<float>::quiet_NaN();
    CHECK(bakeRegionMass(spun, sources, options).empty());

    // Too few members to be worth aggregating, and an unusable cell.
    CHECK(bakeRegionMass(std::span(members).first(2), sources, options).empty());
    auto noCell = options;
    noCell.cellMetres = 0;
    CHECK(bakeRegionMass(members, sources, noCell).empty());
    // An empty model is refused rather than merged into a region-sized hole.
    const RegionSource nothing[]{{}};
    auto blank = members;
    for (auto& member : blank) member.model = 0;
    CHECK(bakeRegionMass(blank, nothing, options).empty());
}

TEST(region_mass_costs_less_than_the_members_and_keeps_their_materials) {
    const auto model = tree(6, 2, 120, 0.6f, 0.0f, 1.0f);
    const RegionSource sources[]{sourceOf(model)};
    RegionMassOptions options;
    options.cellMetres = 1.5;
    const auto members = grid(3, 7.0f);
    const auto baked = bakeRegionMass(members, sources, options);
    CHECK(!baked.empty());
    const std::size_t individual = members.size() * (model.indices.size() / 3);
    // The finest level is what a frame draws instead of the members; the stored
    // index buffer also holds every coarser level of the DAG.
    CHECK(baked.finestTriangles > 0);
    CHECK(baked.finestTriangles < individual);
    CHECK(baked.triangles() >= baked.finestTriangles);

    // Trunk and leaf layers both survive the merge. A shell that averages them
    // into one number paints the trunks with leaves.
    CHECK_EQ(baked.layer.size(), baked.positions.size() / 3);
    float lowest = std::numeric_limits<float>::infinity(), highest = -lowest;
    for (const auto layer : baked.layer) {
        lowest = std::min(lowest, layer);
        highest = std::max(highest, layer);
    }
    CHECK(lowest >= 0.0f);
    CHECK(highest <= 1.0f);
    CHECK(highest - lowest > 0.25f);
    CHECK_EQ(baked.coverage.size(), baked.positions.size() / 3);
    for (const auto coverage : baked.coverage) {
        CHECK(coverage > 0.0f);
        CHECK(coverage <= 1.0f);
    }
}

TEST(region_mass_bakes_a_whole_forest_region_in_a_usable_time) {
    // What the renderer actually hands it: a 128 m region of woodland, at the
    // density the scatter produces, with a real model's worth of leaves.
    const auto pine = tree(9, 2.4f, 130);
    const RegionSource sources[]{sourceOf(pine)};
    std::vector<RegionMember> members;
    for (int i = 0; i < 300; ++i) {
        const double angle = double(i) * 2.39996;
        const double radius = 62.0 * std::sqrt(double(i) / 300.0);
        RegionMember member;
        member.position[0] = float(64 + std::cos(angle) * radius);
        member.position[1] = float(64 + std::sin(angle) * radius);
        member.position[2] = float((i % 7) * 0.5);
        member.scale = 0.8f + 0.04f * float(i % 6);
        member.yaw = float(i % 9) * 0.7f;
        members.push_back(member);
    }
    for (const double cell : {2.5, 4.0, 6.0, 8.0}) {
        RegionMassOptions options;
        options.cellMetres = cell;
        const auto start = std::chrono::steady_clock::now();
        const auto baked = bakeRegionMass(members, sources, options);
        const double seconds = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
        std::printf("  region bake: cell %.1f m -> %zu vertices, %zu indices, %zu triangles "
                    "(finest %zu), %zu finest clusters, error >= %.1f m, %.2f s\n",
                    cell, baked.positions.size() / 3, baked.indices.size(), baked.triangles(),
                    baked.finestTriangles,
                    [&] {
                        std::size_t count = 0;
                        for (const auto& cluster : baked.clusters)
                            if (cluster.level == 0) ++count;
                        return count;
                    }(),
                    2 * baked.cellMetres, seconds);
        CHECK(!baked.empty());
        // One cluster is one indirect draw record. An aggregate that needs
        // dozens of them has not replaced the objects' draw cost, only their
        // geometry - which is the failure this number exists to catch.
        //
        // Only from the cell the renderer uses upwards. Below it the trees have
        // not merged: the region is still three hundred separate masses, and a
        // cluster cannot span two things that do not touch however large it is
        // allowed to be. That is the measured reason the pass merges at four
        // metres rather than at two and a half.
        std::size_t finestClusters = 0;
        for (const auto& cluster : baked.clusters)
            if (cluster.level == 0) ++finestClusters;
        if (cell >= 4.0) CHECK(finestClusters <= 8);
        // A bake runs on a worker, but one that takes many seconds can never
        // catch a camera up: the region would arrive long after it was wanted.
        CHECK(seconds < 4.0);
    }
}

TEST(region_mass_tier_takes_the_coarsest_representation_the_error_allows) {
    const RegionMassTier tiers[]{{4.0, 16, 6144, 65536},
                                 {8.0, 64, 1536, 16384},
                                 {16.0, 128, 768, 8192}};
    // A finer tier than the error asks for is capacity - and bake time - spent
    // on detail the frame cannot show, so the coarsest that fits is the answer.
    CHECK_EQ(chooseRegionMassTier(tiers, 40.0), 2);
    CHECK_EQ(chooseRegionMassTier(tiers, 32.0), 2);   // exactly two cells fits
    CHECK_EQ(chooseRegionMassTier(tiers, 31.9), 1);
    CHECK_EQ(chooseRegionMassTier(tiers, 16.0), 1);
    CHECK_EQ(chooseRegionMassTier(tiers, 15.9), 0);
    CHECK_EQ(chooseRegionMassTier(tiers, 8.0), 0);
    // Nearer than the finest tier can cover, the region is its own objects.
    // Answering with tier zero anyway would be an aggregate promising an error
    // it does not carry, which is the failure this returns -1 to avoid.
    CHECK_EQ(chooseRegionMassTier(tiers, 7.9), -1);
    CHECK_EQ(chooseRegionMassTier(tiers, 0.0), -1);
    CHECK_EQ(chooseRegionMassTier(tiers, -1.0), -1);
    CHECK_EQ(chooseRegionMassTier({}, 40.0), -1);
    // Order is not rank: the table is a set of sizes, not a sequence.
    const RegionMassTier shuffled[]{{16.0, 128, 768, 8192},
                                    {4.0, 16, 6144, 65536},
                                    {8.0, 64, 1536, 16384}};
    CHECK_EQ(chooseRegionMassTier(shuffled, 40.0), 0);
    CHECK_EQ(chooseRegionMassTier(shuffled, 15.9), 1);
    // A tier with no slots, or no room in one, cannot be chosen however well
    // its error fits.
    const RegionMassTier empty[]{{16.0, 0, 768, 8192}, {8.0, 64, 0, 16384},
                                 {4.0, 16, 6144, 0}, {2.0, 8, 256, 4096}};
    CHECK_EQ(chooseRegionMassTier(empty, 40.0), 3);
}

TEST(region_mass_tier_capacities_hold_a_real_forest_region) {
    // The pool reserves fixed slots, so a capacity is a promise. This bakes the
    // same 300-tree region the timing test uses at each tier's own cell and
    // holds the result against what that tier reserved. A guessed capacity is
    // how a region gets baked, refused and baked again for a whole session,
    // with nothing on screen to say so.
    const auto pine = tree(9, 2.4f, 130);
    const RegionSource sources[]{sourceOf(pine)};
    std::vector<RegionMember> members;
    for (int i = 0; i < 300; ++i) {
        const double angle = double(i) * 2.39996;
        const double radius = 62.0 * std::sqrt(double(i) / 300.0);
        RegionMember member;
        member.position[0] = float(64 + std::cos(angle) * radius);
        member.position[1] = float(64 + std::sin(angle) * radius);
        member.position[2] = float((i % 7) * 0.5);
        member.scale = 0.8f + 0.04f * float(i % 6);
        member.yaw = float(i % 9) * 0.7f;
        members.push_back(member);
    }
    std::size_t bytes = 0, slots = 0;
    for (const auto& tier : defaultRegionMassTiers()) {
        RegionMassOptions options;
        options.cellMetres = tier.cellMetres;
        const auto baked = bakeRegionMass(members, sources, options);
        CHECK(!baked.empty());
        std::printf("  tier %.0f m: %u slots, %zu/%u vertices, %zu/%u indices\n",
                    tier.cellMetres, tier.slots, baked.positions.size() / 3,
                    tier.vertices, baked.indices.size(), tier.indices);
        CHECK(baked.positions.size() / 3 <= tier.vertices);
        CHECK(baked.indices.size() <= tier.indices);
        // The bound a tier is chosen on must cover what its bake carries.
        for (const auto& cluster : baked.clusters)
            if (cluster.level == 0) CHECK(double(cluster.error) <= tier.error());
        bytes += std::size_t(tier.slots) * (std::size_t(tier.vertices) * 76 +
                                            std::size_t(tier.indices) * 4);
        slots += tier.slots;
    }
    std::printf("  pool: %zu slots, %.1f MB\n", slots, double(bytes) / (1024 * 1024));
    // Reserved once and never grown, so this is a cost the session simply pays.
    CHECK(bytes < 64u * 1024 * 1024);
    // The point of tiering: more regions than one size of slot afforded, and
    // never fewer at the finest tier, which is where the demand actually is.
    CHECK(slots >= 128);
    CHECK(defaultRegionMassTiers().front().slots >= 48);
}
