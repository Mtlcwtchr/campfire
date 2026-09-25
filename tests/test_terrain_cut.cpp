#include "framework.hpp"
#include <cmath>
#include <set>
#include "game/world/terrain_cut.hpp"
#include "game/world/terrain_view.hpp"

using world::TileId;
using world::terrain::TerrainCut;
namespace {
const std::vector<TileId> roots{{0, 0, 2}};
const auto all = [](TileId) { return true; };
void completeCut(const TerrainCut& cut) {
    std::set<std::int64_t> names;
    double area = 0;
    for (const auto& block : cut.drawing()) {
        CHECK(names.insert(world::tileKeyOf(block.tile)).second);
        CHECK(block.parentMorph >= 0 && block.parentMorph <= 1);
        const double side = world::tileMetresAt(block.tile.lod);
        area += side * side;
    }
    CHECK_EQ(area, double(world::tileMetresAt(2) * world::tileMetresAt(2)));
    for (std::size_t i = 0; i < cut.drawing().size(); ++i)
        for (std::size_t j = i + 1; j < cut.drawing().size(); ++j) {
            const auto a = cut.drawing()[i].tile, b = cut.drawing()[j].tile;
            const auto sa = world::tileMetresAt(a.lod), sb = world::tileMetresAt(b.lod);
            CHECK((a.x + 1) * sa <= b.x * sb || (b.x + 1) * sb <= a.x * sa ||
                  (a.y + 1) * sa <= b.y * sb || (b.y + 1) * sb <= a.y * sa);
        }
}
}
TEST(terrain_cut_never_splits_before_all_children_are_gpu_resident) {
    TerrainCut cut;
    bool fineReady = false;
    const auto ready = [&](TileId tile) { return tile.lod >= 2 || fineReady; };
    cut.update(roots, 0, all, ready, 10);
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
    CHECK_EQ(cut.drawing()[0].tile.lod, 2);
    fineReady = true;
    cut.update(roots, 0, all, ready, 0);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    for (const auto& block : cut.drawing()) CHECK_EQ(block.parentMorph, 1.0f);
    completeCut(cut);
}
TEST(terrain_cut_split_merge_and_direction_reversal_preserve_coverage) {
    TerrainCut cut;
    for (int i = 0; i < 30; ++i) { cut.update(roots, 0, all, all, 0.05); completeCut(cut); }
    CHECK_EQ(cut.drawing().size(), std::size_t(16));
    cut.update(roots, 2, all, all, 0.05);
    CHECK_EQ(cut.drawing().size(), std::size_t(16));
    for (const auto& block : cut.drawing()) CHECK(block.parentMorph > 0);
    for (int i = 0; i < 30; ++i) { cut.update(roots, 0, all, all, 0.05); completeCut(cut); }
    for (int i = 0; i < 30; ++i) { cut.update(roots, 2, all, all, 0.05); completeCut(cut); }
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
    CHECK_EQ(cut.drawing()[0].tile.lod, 2);
}
TEST(terrain_cut_preload_and_invisible_roots_never_become_draw_nodes) {
    TerrainCut cut;
    int requests = 0;
    cut.update(roots, 0, [](TileId) { return false; }, [&](TileId) { ++requests; return true; }, 1);
    CHECK(cut.drawing().empty());
    CHECK_EQ(requests, 0);
}
TEST(terrain_cut_waits_for_a_missing_sibling_and_parent_on_merge) {
    TerrainCut cut;
    const auto missingSibling = [](TileId tile) { return tile.lod == 2 || tile.x != 1 || tile.y != 1; };
    cut.update(roots, 1, all, missingSibling, 1);
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
    cut.update(roots, 1, all, all, 1);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    cut.update(roots, 2, all, [](TileId tile) { return tile.lod != 2; }, 1);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    completeCut(cut);
}

TEST(terrain_cut_budget_rejects_a_whole_family_before_requesting_children) {
    TerrainCut cut;
    int childRequests = 0;
    const auto ready = [&](TileId tile) { if (tile.lod < 2) ++childRequests; return true; };
    cut.update(roots, 1, all, ready, 1, [](TileId) { return false; });
    CHECK_EQ(childRequests, 0);
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
    CHECK(cut.activeParents().empty());
    completeCut(cut);
}

TEST(terrain_cut_active_family_keeps_its_budget_until_reverse_morph_finishes) {
    TerrainCut cut;
    cut.update(roots, 1, all, all, 0.1, all);
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    CHECK_EQ(cut.activeParents().size(), std::size_t(1));
    const float before = cut.drawing()[0].parentMorph;
    cut.update(roots, 1, all, all, 0.05, [](TileId) { return false; });
    CHECK(cut.drawing()[0].parentMorph < before);
    CHECK_EQ(cut.activeParents().size(), std::size_t(1));
    completeCut(cut);
    cut.update(roots, 2, all, all, 0.05, [](TileId) { return false; });
    CHECK_EQ(cut.activeParents().size(), std::size_t(1));
    cut.update(roots, 2, all, all, 1, [](TileId) { return false; });
    CHECK_EQ(cut.drawing().size(), std::size_t(4));
    for (const auto& block : cut.drawing()) CHECK_EQ(block.parentMorph, 1.0f);
    CHECK_EQ(cut.activeParents().size(), std::size_t(1));
    CHECK(cut.needsUpdate());
    completeCut(cut);
    cut.update(roots, 2, all, all, 0, [](TileId) { return false; });
    CHECK(cut.activeParents().empty());
    CHECK_EQ(cut.drawing().size(), std::size_t(1));
    completeCut(cut);
}

