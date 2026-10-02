#include "framework.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

#include "engine/world_source/example_package.hpp"
#include "engine/world_source/package_vectors.hpp"
#include "engine/world_source/png_io.hpp"
#include "engine/world_source/transfer.hpp"

#include <nlohmann/json.hpp>

namespace {
using namespace engine::world_source;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    explicit TempDir(const std::string& name) {
        std::random_device rd;
        path = fs::temp_directory_path() / ("campfire_source_" + name + "_" + std::to_string(rd()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

// 128 km: 4 x 4 source chunks, 512 x 512 samples, the corner chunks sea.
constexpr double kExampleKm = 131.072;

std::optional<Feature> featureOf(const WorldSource& source, const std::string& id) {
    const auto it = source.vectors().find(id);
    if (it == source.vectors().end()) return std::nullopt;
    std::vector<Fragment> parts;
    for (const auto& key : it->second.chunks) {
        const auto chunk = source.read(key);
        if (!chunk) return std::nullopt;
        for (const auto& f : chunk->fragments)
            if (f.id == id) parts.push_back(f);
    }
    return mergeFragments(parts);
}

bool sameSource(const WorldSource& a, const WorldSource& b) {
    if (a.chunks().size() != b.chunks().size()) return false;
    for (const auto& [key, record] : a.chunks()) {
        const auto other = b.chunks().find(key);
        if (other == b.chunks().end() || other->second.layers != record.layers) return false;
    }
    if (a.vectors().size() != b.vectors().size()) return false;
    for (const auto& [id, entry] : a.vectors())
        if (featureOf(a, id) != featureOf(b, id)) return false;
    return true;
}

} // namespace

TEST(world_source_png_16_bit_round_trip) {
    TempDir dir("png");
    Image image{67, 33, 4, 16, {}, {}};
    image.allocate();
    std::mt19937 rng(5);
    for (auto& w : image.words) w = std::uint16_t(rng());
    CHECK(writePng(dir.path / "a.png", image));
    const auto back = readPng(dir.path / "a.png");
    CHECK(back.has_value());
    CHECK_EQ(back->width, image.width);
    CHECK_EQ(int(back->bits), 16);
    CHECK(back->words == image.words);
}

TEST(world_source_import_keeps_only_land) {
    TempDir dir("land");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    const auto report = importPackage(dir.path / "package", dir.path / "source", &why);
    CHECK(report.has_value());
    CHECK(report->created);
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    // Sea chunks have no file and read back as every layer's default.
    CHECK(source->chunks().size() < 16);
    CHECK(!source->chunks().count(ChunkKey{ChunkLevel::SourceChunk, 0, 0}));
    CHECK(!fs::exists(dir.path / "source" / "chunks" / WorldSource::fileName({ChunkLevel::SourceChunk, 0, 0})));
    const auto sea = source->read({ChunkLevel::SourceChunk, 0, 0});
    CHECK(sea.has_value() && sea->empty());
    const auto height = source->tile(*sea, "height");
    CHECK(height.isUniform());
    CHECK_EQ(height.at(5, 5, 0), source->schema().raster("height")->defaultStored(0));
    CHECK(source->chunks().count(ChunkKey{ChunkLevel::SourceChunk, 1, 1}));
}

TEST(world_source_export_then_import_is_identity) {
    TempDir dir("round");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    CHECK(importPackage(dir.path / "package", dir.path / "a", &why).has_value());
    const auto a = WorldSource::open(dir.path / "a", &why);
    CHECK(a.has_value());
    CHECK(exportPackage(dir.path / "a", dir.path / "out", {}, &why).has_value());
    // Into a fresh source: the same source.
    CHECK(importPackage(dir.path / "out", dir.path / "b", &why).has_value());
    const auto b = WorldSource::open(dir.path / "b", &why);
    CHECK(b.has_value());
    CHECK(sameSource(*a, *b));
    // Back into the one it came from: nothing to write.
    const auto again = importPackage(dir.path / "out", dir.path / "a", &why);
    CHECK(again.has_value());
    CHECK_EQ(again->chunksWritten, std::size_t(0));
    CHECK_EQ(again->featuresChanged + again->featuresAdded + again->featuresRemoved, std::size_t(0));
    CHECK(again->dirty.empty());
}

TEST(world_source_features_keep_their_ids_across_chunks) {
    TempDir dir("ids");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    const auto authored = readVectorFile(dir.path / "package" / "vectors" / "rivers.json", "rivers");
    CHECK(authored.has_value() && authored->size() == 1);
    CHECK(source->vectors().at("river:amber").chunks.size() > 1);
    CHECK(featureOf(*source, "river:amber") == authored->front());
    // The region outline covers chunks only by its interior too.
    CHECK(source->vectors().at("region:north").chunks.size() >= 2);
    CHECK(featureOf(*source, "poi:ford").has_value());
}

TEST(world_source_one_sample_rewrites_one_chunk) {
    TempDir dir("partial");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    auto height = readPng(dir.path / "package" / "raster" / "height.png");
    CHECK(height.has_value());
    // Sample (200, 200) is in chunk (1, 1), on land.
    height->set(200, 200, 0, std::uint16_t(height->at(200, 200, 0) + 100));
    CHECK(writePng(dir.path / "package" / "raster" / "height.png", *height));
    const auto report = importPackage(dir.path / "package", dir.path / "source", &why);
    CHECK(report.has_value());
    CHECK_EQ(report->chunksWritten, std::size_t(1));
    const ChunkKey key{ChunkLevel::SourceChunk, 1, 1};
    CHECK_EQ(report->changed.size(), std::size_t(1));
    CHECK(report->changed.count(key) && report->changed.at(key) == std::set<std::string>{"height"});
    CHECK(report->dirty.count(key) && report->dirty.at(key).count("terrain"));
}

TEST(world_source_partial_package_removes_what_it_leaves_out) {
    TempDir dir("delete");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    // A rectangle around the pond only: export, drop the pond, import.
    ExportOptions options;
    options.rect = std::array<double, 4>{0.55 * kExampleKm * 1000, 0.55 * kExampleKm * 1000,
                                         0.59 * kExampleKm * 1000, 0.59 * kExampleKm * 1000};
    CHECK(exportPackage(dir.path / "source", dir.path / "part", options, &why).has_value());
    auto lakes = readVectorFile(dir.path / "part" / "vectors" / "lakes.json", "lakes");
    CHECK(lakes.has_value());
    std::erase_if(*lakes, [](const Feature& f) { return f.id == "lake:pond"; });
    CHECK(writeVectorFile(dir.path / "part" / "vectors" / "lakes.json", "lakes", *lakes));
    const auto report = importPackage(dir.path / "part", dir.path / "source", &why);
    CHECK(report.has_value());
    CHECK_EQ(report->featuresRemoved, std::size_t(1));
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    CHECK(!source->vectors().count("lake:pond"));
    CHECK(source->vectors().count("lake:mere"));
    CHECK(source->vectors().count("river:amber"));
}

TEST(world_source_categorical_layers_are_not_blended) {
    TempDir dir("categorical");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    // Region ids at half the resolution: resampled, they must stay ids.
    const auto full = readPng(dir.path / "package" / "raster" / "region_ids.png");
    CHECK(full.has_value());
    Image half{full->width / 2, full->height / 2, 1, 16, {}, {}};
    half.allocate();
    for (std::uint32_t y = 0; y < half.height; ++y)
        for (std::uint32_t x = 0; x < half.width; ++x) half.set(x, y, 0, full->at(2 * x, 2 * y, 0));
    CHECK(writePng(dir.path / "package" / "raster" / "region_ids.png", half));
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    std::set<std::uint16_t> seen;
    for (const auto& [key, record] : source->chunks()) {
        const auto chunk = source->read(key);
        CHECK(chunk.has_value());
        const auto ids = source->tile(*chunk, "region_ids");
        for (std::int64_t y = 0; y < kChunkSamples; ++y)
            for (std::int64_t x = 0; x < kChunkSamples; ++x) seen.insert(ids.at(x, y, 0));
    }
    for (const auto id : seen) CHECK(id <= 4);
    CHECK(seen.count(1) && seen.count(4));
    CHECK(!source->regions().at("region_ids").empty());
}

TEST(world_source_detail_edits_travel_with_the_package) {
    TempDir dir("details");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    // A hand edit in chunk (1, 1), as engine/biomes DetailEdits writes it.
    fs::create_directories(dir.path / "source" / "details");
    std::ofstream(dir.path / "source" / "details" / "+0001_+0001.json") << R"({"format":"campfire.details","pinned":[]})";
    ExportOptions options;
    CHECK(exportPackage(dir.path / "source", dir.path / "out", options, &why).has_value());
    CHECK(fs::exists(dir.path / "out" / "details" / "+0001_+0001.json"));
    CHECK(importPackage(dir.path / "out", dir.path / "again", &why).has_value());
    CHECK(fs::exists(dir.path / "again" / "details" / "+0001_+0001.json"));
    // A package without it says there is none there now.
    fs::remove_all(dir.path / "out" / "details");
    CHECK(importPackage(dir.path / "out", dir.path / "again", &why).has_value());
    CHECK(!fs::exists(dir.path / "again" / "details" / "+0001_+0001.json"));
}

namespace {
// The example package's region ids, given a legend.
void nameRegions(const fs::path& package, const std::map<std::string, std::string>& ids) {
    std::ifstream in(package / "world.json");
    auto j = nlohmann::json::parse(in);
    in.close();
    j["rasters"]["region_ids"]["ids"] = ids;
    std::ofstream(package / "world.json") << j.dump(1);
}
// Every id the layer holds, sample by sample, as a histogram.
std::map<std::uint16_t, std::size_t> idsIn(const WorldSource& source, const std::string& layer) {
    std::map<std::uint16_t, std::size_t> seen;
    for (const auto& [key, record] : source.chunks()) {
        const auto chunk = source.read(key);
        const auto ids = source.tile(*chunk, layer);
        for (std::int64_t y = 0; y < kChunkSamples; ++y)
            for (std::int64_t x = 0; x < kChunkSamples; ++x) ++seen[ids.at(x, y, 0)];
    }
    return seen;
}
} // namespace

TEST(world_source_categorical_ids_are_renumbered_by_name) {
    TempDir dir("legend");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    nameRegions(dir.path / "package", {{"1", "fertile"}, {"2", "sparse"}, {"3", "rough"}, {"4", "rock"}});
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    CHECK_EQ(source->schema().raster("region_ids")->ids.size(), std::size_t(4));
    CHECK(source->schema().raster("region_ids")->idNamed("rock") == std::optional<std::uint32_t>(4));
    const auto before = idsIn(*source, "region_ids");

    // The same picture with the numbers turned round: by name it is the same ground.
    nameRegions(dir.path / "package", {{"1", "rock"}, {"2", "rough"}, {"3", "sparse"}, {"4", "fertile"}});
    const auto report = importPackage(dir.path / "package", dir.path / "source", &why);
    CHECK(report.has_value());
    CHECK(report->renumbered.count("region_ids"));
    CHECK_EQ(report->renumbered.at("region_ids").at(1), std::uint32_t(4));
    source = WorldSource::open(dir.path / "source", &why);
    const auto after = idsIn(*source, "region_ids");
    // Id 1 of the package is now 4 of the source, and so on round.
    CHECK_EQ(after.count(1) ? after.at(1) : 0, before.count(4) ? before.at(4) : 0);
    CHECK_EQ(after.count(4) ? after.at(4) : 0, before.count(1) ? before.at(1) : 0);
    CHECK_EQ(after.count(0) ? after.at(0) : 0, before.count(0) ? before.at(0) : 0);

    // A new name takes the next free id; the source's legend grows.
    nameRegions(dir.path / "package", {{"1", "fertile"}, {"2", "sparse"}, {"3", "rough"}, {"4", "wastes"}});
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    source = WorldSource::open(dir.path / "source", &why);
    CHECK(source->schema().raster("region_ids")->idNamed("wastes") == std::optional<std::uint32_t>(5));
    CHECK(source->schema().raster("region_ids")->idNamed("rock") == std::optional<std::uint32_t>(4));

    // Against the registry's numbering, a name it does not know is refused.
    ImportTarget target;
    target.legends["region_ids"] = {{"fertile", 1}, {"sparse", 2}, {"rough", 3}, {"rock", 4}};
    why.clear();
    CHECK(!importPackage(dir.path / "package", dir.path / "source", target, &why).has_value());
    CHECK(why.find("wastes") != std::string::npos && why.find("known:") != std::string::npos);
}

namespace {
// A grey picture, 16 bits, `value(x, y)` at every pixel.
template <class F>
bool writeGrey(const fs::path& file, std::uint32_t w, std::uint32_t h, F value) {
    Image image{w, h, 1, 16, {}, {}};
    image.allocate();
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) image.set(x, y, 0, value(x, y));
    return writePng(file, image);
}
// A world of `regions` x `regions` editor regions (131 072 m each).
WorldExtent regionsWorld(int regions) { return {131072.0 * regions, 131072.0 * regions, 256, 32768}; }
double heightAt(const WorldSource& source, std::int64_t sx, std::int64_t sy) {
    const auto key = WorldSource::chunkOfSample(sx, sy);
    const auto chunk = source.read(key);
    const auto tile = source.tile(*chunk, "height");
    return source.schema().raster("height")->decode(0, tile.at(sx - key.x * kChunkSamples, sy - key.y * kChunkSamples, 0));
}
} // namespace

TEST(world_source_loose_picture_lands_in_the_selected_region_only) {
    TempDir dir("loose");
    std::string why;
    // Flat grey at half the range, no sea in it: 1500 m of a coast at a
    // metre and white at 3000.
    CHECK(writeGrey(dir.path / "h.png", 300, 300, [](auto, auto) { return std::uint16_t(32768); }));
    ImportTarget target;
    target.world = regionsWorld(2);
    target.rect = std::array<double, 4>{131072, 0, 262144, 131072};     // region (1, 0)
    target.mask = {*target.rect};
    LooseImages images;
    images.height = dir.path / "h.png";
    images.seaGrey = 0;
    const auto report = importImages(images, dir.path / "source", target, &why);
    CHECK(report.has_value());
    CHECK(report->created);
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    // Four chunks a side of region (1, 0), and nothing anywhere else.
    CHECK_EQ(source->chunks().size(), std::size_t(16));
    for (const auto& [key, record] : source->chunks()) CHECK(key.x >= 4 && key.x < 8 && key.y >= 0 && key.y < 4);
    CHECK(std::abs(heightAt(*source, 700, 100) - 1500.0) < 1.0);
    CHECK(std::abs(heightAt(*source, 100, 100) - -60.0) < 1.0);
}

TEST(world_source_feather_blends_the_selection_edge_into_what_was_there) {
    TempDir dir("feather");
    std::string why;
    CHECK(writeGrey(dir.path / "low.png", 64, 64, [](auto, auto) { return std::uint16_t(0); }));
    CHECK(writeGrey(dir.path / "high.png", 64, 64, [](auto, auto) { return std::uint16_t(65535); }));
    ImportTarget whole;
    whole.world = regionsWorld(2);
    whole.rect = std::array<double, 4>{0, 0, 262144, 262144};
    LooseImages images;
    images.seaGrey = 0;   // black is the coast: land at a metre everywhere
    images.highMetres = 1100;
    images.height = dir.path / "low.png";
    CHECK(importImages(images, dir.path / "source", whole, &why).has_value());
    // Region (0, 0) raised to 1100 m, blended over 8 km inside its edge.
    ImportTarget piece;
    piece.world = whole.world;
    piece.rect = std::array<double, 4>{0, 0, 131072, 131072};
    piece.mask = {*piece.rect};
    piece.featherMetres = 8192;
    images.height = dir.path / "high.png";
    CHECK(importImages(images, dir.path / "source", piece, &why).has_value());
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    CHECK(std::abs(heightAt(*source, 200, 200) - 1100.0) < 1.0);    // deep inside
    CHECK(std::abs(heightAt(*source, 600, 200) - 1.0) < 1.0);       // the next region: untouched
    const double edge = heightAt(*source, 510, 200), band = heightAt(*source, 500, 200);
    CHECK(edge > 1.0 && edge < 400.0);                                // next to the edge: mostly old
    CHECK(band > edge && band < 1100.0);                              // further in: more of the new
    CHECK(std::abs(heightAt(*source, 200, 0) - 1100.0) < 1.0);      // the world's border is not blended
}

TEST(world_source_clear_and_reshape_keep_the_rest) {
    TempDir dir("clear");
    std::string why;
    CHECK(writeGrey(dir.path / "h.png", 32, 32, [](auto x, auto) { return std::uint16_t(x < 16 ? 40000 : 50000); }));
    ImportTarget target;
    target.world = regionsWorld(2);
    target.rect = std::array<double, 4>{0, 0, 262144, 131072};         // the top two regions
    LooseImages images;
    images.height = dir.path / "h.png";
    CHECK(importImages(images, dir.path / "source", target, &why).has_value());
    const double east = [&] { return heightAt(*WorldSource::open(dir.path / "source"), 700, 100); }();
    CHECK(clearRasters(dir.path / "source", {{0, 0, 131072, 131072}}, &why).has_value());
    {
        const auto source = WorldSource::open(dir.path / "source", &why);
        CHECK_EQ(source->chunks().size(), std::size_t(16));             // region (0, 0) is sea again
        CHECK(std::abs(heightAt(*source, 100, 100) - -60.0) < 1.0);
    }
    // One region more in the west and one in the north: the ground moves with it.
    CHECK(reshapeSource(dir.path / "source", 4, 4, regionsWorld(3), &why));
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source.has_value());
    CHECK(source->schema().world == regionsWorld(3));
    CHECK_EQ(source->chunks().size(), std::size_t(16));
    CHECK(std::abs(heightAt(*source, 700 + 512, 100 + 512) - east) < 0.01);
}

TEST(world_source_a_category_brush_paints_land_only) {
    TempDir dir("paint");
    std::string why;
    CHECK(writeExamplePackage(dir.path / "package", kExampleKm, &why).has_value());
    CHECK(importPackage(dir.path / "package", dir.path / "source", &why).has_value());
    // A disc over the island's middle and one over the open-sea corner.
    const double mid = kExampleKm * 500.0;
    const auto report = paintCategorical(dir.path / "source", {kExampleKm * 1000, kExampleKm * 1000, 256, 32768},
                                         "ground", 6, {{mid, mid, 3000}, {500, 500, 3000}}, {{"wastes", 6}}, true, &why);
    CHECK(report.has_value());
    CHECK(report && report->chunksWritten >= 1);
    const auto source = WorldSource::open(dir.path / "source", &why);
    CHECK(source && source->schema().raster("ground"));
    CHECK(source->schema().raster("ground")->idNamed("wastes") == std::optional<std::uint32_t>(6));
    const auto ids = idsIn(*source, "ground");
    CHECK(ids.count(6) && ids.at(6) > 300);   // most of a 3 km disc of 256 m samples
    // The sea corner kept nothing: its chunk was never made.
    CHECK(!source->chunks().count({ChunkLevel::SourceChunk, 0, 0}) ||
          !source->chunks().at({ChunkLevel::SourceChunk, 0, 0}).layers.count("ground"));
}
