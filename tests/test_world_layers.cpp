#include "framework.hpp"
#include "game/generation/world_brush.hpp"
#include "game/generation/world_layers.hpp"
#include "game/generation/world_layout.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/generation/world_noise.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <utility>

namespace {
using namespace generation;

WorldMapParams quick(const WorldLayout& layout) {
    // The generator the editor runs, without the H64 foundation and the hybrid
    // landforms, which cost minutes and are not what is checked here.
    WorldMapParams p = paramsFor(layout);
    p.hybridTerrain = false;
    p.stagedTerrain = false;
    return p;
}

WorldLayout oneRegion(std::uint64_t seed, std::int32_t sea = 50, std::int32_t erosion = 3) {
    WorldLayout layout = emptyLayout(1, 1, seed);
    WorldPreset preset;
    preset.name = "test";
    preset.params.seaPercent = sea;
    preset.params.erosionPasses = erosion;
    generateAll(layout, preset);
    return layout;
}

Brush brushOf(BrushTool tool, double radius, float strength = 1.0f, float hardness = 1.0f, float value = 0.0f) {
    Brush b;
    b.tool = tool;
    b.radius = radius;
    b.strength = strength;
    b.hardness = hardness;
    b.value = value;
    return b;
}

// Cells, and metres at their centres.
double metres(std::int32_t cells) { return (cells + 0.5) * kMetresPerCell; }

template <class Test>
double shareIn(const WorldMapData& w, std::int32_t cx, std::int32_t cy, std::int32_t r, Test&& test) {
    std::int64_t hit = 0, all = 0;
    for (std::int32_t y = cy - r; y <= cy + r; ++y)
        for (std::int32_t x = cx - r; x <= cx + r; ++x) {
            if (!w.inBounds({x, y}) || (x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
            hit += test(w.at({x, y})) ? 1 : 0;
            ++all;
        }
    return all ? double(hit) / double(all) : 0.0;
}

double landIn(const WorldMapData& w, std::int32_t x0, std::int32_t y0, std::int32_t x1, std::int32_t y1) {
    std::int64_t land = 0, all = 0;
    for (std::int32_t y = y0; y < y1; ++y)
        for (std::int32_t x = x0; x < x1; ++x) {
            land += w.at({x, y}).sea ? 0 : 1;
            ++all;
        }
    return all ? double(land) / double(all) : 0.0;
}
} // namespace

TEST(world_layers_have_their_own_resolutions_and_fit_the_regions) {
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const LayerDef& def = layerDef(LayerId(i));
        CHECK(def.texelMetres >= kMetresPerCell);
        CHECK_EQ(def.texelMetres % kMetresPerCell, 0);
        const std::int32_t cells = def.texelMetres / kMetresPerCell;
        CHECK_EQ(cells & (cells - 1), 0);                 // a power of two of the cell
        CHECK_EQ(kRegionMetres % def.texelMetres, 0);      // texel lines fall on region lines
        CHECK(def.low < def.high);
        CHECK(def.paint >= def.low && def.paint <= def.high);
        CHECK(layerNamed(def.name).has_value() && *layerNamed(def.name) == def.id);
    }
    // They are not all the same resolution: that is the point of them.
    CHECK(layerDef(LayerId::Continents).texelMetres < layerDef(LayerId::Rain).texelMetres);
    CHECK(layerDef(LayerId::Rain).texelMetres < layerDef(LayerId::Sea).texelMetres);

    const WorldLayout layout = emptyLayout(3, 2, 7);
    CHECK_EQ(layout.layer(LayerId::Continents).texelsX(), 3 * 64);
    CHECK_EQ(layout.layer(LayerId::Continents).texelsY(), 2 * 64);
    CHECK_EQ(layout.layer(LayerId::Sea).texelsX(), 3 * 8);
    CHECK_EQ(layout.layer(LayerId::Weathering).texelsY(), 2 * 32);
    CHECK(!layout.anyPainted());
}

TEST(world_layer_brush_is_no_finer_than_a_texel_and_no_wider_than_its_limit) {
    WorldLayout layout = oneRegion(3);
    const BrushLimits fine = brushLimits(LayerId::Continents, layout);
    const BrushLimits coarse = brushLimits(LayerId::Sea, layout);
    CHECK_EQ(fine.minRadius, 2048.0);
    CHECK_EQ(coarse.minRadius, 16384.0);
    // No wider than kMaxBrushTexels texels, nor than half the world.
    CHECK(fine.maxRadius <= fine.texel * kMaxBrushTexels);
    CHECK(fine.maxRadius <= double(layout.widthMetres()) * 0.5);
    CHECK(coarse.maxRadius >= coarse.minRadius);
    // Radii are whole texels, and the smallest brush of each layer is its texel.
    CHECK_EQ(legalRadius(LayerId::Continents, layout, 100.0), 2048.0);
    CHECK_EQ(legalRadius(LayerId::Continents, layout, 5000.0), 4096.0);
    CHECK_EQ(legalRadius(LayerId::Sea, layout, 5000.0), 16384.0);
    CHECK_EQ(legalRadius(LayerId::Continents, layout, 1e9), fine.maxRadius);

    // The smallest brush, put down on a texel's centre, paints that texel and
    // nothing else - the finest thing the layer can hold.
    const double centre = 10.5 * 2048.0;
    const TexelRect one = dab(layout, LayerId::Continents,
                              brushOf(BrushTool::Paint, 1.0, 1.0f, 0.5f, 900.0f), centre, centre);
    CHECK(!one.empty());
    CHECK_EQ(one.x0, 10);
    CHECK_EQ(one.x1, 10);
    CHECK_EQ(one.y0, 10);
    CHECK_EQ(one.y1, 10);
    CHECK_EQ(layout.layer(LayerId::Continents).cover(10, 10), 1.0f);
    CHECK_EQ(layout.layer(LayerId::Continents).cover(11, 10), 0.0f);
    // The same smallest brush on the coarse layer covers sixty-four times the
    // ground: a texel of it is eight of the fine one's a side.
    const TexelRect big = dab(layout, LayerId::Sea, brushOf(BrushTool::Paint, 1.0, 1.0f, 0.5f, 80.0f),
                              2.5 * 16384.0, 2.5 * 16384.0);
    CHECK(!big.empty());
    CHECK_EQ(big.x1 - big.x0, 0);
    // A brush no bigger than its limit costs no more than its limit.
    const TexelRect widest = dab(layout, LayerId::Continents, brushOf(BrushTool::Paint, 1e9, 1.0f, 1.0f, 900.0f),
                                 double(layout.widthMetres()) / 2, double(layout.heightMetres()) / 2);
    CHECK(widest.x1 - widest.x0 + 1 <= 2 * kMaxBrushTexels + 1);
}

TEST(world_layer_paint_and_erase_composite_over_what_the_generator_made) {
    WorldLayout layout = oneRegion(5);
    const LayerId sea = LayerId::Sea;
    const double t = 16384.0;
    // Half strength twice: three quarters covered, all of it the painted value.
    dab(layout, sea, brushOf(BrushTool::Paint, t, 0.5f, 1.0f, 90.0f), 2.5 * t, 2.5 * t);
    dab(layout, sea, brushOf(BrushTool::Paint, t, 0.5f, 1.0f, 90.0f), 2.5 * t, 2.5 * t);
    const LayerMap& map = layout.layer(sea);
    CHECK(std::abs(map.cover(2, 2) - 0.75f) < 1e-5f);
    CHECK(std::abs(map.value(2, 2) - 90.0f) < 1e-3f);
    // Over the region's own 50 per cent: a quarter of it, three quarters paint.
    const LayerMap::Sample at = map.sample(2.0, 2.0);
    CHECK(std::abs((50.0f * (1.0f - at.cover) + at.painted) - (50.0f * 0.25f + 90.0f * 0.75f)) < 1e-3f);

    // Between a painted texel and an unpainted one the paint fades into what
    // the generator made - it does not fade towards nought.
    dab(layout, sea, brushOf(BrushTool::Paint, t, 1.0f, 1.0f, 90.0f), 2.5 * t, 2.5 * t);
    const LayerMap::Sample half = map.sample(2.5, 2.0);
    CHECK(std::abs(half.cover - 0.5f) < 1e-5f);
    CHECK(std::abs((50.0f * (1.0f - half.cover) + half.painted) - 70.0f) < 1e-3f);

    // Wiping it off gives back the generator's own, and lets the tile go.
    dab(layout, sea, brushOf(BrushTool::Erase, t, 1.0f, 1.0f), 2.5 * t, 2.5 * t);
    CHECK_EQ(map.cover(2, 2), 0.0f);
    CHECK(map.empty());
    CHECK(!layout.anyPainted());

    // A delta layer adds: nought is no change, and Erase takes it back there.
    dab(layout, LayerId::Ranges, brushOf(BrushTool::Paint, 2048.0, 1.0f, 1.0f, 300.0f), 20.5 * 2048, 20.5 * 2048);
    CHECK_EQ(layout.layer(LayerId::Ranges).value(20, 20), 300.0f);
    CHECK_EQ(layout.layer(LayerId::Ranges).sample(20.0, 20.0).cover, 0.0f);
    dab(layout, LayerId::Ranges, brushOf(BrushTool::Erase, 2048.0, 1.0f, 1.0f), 20.5 * 2048, 20.5 * 2048);
    CHECK(layout.layer(LayerId::Ranges).empty());
}

TEST(world_layer_raise_lower_and_smooth_start_from_what_is_there) {
    WorldLayout layout = oneRegion(8, 50, 3);
    const LayerBase base = layerBase(layout, LayerId::Weathering);
    CHECK(std::abs(base.at(5, 5) - 3.0f) < 1e-4f);         // the region's own three passes
    const double t = 4096.0;
    // A tenth of the layer's range per full-strength dab, from what the place IS.
    dab(layout, LayerId::Weathering, brushOf(BrushTool::Raise, t, 1.0f, 1.0f), 5.5 * t, 5.5 * t, &base);
    CHECK(std::abs(effectiveAt(layout.layer(LayerId::Weathering), &base, 5, 5) - 5.4f) < 1e-4f);
    dab(layout, LayerId::Weathering, brushOf(BrushTool::Lower, t, 0.5f, 1.0f), 5.5 * t, 5.5 * t, &base);
    CHECK(std::abs(effectiveAt(layout.layer(LayerId::Weathering), &base, 5, 5) - 4.2f) < 1e-4f);
    // Nothing goes past the ends of the range.
    for (int i = 0; i < 30; ++i)
        dab(layout, LayerId::Weathering, brushOf(BrushTool::Lower, t, 1.0f, 1.0f), 5.5 * t, 5.5 * t, &base);
    CHECK_EQ(effectiveAt(layout.layer(LayerId::Weathering), &base, 5, 5), 0.0f);

    // Smooth pulls a spike towards what is around it.
    dab(layout, LayerId::Weathering, brushOf(BrushTool::Paint, t, 1.0f, 1.0f, 24.0f), 12.5 * t, 12.5 * t);
    const float spike = effectiveAt(layout.layer(LayerId::Weathering), &base, 12, 12);
    dab(layout, LayerId::Weathering, brushOf(BrushTool::Smooth, 3 * t, 1.0f, 1.0f), 12.5 * t, 12.5 * t, &base);
    const float smoothed = effectiveAt(layout.layer(LayerId::Weathering), &base, 12, 12);
    const float beside = effectiveAt(layout.layer(LayerId::Weathering), &base, 13, 12);
    CHECK(smoothed < spike - 5.0f);
    CHECK(beside > 3.5f);
}

TEST(world_layer_strokes_are_lines_not_beads) {
    double carry = 0.0;
    const auto first = strokeDabs(0, 0, 10000, 0, 1000, carry);
    CHECK_EQ(first.size(), std::size_t(10));
    CHECK(std::abs(first.front().first - 1000.0) < 1e-6);
    CHECK(std::abs(carry) < 1e-6);
    // A slow drag spaces its dabs by distance, not by how often it is asked.
    CHECK(strokeDabs(10000, 0, 10500, 0, 1000, carry).empty());
    const auto later = strokeDabs(10500, 0, 11100, 0, 1000, carry);
    CHECK_EQ(later.size(), std::size_t(1));
    CHECK(std::abs(later.front().first - 11000.0) < 1e-6);

    // Painted with the smallest brush along a row, every texel of the row is hit.
    WorldLayout layout = oneRegion(4);
    Brush brush = brushOf(BrushTool::Paint, 2048.0, 1.0f, 0.5f, 900.0f);
    const double y = 30.5 * 2048;
    double from = 4.5 * 2048;
    carry = dabSpacing(LayerId::Continents, brush);     // a dab where the stroke starts
    for (double to = from + 700; to <= 50.5 * 2048; to += 700) {
        for (const auto& [x, yy] : strokeDabs(from, y, to, y, dabSpacing(LayerId::Continents, brush), carry))
            dab(layout, LayerId::Continents, brush, x, yy);
        from = to;
    }
    bool every = true;
    for (std::int32_t tx = 5; tx <= 49; ++tx) every = every && layout.layer(LayerId::Continents).cover(tx, 30) > 0.5f;
    CHECK(every);
}

TEST(world_layers_save_load_resize_and_copy_on_write) {
    WorldLayout layout = oneRegion(12);
    layout = [&] { WorldLayout l = layout; resizeLayout(l, 2, 1); return l; }();
    dab(layout, LayerId::Continents, brushOf(BrushTool::Paint, 20000.0, 0.7f, 0.3f, 850.0f), 90000, 60000);
    dab(layout, LayerId::Ranges, brushOf(BrushTool::Ranges, 30000.0, 0.8f, 0.4f), 200000, 70000);
    dab(layout, LayerId::Rain, brushOf(BrushTool::Paint, 40000.0, 0.6f, 0.5f, 220.0f), 150000, 65000);
    CHECK(layout.anyPainted());

    const auto file = std::filesystem::temp_directory_path() / "asr_world_layers_test.json";
    CHECK(saveWorldLayout(layout, file));
    const auto loaded = loadWorldLayout(file);
    CHECK(loaded.has_value());
    if (loaded) CHECK(*loaded == layout);

    // A world saved before there were layers still loads, with none painted.
    {
        std::ofstream old(file);
        old << R"({"format": 1, "regions_x": 2, "regions_y": 1, "seed": 4, "plates": 0, "blend_m": 12288,
                  "regions": [{"x": 0, "y": 0, "generated": true, "seed": 9}]})";
    }
    const auto older = loadWorldLayout(file);
    CHECK(older.has_value());
    if (older) {
        CHECK(older->at(0, 0).generated);
        CHECK(!older->anyPainted());
        CHECK_EQ(older->layer(LayerId::Sea).texelsX(), 16);
    }
    std::filesystem::remove(file);

    // A copy shares its tiles until one of the two is painted again: then the
    // other keeps what it had. That is what undo costs.
    const WorldLayout before = layout;
    dab(layout, LayerId::Continents, brushOf(BrushTool::Paint, 20000.0, 1.0f, 1.0f, 100.0f), 90000, 60000);
    CHECK(!(before == layout));
    CHECK(before.layer(LayerId::Continents).value(44, 29) > 800.0f);

    // Smaller: what was painted on the region that is gone goes with it, and
    // what was painted on the one that stays is where it was.
    WorldLayout shrunk = layout;
    resizeLayout(shrunk, 1, 1);
    CHECK_EQ(shrunk.layer(LayerId::Ranges).texelsX(), 64);
    CHECK(shrunk.layer(LayerId::Ranges).empty());                 // it was on the second region
    CHECK_EQ(shrunk.layer(LayerId::Continents).value(44, 29), layout.layer(LayerId::Continents).value(44, 29));
    // And back: nothing reappears.
    resizeLayout(shrunk, 2, 1);
    CHECK(shrunk.layer(LayerId::Ranges).empty());
}

