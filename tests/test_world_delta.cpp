#include "framework.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <thread>

#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/chunk_file.hpp"
#include "game/world/object_id.hpp"
#include "game/world/world_delta.hpp"
#include "game/world/world_system.hpp"

namespace {
using namespace world;
using namespace world::delta;
namespace ws = engine::world_store;
using core::Fixed;

constexpr std::uint64_t kSeed = 91;

struct TempDir {
    std::filesystem::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = std::filesystem::temp_directory_path() / ("campfire_delta_" + name + "_" + std::to_string(rd()));
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ws::WorldRoot root() const { return ws::WorldRoot(path / "world"); }
};

// Land, four macro cells a side: 2 km of flat ground at forty metres.
generation::WorldMapData land(std::uint64_t seed = kSeed) {
    generation::WorldMapData map;
    map.width = map.height = 4;
    map.seed = seed;
    map.cells.resize(16);
    for (auto& cell : map.cells) { cell.sea = false; cell.elevation = 40; }
    return map;
}
streaming::PageStore::Config config() {
    streaming::PageStore::Config c;
    c.workerCount = 1;   // no disk cache
    return c;
}

// The ground a brush reads: flat, plus what the delta already holds.
GroundAt groundOf(const WorldDelta& delta) {
    return [layer = delta.heights()](Fixed x, Fixed y) { return Fixed::fromInt(100) + layer->at(x, y); };
}
core::WorldPos at(double x, double y) {
    return {Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)};
}
Brush raise(double radius = 24, double strength = 3) {
    Brush b;
    b.kind = BrushKind::Raise;
    b.radiusMetres = radius;
    b.strength = strength;
    return b;
}

std::unique_ptr<WorldDelta> reopen(const TempDir& dir, LoadReport* report = nullptr) {
    return WorldDelta::open(dir.root(), kSeed, report);
}

// Some of everything, in four files: 0,0  1,0  0,1  1,1.
void editEverywhere(WorldDelta& d) {
    for (const auto& [x, y] : std::vector<std::pair<double, double>>{{1000, 1000}, {9000, 1200}, {1500, 9500}, {9900, 9900}}) {
        d.brush(raise(), groundOf(d), at(x, y), 0.5, Origin::Authoring);
        d.removeObject(0x1000 + std::uint64_t(x), x + 3, y + 3, Origin::Gameplay);
        ecology::Added tree;
        tree.x = x + 40;
        tree.y = y - 20;
        tree.yaw = 0.25f;
        d.plant(tree, Origin::Gameplay);
        ecology::Cell cell;
        cell.canopy = 0.125f;
        cell.biome = ecology::Biome::Marsh;
        d.setEcology(x - 70, y + 70, cell, Origin::Simulation);
    }
    d.clearPage(1100, 1100, 1u << 0 | 1u << 1, Origin::Gameplay);
}

bool sameHeights(const EditLayer& a, const EditLayer& b) {
    for (int level = 0; level < EditLayer::kLevels; ++level) {
        std::set<std::pair<std::int64_t, std::int64_t>> blocks;
        for (const auto& k : a.blockKeys(level)) blocks.insert(k);
        for (const auto& k : b.blockKeys(level)) blocks.insert(k);
        const std::int64_t side = EditLayer::blockSamplesOf(level);
        for (const auto& [bx, by] : blocks)
            for (std::int64_t y = 0; y < side; ++y)
                for (std::int64_t x = 0; x < side; ++x) {
                    const auto sx = bx * side + x, sy = by * side + y;
                    if (a.sample(level, sx, sy) != b.sample(level, sx, sy)) return false;
                }
    }
    return true;
}

// What the files on disk say, read chunk by chunk in the order given - the
// same decoding the loader does, without the loader choosing the order.
std::pair<ecology::Delta, std::unique_ptr<EditLayer>> readInOrder(const ws::WorldRoot& root,
                                                                    std::vector<ws::DeltaEntry> entries) {
    ecology::Delta objects;
    auto heights = std::make_unique<EditLayer>();
    for (const auto& e : entries) {
        const auto bytes = ws::readFileBytes(root.deltaFile(e.file));
        CHECK(bytes.has_value());
        const auto blocks = ws::decodeChunk(*bytes);
        CHECK(blocks.has_value());
        std::string why;
        CHECK(codec::decodeSnapshot(e.key, *blocks, objects, *heights, &why));
        for (const auto& b : *blocks)
            if (b.type == codec::Journal) {
                const auto ops = codec::decodeJournal(e.key, b, &why);
                CHECK(ops.has_value());
                if (ops)
                    for (const auto& op : *ops) codec::applyTo(op, objects, *heights);
            }
    }
    return {std::move(objects), std::move(heights)};
}
} // namespace

