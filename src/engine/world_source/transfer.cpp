#include "engine/world_source/transfer.hpp"

#include <cstdio>

#include <algorithm>
#include <cmath>
#include <map>

#include "engine/core/progress.hpp"
#include "engine/world_source/package_vectors.hpp"
#include "engine/world_source/png_io.hpp"
#include "engine/world_store/atomic_file.hpp"

namespace engine::world_source {
namespace {

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

std::string stem(const ChunkKey& k) {
    const auto name = WorldSource::fileName(k);
    return name.substr(0, name.size() - 7);
}

// What goes stale when a layer changes (§10). A control channel may name its
// own; otherwise what its kind moves.
std::set<std::string> affectedBy(const Schema& schema, const std::string& layer) {
    if (layer == "vectors" || layer == "poi") return {};
    const auto* d = schema.raster(layer);
    if (!d) return {};
    switch (d->kind) {
        case RasterKind::Height: return {"terrain", "drainage", "soil", "ecology"};
        case RasterKind::Categorical: return {"regions", "ecology"};
        case RasterKind::Flags: return {"terrain", "ecology"};
        case RasterKind::Control: {
            std::set<std::string> out;
            for (const auto& c : d->channels) out.insert(c.affects.begin(), c.affects.end());
            if (out.empty()) out = {"ecology", "vegetation"};
            return out;
        }
    }
    return {};
}
std::set<std::string> affectedByFeature(const std::string& kind) {
    if (kind == "rivers") return {"hydrology", "terrain", "wetlands"};
    if (kind == "lakes") return {"hydrology", "terrain", "wetlands"};
    if (kind == "ridges") return {"terrain", "drainage"};
    if (kind == "coastline") return {"terrain", "drainage"};
    if (kind == "wetlands") return {"hydrology", "ecology"};
    if (kind == "regions") return {"regions", "ecology"};
    if (kind == "roads") return {"travel"};
    if (kind == "poi") return {"settlement"};
    return {kind};
}

// A package raster, resampled onto the samples of the rectangle it covers,
// in the source layer's own encoding.
constexpr std::uint16_t kUnnamed = 0xFFFF;

struct Source2D {
    const Image* image = nullptr;
    const RasterDesc* from = nullptr;   // the package's description
    const RasterDesc* to = nullptr;     // the source's
    std::int64_t samplesX = 0, samplesY = 0;
    bool direct = false;
    // Categorical: the package's id -> the source's, by the legends' names;
    // ids the package's legend does not name become the default. Empty: as stored.
    std::vector<std::uint16_t> renumber;
    std::size_t* unnamed = nullptr;

