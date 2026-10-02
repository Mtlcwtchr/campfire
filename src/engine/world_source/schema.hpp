#pragma once
// What an authoring package and a WorldSource say their layers are
// (world_authoring_import_export_spec §2.1, §5).
//
// Nothing about the meaning of a raster's channels is written into the code:
// world.json names each layer, its storage type, and for every channel its
// semantic name, range, default, interpolation, whether it may be missing, and
// a version. The importer reads by that description and the exporter writes
// it back, so a new control channel is a line of JSON, not a change here.
//
//   {
//     "format": "campfire.world-authoring", "version": 1,
//     "world": { "width_m": 2097152, "height_m": 2097152, "sample_m": 256,
//                "chunk_m": 32768 },
//     "origin_m": [0, 0],          // a partial package: where it lies in the world
//     "size_m": [..., ...],        // and how much of it it covers (default: the world)
//     "rasters": {
//       "height":    { "file": "raster/height.png", "type": "u16", "kind": "height",
//                      "min_m": -1000, "max_m": 7000, "default_m": -60,
//                      "interpolation": "bicubic" },
//       "control_0": { "file": "raster/control_0.png", "type": "rgba8", "kind": "control",
//                      "channels": { "R": { "name": "moisture_bias", "range": [0, 1],
//                                           "default": 0.5, "interpolation": "bilinear",
//                                           "optional": true, "version": 1 }, ... } },
//       "region_ids": { "file": "raster/region_ids.png", "type": "u16", "kind": "categorical" },
//       "ground":     { "file": "raster/ground.png", "type": "u8", "kind": "categorical",
//                      "ids": { "1": "fertile", "6": "wastes" } },   // the legend: id -> name
//       "flags":      { "file": "raster/flags.png", "type": "u8", "kind": "flags",
//                      "bits": { "protect": 0, "force": 1, "disable": 2 } }
//     },
//     "vectors": { "coastline": "vectors/coastline.json", "rivers": "vectors/rivers.json", ... },
//     "poi": "poi/poi.json"
//   }
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace engine::world_source {

inline constexpr int kSchemaVersion = 1;
inline constexpr const char* kPackageFormat = "campfire.world-authoring";

enum class Interpolation : std::uint8_t { Nearest, Bilinear, Bicubic, Exact };
enum class RasterKind : std::uint8_t { Height, Control, Categorical, Flags };
// Storage of one sample: how many channels, and how wide each is.
enum class SampleType : std::uint8_t { U8, U16, RGBA8, RGBA16 };

std::uint8_t channelsOf(SampleType type);
std::uint8_t bytesPerChannel(SampleType type);

struct ChannelDesc {
    std::string name;                 // semantic: "moisture_bias"
    double min = 0, max = 1;          // what the stored range means
    double defaultValue = 0;          // in those units
    Interpolation interpolation = Interpolation::Bilinear;
    bool optional = true;             // may the package leave it out
    int version = 1;
    // Which derived systems go stale when it changes (§10). Empty: the
    // layer kind's own list.
    std::vector<std::string> affects;
    bool operator==(const ChannelDesc&) const = default;
};

struct RasterDesc {
    std::string name;                 // "height", "control_0"
    std::string file;                 // inside the package
    SampleType type = SampleType::U16;
    RasterKind kind = RasterKind::Control;
    std::vector<ChannelDesc> channels;   // one per stored channel, in order
    std::map<std::string, int> bits;     // flags: name -> bit
    // Categorical: what each id means. Ids are a package's or a source's own
    // numbering; what is stable is the name, and an import renumbers by it.
    std::map<std::uint32_t, std::string> ids;
    int version = 1;
    // The number stored for a channel's default, and to and from units.
    [[nodiscard]] std::uint16_t encode(std::size_t channel, double value) const;
    [[nodiscard]] double decode(std::size_t channel, std::uint16_t stored) const;
    [[nodiscard]] std::uint16_t defaultStored(std::size_t channel) const {
        return encode(channel, channels[channel].defaultValue);
    }
    [[nodiscard]] std::uint32_t maximum() const { return bytesPerChannel(type) == 2 ? 65535u : 255u; }
    // The legend's id for a name, if it has one.
    [[nodiscard]] std::optional<std::uint32_t> idNamed(const std::string& name) const;
    bool operator==(const RasterDesc&) const = default;
};

struct WorldExtent {
    double widthMetres = 0, heightMetres = 0;
    double sampleMetres = 256;
    std::int64_t chunkMetres = 32768;
    [[nodiscard]] std::int64_t samplesX() const { return std::int64_t(widthMetres / sampleMetres + 0.5); }
    [[nodiscard]] std::int64_t samplesY() const { return std::int64_t(heightMetres / sampleMetres + 0.5); }
    [[nodiscard]] std::int64_t samplesPerChunk() const { return std::int64_t(double(chunkMetres) / sampleMetres + 0.5); }
    bool operator==(const WorldExtent&) const = default;
};

struct Schema {
    int version = kSchemaVersion;
    WorldExtent world;
    std::vector<RasterDesc> rasters;
    std::map<std::string, std::string> vectors;   // kind -> file
    std::string poi;                              // file, or empty
    [[nodiscard]] const RasterDesc* raster(const std::string& name) const;
    bool operator==(const Schema&) const = default;
};

// A package's world.json: the schema and where it lies in the world.
struct PackageManifest {
    Schema schema;
    double originX = 0, originY = 0;   // metres
    double sizeX = 0, sizeY = 0;       // metres; the world's when not given
};

std::optional<PackageManifest> parsePackage(const nlohmann::json& json, std::string* why = nullptr);
nlohmann::json packageJson(const PackageManifest& manifest);
// The schema alone, as a WorldSource's metadata keeps it.
std::optional<Schema> parseSchema(const nlohmann::json& json, std::string* why = nullptr);
nlohmann::json schemaJson(const Schema& schema);

const char* interpolationName(Interpolation interpolation);
const char* kindName(RasterKind kind);
const char* typeName(SampleType type);

} // namespace engine::world_source