TEST(world_layer_procedural_brush_is_the_generator_pass) {
    // The continents pass, run as a brush with the region's own seed at the
    // generator's own size, paints exactly what the pass makes at every texel
    // it covers - and a world built from that paint is the world built without
    // it, to the resolution of the layer.
    WorldLayout layout = oneRegion(31, 50);
    const std::uint64_t seed = layout.at(0, 0).settings.seed;
    Brush pass = brushOf(BrushTool::Continents, 1e9, 1.0f, 1.0f);
    pass.seed = seed;
    const LayerBase base = layerBase(layout, LayerId::Continents);
    dab(layout, LayerId::Continents, pass, double(layout.widthMetres()) / 2, double(layout.heightMetres()) / 2);
    const LayerMap& map = layout.layer(LayerId::Continents);
    std::int32_t checked = 0;
    bool same = true;
    for (std::int32_t ty = 0; ty < map.texelsY(); ++ty)
        for (std::int32_t tx = 0; tx < map.texelsX(); ++tx) {
            if (map.cover(tx, ty) < 1.0f) continue;
            const std::int32_t cx = tx * 4 + 2, cy = ty * 4 + 2;
            same = same && map.value(tx, ty) == float(continentNoise(seed, cx, cy, layout.widthCells()));
            same = same && std::abs(map.value(tx, ty) - base.at(tx, ty)) < 1e-3f;
            ++checked;
        }
    CHECK(same);
    CHECK(checked > 2000);

    const WorldMapData unpainted = generateWorldMap(quick(oneRegion(31, 50)));
    const WorldMapData painted = generateWorldMap(quick(layout));
    double difference = 0.0;
    std::int64_t agree = 0, cells = 0;
    for (std::int32_t y = 0; y < painted.height; ++y)
        for (std::int32_t x = 0; x < painted.width; ++x) {
            const std::size_t i = std::size_t(y) * painted.width + x;
            if (layout.layer(LayerId::Continents).atCell(x, y).cover < 0.999f) continue;
            difference += std::abs(painted.continentalField[i] - unpainted.continentalField[i]);
            agree += painted.cells[i].sea == unpainted.cells[i].sea ? 1 : 0;
            ++cells;
        }
    CHECK(cells > 30000);
    // What a two-kilometre layer cannot hold is the pass's finest octave, a
    // wave of four cells: it is the whole of the difference, a few per cent of
    // the field's range, and it moves the coast almost nowhere.
    CHECK(difference / double(cells) < 16.0);
    CHECK(double(agree) / double(cells) > 0.95);
    // Painting the pass's own values moves no sea level: the level is read off
    // the ground as the passes made it, and this paint made it the same.
    CHECK_EQ(painted.constants.seaLevel, unpainted.constants.seaLevel);

    // And another seed is another country.
    WorldLayout other = oneRegion(31, 50);
    pass.seed = seed + 1;
    dab(other, LayerId::Continents, pass, double(other.widthMetres()) / 2, double(other.heightMetres()) / 2);
    const WorldMapData moved = generateWorldMap(quick(other));
    double apart = 0.0;
    for (std::size_t i = 0; i < moved.continentalField.size(); ++i)
        apart += std::abs(moved.continentalField[i] - unpainted.continentalField[i]);
    CHECK(apart / double(moved.continentalField.size()) > 40.0);
}

