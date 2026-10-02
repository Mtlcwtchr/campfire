#include "engine/world_source/world_source.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/chunk_file.hpp"
#include "engine/world_store/codec.hpp"

namespace engine::world_source {
namespace {
namespace ws = engine::world_store;

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

constexpr double kChunkMetres = 32768.0;

ChunkKey chunkOfMetres(double x, double y) {
    return {ChunkLevel::SourceChunk, std::int64_t(std::floor(x / kChunkMetres)), std::int64_t(std::floor(y / kChunkMetres))};
}

// Whether the closed segment ab crosses the closed rectangle.
bool segmentTouches(std::array<double, 2> a, std::array<double, 2> b, double x0, double y0, double x1, double y1) {
    double t0 = 0, t1 = 1;
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    const auto clip = [&](double p, double q) {
        if (p == 0) return q >= 0;
        const double r = q / p;
        if (p < 0) { if (r > t1) return false; if (r > t0) t0 = r; }
        else { if (r < t0) return false; if (r < t1) t1 = r; }
        return true;
    };
    return clip(-dx, a[0] - x0) && clip(dx, x1 - a[0]) && clip(-dy, a[1] - y0) && clip(dy, y1 - a[1]);
}

bool insidePolygon(const Feature& f, double x, double y) {
    bool inside = false;
    for (const auto& ring : f.rings) {
        const auto& p = ring.points;
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
            if (((p[i][1] > y) != (p[j][1] > y)) &&
                x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0])
                inside = !inside;
    }
    return inside;
}

// ---- raster blocks ----
std::vector<std::uint8_t> encodeTile(const Tile& tile, std::uint8_t bytesPerChannel) {
    std::vector<std::uint8_t> out{1, std::uint8_t(tile.isUniform() ? 1 : 0), tile.channels, bytesPerChannel,
                                  std::uint8_t(kChunkSamples & 255), std::uint8_t(kChunkSamples >> 8)};
    const auto& values = tile.isUniform() ? tile.uniform : tile.values;
    out.reserve(out.size() + values.size() * bytesPerChannel);
    for (const auto v : values) {
        out.push_back(std::uint8_t(v));
        if (bytesPerChannel == 2) out.push_back(std::uint8_t(v >> 8));
    }
    return out;
}
std::optional<Tile> decodeTile(const std::vector<std::uint8_t>& raw, std::uint8_t channels, std::uint8_t bytes) {
    if (raw.size() < 6 || raw[0] != 1 || raw[2] != channels || raw[3] != bytes ||
        std::int64_t(raw[4] | (raw[5] << 8)) != kChunkSamples)
        return std::nullopt;
    Tile t;
    t.channels = channels;
    const bool uniform = raw[1] != 0;
    const std::size_t count = uniform ? channels : std::size_t(kChunkSamples * kChunkSamples) * channels;
    if (raw.size() != 6 + count * bytes) return std::nullopt;
    auto& values = uniform ? t.uniform : t.values;
    values.resize(count);
    for (std::size_t i = 0; i < count; ++i)
        values[i] = bytes == 2 ? std::uint16_t(raw[6 + 2 * i] | (raw[7 + 2 * i] << 8)) : raw[6 + i];
    return t;
}

// ---- fragment blocks ----
nlohmann::json fragmentJson(const Fragment& f) {
    nlohmann::json j{{"id", f.id}, {"kind", f.kind}, {"geometry", geometryName(f.geometry)}, {"properties", f.properties}};
    j["parts"] = nlohmann::json::array();
    for (const auto& p : f.parts) {
        nlohmann::json part{{"ring", p.ring}, {"count", p.count}, {"indices", p.indices}, {"points", p.points}};
        if (!p.values.empty()) part["values"] = p.values;
        j["parts"].push_back(part);
    }
    return j;
}
std::optional<Geometry> geometryNamed(const std::string& name) {
    if (name == "line") return Geometry::Line;
    if (name == "polygon") return Geometry::Polygon;
    if (name == "point") return Geometry::Point;
    return std::nullopt;
}
std::optional<Fragment> fragmentFrom(const nlohmann::json& j) {
    Fragment f;
    f.id = j.at("id").get<std::string>();
    f.kind = j.at("kind").get<std::string>();
    const auto g = geometryNamed(j.at("geometry").get<std::string>());
    if (!g) return std::nullopt;
    f.geometry = *g;
    f.properties = j.value("properties", nlohmann::json::object());
    for (const auto& pj : j.at("parts")) {
        Fragment::Part p;
        p.ring = pj.at("ring").get<std::uint32_t>();
        p.count = pj.at("count").get<std::uint32_t>();
        p.indices = pj.at("indices").get<std::vector<std::uint32_t>>();
        p.points = pj.at("points").get<std::vector<std::array<double, 2>>>();
        if (pj.contains("values")) p.values = pj["values"].get<std::map<std::string, std::vector<double>>>();
        if (p.indices.size() != p.points.size()) return std::nullopt;
        for (const auto& [name, v] : p.values) if (v.size() != p.indices.size()) return std::nullopt;
        f.parts.push_back(std::move(p));
    }
    return f;
}
std::vector<std::uint8_t> encodeFragments(std::vector<const Fragment*> fragments) {
    std::sort(fragments.begin(), fragments.end(), [](const Fragment* a, const Fragment* b) { return a->id < b->id; });
    nlohmann::json j = nlohmann::json::array();
    for (const auto* f : fragments) j.push_back(fragmentJson(*f));
    const auto text = j.dump();
    return {text.begin(), text.end()};
}

std::string stemOf(const ChunkKey& key) {
    char text[32];
    std::snprintf(text, sizeof text, "%+05lld_%+05lld", static_cast<long long>(key.x), static_cast<long long>(key.y));
    return text;
}
std::optional<ChunkKey> keyOfStem(const std::string& stem) {
    long long x = 0, y = 0;
    if (std::sscanf(stem.c_str(), "%lld_%lld", &x, &y) != 2) return std::nullopt;
    return ChunkKey{ChunkLevel::SourceChunk, x, y};
}

} // namespace

