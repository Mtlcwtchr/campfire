// Lakes hanging in the air, counted.
//
// A lake's water may end only where the ground comes up through its level.
// Anywhere else the edge of the water is a wall standing over lower ground -
// which is what a lake "hanging in the air" is. This bakes the pages over
// every lake the graph holds and counts, along the lake's own wet samples,
// the dry neighbours whose ground is below the water beside them.
//
//   lake_edge_probe [seed] [cells] [sampleMetres]
//
// Exits non-zero when any such edge is found. LAKE_PROBE_CASES=1 prints the
// first dozen with the lattice around them, LAKE_PROBE_VERBOSE=1 every page.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

int main(int argc, char** argv) {
    using namespace world::streaming;
    using world::floorDiv;
    generation::WorldMapParams params;
    params.seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 11;
    params.width = params.height = argc > 2 ? std::atoi(argv[2]) : 64;
    const int sample = argc > 3 ? std::atoi(argv[3]) : 8;
    const auto map = generation::generateWorldMap(params);
    const auto graph = buildHydrologyGraph(map);
    const auto quantisation = hsimQuantisationFor(map);
    const BaseTileBaker baker(map, graph, quantisation);

    std::size_t lakes = 0, edges = 0, hanging = 0, outside = 0, deep = 0, riverside = 0;
    double worst = 0, worstX = 0, worstY = 0;
    int printed = 0;
    for (const auto& body : graph.waterBodies) {
        if (body.kind != WaterBodyKind::Lake || body.basinSamples.empty() || body.basinStep <= 0) continue;
        ++lakes;
        const std::int64_t step = body.basinStep;
        if (std::getenv("LAKE_PROBE_VERBOSE")) {
            const auto s = body.basinSamples[body.basinSamples.size() / 2];
            const core::WorldPos p{core::Fixed::fromInt(s.x * step), core::Fixed::fromInt(s.y * step)};
            const auto pieces = baker.field().piecesAt(p.x, p.y);
            const GraphCarver carver(graph, {p, p}, 900);
            const auto carved = carver.carve(p, pieces.country, pieces.moved);
            std::printf("lake %u level=%.2f samples=%zu step=%d at %lld,%lld country=%.2f moved=%.2f floor=%.2f "
                        "surface=%.2f wet=%d body=%u\n",
                        unsigned(body.id), body.level.toDouble(), body.basinSamples.size(), body.basinStep,
                        (long long)(s.x * step), (long long)(s.y * step), pieces.country.toDouble(),
                        pieces.moved.toDouble(), carved.floor.toDouble(), carved.surface.toDouble(),
                        int(carved.wet), unsigned(carved.body));
        }
        std::int64_t lowX = body.basinSamples.front().x, highX = lowX;
        std::int64_t lowY = body.basinSamples.front().y, highY = lowY;
        for (const auto s : body.basinSamples) {
            lowX = std::min<std::int64_t>(lowX, s.x); highX = std::max<std::int64_t>(highX, s.x);
            lowY = std::min<std::int64_t>(lowY, s.y); highY = std::max<std::int64_t>(highY, s.y);
        }
        const auto inBasin = [&](std::int64_t x, std::int64_t y) {
            return std::binary_search(body.basinSamples.begin(), body.basinSamples.end(),
                                      core::TilePos{std::int32_t(x), std::int32_t(y)},
                                      [](core::TilePos a, core::TilePos b) {
                                          return a.y < b.y || (a.y == b.y && a.x < b.x);
                                      });
        };
        for (std::int64_t py = floorDiv((lowY - 2) * step, kPageMetres);
             py <= floorDiv((highY + 2) * step, kPageMetres); ++py)
            for (std::int64_t px = floorDiv((lowX - 2) * step, kPageMetres);
                 px <= floorDiv((highX + 2) * step, kPageMetres); ++px) {
                const auto page = baker.bakePage({std::int32_t(px), std::int32_t(py), 0}, sample, 2);
                if (!page.base.valid() || !page.water.valid()) {
                    if (std::getenv("LAKE_PROBE_VERBOSE")) std::printf("  page %lld,%lld invalid\n", (long long)px, (long long)py);
                    continue;
                }
                if (std::getenv("LAKE_PROBE_VERBOSE")) {
                    std::size_t mine = 0, any = 0;
                    for (const auto id : page.water.waterBodyId) { mine += id == body.id; any += id != kInvalidWaterBodyId; }
                    std::printf("  page %lld,%lld lake=%zu water=%zu\n", (long long)px, (long long)py, mine, any);
                }
                const int side = page.base.width + 2 * page.base.padding;
                const int pad = page.base.padding;
                for (int row = pad; row < side - pad; ++row)
                    for (int column = pad; column < side - pad; ++column) {
                        const auto at = std::size_t(row) * side + column;
                        if (page.water.waterBodyId[at] != body.id) continue;
                        const double level = quantisation.dequantise(page.water.surfaceQuantized[at]).toDouble();
                        for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                            const auto n = std::size_t(row + dy) * side + column + dx;
                            // Wet in its own right - a body, or a river over its bed. Coverage is
                            // no test here: it is the share of a stencil, and a dry bank beside
                            // the water always has some.
                            const double floor = quantisation.dequantise(page.base.heightQuantized[n]).toDouble();
                            const double head = quantisation.dequantise(page.water.surfaceQuantized[n]).toDouble();
                            if (page.water.waterBodyId[n] != kInvalidWaterBodyId) continue;
                            if (page.water.riverId[n] != kInvalidRiverId && head > floor) continue;
                            ++edges;
                            const double gap = level - floor;
                            if (gap <= 0.5) continue;
                            ++hanging;
                            const double wx = px * kPageMetres + (column + dx - pad) * sample;
                            const double wy = py * kPageMetres + (row + dy - pad) * sample;
                            const auto cx = std::int64_t(wx) / step, cy = std::int64_t(wy) / step;
                            // A node beyond the point along an axis it lies on carries no weight.
                            const bool offX = std::int64_t(wx) % step != 0, offY = std::int64_t(wy) % step != 0;
                            const bool beyond = !inBasin(cx, cy) && !(offX && inBasin(cx + 1, cy)) &&
                                                !(offY && inBasin(cx, cy + 1)) &&
                                                !(offX && offY && inBasin(cx + 1, cy + 1));
                            if (beyond) ++outside;
                            if (gap > 5) ++deep;
                            if (page.water.riverId[n] != kInvalidRiverId && page.water.shoreDecimetres[n] <= 160)
                                ++riverside;
                            if (std::getenv("LAKE_PROBE_CASES") && gap > 3 && printed < 12) {
                                ++printed;
                                const core::WorldPos p{core::Fixed::fromDoubleForContent(wx),
                                                       core::Fixed::fromDoubleForContent(wy)};
                                const auto pieces = baker.field().piecesAt(p.x, p.y);
                                const GraphCarver carver(graph, {p, p}, 900);
                                const auto c = carver.carve(p, pieces.country, pieces.moved);
                                std::printf("  lake %u level=%.2f wetHead=%.2f dry %.0f,%.0f floor=%.2f country=%.2f "
                                            "moved=%.2f carve(floor=%.2f surface=%.2f wet=%d body=%u reach=%u bank=%.1f) %s\n",
                                            unsigned(body.id), body.level.toDouble(), level, wx, wy, floor,
                                            pieces.country.toDouble(), pieces.moved.toDouble(), c.floor.toDouble(),
                                            c.surface.toDouble(), int(c.wet), unsigned(c.body), unsigned(c.reach),
                                            c.bankDistance.toDouble(), beyond ? "BEYOND" : "inside");
                                if (map.terrainFoundation) {
                                    const auto& f = *map.terrainFoundation;
                                    for (std::int64_t ny = cy - 1; ny <= cy + 2; ++ny) {
                                        std::printf("    ");
                                        for (std::int64_t nx = cx - 1; nx <= cx + 2; ++nx)
                                            std::printf(" %8.1f%c", f.heightDm[4][std::size_t(ny) * f.columns + nx] / 10.0,
                                                        inBasin(nx, ny) ? '*' : ' ');
                                        std::printf("\n");
                                    }
                                }
                            }
                            if (gap > worst) { worst = gap; worstX = wx; worstY = wy; }
                        }
                    }
            }
    }
    std::printf("seed=%llu cells=%d sample=%d lakes=%zu shoreline=%zu hanging=%zu (outside footprint %zu, "
                "over 5 m %zu, at a river %zu) worst=%.2f m at %.0f,%.0f\n",
                static_cast<unsigned long long>(params.seed), params.width, sample, lakes, edges, hanging,
                outside, deep, riverside, worst, worstX, worstY);
    return hanging == 0 ? 0 : 1;
}
