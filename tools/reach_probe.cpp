// Prints a reach's course: where it runs, its head, and the ground either
// bank stands on, for looking at one river at a time.
//
//   reach_probe seed cells reachId
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"

int main(int argc, char** argv) {
    using namespace world::streaming;
    using core::Fixed;
    if (argc < 4) { std::fprintf(stderr, "usage: reach_probe seed cells reach\n"); return 2; }
    generation::WorldMapParams params;
    params.seed = std::strtoull(argv[1], nullptr, 10);
    params.width = params.height = std::atoi(argv[2]);
    const auto map = generation::generateWorldMap(params);
    const auto began = std::chrono::steady_clock::now();
    const auto graph = buildHydrologyGraph(map);
    std::printf("graph built in %.0f ms (%zu reaches, %zu bodies)\n",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count(),
                graph.segments.size(), graph.waterBodies.size());
    const auto* segment = riverSegmentOf(graph, RiverId(std::atoi(argv[3])));
    if (!segment) { std::fprintf(stderr, "no such reach\n"); return 1; }
    world::HeightField field(&map, map.seed);
    const char* kinds[] = {"source", "confluence", "inlet", "outlet", "mouth", "terminal"};
    std::printf("reach %d from %s (%.1f) to %s (%.1f) points %zu\n", std::atoi(argv[3]),
                kinds[int(graph.nodes[segment->from - 1].kind)], graph.nodes[segment->from - 1].surface.toDouble(),
                segment->to ? kinds[int(graph.nodes[segment->to - 1].kind)] : "-",
                segment->to ? graph.nodes[segment->to - 1].surface.toDouble() : 0.0, segment->course.size());
    const auto& course = segment->course;
    for (std::size_t i = 0; i < course.size(); ++i) {
        const auto& p = course[i];
        const auto& a = course[i > 0 ? i - 1 : i];
        const auto& b = course[i + 1 < course.size() ? i + 1 : i];
        const Fixed dx = b.position.x - a.position.x, dy = b.position.y - a.position.y;
        const Fixed length = core::max(core::hypot(dx, dy), core::kOne);
        const Fixed out = p.halfWidth + core::max(p.depth, Fixed::fromInt(2));
        const auto bank = [&](int side) {
            const auto pieces = field.piecesAt(p.position.x - dy / length * out * Fixed::fromInt(side),
                                               p.position.y + dx / length * out * Fixed::fromInt(side));
            return (pieces.country + pieces.moved * Fixed::ratio(1, 4)).toDouble();
        };
        const auto centre = field.piecesAt(p.position.x, p.position.y);
        const GraphCarver carver(graph, {p.position, p.position}, 900);
        const auto carved = carver.carve(p.position, centre.country, centre.moved);
        std::printf("%3zu at %8.1f,%8.1f surface=%7.2f hw=%5.1f depth=%4.1f ground=%7.2f banks=%7.2f %7.2f "
                    "wet=%d body=%u estuary=%.2f\n", i,
                    p.position.x.toDouble(), p.position.y.toDouble(), p.surface.toDouble(), p.halfWidth.toDouble(),
                    p.depth.toDouble(), (centre.country + centre.moved).toDouble(), bank(1), bank(-1),
                    int(carved.wet), unsigned(carved.body), carved.estuary.toDouble());
    }
    return 0;
}