TEST(placement_ids_are_named_by_page_and_slot_not_by_the_scatter_that_found_them) {
    const auto id = objectId(1, PlacementStage::Decor, kDecorStableDomain, 3, -4, 17);
    CHECK_EQ(id, objectId(1, PlacementStage::Decor, kDecorStableDomain, 3, -4, 17));
    for (const auto other : {objectId(2, PlacementStage::Decor, kDecorStableDomain, 3, -4, 17),
                             objectId(1, PlacementStage::Planted, kDecorStableDomain, 3, -4, 17),
                             objectId(1, PlacementStage::Decor, kDecorStableDomain + 1, 3, -4, 17),
                             objectId(1, PlacementStage::Decor, kDecorStableDomain, -4, 3, 17),
                             objectId(1, PlacementStage::Decor, kDecorStableDomain, 3, -4, 18)})
        CHECK(other != id);
    std::set<std::uint64_t> seen;
    for (std::int64_t py = -8; py < 8; ++py)
        for (std::int64_t px = -8; px < 8; ++px)
            for (std::uint32_t slot = 0; slot < 256; ++slot)
                seen.insert(objectId(7, PlacementStage::Decor, kDecorStableDomain, px, py, slot));
    CHECK_EQ(seen.size(), std::size_t(16 * 16 * 256));

    // A scattered object's id is recoverable from the 8 m cell it stands in:
    // its page and its slot, nothing about the rectangle that was asked for.
    const decor::LandTest anywhere = [](int, int) { return true; };
    const decor::SiteSample forest = [](double x, double y) { return decor::Site{100 + 0.01 * x, 0, 0.03, 1, 0.4}; };
    const auto scatter = decor::scatter(42, decor::ScatterBounds{-512, 256, 700, 900}, 5000, 5000, anywhere, forest);
    CHECK(!scatter.objects.empty());
    for (const auto& o : scatter.objects) {
        const auto gx = std::int64_t(std::floor(o.x / decor::kCell)), gy = std::int64_t(std::floor(o.y / decor::kCell));
        const auto px = ws::floorDiv(gx, 16), py = ws::floorDiv(gy, 16);
        CHECK_EQ(o.id, objectId(42, PlacementStage::Decor, kDecorStableDomain, px, py,
                                std::uint32_t(ws::floorMod(gx, 16) + 16 * ws::floorMod(gy, 16))));
    }
}

TEST(a_removed_tree_stays_removed_and_a_planted_one_stays_planted_after_reload) {
    TempDir dir("reload");
    std::uint64_t removed = 0, planted = 0;
    const decor::ScatterBounds page{512, 512, 640, 640};
    {
        auto delta = reopen(dir);
        CHECK(delta != nullptr);
        WorldSystem system(config());
        system.attach(delta->objects());
        system.publish(land());
        const auto before = system.read()->scatter(page);
        CHECK(!before.objects.empty());
        const auto victim = before.objects.front();
        removed = victim.id;
        CHECK(delta->removeObject(victim.id, victim.x, victim.y, Origin::Gameplay));
        CHECK(!delta->removeObject(victim.id, victim.x, victim.y, Origin::Gameplay));   // twice is once
        ecology::Added tree;
        tree.x = 600.5;
        tree.y = 530.25;
        tree.model = 1;
        planted = delta->plant(tree, Origin::Gameplay);
        CHECK(planted != 0);
        const auto after = system.read()->scatter(page);
        CHECK(std::none_of(after.objects.begin(), after.objects.end(), [&](const auto& o) { return o.id == removed; }));
        CHECK(std::any_of(after.objects.begin(), after.objects.end(),
                          [&](const auto& o) { return o.id == planted && o.model == 1 && o.x == 600.5; }));
        CHECK_EQ(after.objects.size(), before.objects.size());   // one out, one in
        CHECK(delta->save().ok);
    }
    // Everything unloaded: a new delta, a new world built from the same seed.
    auto delta = reopen(dir);
    CHECK(delta != nullptr);
    WorldSystem system(config());
    system.attach(delta->objects());
    system.publish(land());
    const auto again = system.read()->scatter(page);
    CHECK(std::none_of(again.objects.begin(), again.objects.end(), [&](const auto& o) { return o.id == removed; }));
    CHECK(std::any_of(again.objects.begin(), again.objects.end(), [&](const auto& o) { return o.id == planted; }));
    // A planted object taken away again is simply gone, not "removed".
    CHECK(delta->removeObject(planted, 600.5, 530.25, Origin::Gameplay));
    const auto gone = system.read()->scatter(page);
    CHECK(std::none_of(gone.objects.begin(), gone.objects.end(), [&](const auto& o) { return o.id == planted; }));
    CHECK(!delta->objects()->read()->removed.contains(planted));
}

