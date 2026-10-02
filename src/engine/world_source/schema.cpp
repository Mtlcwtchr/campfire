#include "engine/world_source/schema.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

namespace engine::world_source {
namespace {

bool fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

std::optional<Interpolation> interpolationNamed(const std::string& name) {
    if (name == "nearest") return Interpolation::Nearest;
    if (name == "bilinear") return Interpolation::Bilinear;
    if (name == "bicubic") return Interpolation::Bicubic;
    if (name == "exact") return Interpolation::Exact;
    return std::nullopt;
}
std::optional<RasterKind> kindNamed(const std::string& name) {
    if (name == "height") return RasterKind::Height;
    if (name == "control") return RasterKind::Control;
    if (name == "categorical") return RasterKind::Categorical;
    if (name == "flags") return RasterKind::Flags;
    return std::nullopt;
}
std::optional<SampleType> typeNamed(const std::string& name) {
    if (name == "u8") return SampleType::U8;
    if (name == "u16") return SampleType::U16;
    if (name == "rgba8") return SampleType::RGBA8;
    if (name == "rgba16") return SampleType::RGBA16;
    return std::nullopt;
}

constexpr const char* kChannelKeys[4]{"R", "G", "B", "A"};

} // namespace

std::uint8_t channelsOf(SampleType type) { return type == SampleType::RGBA8 || type == SampleType::RGBA16 ? 4 : 1; }
std::uint8_t bytesPerChannel(SampleType type) { return type == SampleType::U16 || type == SampleType::RGBA16 ? 2 : 1; }

const char* interpolationName(Interpolation i) {
    switch (i) {
        case Interpolation::Nearest: return "nearest";
        case Interpolation::Bilinear: return "bilinear";
        case Interpolation::Bicubic: return "bicubic";
        case Interpolation::Exact: return "exact";
    }
    return "nearest";
}
const char* kindName(RasterKind k) {
    switch (k) {
        case RasterKind::Height: return "height";
        case RasterKind::Control: return "control";
        case RasterKind::Categorical: return "categorical";
        case RasterKind::Flags: return "flags";
    }
    return "control";
}
const char* typeName(SampleType t) {
    switch (t) {
        case SampleType::U8: return "u8";
        case SampleType::U16: return "u16";
        case SampleType::RGBA8: return "rgba8";
        case SampleType::RGBA16: return "rgba16";
    }
    return "u8";
}

std::uint16_t RasterDesc::encode(std::size_t channel, double value) const {
    const auto& c = channels[channel];
    if (kind == RasterKind::Categorical || kind == RasterKind::Flags || !(c.max > c.min))
        return std::uint16_t(std::clamp<double>(std::round(value), 0, maximum()));
    const double t = std::clamp((value - c.min) / (c.max - c.min), 0.0, 1.0);
    return std::uint16_t(std::lround(t * maximum()));
}

double RasterDesc::decode(std::size_t channel, std::uint16_t stored) const {
    const auto& c = channels[channel];
    if (kind == RasterKind::Categorical || kind == RasterKind::Flags || !(c.max > c.min)) return stored;
    return c.min + (c.max - c.min) * double(stored) / double(maximum());
}

std::optional<std::uint32_t> RasterDesc::idNamed(const std::string& name) const {
    for (const auto& [id, n] : ids)
        if (n == name) return id;
    return std::nullopt;
}

const RasterDesc* Schema::raster(const std::string& name) const {
    const auto it = std::find_if(rasters.begin(), rasters.end(), [&](const RasterDesc& r) { return r.name == name; });
    return it == rasters.end() ? nullptr : &*it;
}

std::optional<Schema> parseSchema(const nlohmann::json& j, std::string* why) {
    Schema s;
    if (!j.is_object()) { fail(why, "schema is not an object"); return std::nullopt; }
    s.version = j.value("version", kSchemaVersion);
    if (s.version > kSchemaVersion) { fail(why, "schema is from a newer version"); return std::nullopt; }
    if (!j.contains("world") || !j["world"].is_object()) { fail(why, "schema has no \"world\""); return std::nullopt; }
    const auto& w = j["world"];
    s.world.widthMetres = w.value("width_m", 0.0);
    s.world.heightMetres = w.value("height_m", 0.0);
    s.world.sampleMetres = w.value("sample_m", 256.0);
    s.world.chunkMetres = w.value("chunk_m", std::int64_t(32768));
    if (!(s.world.widthMetres > 0) || !(s.world.heightMetres > 0) || !(s.world.sampleMetres > 0)) {
        fail(why, "world extents must be positive");
        return std::nullopt;
    }
    if (s.world.chunkMetres != 32768 || s.world.sampleMetres != 256) {
        fail(why, "only sample_m 256 and chunk_m 32768 (128 x 128 samples a chunk) are supported");
        return std::nullopt;
    }
    if (std::fmod(double(s.world.chunkMetres), s.world.sampleMetres) != 0) {
        fail(why, "a chunk must be a whole number of samples");
        return std::nullopt;
    }
    if (j.contains("rasters")) {
        if (!j["rasters"].is_object()) { fail(why, "\"rasters\" is not an object"); return std::nullopt; }
        for (const auto& [name, r] : j["rasters"].items()) {
            RasterDesc d;
            d.name = name;
            d.file = r.value("file", std::string());
            const auto type = typeNamed(r.value("type", std::string("u8")));
            const auto kind = kindNamed(r.value("kind", std::string("control")));
            if (!type) { fail(why, "raster " + name + ": unknown type"); return std::nullopt; }
            if (!kind) { fail(why, "raster " + name + ": unknown kind"); return std::nullopt; }
            d.type = *type;
            d.kind = *kind;
            d.version = r.value("version", 1);
            const auto channels = channelsOf(d.type);
            d.channels.resize(channels);
            if (d.kind == RasterKind::Height) {
                if (channels != 1) { fail(why, "raster " + name + ": a height is one channel"); return std::nullopt; }
                auto& c = d.channels[0];
                c.name = "height_m";
                c.min = r.value("min_m", -1000.0);
                c.max = r.value("max_m", 7000.0);
                c.defaultValue = r.value("default_m", -60.0);
                c.interpolation = interpolationNamed(r.value("interpolation", std::string("bicubic")))
                                          .value_or(Interpolation::Bicubic);
                c.optional = r.value("optional", false);
                c.version = d.version;
                if (!(c.max > c.min)) { fail(why, "raster " + name + ": max_m must exceed min_m"); return std::nullopt; }
            } else if (d.kind == RasterKind::Control) {
                const auto& described = r.value("channels", nlohmann::json::object());
                for (std::size_t k = 0; k < channels; ++k) {
                    const std::string key = channels == 1 ? (described.contains("R") ? "R" : "V") : kChannelKeys[k];
                    auto& c = d.channels[k];
                    if (!described.contains(key)) {
                        c.name = name + "." + kChannelKeys[k];
                        continue;
                    }
                    const auto& cj = described[key];
                    c.name = cj.value("name", name + "." + kChannelKeys[k]);
                    if (cj.contains("range") && cj["range"].is_array() && cj["range"].size() == 2) {
                        c.min = cj["range"][0].get<double>();
                        c.max = cj["range"][1].get<double>();
                    }
                    c.defaultValue = cj.value("default", c.min);
                    const auto interpolation = interpolationNamed(cj.value("interpolation", std::string("bilinear")));
                    if (!interpolation) { fail(why, "raster " + name + ": unknown interpolation"); return std::nullopt; }
                    c.interpolation = *interpolation;
                    c.optional = cj.value("optional", true);
                    c.version = cj.value("version", 1);
                    if (cj.contains("affects")) c.affects = cj["affects"].get<std::vector<std::string>>();
                }
            } else {
                // Categorical ids and flags are never filtered (§5): nearest,
                // exact, and their default is nought unless said otherwise.
                for (std::size_t k = 0; k < channels; ++k) {
                    auto& c = d.channels[k];
                    c.name = d.kind == RasterKind::Flags ? "flags" : "id";
                    c.min = 0;
                    c.max = 0;
                    c.defaultValue = r.value("default", 0.0);
                    c.interpolation = d.kind == RasterKind::Flags ? Interpolation::Exact : Interpolation::Nearest;
                    c.optional = r.value("optional", true);
                    c.version = d.version;
                }
                if (d.kind == RasterKind::Flags && r.contains("bits"))
                    d.bits = r["bits"].get<std::map<std::string, int>>();
                if (d.kind == RasterKind::Categorical && r.contains("ids")) {
                    if (!r["ids"].is_object()) { fail(why, "raster " + name + ": \"ids\" is not an object"); return std::nullopt; }
                    std::set<std::string> names;
                    for (const auto& [key, value] : r["ids"].items()) {
                        char* end = nullptr;
                        const unsigned long id = std::strtoul(key.c_str(), &end, 10);
                        if (key.empty() || *end != '\0' || id > d.maximum() || !value.is_string() ||
                            value.get<std::string>().empty()) {
                            fail(why, "raster " + name + ": legend entry \"" + key + "\" must be an id within the type and a name");
                            return std::nullopt;
                        }
                        if (!names.insert(value.get<std::string>()).second) {
                            fail(why, "raster " + name + ": legend names \"" + value.get<std::string>() + "\" twice");
                            return std::nullopt;
                        }
                        d.ids[std::uint32_t(id)] = value.get<std::string>();
                    }
                }
            }
            s.rasters.push_back(std::move(d));
        }
    }
    if (j.contains("vectors")) s.vectors = j["vectors"].get<std::map<std::string, std::string>>();
    s.poi = j.value("poi", std::string());
    return s;
}

nlohmann::json schemaJson(const Schema& s) {
    nlohmann::json j;
    j["format"] = kPackageFormat;
    j["version"] = s.version;
    j["world"] = {{"width_m", s.world.widthMetres}, {"height_m", s.world.heightMetres},
                  {"sample_m", s.world.sampleMetres}, {"chunk_m", s.world.chunkMetres}};
    j["rasters"] = nlohmann::json::object();
    for (const auto& d : s.rasters) {
        nlohmann::json r;
        r["file"] = d.file;
        r["type"] = typeName(d.type);
        r["kind"] = kindName(d.kind);
        r["version"] = d.version;
        if (d.kind == RasterKind::Height) {
            const auto& c = d.channels[0];
            r["min_m"] = c.min;
            r["max_m"] = c.max;
            r["default_m"] = c.defaultValue;
            r["interpolation"] = interpolationName(c.interpolation);
            r["optional"] = c.optional;
        } else if (d.kind == RasterKind::Control) {
            r["channels"] = nlohmann::json::object();
            for (std::size_t k = 0; k < d.channels.size(); ++k) {
                const auto& c = d.channels[k];
                nlohmann::json cj{{"name", c.name}, {"range", {c.min, c.max}}, {"default", c.defaultValue},
                                  {"interpolation", interpolationName(c.interpolation)},
                                  {"optional", c.optional}, {"version", c.version}};
                if (!c.affects.empty()) cj["affects"] = c.affects;
                r["channels"][d.channels.size() == 1 ? "R" : kChannelKeys[k]] = cj;
            }
        } else {
            r["default"] = d.channels[0].defaultValue;
            r["optional"] = d.channels[0].optional;
            if (!d.bits.empty()) r["bits"] = d.bits;
            if (!d.ids.empty()) {
                nlohmann::json legend = nlohmann::json::object();
                for (const auto& [id, n] : d.ids) legend[std::to_string(id)] = n;
                r["ids"] = legend;
            }
        }
        j["rasters"][d.name] = r;
    }
    j["vectors"] = s.vectors;
    if (!s.poi.empty()) j["poi"] = s.poi;
    return j;
}

std::optional<PackageManifest> parsePackage(const nlohmann::json& j, std::string* why) {
    if (j.value("format", std::string(kPackageFormat)) != kPackageFormat) {
        fail(why, "world.json is not a campfire world-authoring package");
        return std::nullopt;
    }
    auto schema = parseSchema(j, why);
    if (!schema) return std::nullopt;
    PackageManifest m;
    m.schema = std::move(*schema);
    if (j.contains("origin_m")) { m.originX = j["origin_m"][0].get<double>(); m.originY = j["origin_m"][1].get<double>(); }
    m.sizeX = m.schema.world.widthMetres - m.originX;
    m.sizeY = m.schema.world.heightMetres - m.originY;
    if (j.contains("size_m")) { m.sizeX = j["size_m"][0].get<double>(); m.sizeY = j["size_m"][1].get<double>(); }
    const double s = m.schema.world.sampleMetres;
    const auto whole = [&](double v) { return std::fabs(v / s - std::round(v / s)) < 1e-9; };
    if (!whole(m.originX) || !whole(m.originY) || !whole(m.sizeX) || !whole(m.sizeY) || !(m.sizeX > 0) ||
        !(m.sizeY > 0) || m.originX < 0 || m.originY < 0 ||
        m.originX + m.sizeX > m.schema.world.widthMetres + 1e-6 ||
        m.originY + m.sizeY > m.schema.world.heightMetres + 1e-6) {
        fail(why, "origin_m/size_m must be whole samples inside the world");
        return std::nullopt;
    }
    return m;
}

nlohmann::json packageJson(const PackageManifest& m) {
    auto j = schemaJson(m.schema);
    j["origin_m"] = {m.originX, m.originY};
    j["size_m"] = {m.sizeX, m.sizeY};
    return j;
}

} // namespace engine::world_source
