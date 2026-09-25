// Looks for the marks a generator leaves on its own work.
//
// Every terrain bug this project has had was found by staring at a picture and
// noticing something that nature does not do: a step where the ground should
// slope, a pattern that lines up with the axes, a river drawn as a dashed line,
// a seam where two levels of detail meet. Staring does not scale and does not
// run in a test, so this is the same look taken by measurement.
//
//     terrain_lint [--seed N] [--world N] [--at x,y] [--span M]
//
// It reports, for a patch of country:
//
//   steps        adjacent samples too far apart in height to be ground, and -
//                the tell - whether they line up with the axes. Nature has
//                cliffs; it does not have cliffs that all run north to south.
//   lattices     how much the ground repeats at each of the sizes the generator
//                is built on: the height lattice, the chunk, the coarse cell,
//                the noise octaves. A world with nothing to hide scores about
//                the same at every period; a spike is the lattice showing
//                through, which is what "I can see the tiles" means.
//   rivers       the longest dry gap along a watercourse that is supposed to
//                hold water, and how much its width varies from reach to reach.
//   detail       whether the same ground comes out the same at two levels of
//                detail. It has to: they are the same samples of the same
//                field, and if they differ the seam is visible where they meet.
//
// Anything it prints as FAULT is something to go and look at. Anything it
// prints as a number is there so the next change can be compared against it.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/macro.hpp"
#include "game/world/terrain_mesh.hpp"
#include "engine/geometry/smart_terrain_mesh.hpp"
#include "game/world/terrain_streaming/hydrology_graph.hpp"

using core::Fixed;
using core::WorldPos;

namespace {

WorldPos at(double x, double y) {
    return {Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)};
}

int faults = 0;
void fault(const std::string& what) {
    std::printf("  FAULT  %s\n", what.c_str());
    ++faults;
}

// --- steps ---------------------------------------------------------------
// Ground that jumps rather than slopes, and whether the jumps line up.
void lookForSteps(const world::HeightField& field, double originX, double originY, double span) {
    const double step = 2.0;
    const int side = static_cast<int>(span / step);
    std::vector<double> heights(static_cast<std::size_t>(side) * side);
    for (int y = 0; y < side; ++y)
        for (int x = 0; x < side; ++x)
            heights[static_cast<std::size_t>(y) * side + x] =
                    field.heightAt(at(originX + x * step, originY + y * step)).toDouble();

    // A step is a rise of more than this between samples two metres apart. Real
    // ground does that at a cliff and nowhere else, and cliffs are rare.
    const double limit = 4.0;
    int alongX = 0, alongY = 0;
    double worst = 0;
    double worstX = 0, worstY = 0;
    for (int y = 1; y < side; ++y)
        for (int x = 1; x < side; ++x) {
            const double here = heights[static_cast<std::size_t>(y) * side + x];
            const double west = heights[static_cast<std::size_t>(y) * side + x - 1];
            const double north = heights[static_cast<std::size_t>(y - 1) * side + x];
            if (std::abs(here - west) > limit) ++alongX;
            if (std::abs(here - north) > limit) ++alongY;
            const double biggest = std::max(std::abs(here - west), std::abs(here - north));
            if (biggest > worst) {
                worst = biggest;
                worstX = originX + x * step;
                worstY = originY + y * step;
            }
        }

    const int total = alongX + alongY;
    std::printf("  steps over %.0f m: %d (%d east-west, %d north-south), worst %.1f m at %.0f, %.0f\n",
                limit, total, alongX, alongY, worst, worstX, worstY);
    if (total > 20) {
        // Lopsided means the steps follow the axes, which is a lattice showing
        // through rather than a landscape.
        const double lopsided = static_cast<double>(std::max(alongX, alongY)) / std::max(1, total);
        if (lopsided > 0.7)
            fault("the steps line up with an axis - something is drawing the lattice");
    }
}

