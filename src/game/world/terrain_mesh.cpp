#include "game/world/terrain_mesh.hpp"

#include <map>

namespace world {
namespace {

using core::Fixed;
using core::WorldPos;

// A point on the half-lattice, in units of half a sample step. Cliff segments
// have their ends here, and this is what lets two of them be recognised as the
// same point without comparing fixed-point coordinates: it is an exact integer
// pair, computed the same way from either side of a chunk border.
struct HalfPoint {
    std::int64_t x, y;
};

// A cliff endpoint, as the exact raw coordinates it was built from. Ends are
// matched on these rather than on nearly-equal positions: every endpoint is a
// whole number of half-steps from the world origin and is computed the same way
// wherever it came from, so two chunks name a shared point identically.
struct EndKey {
    std::int64_t x, y;
};
inline bool operator<(EndKey a, EndKey b) { return a.y != b.y ? a.y < b.y : a.x < b.x; }
inline EndKey endKey(WorldPos p) { return {p.x.raw, p.y.raw}; }

WorldPos worldOf(HalfPoint p) {
    // Half a sample step per unit: exact in fixed point, since kSampleMetres is
    // even, so no rounding creeps in on one side of a border and not the other.
    const Fixed half = Fixed::ratio(kSampleMetres, 2);
    return {half * Fixed::fromInt(p.x), half * Fixed::fromInt(p.y)};
}

// The heights of one chunk's lattice, with a ring of margin around it.
//
// Every vertex wants its own height, the four around it for a normal, the four
// around it again for a slope, and the material wants both - so a mesh built by
// asking the field for each of those in turn samples the same point ten times
// over. The field is a walk through the coarse map, three octaves of noise and
// the drainage of every cell nearby; ten times over is the difference between a
// patch of ground in two milliseconds and in twenty.
struct HeightGrid {
    std::int32_t side = 0;                 // including the margin on both sides
    std::int32_t margin = 0;
    std::int64_t baseX = 0, baseY = 0;     // lattice coordinate of the chunk's corner
    std::int64_t stride = 1;
    std::vector<Fixed> height;