TEST(world_layer_painted_land_and_sea_stay_where_they_are_painted) {
    const WorldLayout plain = oneRegion(17, 50);
    const WorldMapData before = generateWorldMap(quick(plain));
    // Sea painted into the middle of the west half; land painted into the
    // middle of the east half.
    WorldLayout layout = plain;
    dab(layout, LayerId::Continents, brushOf(BrushTool::Paint, 24000.0, 1.0f, 0.9f, 0.0f), metres(70), metres(128));
    dab(layout, LayerId::Continents, brushOf(BrushTool::Paint, 24000.0, 1.0f, 0.9f, 1023.0f), metres(186), metres(128));
    const WorldMapData after = generateWorldMap(quick(layout));
    const auto sea = [](const WorldCell& c) { return c.sea; };
    const auto land = [](const WorldCell& c) { return !c.sea; };
    CHECK(shareIn(after, 70, 128, 36, sea) > 0.95);
    CHECK(shareIn(after, 186, 128, 36, land) > 0.95);
    // And the rest of the region keeps the land it had: the painted ground is
    // not counted towards its share of sea, so nothing else drowns or rises
    // to pay for it.
    const double north = landIn(before, 20, 12, 236, 60), northAfter = landIn(after, 20, 12, 236, 60);
    const double south = landIn(before, 20, 196, 236, 244), southAfter = landIn(after, 20, 196, 236, 244);
    CHECK(std::abs(north - northAfter) < 0.04);
    CHECK(std::abs(south - southAfter) < 0.04);
}