TEST(a_rebuilt_base_keeps_the_history_made_on_the_old_one) {
    auto delta = std::make_unique<WorldDelta>(kSeed);
    WorldSystem system(config());
    system.attach(delta->objects());
    system.publish(land());
    const auto victim = system.read()->scatter({512, 512, 640, 640}).objects.front();
    delta->removeObject(victim.id, victim.x, victim.y, Origin::Authoring);
    // The editor regenerates the base; the delta is not regenerated with it.
    system.publish(land());
    const auto rebuilt = system.read()->scatter({512, 512, 640, 640});
    CHECK(std::none_of(rebuilt.objects.begin(), rebuilt.objects.end(), [&](const auto& o) { return o.id == victim.id; }));
    // Detached, a new world keeps its own history again.
    system.attach(nullptr);
    system.publish(land());
    const auto fresh = system.read()->scatter({512, 512, 640, 640});
    CHECK(std::any_of(fresh.objects.begin(), fresh.objects.end(), [&](const auto& o) { return o.id == victim.id; }));
}

TEST(terrain_edits_survive_reload_bit_for_bit_from_journal_and_from_snapshot) {
    TempDir dir("terrain");
    std::uint64_t hash = 0;
    {
        auto delta = reopen(dir);
        CHECK(delta->brush(raise(30, 4), groundOf(*delta), at(2000, 2000), 0.7, Origin::Authoring) > 100);
        Brush smooth = raise(20);
        smooth.kind = BrushKind::Smooth;
        delta->brush(smooth, groundOf(*delta), at(2010, 1995), 0.5, Origin::Authoring);
        Brush noise = raise(16, 2);
        noise.kind = BrushKind::Noise;
        delta->brush(noise, groundOf(*delta), at(1990, 2012), 0.3, Origin::Gameplay);
        hash = delta->contentHash();
        const auto first = delta->save();
        CHECK(first.ok);
        CHECK_EQ(first.written, std::size_t(1));
        CHECK_EQ(first.compacted, std::size_t(0));   // a young chunk is its journal
        CHECK_EQ(delta->journal({kFileLevel, 0, 0}).size(), std::size_t(3));
    }
    {
        LoadReport report;
        auto delta = reopen(dir, &report);
        CHECK(!report.fresh);
        CHECK_EQ(report.chunks, std::size_t(1));
        CHECK_EQ(report.ops, std::size_t(3));
        CHECK_EQ(delta->contentHash(), hash);
        const auto ops = delta->journal({kFileLevel, 0, 0});
        CHECK_EQ(ops.size(), std::size_t(3));
        CHECK(ops.size() == 3 && ops[0].origin == Origin::Authoring && ops[2].origin == Origin::Gameplay);
        CHECK(ops.size() == 3 && std::get<TerrainOp>(ops[1].what).tool == BrushKind::Smooth);
        CHECK(ops.size() == 3 && ops[0].sequence < ops[1].sequence && ops[1].sequence < ops[2].sequence);
        delta->compact();
        const auto folded = delta->save();
        CHECK(folded.ok);
        CHECK_EQ(folded.compacted, std::size_t(1));
        CHECK(delta->journal({kFileLevel, 0, 0}).empty());
    }
    auto delta = reopen(dir);
    CHECK_EQ(delta->contentHash(), hash);
    CHECK(delta->journal({kFileLevel, 0, 0}).empty());
    // And the heights themselves, sample for sample, against a delta that
    // made the same strokes and never saw a disk.
    WorldDelta live(kSeed);
    live.brush(raise(30, 4), groundOf(live), at(2000, 2000), 0.7, Origin::Authoring);
    Brush smooth = raise(20);
    smooth.kind = BrushKind::Smooth;
    live.brush(smooth, groundOf(live), at(2010, 1995), 0.5, Origin::Authoring);
    Brush noise = raise(16, 2);
    noise.kind = BrushKind::Noise;
    live.brush(noise, groundOf(live), at(1990, 2012), 0.3, Origin::Gameplay);
    CHECK(sameHeights(*live.heights(), *delta->heights()));
}