    // Addressed in the chunk's own coordinates: (0,0) is its corner, and
    // negative or past-the-end reads into the margin. It is the margin that has
    // to be added here rather than a one - a grid with two rings of it,
    // addressed as though it had one, is the whole ground shifted by a sample,
    // which reads as everything being subtly wrong and nothing being obviously
    // broken.
    Fixed at(std::int32_t col, std::int32_t row) const {
        const std::int32_t x = std::clamp(col + margin, 0, side - 1);
        const std::int32_t y = std::clamp(row + margin, 0, side - 1);
        return height[static_cast<std::size_t>(y) * side + x];
    }
};

// A block of the height lattice, sampled at one spacing.
//
// `cells` is how many steps across the block runs before the margin is added,
// so a chunk asks for kSamplesPerChunk and the block that stands for the level
// above it asks for half as many at twice the stride.
HeightGrid gridAt(const HeightField& field, std::int64_t baseX, std::int64_t baseY,
                  std::int64_t stride, std::int32_t cells, std::int32_t margin) {
    HeightGrid grid;
    grid.stride = stride;
    grid.margin = margin;
    grid.side = cells + 1 + margin * 2;
    grid.baseX = baseX;
    grid.baseY = baseY;
    grid.height.resize(static_cast<std::size_t>(grid.side) * grid.side);
    // The field is told how far apart these samples are, and leaves out what
    // cannot be seen at that spacing (height_field.hpp, Detail). That is what
    // makes a coarse patch cheaper to work out and not merely cheaper to draw.
    const std::int64_t spacing = stride * kSampleMetres;
    for (std::int32_t row = 0; row < grid.side; ++row)
        for (std::int32_t col = 0; col < grid.side; ++col)
            grid.height[static_cast<std::size_t>(row) * grid.side + col] =
                    field.sampleHeight(baseX + (col - margin) * stride,
                                       baseY + (row - margin) * stride, spacing);
    return grid;
}

HeightGrid gridFor(const HeightField& field, ChunkId chunk, std::int32_t lod, std::int32_t margin) {
    const std::int64_t stride = std::int64_t(1) << lod;
    return gridAt(field, static_cast<std::int64_t>(chunk.x) * kSamplesPerChunk * stride,
                  static_cast<std::int64_t>(chunk.y) * kSamplesPerChunk * stride, stride,
                  kSamplesPerChunk, margin);
}

// The lie of the ground at a lattice point of the grid, from its neighbours.
Normal normalFrom(const HeightGrid& grid, std::int32_t col, std::int32_t row) {
    const Fixed run = Fixed::fromInt(2 * kSampleMetres * grid.stride);
    const Fixed dzdx = (grid.at(col + 1, row) - grid.at(col - 1, row)) / run;
    const Fixed dzdy = (grid.at(col, row + 1) - grid.at(col, row - 1)) / run;
    Fixed x = -dzdx, y = -dzdy, z = core::kOne;
    const Fixed length = core::sqrt(x * x + y * y + z * z);
    if (length.raw == 0) return {core::kZero, core::kZero, core::kOne};
    return {x / length, y / length, z / length};
}

// How the point stands against the ground two steps out in each direction.
Fixed opennessFrom(const HeightGrid& grid, std::int32_t col, std::int32_t row) {
    const Fixed here = grid.at(col, row);
    const Fixed around = (grid.at(col + 2, row) + grid.at(col - 2, row) + grid.at(col, row + 2) +
                          grid.at(col, row - 2) + grid.at(col + 2, row + 2) +
                          grid.at(col - 2, row - 2) + grid.at(col + 2, row - 2) +
                          grid.at(col - 2, row + 2)) /
                         Fixed::fromInt(8);
    return here - around;
}

// Halfway between two normals, back on the sphere. What the coarse mesh shades
// with along an edge between two of its vertices, which is where the vertices
// this mesh has and that one has not are sitting.
Normal midNormal(Normal a, Normal b) {
    Fixed x = (a.x + b.x) / Fixed::fromInt(2);
    Fixed y = (a.y + b.y) / Fixed::fromInt(2);
    Fixed z = (a.z + b.z) / Fixed::fromInt(2);
    const Fixed length = core::sqrt(x * x + y * y + z * z);
    if (length.raw == 0) return {core::kZero, core::kZero, core::kOne};
    return {x / length, y / length, z / length};
}

Fixed slopeFrom(const HeightGrid& grid, std::int32_t col, std::int32_t row) {
    const Fixed run = Fixed::fromInt(2 * kSampleMetres * grid.stride);
    const Fixed dzdx = (grid.at(col + 1, row) - grid.at(col - 1, row)) / run;
    const Fixed dzdy = (grid.at(col, row + 1) - grid.at(col, row - 1)) / run;
    return core::sqrt(dzdx * dzdx + dzdy * dzdy);
}

} // namespace

TerrainMesh buildChunkMesh(const HeightField& field, ChunkId chunk, std::int32_t lod) {
    TerrainMesh mesh;
    mesh.chunk = chunk;
    mesh.lod = lod;
    // One more than the cells, so the last row and column of vertices sit
    // exactly on the chunk's far edge - and are therefore the same vertices the
    // next chunk starts with.
    mesh.side = kSamplesPerChunk + 1;

    // At a coarser level every step is a stride over the same lattice, so a
    // vertex of a far chunk is still a sample of the same field at the same
    // world coordinate - which is why levels agree with each other where they
    // meet instead of having to be stitched.
    // Two rings of margin: the openness of a vertex is measured against ground
    // two steps away, and the vertices on the chunk's edge have to be able to
    // look outside it.
    const HeightGrid grid = gridFor(field, chunk, lod, 2);
    const std::int64_t stride = grid.stride;
    const std::int64_t baseX = grid.baseX;
    const std::int64_t baseY = grid.baseY;

    mesh.vertices.reserve(static_cast<std::size_t>(mesh.side) * mesh.side);
    mesh.lowest = Fixed::fromInt(1 << 20);
    mesh.highest = Fixed::fromInt(-(1 << 20));
    for (std::int32_t row = 0; row < mesh.side; ++row) {
        for (std::int32_t col = 0; col < mesh.side; ++col) {
            const std::int64_t sx = baseX + col * stride, sy = baseY + row * stride;
            TerrainVertex v;
            v.position = {Fixed::fromInt(sx * kSampleMetres), Fixed::fromInt(sy * kSampleMetres)};
            v.height = grid.at(col, row);
            v.normal = normalFrom(grid, col, row);
            v.materials =
                    field.materialsGiven(sx, sy, v.height, slopeFrom(grid, col, row), stride);
            const auto climate = field.surfaceClimateAt(v.position);
            v.foliage = climate.foliage;
            v.desertCover = climate.desert;
            v.openness = opennessFrom(grid, col, row);
            if (v.height.raw < mesh.lowest.raw) mesh.lowest = v.height;
            if (v.height.raw > mesh.highest.raw) mesh.highest = v.height;
            mesh.vertices.push_back(v);
        }
    }

    // Where the level above puts this same ground, for the morph across a
    // change of level.
    //
    // Above thirty-two metres to the sample the levels no longer answer the same
    // question: the field is told how coarsely it is being read and leaves out
    // the waves and the valleys that spacing cannot draw (height_field.hpp,
    // Detail). So the level above is sampled outright, eleven by eleven of it,
    // at its own cheaper rate. Below that the two models agree, every second
    // sample here is one of its samples, and its grid is this grid read by twos
    // for nothing.
    //
    // Either way the target is the coarse mesh's surface and not an
    // approximation of it: at its own vertices it is its own height, and between
    // them it is the edge or the diagonal of one of its triangles - with the
    // diagonal chosen from the four heights the same way the index loop below
    // chooses it, because the two have to agree to the bit or the swap shows.
    {
        const std::int64_t aboveStride = stride * 2;
        const bool sameModel = detailFor(stride * kSampleMetres) ==
                               detailFor(aboveStride * kSampleMetres);
        HeightGrid above;
        if (!sameModel)
            above = gridAt(field, baseX, baseY, aboveStride, kSamplesPerChunk / 2, 1);
        // Addressed in the level above's own steps: its point p sits on this
        // mesh's vertex 2p.
        const auto aboveAt = [&](std::int32_t pc, std::int32_t pr) {
            return sameModel ? grid.at(pc * 2, pr * 2) : above.at(pc, pr);
        };
        const auto aboveNormal = [&](std::int32_t pc, std::int32_t pr) {
            const Fixed run = Fixed::fromInt(4 * kSampleMetres * stride);
            const Fixed dzdx = (aboveAt(pc + 1, pr) - aboveAt(pc - 1, pr)) / run;
            const Fixed dzdy = (aboveAt(pc, pr + 1) - aboveAt(pc, pr - 1)) / run;
            Fixed x = -dzdx, y = -dzdy, z = core::kOne;
            const Fixed length = core::sqrt(x * x + y * y + z * z);
            if (length.raw == 0) return Normal{core::kZero, core::kZero, core::kOne};
            return Normal{x / length, y / length, z / length};
        };
        const auto half = [](Fixed a, Fixed b) { return (a + b) / Fixed::fromInt(2); };
        const auto magnitude = [](Fixed f) { return f.raw < 0 ? -f.raw : f.raw; };
        for (std::int32_t row = 0; row < mesh.side; ++row)
            for (std::int32_t col = 0; col < mesh.side; ++col) {
                TerrainVertex& v = mesh.vertices[static_cast<std::size_t>(row) * mesh.side + col];
                const std::int32_t pc = col / 2, pr = row / 2;
                const bool acrossOdd = (col & 1) != 0, downOdd = (row & 1) != 0;
                if (!acrossOdd && !downOdd) {
                    v.coarseHeight = aboveAt(pc, pr);
                    v.coarseNormal = aboveNormal(pc, pr);
                } else if (acrossOdd && !downOdd) {
                    v.coarseHeight = half(aboveAt(pc, pr), aboveAt(pc + 1, pr));
                    v.coarseNormal = midNormal(aboveNormal(pc, pr), aboveNormal(pc + 1, pr));
                } else if (!acrossOdd && downOdd) {
                    v.coarseHeight = half(aboveAt(pc, pr), aboveAt(pc, pr + 1));
                    v.coarseNormal = midNormal(aboveNormal(pc, pr), aboveNormal(pc, pr + 1));
                } else {
                    const Fixed a = aboveAt(pc, pr), b = aboveAt(pc + 1, pr);
                    const Fixed c = aboveAt(pc, pr + 1), d = aboveAt(pc + 1, pr + 1);
                    if (magnitude(a - d) <= magnitude(b - c)) {
                        v.coarseHeight = half(a, d);
                        v.coarseNormal =
                                midNormal(aboveNormal(pc, pr), aboveNormal(pc + 1, pr + 1));
                    } else {
                        v.coarseHeight = half(b, c);
                        v.coarseNormal =
                                midNormal(aboveNormal(pc + 1, pr), aboveNormal(pc, pr + 1));
                    }
                }
            }
    }

    mesh.indices.reserve(static_cast<std::size_t>(kSamplesPerChunk) * kSamplesPerChunk * 6);
    for (std::int32_t row = 0; row < kSamplesPerChunk; ++row) {
        for (std::int32_t col = 0; col < kSamplesPerChunk; ++col) {
            const std::uint32_t a = static_cast<std::uint32_t>(row * mesh.side + col);
            const std::uint32_t b = a + 1;
            const std::uint32_t c = a + static_cast<std::uint32_t>(mesh.side);
            const std::uint32_t d = c + 1;
            // Which way to cut the quad. Along the shallower diagonal, so a
            // ridge or a bank stays a straight line instead of a staircase -
            // and the choice is made from the four heights, which both chunks
            // sharing this quad's edge would compute the same way.
            const Fixed acrossOne = mesh.vertices[a].height - mesh.vertices[d].height;
            const Fixed acrossTwo = mesh.vertices[b].height - mesh.vertices[c].height;
            const auto magnitude = [](Fixed f) { return f.raw < 0 ? -f.raw : f.raw; };
            // Wound so the face points UP, which it did not.
            //
            // The world is Z-up, and these came out with a geometric normal of
            // (0,0,-1): the ground's triangles faced the centre of the earth
            // while the vertex normals on them faced the sky, and the sea, built
            // a few files away, faced up. Nothing showed it because the whole
            // engine rasterised every triangle from both sides - so the one
            // thing that would have caught it was switched off everywhere.
            if (magnitude(acrossOne) <= magnitude(acrossTwo)) {
                mesh.indices.insert(mesh.indices.end(), {a, d, c, a, b, d});
            } else {
                mesh.indices.insert(mesh.indices.end(), {a, b, c, b, d, c});
            }
        }
    }
    return mesh;
}

std::vector<CliffSegment> extractCliffs(const HeightField& field, ChunkId chunk) {
    std::vector<CliffSegment> out;
    // Two rings of margin: whether the ground at the foot of a drop is standable
    // is asked of its own neighbours, so the cutter reads two steps beyond the
    // chunk it is cutting.
    const HeightGrid grid = gridFor(field, chunk, 0, 2);
    const std::int64_t baseX = static_cast<std::int64_t>(chunk.x) * kSamplesPerChunk;
    const std::int64_t baseY = static_cast<std::int64_t>(chunk.y) * kSamplesPerChunk;
    const auto height = [&](std::int64_t sx, std::int64_t sy) {
        return grid.at(static_cast<std::int32_t>(sx - baseX), static_cast<std::int32_t>(sy - baseY));
    };
    // Broken ground, from the grid: any of the four steps around a sample too
    // sharp to hold on to. One neighbour may be left out - the drop the ground
    // is standing under, which would otherwise make every bank its own overhang.
    const Fixed limit = cliffSlope() * Fixed::fromInt(kSampleMetres);
    const auto brokenExcept = [&](std::int64_t sx, std::int64_t sy, std::int64_t ignoreX,
                                  std::int64_t ignoreY) {
        const Fixed here = height(sx, sy);
        const std::int64_t offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (const auto& off : offsets) {
            const std::int64_t nx = sx + off[0], ny = sy + off[1];
            if (nx == ignoreX && ny == ignoreY) continue;
            const Fixed there = height(nx, ny);
            const std::int64_t drop = here.raw > there.raw ? here.raw - there.raw
                                                           : there.raw - here.raw;
            if (drop >= limit.raw) return true;
        }
        return false;
    };

    // A cliff is a real drop with standable ground under it.
    //
    // Two things have to be true at once. The edge itself has to break - so the
    // face hangs on the drop rather than a sample away from it - and the ground
    // at the bottom has to be ground, not more cliff. That second test is what
    // keeps a mountainside from being ten thousand overlapping faces: inside
    // broken country there is nothing to draw a face against and nothing to
    // walk on either, so the line comes out at the foot of the crags, which is
    // where a walker actually meets them. On a plain the same rule puts it on
    // the lip of a bank.
    const auto consider = [&](std::int64_t sx, std::int64_t sy, bool eastward) {
        const std::int64_t nx = eastward ? sx + 1 : sx;
        const std::int64_t ny = eastward ? sy : sy + 1;
        const Fixed here = height(sx, sy);
        const Fixed there = height(nx, ny);
        const std::int64_t drop = here.raw > there.raw ? here.raw - there.raw : there.raw - here.raw;
        if (drop < limit.raw) return;

        const bool fallsEast = here.raw > there.raw;
        const std::int64_t lowX = fallsEast ? nx : sx;
        const std::int64_t lowY = fallsEast ? ny : sy;
        const std::int64_t highX = fallsEast ? sx : nx;
        const std::int64_t highY = fallsEast ? sy : ny;
        if (brokenExcept(lowX, lowY, highX, highY)) return;

        // The line along the top runs across the edge that was crossed, through
        // its middle: the dual of the lattice, where segments meet end to end.
        CliffSegment seg;
        if (eastward) {
            seg.a = worldOf({2 * sx + 1, 2 * sy - 1});
            seg.b = worldOf({2 * sx + 1, 2 * sy + 1});
            seg.fallX = fallsEast ? core::kOne : -core::kOne;
            seg.fallY = core::kZero;
        } else {
            seg.a = worldOf({2 * sx - 1, 2 * sy + 1});
            seg.b = worldOf({2 * sx + 1, 2 * sy + 1});
            seg.fallX = core::kZero;
            seg.fallY = fallsEast ? core::kOne : -core::kOne;
        }
        seg.top = fallsEast ? here : there;
        seg.bottom = fallsEast ? there : here;
        // The broken ground is always the high side here: the low side was
        // tested for standing on, which is what let this segment be emitted.
        seg.brokenSideIsHigh = true;
        out.push_back(seg);
    };

    for (std::int32_t row = 0; row <= kSamplesPerChunk; ++row)
        for (std::int32_t col = 0; col <= kSamplesPerChunk; ++col) {
            const std::int64_t sx = baseX + col, sy = baseY + row;
            if (col < kSamplesPerChunk) consider(sx, sy, true);
            if (row < kSamplesPerChunk) consider(sx, sy, false);
        }
    return out;
}

std::vector<CliffLine> chainCliffs(std::vector<CliffSegment> segments) {
    const auto key = [](WorldPos p) { return endKey(p); };

    std::multimap<EndKey, std::size_t> byEnd;
    std::vector<bool> used(segments.size(), false);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        byEnd.emplace(key(segments[i].a), i);
        byEnd.emplace(key(segments[i].b), i);
    }