// --- lattices ------------------------------------------------------------
// How much the ground repeats at the sizes the generator is built on.
void lookForLattices(const world::HeightField& field, double originX, double originY, double span) {
    // The creases a lattice leaves are second differences: the ground carries
    // on smoothly and then bends. Measured across candidate periods and against
    // the same measure at offsets that mean nothing, so the number says "this
    // period is special" rather than "this ground is rough".
    double worstKink = 0, worstAtX = 0, worstAtY = 0;
    struct Period { const char* name; double metres; };
    const Period periods[] = {
            {"height sample", world::kSampleMetres},
            {"chunk", world::kChunkMetres},
            {"coarse lattice", 32},
            {"world cell", generation::kMetresPerCell},
            {"nothing (control)", 37},
            {"nothing (control)", 91},
    };

    const auto creaseAt = [&](double period) {
        // The break in the *slope* across the line, not the bend of the ground
        // at it. A surface may change how fast it is curving without anybody
        // seeing - that is what a spline does at every knot by construction -
        // but a slope that changes abruptly is a crease, and a crease under a
        // light is a line drawn across the country.
        // Compared by the middle of each set rather than by the sum of it.
        //
        // A sum is decided by its largest term, and on steep ground the largest
        // term is one bank that happens to sit on a line: the check then reports
        // a period nobody can see, and reports it differently on every seed. The
        // question is whether the *typical* break is worse on the lines than off
        // them, and that is a median.
        std::vector<double> onLines, between;
        for (double y = originY + period; y < originY + span; y += period) {
            for (double x = originX + period; x < originX + span; x += period) {
                const auto kink = [&](double px, double py) {
                    const double before = field.heightAt(at(px - 3, py)).toDouble() -
                                          field.heightAt(at(px - 1, py)).toDouble();
                    const double after = field.heightAt(at(px + 1, py)).toDouble() -
                                         field.heightAt(at(px + 3, py)).toDouble();
                    return std::abs(after - before);
                };
                const double here = kink(x, y);
                if (here > worstKink) { worstKink = here; worstAtX = x; worstAtY = y; }
                onLines.push_back(here);
                between.push_back(kink(x + period * 0.37, y));
            }
        }
        if (onLines.empty()) return 1.0;
        const auto middle = [](std::vector<double>& all) {
            std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
            return all[all.size() / 2];
        };
        const double off = middle(between);
        return off > 0 ? middle(onLines) / off : 1.0;
    };

    for (const Period& period : periods) {
        worstKink = 0;
        worstAtX = worstAtY = 0;
        const double ratio = creaseAt(period.metres);
        std::printf("  crease at %-18s (%5.0f m): %.2f times the ground elsewhere, worst %.2f m\n",
                    period.name, period.metres, ratio, worstKink);
        // Both, and for a reason: the ratio says the period is special, and the
        // height says it can be seen. Without the second this complained about
        // a fourteen-centimetre kink on a hillside with a hundred metres of
        // relief - true, special, and invisible.
        if (ratio > 1.6 && worstKink > 0.4 && std::strcmp(period.name, "nothing (control)") != 0)
            fault(std::string("the ground breaks its slope on the ") + period.name +
                  " lines, by " + std::to_string(worstKink).substr(0, 4) +
                  " m - that period can be seen");
    }
}