TEST(a_brush_the_size_of_a_country_is_kept_at_its_own_scale_and_survives_reload) {
    TempDir dir("coarse");
    std::uint64_t hash = 0;
    std::size_t wide = 0, fine = 0;
    const auto strokes = [&](WorldDelta& d) {
        // Three kilometres across the border of four files, and a small dig
        // into it: two levels, one over the other.
        wide = d.brush(raise(3000, 20), groundOf(d), at(8192, 8192), 0.5, Origin::Authoring);
        Brush dig = raise(20, 3);
        dig.kind = BrushKind::Lower;
        fine = d.brush(dig, groundOf(d), at(8200, 8190), 0.5, Origin::Authoring);
    };
    {
        auto delta = reopen(dir);
        strokes(*delta);
        // At two hundred and fifty-six metres a sample: hundreds of samples,
        // where four metres would have been two million.
        CHECK(wide > 100 && wide < 2000);
        CHECK(fine > 20);
        CHECK(!delta->heights()->blockKeys(3).empty());
        CHECK(!delta->heights()->blockKeys(0).empty());
        // The ground is the two added together: raised where the dig is not,
        // raised less where it is.
        const double raised = delta->heights()->at(Fixed::fromInt(9000), Fixed::fromInt(9000)).toDouble();
        const double dug = delta->heights()->at(Fixed::fromInt(8200), Fixed::fromInt(8190)).toDouble();
        CHECK(raised > 5);
        CHECK(dug < raised - 0.5);
        hash = delta->contentHash();
        CHECK(delta->save().ok);   // journals
    }
    {
        auto delta = reopen(dir);
        CHECK_EQ(delta->contentHash(), hash);
        delta->compact();
        CHECK(delta->save().ok);   // snapshots
    }
    auto delta = reopen(dir);
    CHECK_EQ(delta->contentHash(), hash);
    WorldDelta live(kSeed);
    strokes(live);
    CHECK(sameHeights(*live.heights(), *delta->heights()));
}

TEST(a_long_journal_is_folded_into_the_snapshot_at_save) {
    TempDir dir("fold");
    DeltaOptions options;
    options.journalLimit = 4;
    auto delta = WorldDelta::open(dir.root(), kSeed, nullptr, options);
    for (int i = 0; i < 4; ++i) delta->removeObject(100 + i, 10 + i, 10, Origin::Gameplay);
    CHECK_EQ(delta->save().compacted, std::size_t(0));
    delta->removeObject(200, 20, 20, Origin::Gameplay);
    const auto report = delta->save();
    CHECK_EQ(report.compacted, std::size_t(1));
    CHECK(delta->journal({kFileLevel, 0, 0}).empty());
    const auto hash = delta->contentHash();
    delta.reset();
    CHECK_EQ(reopen(dir)->contentHash(), hash);
}

TEST(an_edit_across_a_file_border_is_committed_in_both_files_or_in_neither) {
    TempDir dir("border");
    auto delta = reopen(dir);
    // Straddles x = 8192, the border of files 0,0 and 1,0.
    delta->brush(raise(40), groundOf(*delta), at(8192, 3000), 0.5, Origin::Authoring);
    const auto left = delta->journal({kFileLevel, 0, 0}), right = delta->journal({kFileLevel, 1, 0});
    CHECK_EQ(left.size(), std::size_t(1));
    CHECK_EQ(right.size(), std::size_t(1));
    CHECK(!left.empty() && !right.empty() && left[0].sequence == right[0].sequence);
    const auto first = delta->save();
    CHECK(first.ok);
    CHECK_EQ(first.written, std::size_t(2));
    const auto committed = delta->contentHash();

    // A second stroke across the border, and a crash between the chunk files
    // and the manifest: the world on disk is the first commit, whole.
    delta->brush(raise(40), groundOf(*delta), at(8190, 3010), 0.5, Origin::Authoring);
    delta->failBeforeCommitForTesting(true);
    CHECK(!delta->save().ok);
    CHECK(delta->dirty());
    delta.reset();
    LoadReport report;
    auto back = reopen(dir, &report);
    CHECK_EQ(back->contentHash(), committed);
    CHECK(report.swept >= 2);   // the two uncommitted files
    CHECK(report.damaged.empty());
}

