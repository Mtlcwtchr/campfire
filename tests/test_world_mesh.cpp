// The world as geometry: the height field, the chunk meshes cut out of it, and
// the cliff lines.
//
// Most of these tests are about borders, because that is the thing a chunked
// world gets wrong. They are written as identities rather than as tolerances: a
// shared vertex is not "close enough" between two chunks, it is the same
// number, and if that ever stops being true the architecture has been broken
// rather than the maths having drifted.

#include "framework.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <utility>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/world/coords.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_mesh.hpp"

namespace {

using core::Fixed;
using core::WorldPos;

WorldPos at(double x, double y) {
    return {Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)};
}

// A small country to hang the field on, so the tests run against the same two
// layers the game does rather than against the field's own invented ground.
const generation::WorldMapData& country() {
    static generation::WorldMapData world = [] {
        generation::WorldMapParams params;
        params.seed = 4242;
        params.width = params.height = 96;
        // The dials pinned, not taken from the defaults.
        //
        // These tests are about geometry and about getting from one place to
        // another, not about how much of a world is sea - and when the default
        // moved to the share this world has (D141), the flattest landlocked
        // cell on this seed came out a patch hemmed in by water and four
        // pathfinding tests stopped finding a way at all. A test that wants a
        // continent under it has to ask for one.
        params.seaPercent = 62;
        params.erosionPasses = 2;
        return generation::generateWorldMap(params);
    }();
    return world;
}


// A chunk with ground worth testing on, found rather than assumed.
//
// Read off the coarse map, which is what knows where the country keeps its
// mountains and its flat land: hunting for them by sampling the field would be
// a million queries over a world fifty kilometres across, and naming a chunk
// outright is a test that fails the day the map changes scale - and fails as
// "the navmesh is broken" rather than as "you are standing in the sea".
world::ChunkId groundWhere(const world::HeightField& field, bool wantBroken) {
    (void)field;
    const generation::WorldMapData& map = country();
    core::TilePos best{map.width / 2, map.height / 2};
    std::int32_t bestScore = -1;
    for (std::int32_t y = 2; y < map.height - 2; ++y) {
        for (std::int32_t x = 2; x < map.width - 2; ++x) {
            std::int32_t low = 999, high = -999;
            bool anySea = false;
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const generation::WorldCell& c = map.at({x + dx, y + dy});
                    if (c.sea) anySea = true;
                    low = std::min<std::int32_t>(low, c.elevation);
                    high = std::max<std::int32_t>(high, c.elevation);
                }
            if (anySea) continue;
            const std::int32_t relief = high - low;
            const std::int32_t score = wantBroken ? relief : 200 - relief * 20;
            if (score > bestScore) {
                bestScore = score;
                best = {x, y};
            }
        }
    }
    const core::Fixed metres = core::Fixed::fromInt(generation::kMetresPerCell);
    return world::chunkOf({core::Fixed::fromInt(best.x) * metres + metres / core::Fixed::fromInt(2),
                           core::Fixed::fromInt(best.y) * metres + metres / core::Fixed::fromInt(2)});
}

} // namespace

TEST(chunk_addressing_survives_the_origin) {
    // Truncating division would mirror the world along the axes, putting the
    // ground west of the origin one chunk out.
    CHECK(world::chunkOf(at(0.5, 0.5)) == (world::ChunkId{0, 0}));
    CHECK(world::chunkOf(at(-0.5, -0.5)) == (world::ChunkId{-1, -1}));
    CHECK(world::chunkOf(at(world::kChunkMetres - 0.5, 0)) == (world::ChunkId{0, 0}));
    CHECK(world::chunkOf(at(world::kChunkMetres + 0.5, 0)) == (world::ChunkId{1, 0}));
    CHECK(world::chunkOf(at(-world::kChunkMetres - 0.5, 0)) == (world::ChunkId{-2, 0}));

    // And a chunk's origin is inside it, on both sides of the world origin.
    for (world::ChunkId c : {world::ChunkId{0, 0}, world::ChunkId{-3, 5}, world::ChunkId{7, -9}})
        CHECK(world::chunkOf(world::chunkOrigin(c)) == c);
}