// --- rivers --------------------------------------------------------------
void lookAtRivers(const world::HeightField& field, const generation::WorldMapData& world) {
    const world::MacroWorld& macro = field.macro();
    int walked = 0, dashed = 0;
    double longestGap = 0, narrowest = 1e9, widest = 0, worstBank = 0;
    double bankAtX = 0, bankAtY = 0, bankWidth = 0;
    int shores = 0, walls = 0;
    // Walked on the GRAPH, which is the only thing that knows where the water
    // is.
    //
    // This used to walk MacroWorld::channelOf instead - a reach rebuilt from
    // two cell centres, with one arch of meander across it. That is a
    // description of a river, and the river the player sees is carved from the
    // hydrology graph: a chain of knots through many cells, each leg wandering
    // on its own. The two agree in the middle of a straight reach and part
    // company everywhere else, so the check was asking for water at points the
    // water had never been told to go to, and reporting the rivers as dashed
    // when they are continuous. A tool that measures the wrong model reports
    // faults that cannot be fixed and misses the ones that can.
    const world::streaming::HydrologyGraph* graph = field.graph();
    if (graph != nullptr) {
        // A spread of them, however many there are: counted first, then
        // thinned to about forty, so the check does not go quiet on a small
        // world - and a quiet check reads the same as a clean one.
        std::int64_t wetSegments = 0;
        for (const auto& segment : graph->segments)
            if (segment.course.size() > 1) ++wetSegments;
        const std::int64_t everyNth = std::max<std::int64_t>(1, wetSegments / 40);
        std::int64_t seen = 0;
        for (const auto& segment : graph->segments) {
            if (segment.course.size() < 2) continue;
            if (seen++ % everyNth != 0 || walked >= 40) continue;
            ++walked;
            widest = std::max(widest, 2 * segment.width.toDouble());
            narrowest = std::min(narrowest, 2 * segment.width.toDouble());

            // Along the course, every couple of metres, asking whether there is
            // water where the water is supposed to be. A dry stretch in the
            // middle of a river is the mark of a nearest-point search that
            // gives up between its own samples.
            double gap = 0, worstGap = 0;
            for (std::size_t k = 0; k + 1 < segment.course.size(); ++k) {
                const auto& a = segment.course[k];
                const auto& b = segment.course[k + 1];
                const double dx = (b.position.x - a.position.x).toDouble();
                const double dy = (b.position.y - a.position.y).toDouble();
                const double run = std::sqrt(dx * dx + dy * dy);
                if (run < 1e-6) continue;
                const int steps = std::max(2, static_cast<int>(run / 2.0));
                for (int i = 0; i <= steps; ++i) {
                    const double t = static_cast<double>(i) / steps;
                    const WorldPos on{a.position.x + Fixed::fromInt(std::llround(dx * t)),
                                      a.position.y + Fixed::fromInt(std::llround(dy * t))};
                    if (field.underWater(on)) {
                        gap = 0;
                    } else {
                        gap += run / steps;
                        worstGap = std::max(worstGap, gap);
                    }
                }

                // And how deep the water is where it stops.
                //
                // A shore is where the ground comes up to meet the water. If
                // the water instead ends at a width out of a table while the
                // ground under it is still metres down, the last wet sample and
                // the first dry one are a wall apart, and every river gets drawn
                // with a staircase of four-metre treads down each side.
                if (k % 4 != 0) continue;
                const double px = -dy / run, py = dx / run;
                const double reach = 2 * a.halfWidth.toDouble() + 40;
                for (int side = -1; side <= 1; side += 2) {
                    double lastWet = -1;
                    bool left = false;
                    for (double away = 0; away <= reach; away += world::kSampleMetres) {
                        const WorldPos out{
                                a.position.x + Fixed::fromInt(std::llround(px * side * away)),
                                a.position.y + Fixed::fromInt(std::llround(py * side * away))};
                        if (!field.underWater(out)) { left = true; break; }
                        lastWet = field.waterLevelAt(out).toDouble() - field.heightAt(out).toDouble();
                    }
                    if (!left || lastWet < 0) continue;
                    ++shores;
                    if (lastWet > 3.0) ++walls;
                    if (lastWet > worstBank) {
                        worstBank = lastWet;
                        bankAtX = a.position.x.toDouble();
                        bankAtY = a.position.y.toDouble();
                        bankWidth = 2 * a.halfWidth.toDouble();
                    }
                }
            }
            longestGap = std::max(longestGap, worstGap);
            if (worstGap > 60) ++dashed;
        }
    }
    std::printf("  rivers walked: %d, widths %.0f to %.0f m, longest dry stretch %.0f m\n", walked,
                narrowest, widest, longestGap);
    std::printf("  banks: %d shores walked, %d of them a wall of over 3 m; deepest %.1f m at "
                "%.0f, %.0f (a river %.0f m wide)\n",
                shores, walls, worstBank, bankAtX, bankAtY, bankWidth);
    if (dashed > walked / 10 && walked > 0)
        fault("rivers run dry in places - the water is drawn as a dashed line");
    // Only where the world has more than one size of river to draw.
    //
    // Width comes off how much water a channel carries, so identical widths mean
    // the flow is not reaching it - on a world big enough to have a brook and a
    // trunk river both. On a world thirty-five kilometres across the largest
    // catchment on it is small, every river is a small river, and identical
    // widths are the truth rather than a fault. So the map is asked first.
    std::uint8_t leastFlow = 255, mostFlow = 0;
    for (const generation::WorldCell& cell : world.cells) {
        if (cell.sea || !cell.river) continue;
        leastFlow = std::min(leastFlow, cell.drainSize);
        mostFlow = std::max(mostFlow, cell.drainSize);
    }
    const int classes = mostFlow >= leastFlow ? mostFlow - leastFlow + 1 : 0;
    std::printf("  the world holds %d size%s of river, %d to %d doublings of flow\n", classes,
                classes == 1 ? "" : "s", int(leastFlow), int(mostFlow));
    if (walked > 8 && widest < narrowest * 1.5 && classes > 2)
        fault("every river is the same width - the flow is not reaching the channel");
    // One steep bank is a steep bank; a fifth of them is the model. What this
    // is guarding against is the water ending at a width out of a table while
    // the ground under it is still metres down, which draws every river with a
    // staircase of four-metre treads down each side.
    if (shores > 20 && walls * 5 > shores)
        fault(std::to_string(walls) + " shores in " + std::to_string(shores) +
              " end over three metres of water - rivers will be drawn with a staircase for a bank");
}