TEST(chunk_order_does_not_change_the_world_and_the_same_state_makes_the_same_bytes) {
    TempDir dir("order");
    auto delta = reopen(dir);
    editEverywhere(*delta);
    delta->compact();
    CHECK(delta->save().ok);
    const auto manifest = dir.root().readManifest();
    CHECK(manifest && manifest->delta.size() == 4);
    auto forward = manifest->delta, backward = manifest->delta;
    std::reverse(backward.begin(), backward.end());
    const auto a = readInOrder(dir.root(), forward), b = readInOrder(dir.root(), backward);
    CHECK(a.first.sameContent(b.first));
    CHECK(a.first.sameContent(*delta->objects()->read()));
    CHECK(sameHeights(*a.second, *b.second));
    CHECK(sameHeights(*a.second, *delta->heights()));

    // The same edits made in another order across files - each file's own in
    // the same order - compact to the same bytes, file for file.
    TempDir other("order2");
    auto second = WorldDelta::open(other.root(), kSeed);
    for (const auto& [x, y] : std::vector<std::pair<double, double>>{{9900, 9900}, {1500, 9500}, {9000, 1200}, {1000, 1000}}) {
        second->brush(raise(), groundOf(*second), at(x, y), 0.5, Origin::Authoring);
        second->removeObject(0x1000 + std::uint64_t(x), x + 3, y + 3, Origin::Gameplay);
    }
    TempDir third("order3");
    auto first = WorldDelta::open(third.root(), kSeed);
    for (const auto& [x, y] : std::vector<std::pair<double, double>>{{1000, 1000}, {9000, 1200}, {1500, 9500}, {9900, 9900}}) {
        first->brush(raise(), groundOf(*first), at(x, y), 0.5, Origin::Authoring);
        first->removeObject(0x1000 + std::uint64_t(x), x + 3, y + 3, Origin::Gameplay);
    }
    first->compact();
    second->compact();
    CHECK(first->save().ok);
    CHECK(second->save().ok);
    const auto m1 = third.root().readManifest(), m2 = other.root().readManifest();
    CHECK(m1 && m2 && m1->delta.size() == m2->delta.size());
    for (std::size_t i = 0; m1 && m2 && i < m1->delta.size(); ++i) {
        CHECK((m1->delta[i].key == m2->delta[i].key));
        CHECK_EQ(m1->delta[i].payloadHash, m2->delta[i].payloadHash);
    }
    CHECK_EQ(first->contentHash(), second->contentHash());
}

TEST(a_local_edit_rewrites_only_its_own_file) {
    TempDir dir("local");
    auto delta = reopen(dir);
    editEverywhere(*delta);
    CHECK_EQ(delta->save().written, std::size_t(4));
    const auto before = dir.root().readManifest();
    delta->removeObject(0x77, 9100, 1300, Origin::Gameplay);   // file 1,0 only
    const auto report = delta->save();
    CHECK_EQ(report.written, std::size_t(1));
    const auto after = dir.root().readManifest();
    CHECK(before && after && before->delta.size() == after->delta.size());
    for (std::size_t i = 0; before && after && i < after->delta.size(); ++i) {
        const bool edited = after->delta[i].key == ChunkKey{kFileLevel, 1, 0};
        CHECK_EQ(after->delta[i].file == before->delta[i].file, !edited);
    }
    CHECK_EQ(after->commit, before->commit + 1);
    // Nothing changed, nothing written.
    CHECK(delta->save().nothingToDo);
}

TEST(losing_the_derived_cache_loses_nothing_of_the_world) {
    TempDir dir("cache");
    std::uint64_t hash = 0;
    {
        auto delta = reopen(dir);
        editEverywhere(*delta);
        hash = delta->contentHash();
        CHECK(delta->save().ok);
        std::filesystem::create_directories(dir.root().cacheDirectory("terrain"));
        std::ofstream(dir.root().cacheDirectory("terrain") / "0_0.wcache") << "derived";
    }
    std::filesystem::remove_all(dir.root().directory() / "cache");
    CHECK_EQ(reopen(dir)->contentHash(), hash);
}