TEST(the_ground_is_the_same_ground_from_either_chunk) {
    const world::HeightField field(&country(), 99);
    const world::ChunkId west{3, 2};
    const world::ChunkId east{4, 2};
    const world::TerrainMesh a = world::buildChunkMesh(field, west);
    const world::TerrainMesh b = world::buildChunkMesh(field, east);

    // The last column of one is the first column of the other. Not stitched:
    // computed twice from the same world coordinates, which is why it comes out
    // bit-identical - position, height, normal and every material weight.
    int compared = 0;
    for (std::int32_t row = 0; row < a.side; ++row) {
        const world::TerrainVertex& left = a.vertices[row * a.side + (a.side - 1)];
        const world::TerrainVertex& right = b.vertices[row * b.side + 0];
        CHECK(left.position.x.raw == right.position.x.raw);
        CHECK(left.position.y.raw == right.position.y.raw);
        CHECK(left.height.raw == right.height.raw);
        CHECK(left.normal.x.raw == right.normal.x.raw);
        CHECK(left.normal.y.raw == right.normal.y.raw);
        CHECK(left.normal.z.raw == right.normal.z.raw);
        for (std::size_t m = 0; m < world::kMaterialCount; ++m)
            CHECK(left.materials.weight[m].raw == right.materials.weight[m].raw);
        ++compared;
    }
    CHECK(compared == a.side);

    // The same north to south, where the shared edge is a row rather than a
    // column - a different piece of indexing, and the one that usually rots.
    const world::TerrainMesh north = world::buildChunkMesh(field, {3, 2});
    const world::TerrainMesh south = world::buildChunkMesh(field, {3, 3});
    for (std::int32_t col = 0; col < north.side; ++col) {
        const world::TerrainVertex& top = north.vertices[(north.side - 1) * north.side + col];
        const world::TerrainVertex& bottom = south.vertices[0 * south.side + col];
        CHECK(top.height.raw == bottom.height.raw);
        CHECK(top.normal.z.raw == bottom.normal.z.raw);
    }
}

TEST(a_chunk_does_not_know_what_else_is_loaded) {
    // The same chunk built on its own and built after its neighbours is the same
    // chunk. Nothing accumulates, nothing is cached into a different answer.
    const world::HeightField field(&country(), 7);
    const world::TerrainMesh alone = world::buildChunkMesh(field, {-2, 6});
    for (world::ChunkId c : world::withNeighbours({-2, 6})) (void)world::buildChunkMesh(field, c);
    const world::TerrainMesh again = world::buildChunkMesh(field, {-2, 6});

    CHECK(alone.vertices.size() == again.vertices.size());
    CHECK(alone.indices == again.indices);
    for (std::size_t i = 0; i < alone.vertices.size(); ++i) {
        CHECK(alone.vertices[i].height.raw == again.vertices[i].height.raw);
        CHECK(alone.vertices[i].normal.x.raw == again.vertices[i].normal.x.raw);
    }

    // And a second field made from the same seed and country is the same world.
    const world::HeightField twin(&country(), 7);
    const world::TerrainMesh copy = world::buildChunkMesh(twin, {-2, 6});
    for (std::size_t i = 0; i < alone.vertices.size(); ++i)
        CHECK(alone.vertices[i].height.raw == copy.vertices[i].height.raw);
}

