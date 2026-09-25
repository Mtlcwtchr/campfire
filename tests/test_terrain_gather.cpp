#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "engine/render/systems/terrain_gather.hpp"

namespace {
using namespace engine::render;

// A restricted quadtree cut around the camera: fine near it, one level coarser
// per ring out, which is the shape the streaming plan actually produces.
std::vector<TerrainPatch> cut(int rings = 4, double finest = 64) {
    std::vector<TerrainPatch> patches;
    std::uint32_t source = 0;
    for (int ring = 0; ring < rings; ++ring) {
        const double side = finest * std::pow(2.0, ring);
        const double reach = side * 4;
        for (double y = -reach; y < reach; y += side)
            for (double x = -reach; x < reach; x += side) {
                if (ring && std::max(std::abs(x + side * 0.5), std::abs(y + side * 0.5)) < reach * 0.5)
                    continue;   // the finer ring already covers the middle
                TerrainPatch patch;
                patch.originX = x;
                patch.originY = y;
                patch.metres = side;
                patch.level = std::uint32_t(ring);
                patch.lowZ = 0;
                patch.highZ = 40;
                patch.water = std::abs(x) < side && y > 0;
                patch.source = source++;
                patches.push_back(patch);
            }
    }
    return patches;
}

// Camera at the origin looking down +y, z up.
ScreenScale camera(double focal = 900) {
    ScreenScale screen;
    screen.rowX[0] = 2.4f;
    screen.rowY[2] = 2.4f;
    screen.rowW[1] = 1;
    screen.focal = focal;
    return screen;
}
}

TEST(terrain_gather_groups_the_cut_by_level_coarsest_first) {
    const auto patches = cut();
    const auto gathered = gatherTerrain(patches, camera());

    CHECK_EQ(gathered.drawn(), patches.size());
    CHECK_EQ(gathered.rejected, std::size_t(0));
    CHECK_EQ(gathered.levels.size(), std::size_t(4));
    std::size_t covered = 0;
    for (std::size_t i = 0; i < gathered.levels.size(); ++i) {
        const auto& run = gathered.levels[i];
        CHECK(run.count > 0);
        CHECK_EQ(std::size_t(run.first), covered);
        covered += run.count;
        if (i) CHECK(gathered.levels[i - 1].level > run.level);   // coarsest first
        for (std::uint32_t p = 0; p < run.count; ++p)
            CHECK_EQ(gathered.patches[run.first + p].level, run.level);
    }
    CHECK_EQ(covered, gathered.patches.size());
}

TEST(terrain_gather_is_the_same_frame_whichever_way_the_quadtree_was_walked) {
    // The property worth having: the cut's own order is an artefact of the
    // walk, and two machines that walked it differently have to submit the
    // same frame. Shuffle the input and the output must not move.
    auto patches = cut();
    const auto ordered = gatherTerrain(patches, camera());
    std::mt19937 noise(20260916);
    for (int attempt = 0; attempt < 8; ++attempt) {
        std::shuffle(patches.begin(), patches.end(), noise);
        const auto again = gatherTerrain(patches, camera());
        CHECK_EQ(again.patches.size(), ordered.patches.size());
        for (std::size_t i = 0; i < ordered.patches.size(); ++i) {
            CHECK_EQ(again.patches[i].originX, ordered.patches[i].originX);
            CHECK_EQ(again.patches[i].originY, ordered.patches[i].originY);
            CHECK_EQ(again.patches[i].level, ordered.patches[i].level);
            // Including the caller's own index, or the resources a pass looks
            // up beside the patch would follow a different patch.
            CHECK_EQ(again.patches[i].source, ordered.patches[i].source);
        }
        CHECK_EQ(again.water.size(), ordered.water.size());
        for (std::size_t i = 0; i < ordered.water.size(); ++i)
            CHECK_EQ(again.water[i], ordered.water[i]);
    }
}

TEST(terrain_gather_lists_the_water_instead_of_making_a_pass_re_test_for_it) {
    const auto patches = cut();
    const auto gathered = gatherTerrain(patches, camera());
    const auto wet = std::size_t(std::count_if(patches.begin(), patches.end(),
                                               [](const TerrainPatch& p) { return p.water; }));
    CHECK(wet > 0);
    CHECK_EQ(gathered.water.size(), wet);
    // The list is indices into the gathered order, rising, and every one of
    // them really carries water.
    for (std::size_t i = 0; i < gathered.water.size(); ++i) {
        CHECK(gathered.water[i] < gathered.patches.size());
        CHECK(gathered.patches[gathered.water[i]].water);
        if (i) CHECK(gathered.water[i - 1] < gathered.water[i]);
    }
    // And nothing dry is in it.
    std::size_t dry = 0;
    for (std::size_t i = 0; i < gathered.patches.size(); ++i)
        if (!gathered.patches[i].water) ++dry;
    CHECK_EQ(dry + gathered.water.size(), gathered.patches.size());
}