// --- retopology ----------------------------------------------------------
//
// Where the triangles went, and whether they were needed.
//
// A triangle exists to describe a shape. One lying on ground that is already
// flat describes nothing: the plane through its corners is the ground, and
// deleting it would change no pixel. A mesh made mostly of those is not a
// detailed mesh, it is an expensive one - and it is invisible by eye, because
// the picture looks right while costing several times what it should.
//
// So: build the mesh the renderer would build, and for each of its triangles
// ask how far the real ground inside it departs from its own plane. That number
// is what the triangle bought. The distribution of it is the retopology.
void lookAtRetopology(const world::HeightField& field, double centreX, double centreY) {
    constexpr int kCells = 64;
    struct Recipe { const char* name; double tolerance; int stride; double normalDegrees; double morph; };
    // The first line is what the runtime actually builds with; the rest say
    // which of its three criteria is buying the triangles.
    // The runtime's error budget is a PIXEL, converted to metres where the tile
    // is, so these are the tolerances it would use for a tile a quarter of a
    // kilometre away, one kilometre away, and four. The fault is judged on the
    // middle one: ground a kilometre off is the ordinary case, and if most of
    // its triangles are carrying nothing there, the retopology is not working.
    const Recipe recipes[]{
            {"at 1 km", 1.0, kCells / 8, 10, 0.25},
            {"  + loose morph", 1.0, kCells / 8, 10, 1e9},
            {"  + no boundary", 1.0, kCells, 10, 1e9},
            {"at 4 km", 4.0, kCells / 8, 10, 0.25},
            {"  + loose morph", 4.0, kCells, 10, 1e9},
    };
    for (const double step : {8.0, 32.0}) {
        const double originX = std::floor(centreX / (kCells * step)) * kCells * step;
        const double originY = std::floor(centreY / (kCells * step)) * kCells * step;
        // Metres from the tile's own origin, which is what the mesh builder
        // hands its surface callback - not lattice units, which is what the
        // vertices are stored in.
        const auto height = [&](double mx, double my) {
            return field.heightAt({Fixed::fromDoubleForContent(originX + mx),
                                   Fixed::fromDoubleForContent(originY + my)})
                    .toDouble();
        };
        const engine::SurfaceSample surface = [&](double mx, double my) {
            return std::array<float, 2>{float(height(mx, my)), 0.0f};
        };
        for (const Recipe& recipe : recipes) {
            engine::AdaptiveCriteria criteria;
            criteria.boundaryStride = recipe.stride;
            criteria.normalErrorDegrees = recipe.normalDegrees;
            const auto mesh = engine::makeAdaptiveMesh(kCells, step, recipe.tolerance, surface, {},
                                                       recipe.morph, {}, {}, {}, criteria);
            if (!mesh || mesh->surfaceIndices < 3) continue;

            std::vector<double> errors;
            errors.reserve(mesh->surfaceIndices / 3);
            for (std::uint32_t i = 0; i + 2 < mesh->surfaceIndices; i += 3) {
                const auto& a = mesh->vertices[mesh->indices[i]];
                const auto& b = mesh->vertices[mesh->indices[i + 1]];
                const auto& c = mesh->vertices[mesh->indices[i + 2]];
                const double ax = a.x * step, ay = a.y * step;
                const double bx = b.x * step, by = b.y * step;
                const double cx = c.x * step, cy = c.y * step;
                if (std::abs((bx - ax) * (cy - ay) - (by - ay) * (cx - ax)) < 1e-9) continue;
                // Against the TRIANGULATION, not against a plane through three
                // stored heights. Some vertices carry no source height of their
                // own - an edge point takes one from the neighbour it is
                // stitched to - so a plane built from that field reads as the
                // whole elevation of the ground wherever it is missing, which
                // is a hundred metres of nonsense. The mesh answers for its own
                // surface; that is what is drawn and that is what to compare.
                //
                // Nine points inside the triangle, in its own barycentric frame,
                // so the sampling does not favour an axis the mesh was built on.
                double worst = 0;
                for (int u = 1; u <= 3; ++u)
                    for (int v = 1; u + v <= 4; ++v) {
                        const double sp = u / 5.0, t = v / 5.0, r = 1.0 - sp - t;
                        const double x = ax * r + bx * sp + cx * t;
                        const double y = ay * r + by * sp + cy * t;
                        worst = std::max(worst, std::abs(height(x, y) - mesh->sample(x, y)[0]));
                    }
                errors.push_back(worst);
            }
            if (errors.empty()) continue;
            std::sort(errors.begin(), errors.end());
            const auto share = [&](double under) {
                return double(std::lower_bound(errors.begin(), errors.end(), under) -
                              errors.begin()) /
                       double(errors.size()) * 100.0;
            };
            std::printf("  %3.0f m %-15s %6zu tri, median %.3f m, 90th %.2f m, worst %.2f m;"
                        " %.0f%% under 5 cm, %.0f%% under 25 cm\n",
                        step, recipe.name, errors.size(), errors[errors.size() / 2],
                        errors[errors.size() * 9 / 10], errors.back(), share(0.05), share(0.25));
            // Half the mesh describing nothing is a mesh that should be half the
            // size. The bar is deliberately generous: this is not about the last
            // ten per cent, it is about whether the retopology works at all.
            if (recipe.tolerance == recipes[0].tolerance && recipe.morph < 1.0 &&
                share(0.05) > 50.0)
                fault("most triangles lie on ground that is already flat - not retopology");
        }
    }
}