TEST(height_can_be_asked_anywhere_and_agrees_with_the_mesh) {
    const world::HeightField field(&country(), 11);
    const world::ChunkId chunk{5, 5};
    const world::TerrainMesh mesh = world::buildChunkMesh(field, chunk);

    // At a lattice point the field and the mesh are the same number.
    const WorldPos corner = world::chunkOrigin(chunk);
    CHECK(field.heightAt(corner).raw == mesh.vertices[0].height.raw);

    // Between them the field interpolates: a walk across a sample cell changes
    // height in small steps, with no jump at the cell boundary. This is what
    // "continuous field" has to mean in practice - the old tile map answered the
    // same height for a whole square metre and then stepped.
    Fixed previous = field.heightAt(corner);
    Fixed biggestStep = core::kZero;
    for (int i = 1; i <= 400; ++i) {
        const WorldPos p{corner.x + Fixed::ratio(i, 4), corner.y + Fixed::ratio(i, 7)};
        const Fixed here = field.heightAt(p);
        const Fixed step = Fixed::fromRaw(here.raw > previous.raw ? here.raw - previous.raw
                                                                 : previous.raw - here.raw);
        if (step.raw > biggestStep.raw) biggestStep = step;
        previous = here;
    }
    // A quarter of a metre of walking cannot move the ground a whole metre
    // unless the field is stepping somewhere.
    CHECK(biggestStep.raw < core::kOne.raw);

    // Including across a chunk border, which is the place a step would be
    // hidden: the field does not know the border is there.
    const Fixed inside = field.heightAt(at(world::kChunkMetres * 6 - 0.001, 100.0));
    const Fixed over = field.heightAt(at(world::kChunkMetres * 6 + 0.001, 100.0));
    const Fixed jump = Fixed::fromRaw(inside.raw > over.raw ? inside.raw - over.raw
                                                           : over.raw - inside.raw);
    CHECK(jump.raw < Fixed::ratio(1, 100).raw);
}

TEST(a_cliff_line_runs_through_a_chunk_border) {
    // Cliffs are cut from the same field, so a line that leaves a chunk has to
    // arrive in the next one at the same point. Gather the segments of a block
    // of chunks and check that no line simply stops on an interior border.
    const world::HeightField field(&country(), 3);

    // Broken ground, wherever this country keeps it: a flat chunk would let the
    // test pass by having nothing to get wrong.
    const world::ChunkId around = groundWhere(field, true);
    world::ChunkId centre{0, 0};
    std::vector<world::CliffSegment> all;
    // Widely, because a cliff is rare on purpose: it is the line where the
    // ground stops being ground, and a country where every hillside qualified
    // would be a country nobody could walk across (D117).
    //
    // The most broken chunk in the window, not the first one with a few
    // segments in it. The first is as likely to be a single notch in a hillside
    // as a cliff, and a notch chains into stubs however well the chaining
    // works - which made this test a report on where the search happened to
    // stop rather than on whether lines survive a border.
    std::size_t most = 0;
    for (std::int32_t y = around.y - 8; y <= around.y + 8; ++y)
        for (std::int32_t x = around.x - 8; x <= around.x + 8; ++x) {
            const std::size_t here = world::extractCliffs(field, {x, y}).size();
            if (here < 3 || here <= most) continue;
            if (world::extractCliffs(field, {x - 1, y}).empty()) continue;
            most = here;
            centre = {x, y};
        }
    if (most > 0)
        for (world::ChunkId c : world::withNeighbours(centre)) {
            std::vector<world::CliffSegment> here = world::extractCliffs(field, c);
            all.insert(all.end(), here.begin(), here.end());
        }
    if (all.empty()) {
        std::cerr << "    no broken ground anywhere in this country; the test proved nothing\n";
        CHECK(!all.empty());
        return;
    }

    // Every segment the centre chunk produced on its own border is produced
    // identically by the neighbour that shares it - the segments come in pairs
    // rather than the line ending at the seam.
    const std::vector<world::CliffSegment> mine = world::extractCliffs(field, centre);
    const Fixed left = world::chunkOrigin(centre).x;
    int onTheBorder = 0, matched = 0;
    for (const world::CliffSegment& seg : mine) {
        if (seg.a.x.raw != left.raw || seg.b.x.raw != left.raw) continue;
        ++onTheBorder;
        for (const world::CliffSegment& other :
             world::extractCliffs(field, {centre.x - 1, centre.y})) {
            if (other.a.x.raw == seg.a.x.raw && other.a.y.raw == seg.a.y.raw &&
                other.b.x.raw == seg.b.x.raw && other.b.y.raw == seg.b.y.raw &&
                other.top.raw == seg.top.raw && other.bottom.raw == seg.bottom.raw) {
                ++matched;
                break;
            }
        }
    }
    CHECK(onTheBorder == matched);

    // And chaining across the whole block gives lines, not a pile of two-point
    // stubs: if the borders were breaking the lines, every line would be short.
    const std::vector<world::CliffLine> lines = world::chainCliffs(all);
    std::size_t longest = 0;
    for (const world::CliffLine& line : lines) longest = std::max(longest, line.points.size());
    CHECK(longest > 4);
}