TEST(terrain_cut_requests_focus_root_first_and_reprioritizes_after_camera_move) {
    TerrainCut cut;
    const std::vector<TileId> many{{0, 0, 2}, {1, 0, 2}, {2, 0, 2}};
    std::vector<TileId> requests;
    const auto missing = [&](TileId tile) { requests.push_back(tile); return false; };
    double focusX = 2500;
    const auto priority = [&](TileId tile) {
        const double side = world::tileMetresAt(tile.lod);
        return world::terrain::focusDistanceSquared(
            {tile.x * side, tile.y * side, (tile.x + 1) * side, (tile.y + 1) * side}, focusX, 100);
    };
    cut.update(many, 2, all, missing, 0, {}, priority);
    CHECK_EQ(requests.size(), many.size());
    CHECK_EQ(requests.front().x, 2);
    CHECK(cut.drawing().empty());
    requests.clear(); focusX = 100;
    cut.update(many, 2, all, missing, 0, {}, priority);
    CHECK_EQ(requests.front().x, 0);
}

TEST(terrain_cut_admits_near_child_family_before_far_child_family) {
    TerrainCut cut;
    cut.update(roots, 1, all, all, 1); // four fully morphed children
    std::vector<TileId> admitted;
    const auto budget = [&](TileId tile) {
        if (!admitted.empty()) return false;
        admitted.push_back(tile);
        return true;
    };
    const auto priority = [](TileId tile) {
        const double side = world::tileMetresAt(tile.lod);
        return world::terrain::focusDistanceSquared(
            {tile.x * side, tile.y * side, (tile.x + 1) * side, (tile.y + 1) * side}, 900, 900);
    };
    cut.update(roots, 0, all, all, 0, budget, priority);
    CHECK_EQ(admitted.size(), std::size_t(1));
    CHECK_EQ(admitted.front().x, 1);
    CHECK_EQ(admitted.front().y, 1);
    CHECK_EQ(admitted.front().lod, 1);
    completeCut(cut);
}

TEST(terrain_cut_retained_base_survives_culling_split_and_reverse_morph) {
    TerrainCut cut;
    const auto verify = [&] {
        const auto& blocks = cut.coverage();
        double area = 0;
        for (const auto& block : blocks) {
            const auto side = world::tileMetresAt(block.tile.lod);
            area += double(side * side);
        }
        CHECK_EQ(area, double(world::tileMetresAt(2) * world::tileMetresAt(2)));
        for (std::size_t i = 0; i < blocks.size(); ++i)
            for (std::size_t j = i + 1; j < blocks.size(); ++j) {
                const auto a = blocks[i].tile, b = blocks[j].tile;
                const auto sa = world::tileMetresAt(a.lod), sb = world::tileMetresAt(b.lod);
                CHECK((a.x + 1) * sa <= b.x * sb || (b.x + 1) * sb <= a.x * sa ||
                      (a.y + 1) * sa <= b.y * sb || (b.y + 1) * sb <= a.y * sa);
            }
    };
    const auto hidden = [](TileId) { return false; };
    const auto baseOnly = [](TileId tile) { return tile.lod == 2; };
    cut.update(roots, 0, hidden, baseOnly, 0.1, {}, {}, true);
    CHECK(cut.drawing().empty());
    CHECK_EQ(cut.coverage().size(), std::size_t(1));
    CHECK(!cut.needsUpdate());
    verify();
    double x = 100, y = 100;
    const auto visible = [&](TileId tile) {
        const auto side = world::tileMetresAt(tile.lod);
        return x >= tile.x * side && x < (tile.x + 1) * side &&
               y >= tile.y * side && y < (tile.y + 1) * side;
    };
    cut.update(roots, 0, visible, baseOnly, 0.1, {}, {}, true);
    verify();
    for (int i = 0; i < 40; ++i) {
        if (i == 13) { x = 900; y = 900; }
        cut.update(roots, 0, visible, all, 0.05, {}, {}, true);
        verify();
        CHECK(!cut.drawing().empty());
        CHECK(cut.drawing().size() < cut.coverage().size());
    }
    for (int i = 0; i < 30; ++i) {
        cut.update(roots, 2, hidden, all, 0.05, {}, {}, true);
        verify();
        CHECK(cut.drawing().empty());
    }
    CHECK_EQ(cut.coverage().size(), std::size_t(1));
    CHECK(!cut.needsUpdate());
}