TEST(writes_that_bypass_the_delta_api_are_saved_all_the_same) {
    TempDir dir("bypass");
    std::uint64_t hash = 0;
    {
        auto delta = reopen(dir);
        CHECK(!delta->dirty());
        // Straight at the views, as older code and the tests of the views do.
        delta->objects()->remove(0xabc, 700, 700);
        applyBrush(*delta->heights(), groundOf(*delta), raise(), at(9000, 9000), 0.5);
        CHECK(delta->dirty());
        const auto chunks = delta->chunks();
        CHECK_EQ(chunks.size(), std::size_t(2));
        CHECK(std::all_of(chunks.begin(), chunks.end(), [](const auto& c) { return c.stale && c.journal == 0; }));
        hash = delta->contentHash();
        const auto report = delta->save();
        CHECK(report.ok);
        CHECK_EQ(report.compacted, std::size_t(2));
        CHECK(!delta->dirty());
        const auto after = delta->chunks();
        CHECK(std::none_of(after.begin(), after.end(), [](const auto& c) { return c.stale; }));
    }
    CHECK_EQ(reopen(dir)->contentHash(), hash);
}

TEST(revision_tokens_go_stale_when_their_file_changes_and_only_then) {
    WorldDelta delta(kSeed);
    const ChunkKey here{kFileLevel, 0, 0}, there{kFileLevel, 3, 3};
    const auto token = delta.token(here);
    delta.removeObject(1, 3 * 8192 + 5, 3 * 8192 + 5, Origin::Gameplay);
    CHECK(delta.current(token));
    // Any finer key of the same file answers for the file.
    CHECK((delta.token({ChunkLevel::RuntimePatch, 5, 5}).key == here));
    delta.removeObject(2, 10, 10, Origin::Gameplay);
    CHECK(!delta.current(token));
    const auto again = delta.token(here);
    delta.objects()->set(20, 20, ecology::Cell{});   // from outside the API
    CHECK(!delta.current(again));
    CHECK(delta.revision(there) > 0);
}

TEST(an_edit_makes_stale_only_what_it_reaches) {
    WorldDelta delta(kSeed);
    delta.drainInvalidations();
    delta.brush(raise(12, 2), groundOf(delta), at(1000, 1000), 0.5, Origin::Gameplay);
    const auto stale = delta.drainInvalidations();
    CHECK(!stale.empty());
    std::uint16_t layers = 0;
    for (const auto& i : stale) {
        layers |= i.layers;
        CHECK(i.sequence != 0);
        CHECK(i.area.overlaps({999, 999, 1001, 1001}));
        // A dozen metres of stroke a metre high reaches tens of metres, and
        // nothing a kilometre off, whatever the layer.
        CHECK(!i.area.overlaps({1900, 900, 2100, 1100}));
        if (i.layers & TerrainGeometry) CHECK(!i.area.overlaps({1030, 900, 1100, 1100}));
    }
    CHECK_EQ(layers, std::uint16_t(TerrainGeometry | Water | EcologyField | Vegetation | NavCollision | Shadows |
                                   Impostors));
    CHECK(delta.drainInvalidations().empty());
    delta.removeObject(5, 1500, 1500, Origin::Gameplay);
    for (const auto& i : delta.drainInvalidations()) {
        CHECK(!(i.layers & (TerrainGeometry | NavCollision)));   // a tree taken is not ground moved
        if (i.layers & Vegetation) CHECK(!i.area.overlaps({1520, 1400, 1600, 1600}));
    }
}

