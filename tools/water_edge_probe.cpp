// Where the drawn water does not meet the drawn ground, counted.
//
// The renderer draws water wherever a page's head stands above its bed, and
// draws it AT that head, both interpolated linearly between samples (the
// adaptive mesh carries the samples, the pixel reads the page). So between a
// sample that is wet and one that is dry the waterline lands where the two
// differences cross, and the water there stands at whatever height the head
// has fallen to by then. On a real shore the head does not fall at all and the
// water meets the bank at its own level. Where it falls, the water is a sheet
// sloping down to the ground below its level - which, seen from the side, is
// a lake or a river hanging in the air. This measures that fall ("hang") on
// the packed pages the renderer actually receives, at any page spacing, over
// a whole world or a window of it.
//
// It also counts phantom water: samples the graph says are dry whose packed
// head still stands above their bed, so they are drawn wet anyway.
//
//   water_edge_probe [seed] [cells] [sampleMetres] [x0 y0 x1 y1]
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/render/height_page_stream.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

int main(int argc, char** argv) {
    using namespace world::streaming;
    using world::floorDiv;
    generation::WorldMapParams params;
    params.seed = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 11;
    params.width = params.height = argc > 2 ? std::atoi(argv[2]) : 60;
    const int sample = argc > 3 ? std::atoi(argv[3]) : 16;
    const auto map = generation::generateWorldMap(params);
    const auto graph = buildHydrologyGraph(map);
    const auto quantisation = hsimQuantisationFor(map);
    const BaseTileBaker baker(map, graph, quantisation);
    const std::int64_t worldMetres = std::int64_t(params.width) * generation::kMetresPerCell;
    std::int64_t x0 = 0, y0 = 0, x1 = worldMetres, y1 = worldMetres;
    if (argc > 7) {
        x0 = std::atoll(argv[4]); y0 = std::atoll(argv[5]);
        x1 = std::atoll(argv[6]); y1 = std::atoll(argv[7]);
    }
    const bool cases = std::getenv("WATER_PROBE_CASES") != nullptr;

    struct Kind { const char* name; std::size_t edges = 0, over1 = 0, over5 = 0, over20 = 0, curtains = 0; double worst = 0, wx = 0, wy = 0; };
    std::array<Kind, 3> kinds{Kind{"lake"}, Kind{"river"}, Kind{"sea"}};
    std::size_t phantom = 0, phantomOver1 = 0, drawnWet = 0, printed = 0;
    std::array<std::size_t, 8> curtainsTo{};
    for (std::int64_t py = floorDiv(y0, kPageMetres); py <= floorDiv(y1 - 1, kPageMetres); ++py)
        for (std::int64_t px = floorDiv(x0, kPageMetres); px <= floorDiv(x1 - 1, kPageMetres); ++px) {
            auto baked = std::make_shared<BakedPage>(
                    baker.bakePage({std::int32_t(px), std::int32_t(py), 0}, sample, 2));
            if (!baked->base.valid() || !baked->water.valid()) continue;
            const auto packed = game::packHeightPage(baked);
            const auto& s = *packed.surface;
            const int side = s.side, pad = s.padding;
            const auto kindOf = [&](std::size_t i) {
                if (packed.fields[3][i * 4 + 3] > 32767) return 0;   // lake share
                if (packed.fields[3][i * 4 + 2] > 32767) return 1;   // river share
                return 2;
            };
            const GraphCarver carver(graph, tileBounds({std::int32_t(px), std::int32_t(py), 0}),
                                     800 + sample * 3);
            for (int row = pad; row < side - pad; ++row)
                for (int column = pad; column < side - pad; ++column) {
                    const auto i = std::size_t(row) * side + column;
                    const double di = s.head[i] - s.bed[i];
                    if (di > 0.0) {
                        ++drawnWet;
                        const double wx = double(px * kPageMetres + (column - pad) * sample);
                        const double wy = double(py * kPageMetres + (row - pad) * sample);
                        const core::WorldPos p{core::Fixed::fromDoubleForContent(wx),
                                               core::Fixed::fromDoubleForContent(wy)};
                        const auto pieces = baker.field().piecesAt(p.x, p.y);
                        if (!carver.carve(p, pieces.country, pieces.moved).wet) {
                            ++phantom;
                            if (di > 1.0) ++phantomOver1;
                        }
                    }
                    for (const auto [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}}) {
                        if (column + dx >= side - pad || row + dy >= side - pad) continue;
                        const auto n = std::size_t(row + dy) * side + column + dx;
                        const double dn = s.head[n] - s.bed[n];
                        if ((di > 0.0) == (dn > 0.0)) continue;
                        const auto wet = di > 0.0 ? i : n, dry = di > 0.0 ? n : i;
                        const double dw = std::max(di, dn), dd = std::min(di, dn);
                        const double t = dw / (dw - dd);
                        const double hang = t * (s.head[wet] - s.head[dry]);
                        Kind& kind = kinds[std::size_t(kindOf(wet))];
                        ++kind.edges;
                        if (hang > 1.0) ++kind.over1;
                        if (hang > 5.0) ++kind.over5;
                        if (hang > 20.0) ++kind.over20;
                        // Steeper than one in one before it meets the ground:
                        // a curtain of water, not a surface tilted to follow it.
                        if (hang > 1.0 && hang > t * sample) {
                            ++kind.curtains;
                            if (const auto* reach = riverSegmentOf(graph, baked->water.riverId[wet])) {
                                const int to = reach->to ? int(graph.nodes[reach->to - 1].kind) : 6;
                                ++curtainsTo[std::size_t(to)];
                            } else {
                                ++curtainsTo[7];
                            }
                        }
                        const double wx = double(px * kPageMetres + (int(wet % side) - pad) * sample);
                        const double wy = double(py * kPageMetres + (int(wet / side) - pad) * sample);
                        if (hang > kind.worst) { kind.worst = hang; kind.wx = wx; kind.wy = wy; }
                        if (cases && hang > 5.0 && printed < 20) {
                            ++printed;
                            const auto reachId = baked->water.riverId[wet];
                            const auto* segment = riverSegmentOf(graph, reachId);
                            const char* kinds[] = {"source", "confluence", "inlet", "outlet", "mouth", "terminal"};
                            if (segment)
                                std::printf("    reach %u from=%s to=%s dest=%u src=%u\n", unsigned(reachId),
                                            kinds[int(graph.nodes[segment->from - 1].kind)],
                                            segment->to ? kinds[int(graph.nodes[segment->to - 1].kind)] : "-",
                                            unsigned(segment->destinationWaterBody), unsigned(segment->sourceWaterBody));
                            std::printf("  %s hang=%.1f at %.0f,%.0f wet(head=%.1f bed=%.1f) dry(head=%.1f bed=%.1f) "
                                        "river=%u shore=%.1f body=%u\n",
                                        kind.name, hang, wx, wy, s.head[wet], s.bed[wet], s.head[dry], s.bed[dry],
                                        unsigned(baked->water.riverId[dry]), baked->water.shoreDecimetres[dry] / 10.0,
                                        unsigned(baked->water.waterBodyId[wet]));
                        }
                    }
                }
        }
    std::printf("seed=%llu cells=%d sample=%d drawn-wet=%zu phantom=%zu (over 1 m deep %zu)\n",
                static_cast<unsigned long long>(params.seed), params.width, sample, drawnWet, phantom,
                phantomOver1);
    for (const auto& kind : kinds)
        std::printf("  %-5s edges=%zu hang>1m=%zu >5m=%zu >20m=%zu curtains=%zu worst=%.1f m at %.0f,%.0f\n", kind.name,
                    kind.edges, kind.over1, kind.over5, kind.over20, kind.curtains, kind.worst, kind.wx, kind.wy);
    std::printf("  curtains by where the reach ends: source %zu confluence %zu inlet %zu outlet %zu mouth %zu "
                "terminal %zu open %zu no-reach %zu\n",
                curtainsTo[0], curtainsTo[1], curtainsTo[2], curtainsTo[3], curtainsTo[4], curtainsTo[5],
                curtainsTo[6], curtainsTo[7]);
    return 0;
}