TEST(world_layer_ranges_raise_the_ground_and_harden_it) {
    const WorldLayout plain = oneRegion(23, 45);
    const WorldMapData before = generateWorldMap(quick(plain));
    WorldLayout layout = plain;
    Brush range = brushOf(BrushTool::Ranges, 10000.0, 0.8f, 0.3f);
    double carry = dabSpacing(LayerId::Ranges, range);
    for (const auto& [x, y] : strokeDabs(metres(60), metres(128), metres(196), metres(128),
                                         dabSpacing(LayerId::Ranges, range), carry))
        dab(layout, LayerId::Ranges, range, x, y);
    const WorldMapData after = generateWorldMap(quick(layout));
    double raised = 0.0, hard = 0.0;
    std::int32_t cells = 0;
    for (std::int32_t x = 70; x <= 186; ++x)
        for (std::int32_t y = 124; y <= 132; ++y) {
            const std::size_t i = std::size_t(y) * after.width + x;
            raised += after.macroHeightField[i] - before.macroHeightField[i];
            hard += (after.rockTypeField[i] == RockType::HardRock || after.rockTypeField[i] == RockType::Volcanic ||
                     after.rockTypeField[i] == RockType::Limestone) ? 1.0 : 0.0;
            CHECK(after.upliftField[i] >= before.upliftField[i]);
            ++cells;
        }
    CHECK(raised / cells > 150.0);
    CHECK(hard / cells > 0.5);
    // Its crest is not a wall: the relief pass puts peaks and passes along it.
    std::int32_t low = 1 << 30, high = -(1 << 30);
    for (std::int32_t x = 80; x <= 176; ++x) {
        const std::int32_t h = after.macroHeightField[std::size_t(128) * after.width + x];
        low = std::min(low, h);
        high = std::max(high, h);
    }
    CHECK(high - low > 40);
}