TEST(a_cliff_has_a_face_and_stops_a_walker) {
    const world::HeightField field(&country(), 3);
    const world::ChunkId around = groundWhere(field, true);
    std::vector<world::CliffSegment> segments;
    for (std::int32_t y = around.y - 8; y <= around.y + 8 && segments.empty(); ++y)
        for (std::int32_t x = around.x - 8; x <= around.x + 8 && segments.empty(); ++x) {
            std::vector<world::CliffSegment> here = world::extractCliffs(field, {x, y});
            if (here.size() >= 3) segments = here;
        }
    CHECK(!segments.empty());
    if (segments.empty()) return;

    const world::CliffStrip strip = world::buildCliffStrip(segments);
    CHECK(strip.indices.size() == segments.size() * 6);
    CHECK(strip.vertices.size() == segments.size() * 4);

    // A cliff line has broken ground on one side of it. That is what it is for:
    // the navmesh will cut its walkable polygons off along this line, and it
    // reads the answer off the geometry rather than off a flag on a tile.
    int withBrokenGround = 0;
    for (const world::CliffSegment& seg : segments) {
        const Fixed midX = (seg.a.x + seg.b.x) / Fixed::fromInt(2);
        const Fixed midY = (seg.a.y + seg.b.y) / Fixed::fromInt(2);
        const Fixed reach = Fixed::fromInt(world::kSampleMetres) / Fixed::fromInt(2);
        const WorldPos high{midX + seg.fallX * -reach, midY + seg.fallY * -reach};
        const WorldPos low{midX + seg.fallX * reach, midY + seg.fallY * reach};
        if (!field.walkable(high) || !field.walkable(low)) ++withBrokenGround;
        // And the face is drawn between the two heights the line was cut from.
        CHECK(seg.top.raw >= seg.bottom.raw);
    }
    CHECK(withBrokenGround * 4 >= static_cast<int>(segments.size()) * 3);

    // The faces are rock, whatever is growing on the ground above them.
    for (const world::TerrainVertex& v : strip.vertices)
        CHECK(v.materials.strongest() == world::Material::Rock);
}

TEST(materials_are_weights_and_they_blend) {
    const world::HeightField field(&country(), 21);

    // Weights always sum to one, everywhere - the invariant the renderer and
    // the walking cost both lean on.
    for (int i = 0; i < 200; ++i) {
        const WorldPos p{Fixed::ratio(i * 37, 4), Fixed::ratio(i * 53, 3)};
        const world::MaterialWeights w = field.materialsAt(p);
        Fixed total = core::kZero;
        for (Fixed each : w.weight) {
            CHECK(each.raw >= 0);
            total += each;
        }
        const std::int64_t off = total.raw > core::kOne.raw ? total.raw - core::kOne.raw
                                                            : core::kOne.raw - total.raw;
        CHECK(off < (core::kOne.raw >> 20));
    }

    // And a transition is a gradient rather than an edge: walking from one
    // material's ground to another's, the weights move a little at a time. A
    // grid-locked material would jump from nought to one at a sample boundary.
    Fixed previousGrass = field.materialsAt(at(0, 0)).of(world::Material::Grass);
    Fixed biggestJump = core::kZero;
    for (int i = 1; i <= 600; ++i) {
        const WorldPos p{Fixed::ratio(i, 5), Fixed::ratio(i, 9)};
        const Fixed grass = field.materialsAt(p).of(world::Material::Grass);
        const Fixed step = Fixed::fromRaw(grass.raw > previousGrass.raw
                                                  ? grass.raw - previousGrass.raw
                                                  : previousGrass.raw - grass.raw);
        if (step.raw > biggestJump.raw) biggestJump = step;
        previousGrass = grass;
    }
    CHECK(biggestJump.raw < Fixed::ratio(1, 4).raw);
}