TEST(a_damaged_chunk_costs_that_chunk_not_the_world) {
    TempDir dir("damage");
    std::string damagedFile;
    {
        auto delta = reopen(dir);
        editEverywhere(*delta);
        CHECK(delta->save().ok);
        const auto manifest = dir.root().readManifest();
        damagedFile = manifest->delta[1].file;
        auto bytes = *ws::readFileBytes(dir.root().deltaFile(damagedFile));
        bytes[bytes.size() - 3] ^= 0x40;
        std::ofstream(dir.root().deltaFile(damagedFile), std::ios::binary | std::ios::trunc)
                .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    LoadReport report;
    auto delta = reopen(dir, &report);
    CHECK(delta != nullptr);
    CHECK_EQ(report.damaged.size(), std::size_t(1));
    CHECK_EQ(report.chunks, std::size_t(3));
    const auto chunks = delta->chunks();
    CHECK_EQ(std::count_if(chunks.begin(), chunks.end(), [](const auto& c) { return c.damaged; }), std::ptrdiff_t(1));
    // Untouched, it is not rewritten; edited, what could not be read is kept aside.
    CHECK(delta->save().nothingToDo);
    const auto damagedKey = std::find_if(chunks.begin(), chunks.end(), [](const auto& c) { return c.damaged; })->key;
    delta->removeObject(0x55, double(damagedKey.minX()) + 5, double(damagedKey.minY()) + 5, Origin::Gameplay);
    CHECK(delta->save().ok);
    CHECK(std::filesystem::exists(dir.root().deltaFile(damagedFile + ".damaged")));

    // A manifest that cannot be read is not a new world: nothing is opened,
    // so nothing can be saved over it.
    std::ofstream(dir.root().manifestFile(), std::ios::trunc) << "{ damaged";
    LoadReport broken;
    CHECK(reopen(dir, &broken) == nullptr);
    CHECK_EQ(broken.damaged.size(), std::size_t(1));
}

TEST(a_delta_for_another_seed_or_lattice_is_loaded_with_a_warning) {
    TempDir dir("seed");
    {
        auto delta = reopen(dir);
        delta->removeObject(9, 10, 10, Origin::Gameplay);
        CHECK(delta->save().ok);
    }
    LoadReport report;
    auto other = WorldDelta::open(dir.root(), kSeed + 1, &report);
    CHECK(other != nullptr);
    CHECK_EQ(report.warnings.size(), std::size_t(1));
    CHECK(other->objects()->read()->removed.contains(9));
    other.reset();
    // A delta whose removals were named on another candidate lattice is a
    // migration, and says so rather than quietly matching nothing.
    auto manifest = dir.root().readManifest();
    CHECK(manifest.has_value());
    manifest->stableDomains["decor"] = kDecorStableDomain + 1;
    CHECK(dir.root().writeManifest(*manifest));
    LoadReport migrated;
    CHECK(reopen(dir, &migrated) != nullptr);
    CHECK_EQ(migrated.warnings.size(), std::size_t(1));
    CHECK(!migrated.warnings.empty() && migrated.warnings[0].find("lattice") != std::string::npos);
}

TEST(a_background_save_never_blocks_edits_and_finishes_before_the_delta_goes) {
    TempDir dir("background");
    std::uint64_t hash = 0;
    {
        auto delta = reopen(dir);
        editEverywhere(*delta);
        delta->saveInBackground();
        // Edits go on while it runs; they belong to the next save.
        for (int i = 0; i < 50; ++i) delta->removeObject(5000 + i, 300 + i, 300, Origin::Gameplay);
        CHECK(delta->waitForSave().ok);
        delta->saveInBackground();
        hash = delta->contentHash();
        // Destroyed with a save asked for: it is finished, not dropped.
    }
    CHECK_EQ(reopen(dir)->contentHash(), hash);
}

TEST(clearing_a_page_takes_whole_kinds_of_object_in_one_record) {
    TempDir dir("clear");
    const decor::ScatterBounds page{512, 512, 640, 640};
    auto delta = reopen(dir);
    WorldSystem system(config());
    system.attach(delta->objects());
    system.publish(land());
    const auto before = system.read()->scatter(page);
    CHECK(!before.objects.empty());
    if (before.objects.empty()) return;
    // Whatever kind this page grows most of - on this flat test ground that
    // is not necessarily trees.
    std::uint32_t kind = 0;
    for (std::uint32_t m = 0; m < decor::kModels.size(); ++m)
        if (before.populations[m] > before.populations[kind]) kind = m;
    const auto ofKind = [kind](const decor::Scatter& s) {
        return std::count_if(s.objects.begin(), s.objects.end(), [kind](const auto& o) { return o.model == kind; });
    };
    CHECK(ofKind(before) > 0);
    CHECK(delta->clearPage(600, 600, 1u << kind, Origin::Gameplay));
    CHECK(!delta->clearPage(600, 600, 1u << kind, Origin::Gameplay));   // already clear
    const auto after = system.read()->scatter(page);
    CHECK_EQ(ofKind(after), std::ptrdiff_t(0));
    CHECK_EQ(after.objects.size() + std::size_t(ofKind(before)), before.objects.size());
    CHECK(delta->objects()->read()->removed.empty());   // one record, not one per object
    CHECK(delta->save().ok);
    delta.reset();
    delta = reopen(dir);
    system.attach(delta->objects());
    system.publish(land());
    CHECK_EQ(ofKind(system.read()->scatter(page)), std::ptrdiff_t(0));
}