const char* geometryName(Geometry g) {
    switch (g) {
        case Geometry::Line: return "line";
        case Geometry::Polygon: return "polygon";
        case Geometry::Point: return "point";
    }
    return "line";
}

Bounds boundsOf(const Feature& f) {
    Bounds b{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
             std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
    for (const auto& r : f.rings)
        for (const auto& p : r.points) {
            b.minX = std::min(b.minX, p[0]); b.minY = std::min(b.minY, p[1]);
            b.maxX = std::max(b.maxX, p[0]); b.maxY = std::max(b.maxY, p[1]);
        }
    return b;
}

std::map<ChunkKey, Fragment> fragmentsOf(const Feature& f) {
    std::map<ChunkKey, std::map<std::uint32_t, std::set<std::uint32_t>>> picked;   // chunk -> ring -> vertices
    for (std::uint32_t r = 0; r < f.rings.size(); ++r) {
        const auto& p = f.rings[r].points;
        if (p.empty()) continue;
        if (p.size() == 1 || f.geometry == Geometry::Point) {
            for (std::uint32_t i = 0; i < p.size(); ++i) picked[chunkOfMetres(p[i][0], p[i][1])][r].insert(i);
            continue;
        }
        const std::size_t segments = f.geometry == Geometry::Polygon ? p.size() : p.size() - 1;
        for (std::size_t s = 0; s < segments; ++s) {
            const auto i = std::uint32_t(s), j = std::uint32_t((s + 1) % p.size());
            const auto a = p[i], b = p[j];
            const auto lo = chunkOfMetres(std::min(a[0], b[0]), std::min(a[1], b[1]));
            const auto hi = chunkOfMetres(std::max(a[0], b[0]), std::max(a[1], b[1]));
            for (auto cy = lo.y; cy <= hi.y; ++cy)
                for (auto cx = lo.x; cx <= hi.x; ++cx) {
                    const double x0 = double(cx) * kChunkMetres, y0 = double(cy) * kChunkMetres;
                    if (!segmentTouches(a, b, x0, y0, x0 + kChunkMetres, y0 + kChunkMetres)) continue;
                    auto& set = picked[{ChunkLevel::SourceChunk, cx, cy}][r];
                    set.insert(i);
                    set.insert(j);
                }
        }
    }
    std::map<ChunkKey, Fragment> out;
    for (const auto& [key, rings] : picked) {
        Fragment fr;
        fr.id = f.id;
        fr.kind = f.kind;
        fr.geometry = f.geometry;
        fr.properties = f.properties;
        for (const auto& [r, vertices] : rings) {
            Fragment::Part part;
            part.ring = r;
            part.count = std::uint32_t(f.rings[r].points.size());
            for (const auto v : vertices) {
                part.indices.push_back(v);
                part.points.push_back(f.rings[r].points[v]);
                for (const auto& [name, values] : f.rings[r].values) part.values[name].push_back(values[v]);
            }
            fr.parts.push_back(std::move(part));
        }
        out.emplace(key, std::move(fr));
    }
    return out;
}

std::set<ChunkKey> chunksTouched(const Feature& f) {
    std::set<ChunkKey> out;
    for (const auto& [key, fragment] : fragmentsOf(f)) out.insert(key);
    if (f.geometry == Geometry::Polygon && !f.rings.empty() && f.rings[0].points.size() >= 3) {
        const auto b = boundsOf(f);
        const auto lo = chunkOfMetres(b.minX, b.minY), hi = chunkOfMetres(b.maxX, b.maxY);
        for (auto cy = lo.y; cy <= hi.y; ++cy)
            for (auto cx = lo.x; cx <= hi.x; ++cx)
                if (insidePolygon(f, (double(cx) + 0.5) * kChunkMetres, (double(cy) + 0.5) * kChunkMetres))
                    out.insert({ChunkLevel::SourceChunk, cx, cy});
    }
    return out;
}

std::optional<Feature> mergeFragments(const std::vector<Fragment>& fragments, std::string* why) {
    if (fragments.empty()) { fail(why, "no fragments"); return std::nullopt; }
    Feature f;
    f.id = fragments[0].id;
    f.kind = fragments[0].kind;
    f.geometry = fragments[0].geometry;
    f.properties = fragments[0].properties;
    std::vector<std::vector<bool>> seen;
    for (const auto& fr : fragments) {
        if (fr.id != f.id || fr.kind != f.kind || fr.geometry != f.geometry || fr.properties != f.properties) {
            fail(why, "fragments of " + f.id + " disagree");
            return std::nullopt;
        }
        for (const auto& part : fr.parts) {
            if (part.ring >= f.rings.size()) { f.rings.resize(part.ring + 1); seen.resize(part.ring + 1); }
            auto& ring = f.rings[part.ring];
            if (ring.points.empty()) { ring.points.resize(part.count); seen[part.ring].assign(part.count, false); }
            if (ring.points.size() != part.count) { fail(why, "fragments of " + f.id + " disagree on a ring"); return std::nullopt; }
            for (std::size_t k = 0; k < part.indices.size(); ++k) {
                const auto v = part.indices[k];
                if (v >= part.count) { fail(why, "fragment of " + f.id + " names a vertex it does not have"); return std::nullopt; }
                ring.points[v] = part.points[k];
                seen[part.ring][v] = true;
                for (const auto& [name, values] : part.values) {
                    auto& into = ring.values[name];
                    if (into.empty()) into.assign(part.count, 0.0);
                    into[v] = values[k];
                }
            }
        }
    }
    for (const auto& ring : seen)
        if (std::find(ring.begin(), ring.end(), false) != ring.end()) {
            fail(why, "fragments of " + f.id + " leave a vertex out");
            return std::nullopt;
        }
    return f;
}

void Tile::expand() {
    if (!isUniform()) return;
    values.resize(std::size_t(kChunkSamples * kChunkSamples) * channels);
    for (std::size_t i = 0; i < values.size(); ++i) values[i] = uniform[i % channels];
    uniform.clear();
}

void Tile::settle() {
    if (isUniform()) return;
    for (std::size_t i = channels; i < values.size(); ++i)
        if (values[i] != values[i % channels]) return;
    uniform.assign(values.begin(), values.begin() + channels);
    values.clear();
}

Tile defaultTile(const RasterDesc& layer) {
    Tile t;
    t.channels = channelsOf(layer.type);
    for (std::size_t c = 0; c < t.channels; ++c) t.uniform.push_back(layer.defaultStored(c));
    return t;
}

ChunkKey WorldSource::chunkOfPoint(double x, double y, double) { return chunkOfMetres(x, y); }
std::string WorldSource::fileName(const ChunkKey& key) { return stemOf(key) + ".wchunk"; }

std::uint16_t WorldSource::blockTypeOf(const std::string& layer) const {
    const auto it = blockTypes_.find(layer);
    return it == blockTypes_.end() ? 0 : it->second;
}

bool WorldSource::exists(const std::filesystem::path& root) {
    std::error_code ec;
    return std::filesystem::exists(root / "metadata.world", ec);
}

std::optional<WorldSource> WorldSource::create(const std::filesystem::path& root, Schema schema, std::string* why) {
    if (exists(root)) { fail(why, "a WorldSource is already there"); return std::nullopt; }
    WorldSource source;
    source.root_ = root;
    source.schema_ = Schema{};
    source.schema_.version = schema.version;
    source.schema_.world = schema.world;
    source.schema_.vectors = schema.vectors;
    source.schema_.poi = schema.poi;
    for (const auto& layer : schema.rasters)
        if (!source.addRaster(layer, why)) return std::nullopt;
    std::error_code ec;
    std::filesystem::create_directories(root / "chunks", ec);
    std::filesystem::create_directories(root / "indexes", ec);
    if (!source.commit(why)) return std::nullopt;
    return source;
}

bool WorldSource::addRaster(const RasterDesc& layer, std::string* why) {
    if (schema_.raster(layer.name)) return fail(why, "raster " + layer.name + " is already in the source");
    schema_.rasters.push_back(layer);
    blockTypes_[layer.name] = nextBlockType_++;
    return true;
}

bool WorldSource::setLegend(const std::string& layer, std::map<std::uint32_t, std::string> ids, std::string* why) {
    for (auto& r : schema_.rasters)
        if (r.name == layer) {
            if (r.kind != RasterKind::Categorical) return fail(why, "raster " + layer + " is not categorical");
            r.ids = std::move(ids);
            return true;
        }
    return fail(why, "raster " + layer + " is not in the source");
}

std::optional<WorldSource> WorldSource::open(const std::filesystem::path& root, std::string* why) {
    const auto bytes = ws::readFileBytes(root / "metadata.world", why);
    if (!bytes) return std::nullopt;
    const auto j = nlohmann::json::parse(bytes->begin(), bytes->end(), nullptr, false);
    if (j.is_discarded() || j.value("format", 0) != kSourceFormat) { fail(why, "metadata.world is damaged or newer"); return std::nullopt; }
    WorldSource s;
    s.root_ = root;
    auto schema = parseSchema(j.at("schema"), why);
    if (!schema) return std::nullopt;
    s.schema_ = std::move(*schema);
    s.blockTypes_ = j.at("blocks").get<std::map<std::string, std::uint16_t>>();
    s.nextBlockType_ = j.value("next_block", std::uint16_t(16));
    for (const auto& [stem, c] : j.at("chunks").items()) {
        const auto key = keyOfStem(stem);
        if (!key) { fail(why, "metadata.world names a chunk it cannot parse: " + stem); return std::nullopt; }
        ChunkRecord r;
        r.file = c.at("file").get<std::string>();
        r.payloadHash = c.at("payload").get<std::uint64_t>();
        r.layers = c.at("layers").get<std::map<std::string, std::uint64_t>>();
        s.chunks_.emplace(*key, std::move(r));
    }
    // The indexes: derived from the chunks and the features, kept beside them.
    const auto index = [&](const char* name) -> nlohmann::json {
        const auto b = ws::readFileBytes(root / "indexes" / name);
        if (!b) return nlohmann::json::object();
        auto parsed = nlohmann::json::parse(b->begin(), b->end(), nullptr, false);
        return parsed.is_discarded() ? nlohmann::json::object() : parsed;
    };
    // Held: a range-for over items() of a temporary outlives the temporary.
    const auto vectorIndex = index("vectors.idx"), regionIndex = index("regions.idx"), poiIndex = index("poi.idx");
    for (const auto& [id, e] : vectorIndex.items()) {
        VectorIndexEntry entry;
        entry.kind = e.at("kind").get<std::string>();
        const auto b = e.at("bounds").get<std::array<double, 4>>();
        entry.bounds = {b[0], b[1], b[2], b[3]};
        for (const auto& stem : e.at("chunks")) if (const auto k = keyOfStem(stem.get<std::string>())) entry.chunks.insert(*k);
        s.vectors_.emplace(id, std::move(entry));
    }
    for (const auto& [layer, ids] : regionIndex.items())
        for (const auto& [id, chunks] : ids.items())
            for (const auto& [stem, box] : chunks.items())
                if (const auto k = keyOfStem(stem))
                    s.regions_[layer][std::uint32_t(std::stoul(id))][*k] = box.get<std::array<std::int64_t, 4>>();
    for (const auto& [id, stem] : poiIndex.items())
        if (const auto k = keyOfStem(stem.get<std::string>())) s.poi_[id] = *k;
    return s;
}

std::optional<Chunk> WorldSource::read(const ChunkKey& key, std::string* why) const {
    Chunk chunk;
    const auto record = chunks_.find(key);
    if (record == chunks_.end()) return chunk;
    std::string problem;
    const auto file = ws::readFileBytes(root_ / "chunks" / record->second.file, &problem);
    if (!file) { fail(why, "chunk " + stemOf(key) + ": " + problem); return std::nullopt; }
    ws::ChunkHeader header;
    const auto blocks = ws::decodeChunk(*file, &header, &problem);
    if (!blocks) { fail(why, "chunk " + stemOf(key) + ": " + problem); return std::nullopt; }
    if (header.kind != ws::ChunkKind::Source || !(header.key == key)) {
        fail(why, "chunk " + stemOf(key) + " is not the chunk its name says");
        return std::nullopt;
    }
    for (const auto& b : *blocks) {
        if (b.type == kVectorBlock || b.type == kPoiBlock) {
            const auto j = nlohmann::json::parse(b.bytes.begin(), b.bytes.end(), nullptr, false);
            if (j.is_discarded() || !j.is_array()) { fail(why, "chunk " + stemOf(key) + ": vectors damaged"); return std::nullopt; }
            for (const auto& fj : j) {
                auto f = fragmentFrom(fj);
                if (!f) { fail(why, "chunk " + stemOf(key) + ": a fragment is damaged"); return std::nullopt; }
                chunk.fragments.push_back(std::move(*f));
            }
            continue;
        }
        const auto layer = std::find_if(blockTypes_.begin(), blockTypes_.end(), [&](const auto& e) { return e.second == b.type; });
        if (layer == blockTypes_.end()) continue;   // a layer this build does not know
        const auto* desc = schema_.raster(layer->first);
        if (!desc) continue;
        auto tile = decodeTile(b.bytes, channelsOf(desc->type), bytesPerChannel(desc->type));
        if (!tile) { fail(why, "chunk " + stemOf(key) + ": layer " + layer->first + " damaged"); return std::nullopt; }
        chunk.rasters.emplace(layer->first, std::move(*tile));
    }
    return chunk;
}

Tile WorldSource::tile(const Chunk& chunk, const std::string& layer) const {
    if (const auto it = chunk.rasters.find(layer); it != chunk.rasters.end()) return it->second;
    const auto* desc = schema_.raster(layer);
    return desc ? defaultTile(*desc) : Tile{};
}

bool WorldSource::write(const ChunkKey& key, const Chunk& chunk, std::string* why) {
    std::vector<ws::Block> blocks;
    ChunkRecord record;
    for (const auto& [name, t] : chunk.rasters) {
        const auto* desc = schema_.raster(name);
        if (!desc) return fail(why, "chunk names a raster the source does not have: " + name);
        Tile settled = t;
        settled.settle();
        if (settled.isUniform() && settled.uniform == defaultTile(*desc).uniform) continue;   // its default: nothing
        blocks.push_back({blockTypeOf(name), std::uint16_t(desc->version), encodeTile(settled, bytesPerChannel(desc->type))});
    }
    std::vector<const Fragment*> vectors, poi;
    for (const auto& f : chunk.fragments) (f.kind == "poi" ? poi : vectors).push_back(&f);
    if (!vectors.empty()) blocks.push_back({kVectorBlock, 1, encodeFragments(vectors)});
    if (!poi.empty()) blocks.push_back({kPoiBlock, 1, encodeFragments(poi)});
    std::sort(blocks.begin(), blocks.end(), [](const auto& a, const auto& b) { return a.type < b.type; });
    const auto path = root_ / "chunks" / fileName(key);
    if (blocks.empty()) {
        // Every layer its default and nothing on it: open sea, no file (§12.1).
        std::error_code ec;
        std::filesystem::remove(path, ec);
        chunks_.erase(key);
    } else {
        for (const auto& b : blocks) {
            std::string layer = b.type == kVectorBlock ? "vectors" : b.type == kPoiBlock ? "poi" : "";
            for (const auto& [name, type] : blockTypes_) if (type == b.type) layer = name;
            record.layers[layer] = ws::contentHash(b.bytes);
        }
        record.file = fileName(key);
        record.payloadHash = ws::payloadHashOf(blocks);
        const auto existing = chunks_.find(key);
        if (existing == chunks_.end() || !(existing->second == record)) {
            ws::ChunkHeader header;
            header.kind = ws::ChunkKind::Source;
            header.key = key;
            const auto bytes = ws::encodeChunk(header, blocks);
            if (!ws::writeFileAtomic(path, bytes, why)) return false;
            chunks_[key] = record;
        }
    }
    reindex(key, chunk);
    return true;
}

void WorldSource::reindex(const ChunkKey& key, const Chunk& chunk) {
    for (auto& [layer, ids] : regions_)
        for (auto it = ids.begin(); it != ids.end();) {
            it->second.erase(key);
            it = it->second.empty() ? ids.erase(it) : std::next(it);
        }
    for (const auto& desc : schema_.rasters) {
        if (desc.kind != RasterKind::Categorical) continue;
        const Tile t = tile(chunk, desc.name);
        std::map<std::uint32_t, std::array<std::int64_t, 4>> boxes;
        const std::int64_t ox = key.x * kChunkSamples, oy = key.y * kChunkSamples;
        for (std::int64_t y = 0; y < kChunkSamples; ++y)
            for (std::int64_t x = 0; x < kChunkSamples; ++x) {
                const std::uint32_t id = t.at(x, y, 0);
                if (id == desc.defaultStored(0)) continue;
                auto [it, fresh] = boxes.try_emplace(id, std::array<std::int64_t, 4>{ox + x, oy + y, ox + x + 1, oy + y + 1});
                if (!fresh) {
                    auto& b = it->second;
                    b[0] = std::min(b[0], ox + x); b[1] = std::min(b[1], oy + y);
                    b[2] = std::max(b[2], ox + x + 1); b[3] = std::max(b[3], oy + y + 1);
                }
            }
        for (const auto& [id, box] : boxes) regions_[desc.name][id][key] = box;
    }
    for (auto it = poi_.begin(); it != poi_.end();) it = it->second == key ? poi_.erase(it) : std::next(it);
    for (const auto& f : chunk.fragments) if (f.kind == "poi") poi_[f.id] = key;
}

void WorldSource::index(const std::string& id, const std::optional<VectorIndexEntry>& entry) {
    if (entry) vectors_[id] = *entry;
    else vectors_.erase(id);
}

bool WorldSource::commit(std::string* why) {
    nlohmann::json j;
    j["format"] = kSourceFormat;
    j["kind"] = "campfire world source";
    j["schema"] = schemaJson(schema_);
    j["blocks"] = blockTypes_;
    j["next_block"] = nextBlockType_;
    j["chunks"] = nlohmann::json::object();
    for (const auto& [key, r] : chunks_)
        j["chunks"][stemOf(key)] = {{"file", r.file}, {"payload", r.payloadHash}, {"layers", r.layers}};
    nlohmann::json vectors = nlohmann::json::object();
    for (const auto& [id, e] : vectors_) {
        nlohmann::json chunks = nlohmann::json::array();
        for (const auto& k : e.chunks) chunks.push_back(stemOf(k));
        vectors[id] = {{"kind", e.kind}, {"bounds", {e.bounds.minX, e.bounds.minY, e.bounds.maxX, e.bounds.maxY}},
                       {"chunks", chunks}};
    }
    nlohmann::json regions = nlohmann::json::object();
    for (const auto& [layer, ids] : regions_)
        for (const auto& [id, chunks] : ids)
            for (const auto& [key, box] : chunks) regions[layer][std::to_string(id)][stemOf(key)] = box;
    nlohmann::json poi = nlohmann::json::object();
    for (const auto& [id, key] : poi_) poi[id] = stemOf(key);
    std::error_code ec;
    std::filesystem::create_directories(root_ / "indexes", ec);
    std::filesystem::create_directories(root_ / "chunks", ec);
    // Indexes first, the metadata last: the metadata is the commit.
    return ws::writeFileAtomic(root_ / "indexes" / "vectors.idx", vectors.dump(1), why) &&
           ws::writeFileAtomic(root_ / "indexes" / "regions.idx", regions.dump(1), why) &&
           ws::writeFileAtomic(root_ / "indexes" / "poi.idx", poi.dump(1), why) &&
           ws::writeFileAtomic(root_ / "metadata.world", j.dump(1), why);
}

} // namespace engine::world_source