TEST(the_lattice_is_not_the_chunk_and_not_the_tile) {
    // The three resolutions are independent by construction. This is a test
    // because the whole point of the rewrite is that they stop being the same
    // number, and the cheapest way to lose that is for somebody to "simplify"
    // one of them into the other.
    CHECK(world::kSampleMetres != world::kChunkMetres);
    CHECK(world::kChunkMetres % world::kSampleMetres == 0);
    CHECK(world::kSampleMetres != static_cast<std::int32_t>(core::kTileSize.toInt()));

    // Height is asked for at a point, and a point is not a tile: two positions
    // inside the same square metre can stand at different heights.
    const world::HeightField field(&country(), 5);
    int different = 0;
    for (int i = 0; i < 50; ++i) {
        const WorldPos low{Fixed::fromInt(120 + i), Fixed::fromInt(80 + i)};
        const WorldPos high{low.x + Fixed::ratio(9, 10), low.y + Fixed::ratio(9, 10)};
        if (field.heightAt(low).raw != field.heightAt(high).raw) ++different;
    }
    CHECK(different > 40);
}

// The morph target is not "about right": it is the coarser mesh, evaluated by
// walking that mesh's own triangles. If the two ever disagree, a change of
// level is a change of shape again - which is the thing the morph exists to
// abolish - and it will disagree first at the diagonals, because that is the
// one place where the coarse mesh makes a choice this one has to make the same
// way.
TEST(a_patch_carries_the_level_above_it_exactly) {
    const world::HeightField field(&country(), 4242);
    // At level nought the two levels read the field the same way, so the target
    // is this mesh's own samples read by twos. At level four they do not - the
    // level above leaves out waves and valleys it cannot draw - and the target
    // has to be the level above's own answer rather than a smoothing of this
    // one. Both are checked, because the second is where it would break.
    for (std::int32_t lod : {0, 4}) {
        const world::ChunkId fine = groundWhere(field, true);
        const world::ChunkId coarse{fine.x >> 1, fine.y >> 1};
        const world::TerrainMesh sharp = world::buildChunkMesh(field, fine, lod);
        const world::TerrainMesh above = world::buildChunkMesh(field, coarse, lod + 1);

        // Where the coarse mesh's surface passes over a point, from its triangles.
        const auto surfaceAt = [&above](double x, double y) {
            for (std::size_t t = 0; t + 2 < above.indices.size(); t += 3) {
                const world::TerrainVertex& a = above.vertices[above.indices[t]];
                const world::TerrainVertex& b = above.vertices[above.indices[t + 1]];
                const world::TerrainVertex& c = above.vertices[above.indices[t + 2]];
                const double ax = a.position.x.toDouble(), ay = a.position.y.toDouble();
                const double bx = b.position.x.toDouble(), by = b.position.y.toDouble();
                const double cx = c.position.x.toDouble(), cy = c.position.y.toDouble();
                const double area = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
                if (area == 0) continue;
                const double u = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / area;
                const double v = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / area;
                const double w = 1.0 - u - v;
                const double edge = -1e-6;
                if (u < edge || v < edge || w < edge) continue;
                return u * a.height.toDouble() + v * b.height.toDouble() + w * c.height.toDouble();
            }
            return 1e9;   // outside the coarse patch: not a case this test allows
        };

        std::size_t checked = 0;
        for (const world::TerrainVertex& v : sharp.vertices) {
            const double found = surfaceAt(v.position.x.toDouble(), v.position.y.toDouble());
            CHECK(found < 1e8);
            // A millimetre, and that is the barycentric arithmetic in doubles
            // rather than any disagreement about the ground.
            CHECK(std::abs(found - v.coarseHeight.toDouble()) < 0.001);
            ++checked;
        }
        CHECK(checked == sharp.vertices.size());
    }
}