TEST(terrain_gather_measures_a_patch_at_its_middle_not_at_sea_level) {
    // A summit is nearer than the water beside it. Measuring both at zero says
    // they are the same size on screen, which is how a coarse mountain ends up
    // growing grass at a valley's density.
    TerrainPatch low;
    low.originX = 0;
    low.originY = 500;
    low.metres = 64;
    low.lowZ = low.highZ = 0;
    TerrainPatch high = low;
    high.originX = 100;          // to the right, so the sweep orders them
    high.lowZ = 0;
    high.highZ = 400;            // measured 200 m up

    // Looking down +y with z up, raising a patch does not move it nearer, so
    // tilt the view until it does.
    ScreenScale tilted;
    tilted.rowX[0] = 2.4f;
    tilted.rowY[2] = 2.4f;
    tilted.rowW[1] = 0.8f;       // depth leans into y
    tilted.rowW[2] = -0.6f;      // and out of z: height comes towards the eye
    tilted.focal = 900;

    const TerrainPatch both[]{low, high};
    const auto gathered = gatherTerrain(both, tilted);
    CHECK_EQ(gathered.drawn(), std::size_t(2));
    CHECK_EQ(gathered.patches[0].originX, 0.0);
    CHECK_EQ(gathered.patches[1].originX, 100.0);

    const double middle = (500 + 32) * 0.8;      // both patches, at sea level
    CHECK(std::abs(double(gathered.pixelsPerMetre[0]) - 900.0 / middle) < 1e-3);
    CHECK(std::abs(double(gathered.pixelsPerMetre[1]) - 900.0 / (middle - 120)) < 1e-3);
    CHECK(gathered.pixelsPerMetre[1] > gathered.pixelsPerMetre[0]);
}

TEST(terrain_gather_gives_a_patch_behind_the_camera_no_size_rather_than_a_negative_one) {
    // The cut keeps a margin behind the camera, so patches back there are
    // normal. A negative pixels-per-metre would reach whatever divides by it.
    std::vector<TerrainPatch> patches;
    for (int i = -6; i <= 6; ++i) {
        TerrainPatch patch;
        patch.originX = 0;
        patch.originY = double(i) * 200;
        patch.metres = 128;
        patch.lowZ = patch.highZ = 0;
        patch.source = std::uint32_t(i + 6);
        patches.push_back(patch);
    }
    const auto gathered = gatherTerrain(patches, camera());
    CHECK_EQ(gathered.drawn(), patches.size());
    std::size_t sized = 0;
    for (std::size_t i = 0; i < gathered.patches.size(); ++i) {
        CHECK(gathered.pixelsPerMetre[i] >= 0);
        const double middle = gathered.patches[i].originY + 64;
        if (middle > 0) {
            CHECK(std::abs(double(gathered.pixelsPerMetre[i]) - 900.0 / middle) < 1e-3);
            ++sized;
        } else {
            CHECK_EQ(gathered.pixelsPerMetre[i], 0.0f);
        }
    }
    CHECK_EQ(sized, std::size_t(7));

    // An overview has one scale everywhere and no eye to be behind.
    ScreenScale flat;
    flat.rowW[3] = 1;
    flat.focal = 0;
    flat.scale = 0.45;
    const auto overview = gatherTerrain(patches, flat);
    for (const float size : overview.pixelsPerMetre) CHECK(std::abs(double(size) - 0.45) < 1e-6);
}

TEST(terrain_gather_counts_a_square_with_no_extent_rather_than_drawing_it) {
    std::vector<TerrainPatch> patches = cut(2);
    const auto good = patches.size();
    TerrainPatch broken;
    broken.metres = 0;
    patches.push_back(broken);
    broken.metres = -128;
    patches.push_back(broken);
    broken.metres = std::numeric_limits<double>::quiet_NaN();
    patches.push_back(broken);
    broken.metres = 64;
    broken.highZ = -10;          // a range that runs backwards
    patches.push_back(broken);
    broken.highZ = 10;
    broken.originX = std::numeric_limits<double>::infinity();
    patches.push_back(broken);

    const auto gathered = gatherTerrain(patches, camera());
    CHECK_EQ(gathered.rejected, std::size_t(5));
    CHECK_EQ(gathered.drawn(), good);
    for (const float size : gathered.pixelsPerMetre) CHECK(std::isfinite(size));
}

TEST(terrain_gather_of_an_empty_cut_is_empty_rather_than_undefined) {
    const auto gathered = gatherTerrain({}, camera());
    CHECK_EQ(gathered.drawn(), std::size_t(0));
    CHECK_EQ(gathered.levels.size(), std::size_t(0));
    CHECK_EQ(gathered.water.size(), std::size_t(0));
    CHECK_EQ(gathered.area, 0.0);
    CHECK_EQ(gathered.rejected, std::size_t(0));
}

TEST(terrain_gather_reports_the_ground_it_covers_so_a_wasteful_cut_says_so) {
    // Area is what says whether the cut is spending its squares near the camera
    // or far from it: the same square count over four times the ground is a cut
    // that went coarse, and that is a fact about the plan, not the frame time.
    const auto near = gatherTerrain(cut(2, 32), camera());
    const auto wide = gatherTerrain(cut(2, 64), camera());
    CHECK_EQ(near.drawn(), wide.drawn());
    CHECK(std::abs(wide.area - near.area * 4) < 1e-6);
    double summed = 0;
    for (const auto& patch : near.patches) summed += patch.metres * patch.metres;
    CHECK(std::abs(summed - near.area) < 1e-6);
}