    // The package's stored value at image pixel (px, py), channel c, widened
    // or narrowed to the package layer's own depth.
    [[nodiscard]] double pixel(std::int64_t px, std::int64_t py, std::uint8_t c) const {
        px = std::clamp<std::int64_t>(px, 0, image->width - 1);
        py = std::clamp<std::int64_t>(py, 0, image->height - 1);
        const std::uint8_t channel = std::min<std::uint8_t>(c, std::uint8_t(image->channels - 1));
        if (c >= image->channels) return from->defaultStored(c);   // RGB without alpha: the default
        double v = image->at(std::uint32_t(px), std::uint32_t(py), channel);
        const double imageMax = image->maximum(), layerMax = from->maximum();
        if (imageMax != layerMax) v = std::round(v * layerMax / imageMax);
        return v;
    }
    // Sample (i, j) of the rectangle, channel c, in the source encoding.
    [[nodiscard]] std::uint16_t at(std::int64_t i, std::int64_t j, std::uint8_t c) const {
        double stored;
        if (direct) {
            stored = pixel(i, j, c);
        } else {
            const double u = (double(i) + 0.5) * image->width / double(samplesX) - 0.5;
            const double v = (double(j) + 0.5) * image->height / double(samplesY) - 0.5;
            const auto mode = from->channels[c].interpolation;
            if (mode == Interpolation::Nearest || mode == Interpolation::Exact) {
                stored = pixel(std::int64_t(std::lround(u)), std::int64_t(std::lround(v)), c);
            } else if (mode == Interpolation::Bilinear) {
                const auto x0 = std::int64_t(std::floor(u)), y0 = std::int64_t(std::floor(v));
                const double fx = u - double(x0), fy = v - double(y0);
                const double a = pixel(x0, y0, c) + (pixel(x0 + 1, y0, c) - pixel(x0, y0, c)) * fx;
                const double b = pixel(x0, y0 + 1, c) + (pixel(x0 + 1, y0 + 1, c) - pixel(x0, y0 + 1, c)) * fx;
                stored = a + (b - a) * fy;
            } else {
                // Catmull-Rom, clamped to the stored range.
                const auto x0 = std::int64_t(std::floor(u)), y0 = std::int64_t(std::floor(v));
                const double fx = u - double(x0), fy = v - double(y0);
                const auto cubic = [](double p0, double p1, double p2, double p3, double t) {
                    return p1 + 0.5 * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
                };
                double rows[4];
                for (int k = 0; k < 4; ++k) {
                    const auto y = y0 - 1 + k;
                    rows[k] = cubic(pixel(x0 - 1, y, c), pixel(x0, y, c), pixel(x0 + 1, y, c), pixel(x0 + 2, y, c), fx);
                }
                stored = std::clamp(cubic(rows[0], rows[1], rows[2], rows[3], fy), 0.0, double(from->maximum()));
            }
        }
        // Into the source's encoding by what the value means - a copy when the
        // two describe the layer the same way.
        if (from->kind == RasterKind::Categorical && !renumber.empty()) {
            const auto id = std::size_t(std::lround(stored));
            if (id < renumber.size() && renumber[id] != kUnnamed) return renumber[id];
            if (unnamed) ++*unnamed;
            return to->defaultStored(c);
        }
        if (from->channels[c] == to->channels[c] && from->type == to->type) return std::uint16_t(std::lround(stored));
        if (from->kind == RasterKind::Categorical || from->kind == RasterKind::Flags) return std::uint16_t(std::lround(stored));
        return to->encode(c, from->decode(c, std::uint16_t(std::lround(stored))));
    }
};

bool compatible(const RasterDesc& a, const RasterDesc& b) {
    return a.kind == b.kind && channelsOf(a.type) == channelsOf(b.type);
}

// A feature as the source holds it now, from every chunk it is in.
std::optional<Feature> storedFeature(const WorldSource& source, const std::string& id,
                                     std::map<ChunkKey, Chunk>& cache, std::string* why) {
    const auto entry = source.vectors().find(id);
    if (entry == source.vectors().end()) return std::nullopt;
    std::vector<Fragment> parts;
    for (const auto& key : entry->second.chunks) {
        auto it = cache.find(key);
        if (it == cache.end()) {
            auto chunk = source.read(key, why);
            if (!chunk) return std::nullopt;
            it = cache.emplace(key, std::move(*chunk)).first;
        }
        for (const auto& f : it->second.fragments) if (f.id == id) parts.push_back(f);
    }
    if (parts.empty()) return std::nullopt;
    return mergeFragments(parts, why);
}

} // namespace

nlohmann::json ImportReport::json() const {
    nlohmann::json j{{"created", created}, {"chunks_written", chunksWritten}, {"chunks_removed", chunksRemoved},
                     {"chunks_unchanged", chunksUnchanged}, {"features_added", featuresAdded},
                     {"features_changed", featuresChanged}, {"features_removed", featuresRemoved},
                     {"features_unchanged", featuresUnchanged}};
    j["changed"] = nlohmann::json::object();
    for (const auto& [k, layers] : changed) j["changed"][stem(k)] = layers;
    j["dirty"] = nlohmann::json::object();
    for (const auto& [k, systems] : dirty) j["dirty"][stem(k)] = systems;
    if (!renumbered.empty()) {
        j["renumbered"] = nlohmann::json::object();
        for (const auto& [layer, m] : renumbered) {
            auto& out = j["renumbered"][layer];
            out = nlohmann::json::object();
            for (const auto& [a, b] : m) out[std::to_string(a)] = b;
        }
    }
    if (!unnamed.empty()) j["unnamed_samples"] = unnamed;
    return j;
}

namespace {

// How much of the package each sample of the rectangle takes, 0..255: all of
// it inside the mask, none outside, and a smooth step across the feather band
// inside the mask's edge. Empty: all of it everywhere.
std::vector<std::uint8_t> maskWeights(const ImportTarget& target, const WorldExtent& world, std::int64_t sx0,
                                      std::int64_t sy0, std::int64_t sw, std::int64_t sh) {
    if (target.mask.empty()) return {};
    const double sm = world.sampleMetres;
    // The mask over a box of samples, painted rectangle by rectangle: a sample
    // is in when its centre is. (Asking every rectangle about every sample was
    // a whole world's samples times its 256 regions.)
    const auto paint = [&](std::int64_t bx, std::int64_t by, std::int64_t bw, std::int64_t bh) {
        std::vector<std::uint8_t> in(std::size_t(bw * bh), 0);
        for (const auto& r : target.mask) {
            const auto x0 = std::max(bx, std::int64_t(std::ceil(r[0] / sm - 0.5)));
            const auto x1 = std::min(bx + bw, std::int64_t(std::ceil(r[2] / sm - 0.5)));
            const auto y0 = std::max(by, std::int64_t(std::ceil(r[1] / sm - 0.5)));
            const auto y1 = std::min(by + bh, std::int64_t(std::ceil(r[3] / sm - 0.5)));
            for (auto y = y0; y < y1; ++y)
                std::fill_n(in.begin() + std::ptrdiff_t((y - by) * bw + (x0 - bx)), std::max<std::int64_t>(0, x1 - x0), 1);
        }
        return in;
    };
    if (!(target.featherMetres > 0)) {
        auto weight = paint(sx0, sy0, sw, sh);
        for (auto& w : weight) w = w ? 255 : 0;
        return weight;
    }
    std::vector<std::uint8_t> weight(std::size_t(sw * sh), 0);
    // A chamfer distance (3 across, 4 diagonally) to the nearest sample that
    // is in the world and not in the mask, over the rectangle and a band
    // around it: past the world's edge there is nothing to blend with.
    const std::int64_t pad = std::int64_t(std::ceil(target.featherMetres / sm)) + 1;
    const std::int64_t px0 = std::max<std::int64_t>(0, sx0 - pad), py0 = std::max<std::int64_t>(0, sy0 - pad);
    const std::int64_t px1 = std::min(world.samplesX(), sx0 + sw + pad), py1 = std::min(world.samplesY(), sy0 + sh + pad);
    const std::int64_t pw = px1 - px0, ph = py1 - py0;
    constexpr std::uint16_t kFar = 65535;
    const auto in = paint(px0, py0, pw, ph);
    std::vector<std::uint16_t> d(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) d[i] = in[i] ? kFar : 0;
    const auto relax = [&](std::int64_t x, std::int64_t y, std::int64_t nx, std::int64_t ny, int step) {
        if (nx < 0 || ny < 0 || nx >= pw || ny >= ph) return;
        auto& here = d[std::size_t(y * pw + x)];
        here = std::uint16_t(std::min<int>(here, int(d[std::size_t(ny * pw + nx)]) + step));
    };
    for (std::int64_t y = 0; y < ph; ++y)
        for (std::int64_t x = 0; x < pw; ++x) {
            relax(x, y, x - 1, y, 3); relax(x, y, x, y - 1, 3);
            relax(x, y, x - 1, y - 1, 4); relax(x, y, x + 1, y - 1, 4);
        }
    for (std::int64_t y = ph - 1; y >= 0; --y)
        for (std::int64_t x = pw - 1; x >= 0; --x) {
            relax(x, y, x + 1, y, 3); relax(x, y, x, y + 1, 3);
            relax(x, y, x + 1, y + 1, 4); relax(x, y, x - 1, y + 1, 4);
        }
    for (std::int64_t y = 0; y < sh; ++y)
        for (std::int64_t x = 0; x < sw; ++x) {
            const auto v = d[std::size_t((sy0 + y - py0) * pw + (sx0 + x - px0))];
            if (v == 0) continue;
            // From the sample's centre to the edge of the mask, in metres.
            const double metres = (double(v) / 3.0 - 0.5) * sm;
            const double t = std::clamp(metres / target.featherMetres, 0.0, 1.0);
            weight[std::size_t(y * sw + x)] = std::uint8_t(std::lround(255.0 * t * t * (3.0 - 2.0 * t)));
        }
    return weight;
}

// The hand edits to the terrain categories' details (engine/biomes
// DetailEdits): a file a 32 km chunk under details/, carried like a vector
// layer. Every file of `from` whose chunk reaches into the rectangle goes to
// `to`; with `replace`, a file of `to` wholly inside it that `from` does not
// have goes away - the package's rectangle says what is there.
std::size_t transferDetails(const std::filesystem::path& from, const std::filesystem::path& to, double x0, double y0,
                            double x1, double y1, bool replace) {
    namespace fs = std::filesystem;
    constexpr double kChunk = 32768.0;
    const auto chunkOf = [](const fs::path& file, long long& cx, long long& cy) {
        char tail = 0;
        return file.extension() == ".json" && std::sscanf(file.stem().string().c_str(), "%lld_%lld%c", &cx, &cy, &tail) == 2;
    };
    std::error_code ec;
    std::set<std::string> brought;
    std::size_t copied = 0;
    if (fs::is_directory(from, ec))
        for (const auto& entry : fs::directory_iterator(from, ec)) {
            long long cx = 0, cy = 0;
            if (!chunkOf(entry.path(), cx, cy)) continue;
            if ((cx + 1) * kChunk <= x0 || cx * kChunk >= x1 || (cy + 1) * kChunk <= y0 || cy * kChunk >= y1) continue;
            fs::create_directories(to, ec);
            fs::copy_file(entry.path(), to / entry.path().filename(), fs::copy_options::overwrite_existing, ec);
            brought.insert(entry.path().filename().string());
            ++copied;
        }
    if (replace && fs::is_directory(to, ec))
        for (const auto& entry : fs::directory_iterator(to, ec)) {
            long long cx = 0, cy = 0;
            if (!chunkOf(entry.path(), cx, cy) || brought.count(entry.path().filename().string())) continue;
            if (cx * kChunk >= x0 && (cx + 1) * kChunk <= x1 && cy * kChunk >= y0 && (cy + 1) * kChunk <= y1)
                fs::remove(entry.path(), ec);
        }
    return copied;
}

// The legends' ids, ascending, as a comma list ("1 fertile, 2 sparse").
std::string legendList(const std::map<std::string, std::uint32_t>& names) {
    std::map<std::uint32_t, std::string> byId;
    for (const auto& [n, id] : names) byId[id] = n;
    std::string out;
    for (const auto& [id, n] : byId) out += (out.empty() ? "" : ", ") + std::to_string(id) + " " + n;
    return out;
}

} // namespace

bool renumberLegend(const std::string& layer, const std::map<std::uint32_t, std::string>& package,
                    std::map<std::uint32_t, std::string>& source,
                    const std::map<std::string, std::uint32_t>* canonical,
                    std::map<std::uint32_t, std::uint32_t>& mapping, std::string* why) {
    mapping.clear();
    for (const auto& [id, name] : package) {
        // Nought is the layer's rest, whatever the package calls it.
        if (id == 0) { mapping[0] = 0; continue; }
        std::optional<std::uint32_t> to;
        if (canonical) {
            const auto it = canonical->find(name);
            if (it == canonical->end()) {
                fail(why, "raster " + layer + ": the package names \"" + name + "\" (id " + std::to_string(id) +
                          "), which the registry does not know; known: " + legendList(*canonical));
                return false;
            }
            to = it->second;
            // The registry's name wins over whatever the source called that id.
            source[*to] = name;
        } else {
            for (const auto& [sid, sname] : source)
                if (sname == name) { to = sid; break; }
            if (!to) {
                std::uint32_t free = 1;
                while (source.count(free)) ++free;
                to = free;
                source[free] = name;
            }
        }
        mapping[id] = *to;
    }
    return true;
}

namespace {

std::optional<ImportReport> importManifest(const PackageManifest& manifestIn, const std::filesystem::path& package,
                                           const std::filesystem::path& sourceRoot, const ImportTarget& target,
                                           std::string* why, std::map<std::string, Image>* preloaded = nullptr) {
    const std::optional<PackageManifest> manifest = manifestIn;
    const Schema& ps = manifest->schema;
    const WorldExtent world = target.world.value_or(ps.world);
    ImportReport report;
    std::optional<WorldSource> source;
    if (WorldSource::exists(sourceRoot)) {
        source = WorldSource::open(sourceRoot, why);
        if (!source) return std::nullopt;
        const auto& have = source->schema().world;
        if (have.widthMetres != world.widthMetres || have.heightMetres != world.heightMetres ||
            have.chunkMetres != world.chunkMetres) {
            fail(why, "the package's world is not the source's: world extents differ");
            return std::nullopt;
        }
        if (have.sampleMetres != world.sampleMetres) {
            source.reset();
            if (!resampleSource(sourceRoot, world, why)) return std::nullopt;
            source = WorldSource::open(sourceRoot, why);
            if (!source) return std::nullopt;
        }
    } else {
        Schema fresh = ps;
        fresh.world = world;
        source = WorldSource::create(sourceRoot, fresh, why);
        if (!source) return std::nullopt;
        report.created = true;
    }
    for (const auto& layer : ps.rasters) {
        const auto* have = source->schema().raster(layer.name);
        if (!have) {
            if (!source->addRaster(layer, why)) return std::nullopt;
        } else if (!compatible(*have, layer)) {
            fail(why, "raster " + layer.name + " in the package is another kind or shape than in the source");
            return std::nullopt;
        }
    }
    // Categorical layers with a legend: the package's ids onto the source's.
    std::map<std::string, std::vector<std::uint16_t>> renumbers;
    for (const auto& layer : ps.rasters) {
        if (layer.kind != RasterKind::Categorical || layer.ids.empty()) continue;
        auto legend = source->schema().raster(layer.name)->ids;
        const auto canonical = target.legends.find(layer.name);
        std::map<std::uint32_t, std::uint32_t> mapping;
        if (!renumberLegend(layer.name, layer.ids, legend,
                            canonical == target.legends.end() ? nullptr : &canonical->second, mapping, why))
            return std::nullopt;
        if (!source->setLegend(layer.name, legend, why)) return std::nullopt;
        std::vector<std::uint16_t> table(std::size_t(layer.maximum()) + 1, kUnnamed);
        table[0] = 0;
        bool moved = false;
        for (const auto& [a, b] : mapping) {
            table[a] = std::uint16_t(b);
            moved = moved || a != b;
        }
        if (moved) report.renumbered[layer.name] = mapping;
        renumbers[layer.name] = std::move(table);
    }
    const Schema& ss = source->schema();

    // --- the rectangle, in samples ---
    const double sm = ss.world.sampleMetres;
    const std::int64_t chunkSamples = source->chunkSamples();
    std::int64_t sx0 = std::llround(manifest->originX / sm), sy0 = std::llround(manifest->originY / sm);
    std::int64_t sw = std::llround(manifest->sizeX / sm), sh = std::llround(manifest->sizeY / sm);
    if (target.rect) {
        const auto& r = *target.rect;
        sx0 = std::clamp<std::int64_t>(std::llround(std::floor(r[0] / sm)), 0, ss.world.samplesX());
        sy0 = std::clamp<std::int64_t>(std::llround(std::floor(r[1] / sm)), 0, ss.world.samplesY());
        sw = std::clamp<std::int64_t>(std::llround(std::ceil(r[2] / sm)), 0, ss.world.samplesX()) - sx0;
        sh = std::clamp<std::int64_t>(std::llround(std::ceil(r[3] / sm)), 0, ss.world.samplesY()) - sy0;
        if (sw <= 0 || sh <= 0) { fail(why, "the rectangle to import into is empty"); return std::nullopt; }
    }
    const std::vector<std::uint8_t> weight = maskWeights(target, ss.world, sx0, sy0, sw, sh);

    core::progress("import: reading the pictures");
    // --- rasters: loaded once, whole (bounded by the package's own size) ---
    std::vector<Image> images;
    std::vector<const RasterDesc*> loaded;
    std::vector<Source2D> rasters;
    images.reserve(ps.rasters.size());
    for (const auto& layer : ps.rasters) {
        // A picture already read and worked into this layer's numbers.
        if (preloaded)
            if (auto it = preloaded->find(layer.name); it != preloaded->end()) {
                images.push_back(std::move(it->second));
                loaded.push_back(&layer);
                continue;
            }
        const auto file = package / layer.file;
        std::error_code ec;
        if (layer.file.empty() || !std::filesystem::exists(file, ec)) {
            const bool optional = std::all_of(layer.channels.begin(), layer.channels.end(), [](const auto& c) { return c.optional; });
            if (optional) continue;
            fail(why, "raster " + layer.name + ": " + layer.file + " is missing and not optional");
            return std::nullopt;
        }
        std::string problem;
        auto image = readPng(file, &problem);
        if (!image) { fail(why, "raster " + layer.name + ": " + problem); return std::nullopt; }
        if (channelsOf(layer.type) == 1 && image->channels > 2 && layer.kind != RasterKind::Height &&
            layer.kind != RasterKind::Control) {
            fail(why, "raster " + layer.name + ": ids and flags must be one channel (a grey PNG)");
            return std::nullopt;
        }
        images.push_back(std::move(*image));
        loaded.push_back(&layer);
    }
    {
        for (std::size_t k = 0; k < loaded.size(); ++k) {
            const RasterDesc& layer = *loaded[k];
            Source2D s;
            s.image = &images[k];
            s.from = &layer;
            s.to = ss.raster(layer.name);
            s.samplesX = sw;
            s.samplesY = sh;
            s.direct = s.image->width == std::uint32_t(sw) && s.image->height == std::uint32_t(sh);
            if (const auto it = renumbers.find(layer.name); it != renumbers.end()) {
                s.renumber = it->second;
                s.unnamed = &report.unnamed[layer.name];
            }
            rasters.push_back(s);
        }
    }

    // --- vectors: what the package says, and what it replaces ---
    std::map<std::string, Feature> incoming;
    std::set<std::string> kinds;
    const auto take = [&](std::vector<Feature> features) {
        for (auto& f : features) {
            if (incoming.count(f.id)) return fail(why, "stable id " + f.id + " appears twice in the package");
            const auto b = boundsOf(f);
            if (b.minX < 0 || b.minY < 0 || b.maxX > ss.world.widthMetres || b.maxY > ss.world.heightMetres)
                return fail(why, f.id + " lies outside the world");
            incoming.emplace(f.id, std::move(f));
        }
        return true;
    };
    for (const auto& [kind, file] : ps.vectors) {
        std::error_code ec;
        if (target.rastersOnly || !std::filesystem::exists(package / file, ec)) continue;
        auto features = readVectorFile(package / file, kind, why);
        if (!features || !take(std::move(*features))) return std::nullopt;
        kinds.insert(kind);
    }
    if (!ps.poi.empty() && !target.rastersOnly) {
        std::error_code ec;
        if (std::filesystem::exists(package / ps.poi, ec)) {
            auto poi = readPoiFile(package / ps.poi, why);
            if (!poi || !take(std::move(*poi))) return std::nullopt;
            kinds.insert("poi");
        }
    }
    const double rx0 = double(sx0) * sm, ry0 = double(sy0) * sm;
    const double rx1 = double(sx0 + sw) * sm, ry1 = double(sy0 + sh) * sm;
    std::map<ChunkKey, Chunk> cache;
    std::set<std::string> replaced, removed;   // ids whose fragments go
    std::map<std::string, Feature> added;      // ids whose fragments come
    for (const auto& [id, entry] : source->vectors()) {
        if (!kinds.count(entry.kind) || incoming.count(id)) continue;
        if (entry.bounds.overlaps(rx0, ry0, rx1, ry1)) removed.insert(id);
    }
    for (auto& [id, feature] : incoming) {
        const auto stored = storedFeature(*source, id, cache, nullptr);
        if (stored && *stored == feature) { ++report.featuresUnchanged; continue; }
        (stored ? report.featuresChanged : report.featuresAdded) += 1;
        if (source->vectors().count(id)) replaced.insert(id);
        added.emplace(id, feature);
    }
    report.featuresRemoved = removed.size();

    // --- every chunk anything reaches ---
    std::set<ChunkKey> affected;
    if (!rasters.empty())
        for (auto cy = world_store::floorDiv(sy0, chunkSamples); cy <= world_store::floorDiv(sy0 + sh - 1, chunkSamples); ++cy)
            for (auto cx = world_store::floorDiv(sx0, chunkSamples); cx <= world_store::floorDiv(sx0 + sw - 1, chunkSamples); ++cx)
                if (target.mask.empty() ||
                    std::any_of(target.mask.begin(), target.mask.end(), [&](const std::array<double, 4>& m) {
                        const double cm = double(chunkSamples) * sm;
                        return m[0] < double(cx + 1) * cm && m[2] > double(cx) * cm &&
                               m[1] < double(cy + 1) * cm && m[3] > double(cy) * cm;
                    }))
                    affected.insert({ChunkLevel::SourceChunk, cx, cy});
    std::map<ChunkKey, std::vector<Fragment>> newFragments;
    std::map<std::string, VectorIndexEntry> newIndex;
    for (const auto& id : replaced) for (const auto& k : source->vectors().at(id).chunks) affected.insert(k);
    for (const auto& id : removed) for (const auto& k : source->vectors().at(id).chunks) affected.insert(k);
    for (const auto& [id, feature] : added) {
        VectorIndexEntry entry;
        entry.kind = feature.kind;
        entry.bounds = boundsOf(feature);
        entry.chunks = chunksTouched(feature);
        for (auto& [k, fragment] : fragmentsOf(feature)) newFragments[k].push_back(std::move(fragment));
        affected.insert(entry.chunks.begin(), entry.chunks.end());
        newIndex.emplace(id, std::move(entry));
    }
    std::set<std::string> gone = replaced;
    gone.insert(removed.begin(), removed.end());

    // --- chunk by chunk: what it was, with the package laid over it ---
    core::progress("import: writing chunks", 0, std::int64_t(affected.size()));
    std::int64_t chunkIndex = 0;
    for (const auto& key : affected) {
        core::progressStep(++chunkIndex);
        Chunk chunk;
        if (auto it = cache.find(key); it != cache.end()) chunk = std::move(it->second), cache.erase(it);
        else {
            auto read = source->read(key, why);
            if (!read) return std::nullopt;
            chunk = std::move(*read);
        }
        const auto before = source->chunks().count(key) ? source->chunks().at(key) : ChunkRecord{};
        const std::int64_t ox = key.x * chunkSamples, oy = key.y * chunkSamples;
        const bool inside = ox < sx0 + sw && ox + chunkSamples > sx0 && oy < sy0 + sh && oy + chunkSamples > sy0;
        if (inside)
            for (const auto& r : rasters) {
                Tile tile = source->tile(chunk, r.to->name);
                tile.expand();
                const std::uint8_t channels = tile.channels;
                const std::int64_t x0 = std::max(ox, sx0), x1 = std::min(ox + chunkSamples, sx0 + sw);
                const std::int64_t y0 = std::max(oy, sy0), y1 = std::min(oy + chunkSamples, sy0 + sh);
                const bool blends = r.to->kind == RasterKind::Height || r.to->kind == RasterKind::Control;
                for (auto y = y0; y < y1; ++y)
                    for (auto x = x0; x < x1; ++x) {
                        const int w = weight.empty() ? 255 : weight[std::size_t((y - sy0) * sw + (x - sx0))];
                        if (w == 0) continue;
                        for (std::uint8_t c = 0; c < channels; ++c) {
                            auto& value = tile.values[std::size_t(((y - oy) * chunkSamples + (x - ox)) * channels + c)];
                            const std::uint16_t incoming = r.at(x - sx0, y - sy0, c);
                            // Ids and flags are taken or not; amounts blend.
                            if (w == 255 || (!blends && w >= 128)) value = incoming;
                            else if (blends)
                                value = std::uint16_t(int(value) + (int(incoming) - int(value)) * w / 255);
                        }
                    }
                tile.settle();
                chunk.rasters[r.to->name] = std::move(tile);
            }
        std::erase_if(chunk.fragments, [&](const Fragment& f) { return gone.count(f.id) > 0; });
        if (auto it = newFragments.find(key); it != newFragments.end())
            chunk.fragments.insert(chunk.fragments.end(), it->second.begin(), it->second.end());
        if (!source->write(key, chunk, why)) return std::nullopt;
        const auto after = source->chunks().count(key) ? source->chunks().at(key) : ChunkRecord{};
        if (after == before) { ++report.chunksUnchanged; continue; }
        if (after.file.empty()) ++report.chunksRemoved; else ++report.chunksWritten;
        std::set<std::string> layers;
        for (const auto& [layer, hash] : after.layers)
            if (!before.layers.count(layer) || before.layers.at(layer) != hash) layers.insert(layer);
        for (const auto& [layer, hash] : before.layers) if (!after.layers.count(layer)) layers.insert(layer);
        report.changed[key] = layers;
        auto& dirty = report.dirty[key];
        for (const auto& layer : layers) {
            const auto a = affectedBy(ss, layer);
            dirty.insert(a.begin(), a.end());
        }
    }
    // Features: every chunk each one reaches is stale for what it moves, even
    // those holding no fragment of it (a lake's interior).
    const auto staleFor = [&](const std::string& kind, const std::set<ChunkKey>& chunks) {
        const auto systems = affectedByFeature(kind);
        for (const auto& k : chunks) report.dirty[k].insert(systems.begin(), systems.end());
    };
    for (const auto& id : gone) {
        const auto& e = source->vectors().at(id);
        staleFor(e.kind, e.chunks);
        source->index(id, std::nullopt);
    }
    for (auto& [id, entry] : newIndex) {
        staleFor(entry.kind, entry.chunks);
        source->index(id, entry);
    }
    core::progress("import: indexes");
    if (!source->commit(why)) return std::nullopt;
    if (!target.rastersOnly && !package.empty())
        transferDetails(package / "details", sourceRoot / "details", double(sx0) * sm, double(sy0) * sm,
                        double(sx0 + sw) * sm, double(sy0 + sh) * sm, true);
    return report;
}

} // namespace

std::optional<ImportReport> importPackage(const std::filesystem::path& package, const std::filesystem::path& sourceRoot,
                                          std::string* why) {
    return importPackage(package, sourceRoot, ImportTarget{}, why);
}

std::optional<ImportReport> importPackage(const std::filesystem::path& package, const std::filesystem::path& sourceRoot,
                                          const ImportTarget& target, std::string* why) {
    const auto manifestBytes = world_store::readFileBytes(package / "world.json", why);
    if (!manifestBytes) return std::nullopt;
    const auto manifestJson = nlohmann::json::parse(manifestBytes->begin(), manifestBytes->end(), nullptr, false);
    if (manifestJson.is_discarded()) { fail(why, "world.json is not valid JSON"); return std::nullopt; }
    auto manifest = parsePackage(manifestJson, why);
    if (!manifest) return std::nullopt;
    if (target.world && !target.rect && !(manifest->schema.world == *target.world)) {
        fail(why, "the package is cut for another world: import it into a selection instead");
        return std::nullopt;
    }
    auto report = importManifest(*manifest, package, sourceRoot, target, why);
    if (report && manifestJson.is_object()) report->stage = manifestJson.value("stage", std::string());
    return report;
}

Schema canonicalSchema(const WorldExtent& world) {
    Schema s;
    s.world = world;
    RasterDesc height;
    height.name = "height"; height.file = "raster/height.png"; height.type = SampleType::U16; height.kind = RasterKind::Height;
    height.channels = {ChannelDesc{"height_m", -2000, 8000, -60, Interpolation::Bicubic, false, 1, {}}};
    RasterDesc control;
    control.name = "control_0"; control.file = "raster/control_0.png"; control.type = SampleType::RGBA8;
    control.kind = RasterKind::Control;
    control.channels = {ChannelDesc{"moisture_bias", 0, 1, 0.5, Interpolation::Bilinear, true, 1, {"climate", "ecology"}},
                        ChannelDesc{"forest_bias", 0, 1, 0.5, Interpolation::Bilinear, true, 1, {"ecology", "vegetation"}},
                        ChannelDesc{"mountain_strength", 0, 1, 0, Interpolation::Bilinear, true, 1, {"terrain", "drainage"}},
                        ChannelDesc{"erosion_strength", 0, 1, 0.5, Interpolation::Bilinear, true, 1, {"terrain"}}};
    s.rasters = {height, control};
    return s;
}

std::optional<ImportReport> importImages(const LooseImages& images, const std::filesystem::path& sourceRoot,
                                         const ImportTarget& target, std::string* why) {
    if (images.height.empty() && images.control.empty()) { fail(why, "no picture to import"); return std::nullopt; }
    if (!target.world || !target.rect) { fail(why, "loose pictures need a world and a rectangle to go into"); return std::nullopt; }
    if (!(images.highMetres > images.lowMetres)) { fail(why, "white must stand higher than black"); return std::nullopt; }
    const Schema canonical = canonicalSchema(*target.world);
    bool created = false;
    if (!WorldSource::exists(sourceRoot)) {
        if (!WorldSource::create(sourceRoot, canonical, why)) return std::nullopt;
        created = true;
    }
    // The pictures, described as a package would describe them: black to
    // white over the range asked for, the controls as the canonical ones.
    PackageManifest m;
    m.schema.world = *target.world;
    std::map<std::string, Image> preloaded;
    int seaGrey = -1;
    if (!images.height.empty()) {
        // Read here and worked into the canonical numbers: the picture's sea
        // is not a height, and its coast is wherever its dark peak ends.
        std::string problem;
        auto picture = readPng(images.height, &problem);
        if (!picture) { fail(why, "height map: " + problem); return std::nullopt; }
        const std::uint32_t colours = std::min<std::uint32_t>(3, picture->channels >= 3 ? 3 : 1);
        const double top = picture->maximum();
        // Grey 0..255: the mean of the colours (a grey picture saved tinted).
        const auto greyAt = [&](std::uint32_t x, std::uint32_t y) {
            double sum = 0;
            for (std::uint32_t c = 0; c < colours; ++c) sum += picture->at(x, y, std::uint8_t(c));
            return sum / colours * 255.0 / top;
        };
        double sea = images.seaGrey;
        if (sea < 0) {
            std::array<std::uint64_t, 256> histogram{};
            for (std::uint32_t y = 0; y < picture->height; y += 2)
                for (std::uint32_t x = 0; x < picture->width; x += 2)
                    ++histogram[std::size_t(std::clamp(greyAt(x, y), 0.0, 255.0))];
            std::uint64_t total = 0;
            for (const auto c : histogram) total += c;
            std::size_t peak = 0;
            for (std::size_t g = 0; g < 48; ++g) if (histogram[g] > histogram[peak]) peak = g;
            // A dark peak worth the name - a twentieth of the picture - is the
            // sea, up to where it falls under a twentieth of itself.
            std::size_t end = 0;
            if (histogram[peak] * 20 >= total) {
                end = peak;
                while (end + 1 < 64 && histogram[end + 1] * 20 > histogram[peak]) ++end;
            }
            sea = double(end) + 1.0;
        }
        seaGrey = int(std::lround(sea));
        const RasterDesc& canonicalHeight = canonical.rasters[0];
        Image heights;
        heights.width = picture->width;
        heights.height = picture->height;
        heights.channels = 1;
        heights.bits = 16;
        heights.allocate();
        const std::uint16_t openSea = canonicalHeight.defaultStored(0);
        const double high = std::max(2.0, images.highMetres);
        for (std::uint32_t y = 0; y < picture->height; ++y)
            for (std::uint32_t x = 0; x < picture->width; ++x) {
                const double g = greyAt(x, y);
                heights.set(x, y, 0, g < sea ? openSea
                                             : canonicalHeight.encode(0, 1.0 + (g - sea) / std::max(1.0, 255.0 - sea) * (high - 1.0)));
            }
        picture.reset();
        RasterDesc height = canonicalHeight;
        height.file = images.height.string();
        m.schema.rasters.push_back(height);
        preloaded.emplace(height.name, std::move(heights));
    }
    if (!images.control.empty()) {
        RasterDesc control = canonical.rasters[1];
        control.file = images.control.string();
        m.schema.rasters.push_back(control);
    }
    m.sizeX = target.world->widthMetres;
    m.sizeY = target.world->heightMetres;
    ImportTarget t = target;
    t.rastersOnly = true;
    auto report = importManifest(m, std::filesystem::path(), sourceRoot, t, why, &preloaded);
    if (report) {
        report->created = created;
        report->seaGrey = seaGrey;
    }
    return report;
}

std::optional<ImportReport> importGrids(GridRasters rasters, const std::filesystem::path& sourceRoot,
                                        const ImportTarget& target, std::string* why) {
    if (!rasters.height && !rasters.control) { fail(why, "no raster to write"); return std::nullopt; }
    if (!target.world || !target.rect) { fail(why, "worked-out rasters need a world and a rectangle to go into"); return std::nullopt; }
    const Schema canonical = canonicalSchema(*target.world);
    // One pixel a sample of the rectangle, as the importer counts them, or
    // they would be stretched - and a stretched grid is not the one worked out.
    const double sm = target.world->sampleMetres;
    const auto& r = *target.rect;
    const auto wantX = std::uint32_t(std::clamp<std::int64_t>(std::llround(std::ceil(r[2] / sm)), 0, target.world->samplesX()) -
                                     std::clamp<std::int64_t>(std::llround(std::floor(r[0] / sm)), 0, target.world->samplesX()));
    const auto wantY = std::uint32_t(std::clamp<std::int64_t>(std::llround(std::ceil(r[3] / sm)), 0, target.world->samplesY()) -
                                     std::clamp<std::int64_t>(std::llround(std::floor(r[1] / sm)), 0, target.world->samplesY()));
    const auto check = [&](const std::optional<Image>& image, const char* name, std::uint8_t channels, std::uint8_t bits) {
        if (!image) return true;
        if (image->width != wantX || image->height != wantY)
            return fail(why, std::string(name) + ": " + std::to_string(image->width) + " x " + std::to_string(image->height) +
                                 " samples for a rectangle of " + std::to_string(wantX) + " x " + std::to_string(wantY));
        if (image->channels != channels || image->bits != bits)
            return fail(why, std::string(name) + " is not in the canonical numbers");
        const std::size_t n = std::size_t(image->width) * image->height * channels;
        if ((bits == 16 ? image->words.size() : image->bytes.size()) != n) return fail(why, std::string(name) + " is short");
        return true;
    };
    if (!check(rasters.height, "height", 1, 16) || !check(rasters.control, "control", 4, 8)) return std::nullopt;
    bool created = false;
    if (!WorldSource::exists(sourceRoot)) {
        if (!WorldSource::create(sourceRoot, canonical, why)) return std::nullopt;
        created = true;
    }
    PackageManifest m;
    m.schema.world = *target.world;
    m.sizeX = target.world->widthMetres;
    m.sizeY = target.world->heightMetres;
    std::map<std::string, Image> preloaded;
    if (rasters.height) {
        m.schema.rasters.push_back(canonical.rasters[0]);
        preloaded.emplace(canonical.rasters[0].name, std::move(*rasters.height));
    }
    if (rasters.control) {
        m.schema.rasters.push_back(canonical.rasters[1]);
        preloaded.emplace(canonical.rasters[1].name, std::move(*rasters.control));
    }
    ImportTarget t = target;
    t.rastersOnly = true;
    auto report = importManifest(m, std::filesystem::path(), sourceRoot, t, why, &preloaded);
    if (report) report->created = created;
    return report;
}

std::optional<ImportReport> clearRasters(const std::filesystem::path& sourceRoot,
                                         const std::vector<std::array<double, 4>>& rects, std::string* why) {
    auto source = WorldSource::open(sourceRoot, why);
    if (!source) return std::nullopt;
    const Schema& ss = source->schema();
    const std::int64_t chunkSamples = source->chunkSamples();
    const double sm = ss.world.sampleMetres, cm = double(chunkSamples) * sm;
    ImportReport report;
    std::set<ChunkKey> keys;
    for (const auto& [key, record] : source->chunks())
        for (const auto& r : rects)
            if (r[0] < double(key.x + 1) * cm && r[2] > double(key.x) * cm && r[1] < double(key.y + 1) * cm &&
                r[3] > double(key.y) * cm)
                keys.insert(key);
    for (const auto& key : keys) {
        auto chunk = source->read(key, why);
        if (!chunk) return std::nullopt;
        const auto before = source->chunks().at(key);
        for (auto& [name, tile] : chunk->rasters) {
            const auto* desc = ss.raster(name);
            if (!desc) continue;
            tile.expand();
            for (std::int64_t y = 0; y < chunkSamples; ++y)
                for (std::int64_t x = 0; x < chunkSamples; ++x) {
                    const double cx = (double(key.x * chunkSamples + x) + 0.5) * sm;
                    const double cy = (double(key.y * chunkSamples + y) + 0.5) * sm;
                    const bool hit = std::any_of(rects.begin(), rects.end(), [&](const std::array<double, 4>& r) {
                        return cx >= r[0] && cx < r[2] && cy >= r[1] && cy < r[3];
                    });
                    if (!hit) continue;
                    for (std::uint8_t c = 0; c < tile.channels; ++c)
                        tile.values[std::size_t((y * chunkSamples + x) * tile.channels + c)] = desc->defaultStored(c);
                }
            tile.settle();
        }
        if (!source->write(key, *chunk, why)) return std::nullopt;
        const auto after = source->chunks().count(key) ? source->chunks().at(key) : ChunkRecord{};
        if (after == before) { ++report.chunksUnchanged; continue; }
        if (after.file.empty()) ++report.chunksRemoved; else ++report.chunksWritten;
        auto& layers = report.changed[key];
        for (const auto& [layer, hash] : before.layers)
            if (!after.layers.count(layer) || after.layers.at(layer) != hash) layers.insert(layer);
        for (const auto& layer : layers) {
            const auto a = affectedBy(ss, layer);
            report.dirty[key].insert(a.begin(), a.end());
        }
    }
    if (!source->commit(why)) return std::nullopt;
    return report;
}

bool reshapeSource(const std::filesystem::path& sourceRoot, std::int64_t westChunks, std::int64_t northChunks,
                   const WorldExtent& world, std::string* why) {
    auto source = WorldSource::open(sourceRoot, why);
    if (!source) return false;
    const Schema& ss = source->schema();
    if (world.sampleMetres != ss.world.sampleMetres) {
        fail(why, "changing the imported sample spacing while reshaping needs a source resample");
        return false;
    }
    const std::int64_t chunkSamples = source->chunkSamples();
    const double cm = double(chunkSamples) * ss.world.sampleMetres;
    const double dx = double(westChunks) * cm, dy = double(northChunks) * cm;
    const std::int64_t chunksX = std::int64_t(std::ceil(world.widthMetres / cm));
    const std::int64_t chunksY = std::int64_t(std::ceil(world.heightMetres / cm));
    // Written beside it, then swapped in: a reshape that fails half-way leaves
    // the source as it was.
    const auto fresh = sourceRoot.parent_path() / (sourceRoot.filename().string() + ".reshape");
    std::error_code ec;
    std::filesystem::remove_all(fresh, ec);
    Schema moved = ss;
    moved.world = world;
    auto out = WorldSource::create(fresh, moved, why);
    if (!out) return false;
    // Every feature whole, moved, and kept if it still lies in the world.
    std::map<ChunkKey, Chunk> cache;
    std::map<ChunkKey, std::vector<Fragment>> fragments;
    for (const auto& [id, entry] : source->vectors()) {
        auto feature = storedFeature(*source, id, cache, why);
        if (!feature) { fail(why, "feature " + id + " could not be read"); return false; }
        for (auto& ring : feature->rings)
            for (auto& p : ring.points) { p[0] += dx; p[1] += dy; }
        const auto b = boundsOf(*feature);
        if (b.minX < 0 || b.minY < 0 || b.maxX > world.widthMetres || b.maxY > world.heightMetres) continue;
        VectorIndexEntry e;
        e.kind = feature->kind;
        e.bounds = b;
        e.chunks = chunksTouched(*feature);
        for (auto& [k, f] : fragmentsOf(*feature)) fragments[k].push_back(std::move(f));
        out->index(id, e);
    }
    std::set<ChunkKey> keys;
    for (const auto& [key, record] : source->chunks()) {
        const ChunkKey to{ChunkLevel::SourceChunk, key.x + westChunks, key.y + northChunks};
        if (to.x >= 0 && to.y >= 0 && to.x < chunksX && to.y < chunksY) keys.insert(key);
    }
    std::set<ChunkKey> written;
    for (const auto& key : keys) {
        auto chunk = source->read(key, why);
        if (!chunk) return false;
        const ChunkKey to{ChunkLevel::SourceChunk, key.x + westChunks, key.y + northChunks};
        chunk->fragments.clear();
        if (auto it = fragments.find(to); it != fragments.end()) chunk->fragments = std::move(it->second), fragments.erase(it);
        if (!out->write(to, *chunk, why)) return false;
    }
    for (auto& [key, list] : fragments) {
        Chunk chunk;
        chunk.fragments = std::move(list);
        if (!out->write(key, chunk, why)) return false;
    }
    if (!out->commit(why)) return false;
    const auto old = sourceRoot.parent_path() / (sourceRoot.filename().string() + ".old");
    std::filesystem::remove_all(old, ec);
    std::filesystem::rename(sourceRoot, old, ec);
    if (ec) { fail(why, "could not move the old source aside: " + ec.message()); return false; }
    std::filesystem::rename(fresh, sourceRoot, ec);
    if (ec) {
        std::filesystem::rename(old, sourceRoot, ec);
        fail(why, "could not put the reshaped source in place");
        return false;
    }
    std::filesystem::remove_all(old, ec);
    return true;
}

std::optional<ImportReport> paintCategorical(const std::filesystem::path& sourceRoot, const WorldExtent& world,
                                             const std::string& layer, std::uint32_t id,
                                             const std::vector<CategoricalDab>& dabs,
                                             const std::map<std::string, std::uint32_t>& legend, bool landOnly,
                                             std::string* why) {
    ImportReport report;
    std::optional<WorldSource> source;
    if (WorldSource::exists(sourceRoot)) {
        source = WorldSource::open(sourceRoot, why);
        if (!source) return std::nullopt;
        if (!(source->schema().world == world)) { fail(why, "the world is not the source's: extents differ"); return std::nullopt; }
    } else {
        source = WorldSource::create(sourceRoot, canonicalSchema(world), why);
        if (!source) return std::nullopt;
        report.created = true;
    }
    std::map<std::uint32_t, std::string> names;
    for (const auto& [name, i] : legend) names[i] = name;
    if (const auto* have = source->schema().raster(layer)) {
        if (have->kind != RasterKind::Categorical) { fail(why, "raster " + layer + " is not categorical"); return std::nullopt; }
        if (id > have->maximum()) { fail(why, "id " + std::to_string(id) + " does not fit raster " + layer); return std::nullopt; }
        if (!names.empty()) {
            auto merged = have->ids;
            for (const auto& [i, name] : names) merged[i] = name;
            if (!source->setLegend(layer, merged, why)) return std::nullopt;
        }
    } else {
        RasterDesc d;
        d.name = layer;
        d.file = "raster/" + layer + ".png";
        d.type = id > 255 ? SampleType::U16 : SampleType::U8;
        d.kind = RasterKind::Categorical;
        d.channels = {ChannelDesc{"id", 0, 0, 0, Interpolation::Nearest, true, 1, {}}};
        d.ids = names;
        if (!source->addRaster(d, why)) return std::nullopt;
    }
    const Schema& ss = source->schema();
    const RasterDesc* desc = ss.raster(layer);
    const RasterDesc* height = nullptr;
    for (const auto& r : ss.rasters) if (r.kind == RasterKind::Height) { height = &r; break; }
    const double sm = ss.world.sampleMetres;
    const std::int64_t chunkSamples = source->chunkSamples();
    // The samples each chunk takes, from every disc.
    std::map<ChunkKey, std::set<std::pair<std::int64_t, std::int64_t>>> touched;
    for (const auto& dab : dabs) {
        if (!(dab.radius > 0)) continue;
        const auto sx0 = std::max<std::int64_t>(0, std::int64_t(std::floor((dab.x - dab.radius) / sm)));
        const auto sy0 = std::max<std::int64_t>(0, std::int64_t(std::floor((dab.y - dab.radius) / sm)));
        const auto sx1 = std::min<std::int64_t>(ss.world.samplesX() - 1, std::int64_t(std::floor((dab.x + dab.radius) / sm)));
        const auto sy1 = std::min<std::int64_t>(ss.world.samplesY() - 1, std::int64_t(std::floor((dab.y + dab.radius) / sm)));
        for (auto sy = sy0; sy <= sy1; ++sy)
            for (auto sx = sx0; sx <= sx1; ++sx) {
                const double cx = (double(sx) + 0.5) * sm, cy = (double(sy) + 0.5) * sm;
                if (std::hypot(cx - dab.x, cy - dab.y) > dab.radius) continue;
                touched[source->chunkAtSample(sx, sy)].insert({sx, sy});
            }
    }
    for (const auto& [key, samples] : touched) {
        auto chunk = source->read(key, why);
        if (!chunk) return std::nullopt;
        const auto before = source->chunks().count(key) ? source->chunks().at(key) : ChunkRecord{};
        if (landOnly && (!height || !before.layers.count(height->name))) continue;   // all of it open sea
        Tile tile = source->tile(*chunk, layer);
        tile.expand();
        const Tile ground = height ? source->tile(*chunk, height->name) : Tile{};
        const double sea = height ? height->channels[0].defaultValue + 0.05 : 0.0;
        for (const auto& [sx, sy] : samples) {
            const std::int64_t lx = sx - key.x * chunkSamples, ly = sy - key.y * chunkSamples;
            if (landOnly && height && height->decode(0, ground.at(lx, ly, 0)) <= sea) continue;
            tile.values[std::size_t(ly * chunkSamples + lx) * tile.channels] = std::uint16_t(id);
        }
        tile.settle();
        chunk->rasters[layer] = std::move(tile);
        if (!source->write(key, *chunk, why)) return std::nullopt;
        const auto after = source->chunks().count(key) ? source->chunks().at(key) : ChunkRecord{};
        if (after == before) { ++report.chunksUnchanged; continue; }
        ++report.chunksWritten;
        report.changed[key] = {layer};
        const auto a = affectedBy(ss, layer);
        report.dirty[key].insert(a.begin(), a.end());
    }
    (void)desc;
    if (!source->commit(why)) return std::nullopt;
    return report;
}

std::optional<ExportReport> exportPackage(const std::filesystem::path& sourceRoot, const std::filesystem::path& package,
                                          const ExportOptions& options, std::string* why) {
    if (options.mode == ExportOptions::Mode::Preview) {
        fail(why, "preview export is the procedural compiler's result, and there is no compiler yet: use source mode");
        return std::nullopt;
    }
    auto source = WorldSource::open(sourceRoot, why);
    if (!source) return std::nullopt;
    const Schema& ss = source->schema();
    const double sm = ss.world.sampleMetres;
    const std::int64_t chunkSamples = source->chunkSamples();
    // --- the rectangle, snapped outward to whole samples ---
    double x0 = 0, y0 = 0, x1 = ss.world.widthMetres, y1 = ss.world.heightMetres;
    if (options.rect) {
        x0 = (*options.rect)[0]; y0 = (*options.rect)[1]; x1 = (*options.rect)[2]; y1 = (*options.rect)[3];
    } else if (options.regionId) {
        const auto layer = source->regions().find(options.regionId->first);
        const auto id = layer == source->regions().end() ? decltype(layer->second.begin()){} : layer->second.find(options.regionId->second);
        if (layer == source->regions().end() || id == layer->second.end()) {
            fail(why, "no region " + std::to_string(options.regionId->second) + " in " + options.regionId->first);
            return std::nullopt;
        }
        std::int64_t bx0 = INT64_MAX, by0 = INT64_MAX, bx1 = INT64_MIN, by1 = INT64_MIN;
        for (const auto& [k, box] : id->second) {
            bx0 = std::min(bx0, box[0]); by0 = std::min(by0, box[1]);
            bx1 = std::max(bx1, box[2]); by1 = std::max(by1, box[3]);
        }
        x0 = double(bx0) * sm; y0 = double(by0) * sm; x1 = double(bx1) * sm; y1 = double(by1) * sm;
    } else if (options.feature) {
        // By stable id, or by the name a region outline is given.
        std::optional<Bounds> found;
        if (const auto e = source->vectors().find(*options.feature); e != source->vectors().end()) found = e->second.bounds;
        std::map<ChunkKey, Chunk> named;
        for (const auto& [id, e] : source->vectors()) {
            if (found) break;
            const auto f = storedFeature(*source, id, named, nullptr);
            if (f && f->properties.is_object() && f->properties.value("name", std::string()) == *options.feature)
                found = e.bounds;
        }
        if (!found) { fail(why, "no feature with id or name " + *options.feature); return std::nullopt; }
        x0 = found->minX; y0 = found->minY; x1 = found->maxX; y1 = found->maxY;
    }
    x0 = std::clamp(std::floor(x0 / sm) * sm, 0.0, ss.world.widthMetres);
    y0 = std::clamp(std::floor(y0 / sm) * sm, 0.0, ss.world.heightMetres);
    x1 = std::clamp(std::ceil(x1 / sm) * sm, 0.0, ss.world.widthMetres);
    y1 = std::clamp(std::ceil(y1 / sm) * sm, 0.0, ss.world.heightMetres);
    if (!(x1 > x0) || !(y1 > y0)) { fail(why, "nothing to export: the rectangle is empty"); return std::nullopt; }
    const auto wanted = [&](const std::string& name) { return options.layers.empty() || options.layers.count(name) > 0; };

    PackageManifest out;
    out.schema.version = ss.version;
    out.schema.world = ss.world;
    out.originX = x0; out.originY = y0; out.sizeX = x1 - x0; out.sizeY = y1 - y0;
    const std::int64_t sx0 = std::llround(x0 / sm), sy0 = std::llround(y0 / sm);
    const std::int64_t sw = std::llround((x1 - x0) / sm), sh = std::llround((y1 - y0) / sm);

    // --- rasters, stitched from whatever chunks the rectangle crosses ---
    std::vector<std::pair<const RasterDesc*, Image>> images;
    for (const auto& layer : ss.rasters) {
        if (!wanted(layer.name)) continue;
        Image image;
        image.width = std::uint32_t(sw);
        image.height = std::uint32_t(sh);
        image.channels = channelsOf(layer.type);
        image.bits = std::uint8_t(bytesPerChannel(layer.type) * 8);
        image.allocate();
        images.emplace_back(&layer, std::move(image));
        RasterDesc described = layer;
        described.file = "raster/" + layer.name + ".png";
        out.schema.rasters.push_back(described);
    }
    if (!images.empty())
        for (auto cy = world_store::floorDiv(sy0, chunkSamples); cy <= world_store::floorDiv(sy0 + sh - 1, chunkSamples); ++cy)
            for (auto cx = world_store::floorDiv(sx0, chunkSamples); cx <= world_store::floorDiv(sx0 + sw - 1, chunkSamples); ++cx) {
                const ChunkKey key{ChunkLevel::SourceChunk, cx, cy};
                auto chunk = source->read(key, why);
                if (!chunk) return std::nullopt;
                const std::int64_t ox = cx * chunkSamples, oy = cy * chunkSamples;
                const std::int64_t ax = std::max(ox, sx0), bx = std::min(ox + chunkSamples, sx0 + sw);
                const std::int64_t ay = std::max(oy, sy0), by = std::min(oy + chunkSamples, sy0 + sh);
                for (auto& [layer, image] : images) {
                    const Tile tile = source->tile(*chunk, layer->name);
                    for (auto y = ay; y < by; ++y)
                        for (auto x = ax; x < bx; ++x)
                            for (std::uint8_t c = 0; c < image.channels; ++c)
                                image.set(std::uint32_t(x - sx0), std::uint32_t(y - sy0), c, tile.at(x - ox, y - oy, c));
                }
            }
    ExportReport report;
    report.originX = x0; report.originY = y0; report.sizeX = x1 - x0; report.sizeY = y1 - y0;
    for (const auto& [layer, image] : images) {
        if (!writePng(package / "raster" / (layer->name + ".png"), image, why)) return std::nullopt;
        ++report.rasters;
    }

    // --- features reaching into the rectangle, whole ---
    std::map<ChunkKey, Chunk> cache;
    std::map<std::string, std::vector<Feature>> byKind;
    for (const auto& [id, entry] : source->vectors()) {
        if (!wanted(entry.kind) || !entry.bounds.overlaps(x0, y0, x1, y1)) continue;
        auto feature = storedFeature(*source, id, cache, why);
        if (!feature) { if (why && why->empty()) *why = "feature " + id + " could not be read"; return std::nullopt; }
        byKind[entry.kind].push_back(std::move(*feature));
        ++report.features;
    }
    // Every kind the source knows, even with nothing in the rectangle: an
    // empty file says "none here", which is what a re-import has to hear.
    std::set<std::string> kinds;
    for (const auto& [kind, file] : ss.vectors) kinds.insert(kind);
    for (const auto& [id, entry] : source->vectors()) if (entry.kind != "poi") kinds.insert(entry.kind);
    for (const auto& kind : kinds) {
        if (!wanted(kind)) continue;
        const auto file = "vectors/" + kind + ".json";
        out.schema.vectors[kind] = file;
        if (!writeVectorFile(package / file, kind, byKind[kind], why)) return std::nullopt;
    }
    if (wanted("poi")) {
        out.schema.poi = "poi/poi.json";
        if (!writePoiFile(package / out.schema.poi, byKind["poi"], why)) return std::nullopt;
    }
    if (wanted("details")) transferDetails(sourceRoot / "details", package / "details", x0, y0, x1, y1, false);
    if (!world_store::writeFileAtomic(package / "world.json", packageJson(out).dump(2), why)) return std::nullopt;
    return report;
}

bool resampleSource(const std::filesystem::path& sourceRoot, const WorldExtent& world, std::string* why) {
    auto source = WorldSource::open(sourceRoot, why);
    if (!source) return false;
    const auto& before = source->schema().world;
    if (before.widthMetres != world.widthMetres || before.heightMetres != world.heightMetres ||
        before.chunkMetres != world.chunkMetres) {
        fail(why, "resampling changes only the sample spacing, not world extent or chunk size");
        return false;
    }
    if (before.sampleMetres == world.sampleMetres) return true;
    const auto package = sourceRoot.parent_path() / (sourceRoot.filename().string() + ".resample-package");
    const auto fresh = sourceRoot.parent_path() / (sourceRoot.filename().string() + ".resample");
    const auto backup = sourceRoot.parent_path() / (sourceRoot.filename().string() + ".resample-old");
    std::error_code ec;
    std::filesystem::remove_all(package, ec);
    std::filesystem::remove_all(fresh, ec);
    std::filesystem::remove_all(backup, ec);
    ExportOptions options;
    if (!exportPackage(sourceRoot, package, options, why)) return false;
    ImportTarget target;
    target.world = world;
    target.rect = std::array<double, 4>{0, 0, world.widthMetres, world.heightMetres};
    if (!importPackage(package, fresh, target, why)) {
        std::filesystem::remove_all(package, ec);
        std::filesystem::remove_all(fresh, ec);
        return false;
    }
    const auto details = sourceRoot / "details";
    if (std::filesystem::exists(details, ec)) {
        std::filesystem::copy(details, fresh / "details", std::filesystem::copy_options::recursive, ec);
        if (ec) {
            fail(why, "could not preserve source detail edits: " + ec.message());
            std::filesystem::remove_all(package, ec);
            std::filesystem::remove_all(fresh, ec);
            return false;
        }
    }
    std::filesystem::rename(sourceRoot, backup, ec);
    if (ec) { fail(why, "could not move old source aside: " + ec.message()); return false; }
    std::filesystem::rename(fresh, sourceRoot, ec);
    if (ec) {
        std::filesystem::rename(backup, sourceRoot, ec);
        fail(why, "could not install resampled source: " + ec.message());
        return false;
    }
    std::filesystem::remove_all(backup, ec);
    std::filesystem::remove_all(package, ec);
    return true;
}

} // namespace engine::world_source