// Half of them do not move at all: they are the coarser mesh's own vertices, at
// the same world coordinate. That is what makes the morph free of seams - the
// points two levels share are never in flight.
TEST(every_second_vertex_is_already_where_the_level_above_puts_it) {
    const world::HeightField field(&country(), 4242);
    const world::ChunkId chunk = groundWhere(field, true);
    const world::TerrainMesh mesh = world::buildChunkMesh(field, chunk, 2);
    std::size_t still = 0, moved = 0;
    for (std::int32_t row = 0; row < mesh.side; ++row)
        for (std::int32_t col = 0; col < mesh.side; ++col) {
            const world::TerrainVertex& v =
                    mesh.vertices[static_cast<std::size_t>(row) * mesh.side + col];
            if ((col & 1) == 0 && (row & 1) == 0) {
                CHECK(v.coarseHeight.raw == v.height.raw);
                ++still;
            } else if (v.coarseHeight.raw != v.height.raw) {
                ++moved;
            }
        }
    CHECK(still == 81);
    // On broken ground practically all of the rest move; the point is that some
    // do, so the test is not passing because the target was never filled in.
    CHECK(moved > 100);
}

// The simplification is for the far view and nothing else. Everything the
// simulation asks - where the ground is, whether it can be walked, where the
// water stands - goes through the unqualified call, and that one has to be the
// whole truth however coarsely somebody else happens to be drawing.
TEST(nothing_is_left_out_of_the_ground_that_is_walked_on) {
    const world::HeightField field(&country(), 4242);
    std::size_t compared = 0, differ = 0;
    for (std::int64_t y = 2000; y < 2040; ++y)
        for (std::int64_t x = 2000; x < 2040; ++x) {
            // Up to the coarse lattice itself, which is the last spacing that
            // leaves nothing out.
            CHECK(field.sampleHeight(x, y).raw ==
                  field.sampleHeight(x, y, world::kCoarseLatticeMetres).raw);
            ++compared;
            // And past it, it does leave something out - or this test is
            // checking that a switch nobody ever throws does nothing.
            if (field.sampleHeight(x, y).raw != field.sampleHeight(x, y, 1024).raw) ++differ;
        }
    CHECK(compared == 1600);
    CHECK(differ > 1000);
}

// What it leaves out is detail, not country. A kilometre to the sample is a
// different picture of the same ground, not a different ground - which is what
// lets the morph between two levels be short rather than a slow dissolve.
TEST(a_coarse_reading_is_the_same_country) {
    const world::HeightField field(&country(), 4242);
    double sum = 0, worst = 0;
    int n = 0;
    for (std::int64_t j = 0; j < 40; ++j)
        for (std::int64_t i = 0; i < 40; ++i) {
            const std::int64_t sx = 2000 + i * 256, sy = 2000 + j * 256;
            const double truth = field.sampleHeight(sx, sy).toDouble();
            const double seen = field.sampleHeight(sx, sy, 1024).toDouble();
            sum += (truth - seen) * (truth - seen);
            worst = std::max(worst, std::abs(truth - seen));
            ++n;
        }
    // The detail layer swings about ninety metres all told; what is dropped at
    // a kilometre to the sample is the part of it that no sample a kilometre
    // apart could have shown anyway.
    CHECK(std::sqrt(sum / n) < 60.0);
    CHECK(worst < 250.0);
}