    std::vector<CliffLine> lines;
    for (std::size_t start = 0; start < segments.size(); ++start) {
        if (used[start]) continue;
        used[start] = true;
        CliffLine line;
        line.points = {segments[start].a, segments[start].b};
        line.top = {segments[start].top, segments[start].top};
        line.bottom = {segments[start].bottom, segments[start].bottom};

        // Walk forward from the tail, then backwards from the head. A junction -
        // three segments meeting, which happens where two banks run into each
        // other - ends the line rather than picking a branch: two lines meeting
        // at a point is the truth, and guessing a continuation there is how a
        // cliff ends up drawn round a corner it does not turn.
        for (int direction = 0; direction < 2; ++direction) {
            for (;;) {
                const WorldPos tip = direction == 0 ? line.points.back() : line.points.front();
                const auto range = byEnd.equal_range(key(tip));
                std::size_t next = segments.size();
                int found = 0;
                for (auto it = range.first; it != range.second; ++it) {
                    if (used[it->second]) continue;
                    ++found;
                    next = it->second;
                }
                if (found != 1) break;
                used[next] = true;
                const CliffSegment& seg = segments[next];
                const bool tipIsA = seg.a.x.raw == tip.x.raw && seg.a.y.raw == tip.y.raw;
                const WorldPos far = tipIsA ? seg.b : seg.a;
                if (direction == 0) {
                    line.points.push_back(far);
                    line.top.push_back(seg.top);
                    line.bottom.push_back(seg.bottom);
                } else {
                    line.points.insert(line.points.begin(), far);
                    line.top.insert(line.top.begin(), seg.top);
                    line.bottom.insert(line.bottom.begin(), seg.bottom);
                }
                if (line.points.size() > 2 &&
                    line.points.front().x.raw == line.points.back().x.raw &&
                    line.points.front().y.raw == line.points.back().y.raw) {
                    line.closed = true;
                    break;
                }
            }
            if (line.closed) break;
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

CliffStrip buildCliffStrip(const std::vector<CliffSegment>& segments) {
    CliffStrip strip;
    strip.vertices.reserve(segments.size() * 4);
    strip.indices.reserve(segments.size() * 6);

    MaterialWeights rock{};
    rock.add(Material::Rock, core::kOne);
    rock.normalise();

    for (const CliffSegment& seg : segments) {
        // The face hangs straight down from the top line. Its normal points the
        // way the ground falls and lies flat - which is what makes a cliff read
        // as a wall of rock under the light rather than as a stripe of dark
        // ground.
        const Normal facing{seg.fallX, seg.fallY, core::kZero};
        const std::uint32_t first = static_cast<std::uint32_t>(strip.vertices.size());
        // A face has no coarser self to walk towards - the level above does not
        // cut cliffs at all - so its morph target is where it already is.
        const auto vertex = [&](core::WorldPos position, Fixed height) {
            TerrainVertex v{};
            v.position = position;
            v.height = v.coarseHeight = height;
            v.normal = v.coarseNormal = facing;
            v.materials = rock;
            v.foliage.fill(core::kZero);
            return v;
        };
        strip.vertices.push_back(vertex(seg.a, seg.top));
        strip.vertices.push_back(vertex(seg.b, seg.top));
        strip.vertices.push_back(vertex(seg.a, seg.bottom));
        strip.vertices.push_back(vertex(seg.b, seg.bottom));
        strip.indices.insert(strip.indices.end(),
                             {first, first + 2, first + 3, first, first + 3, first + 1});
    }
    return strip;
}

} // namespace world