TEST(world_layer_weathering_sea_and_rain_reach_their_passes) {
    const WorldLayout plain = oneRegion(41, 40, 3);
    const auto paintDisc = [](WorldLayout& layout, LayerId id, float value) {
        dab(layout, id, brushOf(BrushTool::Paint, 60000.0, 1.0f, 1.0f, value), metres(128), metres(128));
    };
    // Weathering: painted solid over a region, the layer IS the region's dial -
    // the thermal pass runs as many passes as the paint asks and every place
    // takes part in all of them - and the two dials are different countries.
    const auto paintedSolid = [&](float passes) {
        WorldLayout layout = plain;
        for (double qx : {0.25, 0.75})
            for (double qy : {0.25, 0.75})
                dab(layout, LayerId::Weathering, brushOf(BrushTool::Paint, 1e9, 1.0f, 1.0f, passes),
                    qx * double(layout.widthMetres()), qy * double(layout.heightMetres()));
        return layout;
    };
    const WorldLayout old = paintedSolid(24.0f);
    CHECK_EQ(paramsFor(old).erosionPasses, 24);
    const WorldMapData young = generateWorldMap(quick(paintedSolid(0.0f)));
    const WorldMapData aged = generateWorldMap(quick(old));
    CHECK(young.thermallyRelaxedHeightField == generateWorldMap(quick(oneRegion(41, 40, 0))).thermallyRelaxedHeightField);
    CHECK(aged.thermallyRelaxedHeightField == generateWorldMap(quick(oneRegion(41, 40, 24))).thermallyRelaxedHeightField);
    std::int64_t weathered = 0;
    for (std::size_t i = 0; i < aged.thermallyRelaxedHeightField.size(); ++i)
        weathered += aged.thermallyRelaxedHeightField[i] != young.thermallyRelaxedHeightField[i];
    CHECK(weathered > std::int64_t(aged.thermallyRelaxedHeightField.size() / 4));

    // Rain: the same wind, but a country painted wet is wetter than one
    // painted dry.
    WorldLayout dry = plain, wet = plain;
    paintDisc(dry, LayerId::Rain, 20.0f);
    paintDisc(wet, LayerId::Rain, 250.0f);
    const WorldMapData d = generateWorldMap(quick(dry));
    const WorldMapData w = generateWorldMap(quick(wet));
    double dryRain = 0.0, wetRain = 0.0;
    std::int32_t landCells = 0;
    for (std::int32_t y = 110; y < 146; ++y)
        for (std::int32_t x = 110; x < 146; ++x) {
            const std::size_t i = std::size_t(y) * d.width + x;
            if (d.cells[i].sea || w.cells[i].sea) continue;
            dryRain += d.annualRainfallField[i];
            wetRain += w.annualRainfallField[i];
            ++landCells;
        }
    if (landCells > 50) CHECK(wetRain > dryRain * 2.0);

    // Sea share: painted over the west half, the west floods and the east
    // keeps its own.
    WorldLayout flooded = plain;
    for (std::int32_t ty = 0; ty < flooded.layer(LayerId::Sea).texelsY(); ++ty)
        for (std::int32_t tx = 0; tx < 4; ++tx)
            dab(flooded, LayerId::Sea, brushOf(BrushTool::Paint, 1.0, 1.0f, 1.0f, 95.0f),
                (tx + 0.5) * 16384.0, (ty + 0.5) * 16384.0);
    const WorldMapData f = generateWorldMap(quick(flooded));
    const WorldMapData p = generateWorldMap(quick(plain));
    CHECK(landIn(f, 16, 16, 100, 240) < landIn(p, 16, 16, 100, 240) - 0.2);
    CHECK(std::abs(landIn(f, 160, 16, 240, 240) - landIn(p, 160, 16, 240, 240)) < 0.05);
}