// --- levels of detail ----------------------------------------------------
void lookAtLevels(const world::HeightField& field, world::ChunkId chunk) {
    // A coarse patch and a fine one are the same samples of the same field, so
    // where they share a vertex they have to share its height exactly.
    const world::TerrainMesh fine = world::buildChunkMesh(field, chunk, 0);
    const world::ChunkId coarseChunk{static_cast<std::int32_t>(world::floorDiv(chunk.x, 4)),
                                     static_cast<std::int32_t>(world::floorDiv(chunk.y, 4))};
    const world::TerrainMesh coarse = world::buildChunkMesh(field, coarseChunk, 2);

    int shared = 0, differ = 0;
    double worst = 0;
    for (const world::TerrainVertex& c : coarse.vertices)
        for (const world::TerrainVertex& f : fine.vertices) {
            if (c.position.x.raw != f.position.x.raw || c.position.y.raw != f.position.y.raw)
                continue;
            ++shared;
            const double apart = std::abs(c.height.toDouble() - f.height.toDouble());
            worst = std::max(worst, apart);
            if (c.height.raw != f.height.raw) ++differ;
        }
    std::printf("  levels: %d shared vertices, %d disagree, worst by %.3f m\n", shared, differ,
                worst);
    if (shared == 0) fault("no shared vertices between levels - the lattices do not line up");
    if (differ > 0) fault("the same ground has two heights depending on the level drawn");
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t seed = 11;
    std::int32_t cells = generation::kDefaultWorldCells;
    double spanMetres = 1200;
    double atX = -1, atY = -1;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (a == "--world" && i + 1 < argc) cells = std::atoi(argv[++i]);
        else if (a == "--span" && i + 1 < argc) spanMetres = std::atof(argv[++i]);
        else if (a == "--at" && i + 2 < argc) { atX = std::atof(argv[++i]); atY = std::atof(argv[++i]); }
        else if (a == "--help") {
            std::printf("usage: terrain_lint [--seed N] [--world CELLS] [--at X Y] [--span M]\n");
            return 0;
        }
    }

    generation::WorldMapParams params;
    params.seed = seed;
    params.width = params.height = cells;
    const generation::WorldMapData world = generation::generateWorldMap(params);
    const world::HeightField field(&world, seed);

    // Somewhere with ground on it, unless told otherwise: the sea has no
    // artefacts worth finding.
    if (atX < 0) {
        // Land, and away from the edge of the map: the coarse map has to repeat
        // its edge cells to interpolate at the border, and that repetition is an
        // artefact of the border rather than of the generator.
        const std::int32_t margin = std::max(6, world.width / 8);
        for (std::int32_t y = margin; y < world.height - margin && atX < 0; y += 3)
            for (std::int32_t x = margin; x < world.width - margin && atX < 0; x += 3) {
                bool land = true;
                for (std::int32_t dy = -1; dy <= 1; ++dy)
                    for (std::int32_t dx = -1; dx <= 1; ++dx)
                        if (world.at({x + dx, y + dy}).sea) land = false;
                if (!land) continue;
                atX = static_cast<double>(x) * generation::kMetresPerCell;
                atY = static_cast<double>(y) * generation::kMetresPerCell;
            }
    }

    std::printf("world %d cells (%.0f km), seed %llu; looking at %.0f, %.0f over %.0f m\n", cells,
                cells * generation::kMetresPerCell / 1000.0,
                static_cast<unsigned long long>(seed), atX, atY, spanMetres);

    std::printf("steps:\n");
    lookForSteps(field, atX, atY, spanMetres);
    std::printf("lattices:\n");
    lookForLattices(field, atX, atY, spanMetres);
    std::printf("rivers:\n");
    lookAtRivers(field, world);
    std::printf("retopology:\n");
    lookAtRetopology(field, atX, atY);
    std::printf("levels of detail:\n");
    lookAtLevels(field, world::chunkOf(at(atX, atY)));

    std::printf("%s\n", faults == 0 ? "nothing to go and look at" : "");
    return faults == 0 ? 0 : 1;
}
