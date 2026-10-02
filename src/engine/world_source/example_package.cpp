#include "engine/world_source/example_package.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "engine/world_source/package_vectors.hpp"
#include "engine/world_source/png_io.hpp"
#include "engine/world_store/atomic_file.hpp"

namespace engine::world_source {
namespace {

double hash01(int x, int y, int seed) {
    std::uint32_t h = std::uint32_t(x) * 374761393u + std::uint32_t(y) * 668265263u + std::uint32_t(seed) * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return double((h ^ (h >> 16)) & 0xffffff) / double(0xffffff);
}
double noise(double x, double y, int seed) {
    const int ix = int(std::floor(x)), iy = int(std::floor(y));
    const double fx = x - ix, fy = y - iy, sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const double a = hash01(ix, iy, seed), b = hash01(ix + 1, iy, seed);
    const double c = hash01(ix, iy + 1, seed), d = hash01(ix + 1, iy + 1, seed);
    return a + (b - a) * sx + (c - a) * sy + (a - b - c + d) * sx * sy;
}
double fractal(double x, double y, int seed) {
    double sum = 0, amplitude = 1, total = 0;
    for (int o = 0; o < 6; ++o, amplitude *= 0.5, x *= 2, y *= 2) {
        sum += noise(x, y, seed + o) * amplitude;
        total += amplitude;
    }
    return sum / total;
}

} // namespace

std::optional<std::string> writeExamplePackage(const std::filesystem::path& package, double km, std::string* why) {
    const double metres = std::max(1.0, std::round(km * 1000 / 32768)) * 32768;   // whole chunks
    const std::uint32_t n = std::uint32_t(metres / 256);
    PackageManifest m;
    m.schema.world = {metres, metres, 256, 32768};
    m.sizeX = m.sizeY = metres;
    RasterDesc height;
    height.name = "height"; height.file = "raster/height.png"; height.type = SampleType::U16; height.kind = RasterKind::Height;
    height.channels = {ChannelDesc{"height_m", -1000, 7000, -60, Interpolation::Bicubic, false, 1, {}}};
    RasterDesc control;
    control.name = "control_0"; control.file = "raster/control_0.png"; control.type = SampleType::RGBA8; control.kind = RasterKind::Control;
    control.channels = {ChannelDesc{"moisture_bias", 0, 1, 0.5, Interpolation::Bilinear, true, 1, {"climate", "ecology"}},
                        ChannelDesc{"forest_bias", 0, 1, 0.5, Interpolation::Bilinear, true, 1, {"ecology", "vegetation"}},
                        ChannelDesc{"mountain_strength", 0, 1, 0, Interpolation::Bilinear, true, 1, {"terrain", "drainage"}},
                        ChannelDesc{"erosion_strength", 0, 1, 0.5, Interpolation::Bilinear, true, 1, {"terrain"}}};
    RasterDesc regions;
    regions.name = "region_ids"; regions.file = "raster/region_ids.png"; regions.type = SampleType::U16;
    regions.kind = RasterKind::Categorical;
    regions.channels = {ChannelDesc{"id", 0, 0, 0, Interpolation::Nearest, true, 1, {}}};
    RasterDesc flags;
    flags.name = "flags"; flags.file = "raster/flags.png"; flags.type = SampleType::U8; flags.kind = RasterKind::Flags;
    flags.channels = {ChannelDesc{"flags", 0, 0, 0, Interpolation::Exact, true, 1, {}}};
    flags.bits = {{"protect", 0}, {"force", 1}, {"disable", 2}};
    m.schema.rasters = {height, control, regions, flags};
    m.schema.vectors = {{"rivers", "vectors/rivers.json"}, {"lakes", "vectors/lakes.json"},
                        {"ridges", "vectors/ridges.json"}, {"regions", "vectors/regions.json"}};
    m.schema.poi = "poi/poi.json";

    // An island: fractal ground under a radial fall-off, sea everywhere else.
    // Sea samples carry every layer's default, so sea chunks keep nothing.
    Image h{n, n, 1, 16, {}, {}}, c{n, n, 4, 8, {}, {}}, r{n, n, 1, 16, {}, {}}, f{n, n, 1, 8, {}, {}};
    h.allocate(); c.allocate(); r.allocate(); f.allocate();
    for (std::uint32_t y = 0; y < n; ++y)
        for (std::uint32_t x = 0; x < n; ++x) {
            const double u = (x + 0.5) / n, v = (y + 0.5) / n;
            const double d = std::hypot(u - 0.5, v - 0.5);
            const double land = 0.5 + 0.6 * fractal(u * 6, v * 6, 7) - d * 2.5;
            const bool isLand = land > 0.3;
            h.set(x, y, 0, isLand ? height.encode(0, std::min(7000.0, 1 + (land - 0.3) * 6000)) : height.defaultStored(0));
            for (std::uint8_t ch = 0; ch < 4; ++ch) c.set(x, y, ch, control.defaultStored(ch));
            if (isLand) {
                c.set(x, y, 0, std::uint16_t(255 * fractal(u * 3, v * 3, 11)));
                c.set(x, y, 1, std::uint16_t(255 * fractal(u * 4, v * 4, 12)));
                c.set(x, y, 2, std::uint16_t(255 * std::clamp((land - 0.5) * 3, 0.0, 1.0)));
            }
            r.set(x, y, 0, isLand ? std::uint16_t(1 + (u > 0.5) + 2 * (v > 0.5)) : 0);
            f.set(x, y, 0, isLand && d < 0.05 ? 1 : 0);
        }
    if (!writePng(package / height.file, h, why) || !writePng(package / control.file, c, why) ||
        !writePng(package / regions.file, r, why) || !writePng(package / flags.file, f, why))
        return std::nullopt;

    const auto at = [&](double u, double v) { return std::array<double, 2>{u * metres, v * metres}; };
    const Feature river{"river:amber", "rivers", Geometry::Line,
                        {Ring{{at(0.5, 0.45), at(0.55, 0.5), at(0.62, 0.52), at(0.7, 0.6)}, {{"width", {4, 8, 16, 30}}}}},
                        {{"flow_class", 2}, {"strength", 200}}};
    const Feature lake{"lake:mere", "lakes", Geometry::Polygon,
                       {Ring{{at(0.42, 0.40), at(0.47, 0.39), at(0.48, 0.44), at(0.43, 0.45)}, {}}},
                       {{"water_level_m", 120.0}}};
    const Feature pond{"lake:pond", "lakes", Geometry::Polygon,
                       {Ring{{at(0.56, 0.56), at(0.58, 0.56), at(0.57, 0.58)}, {}}}, {{"water_level_m", 40.0}}};
    const Feature ridge{"ridge:spine", "ridges", Geometry::Line,
                        {Ring{{at(0.35, 0.6), at(0.5, 0.55), at(0.62, 0.42)}, {}}},
                        {{"strength", 0.8}, {"profile", "sharp"}}};
    const Feature north{"region:north", "regions", Geometry::Polygon,
                        {Ring{{at(0.3, 0.3), at(0.7, 0.3), at(0.7, 0.5), at(0.3, 0.5)}, {}}}, {{"name", "North"}}};
    const Feature ford{"poi:ford", "poi", Geometry::Point, {Ring{{at(0.55, 0.5)}, {}}},
                       {{"type", "settlement"}, {"z", 0.0}, {"yaw_deg", 0.0}, {"scale", 1.0}, {"metadata", {{"people", 40}}}}};
    if (!writeVectorFile(package / "vectors/rivers.json", "rivers", {river}, why) ||
        !writeVectorFile(package / "vectors/lakes.json", "lakes", {lake, pond}, why) ||
        !writeVectorFile(package / "vectors/ridges.json", "ridges", {ridge}, why) ||
        !writeVectorFile(package / "vectors/regions.json", "regions", {north}, why) ||
        !writePoiFile(package / "poi/poi.json", {ford}, why) ||
        !world_store::writeFileAtomic(package / "world.json", packageJson(m).dump(2), why))
        return std::nullopt;
    return "example package: " + std::to_string(n) + " x " + std::to_string(n) + " samples, " +
           std::to_string(int(metres / 1000)) + " km a side, in " + package.string();
}

} // namespace engine::world_source