TEST(world_layer_wiped_clean_is_no_layer_at_all) {
    // Everything painted and then wiped off again: the world is the world
    // that was never painted, cell for cell.
    const WorldLayout plain = oneRegion(53, 55);
    WorldLayout layout = plain;
    for (std::size_t i = 0; i < kLayerCount; ++i) {
        const LayerId id = LayerId(i);
        Brush b = brushOf(toolsFor(id).front(), 40000.0, 0.8f, 0.5f, layerDef(id).paint);
        dab(layout, id, b, metres(128), metres(128));
    }
    CHECK(layout.anyPainted());
    for (std::size_t i = 0; i < kLayerCount; ++i)
        for (int k = 0; k < 4; ++k)
            dab(layout, LayerId(i), brushOf(BrushTool::Erase, 1e9, 1.0f, 1.0f), metres(128), metres(128));
    CHECK(!layout.anyPainted());
    CHECK(layout == plain);
    const WorldMapData a = generateWorldMap(quick(plain));
    const WorldMapData b = generateWorldMap(quick(layout));
    bool same = a.continentalField == b.continentalField && a.macroHeightField == b.macroHeightField &&
                a.annualRainfallField == b.annualRainfallField;
    for (std::size_t i = 0; i < a.cells.size() && same; ++i)
        same = a.cells[i].elevation == b.cells[i].elevation && a.cells[i].sea == b.cells[i].sea &&
               a.cells[i].river == b.cells[i].river;
    CHECK(same);
}

