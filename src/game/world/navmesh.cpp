#include "game/world/navmesh.hpp"

#include <algorithm>
#include <queue>

namespace world {
namespace {

using core::Fixed;
using core::WorldPos;

// Navigation is built on the same lattice the ground is sampled on. Not because
// walking is a grid - the polygons below are as big as the ground allows and a
// path runs corner to corner through them - but because a cell of that lattice
// is the smallest piece of ground the field can answer a question about, and
// there is nothing to be gained by asking finer.
constexpr std::int32_t kCellMetres = kSampleMetres;
constexpr std::int32_t kCellsPerChunk = kChunkMetres / kCellMetres;

Fixed flatDistance(WorldPos a, WorldPos b) { return core::hypot(b.x - a.x, b.y - a.y); }

// Twice the signed area of a polygon, which is what says which way round it is
// wound and whether it is a real polygon at all. In raw fixed point rather than
// metres: the numbers are only ever compared with each other and with nought.
std::int64_t turnOf(const std::vector<WorldPos>& points) {
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const WorldPos& a = points[i];
        const WorldPos& b = points[(i + 1) % points.size()];
        // Shifted before multiplying: a chunk is sixty-four metres and Q32.32
        // raws are of the order 2^32, so the product of two of them overflows.
        // Metres are enough here - this decides a winding, not a position.
        sum += (a.x.toInt() * b.y.toInt()) - (b.x.toInt() * a.y.toInt());
    }
    return sum;
}

// Which side of the line from a to b the point c falls on. Positive one way,
// negative the other. The funnel is three of these.
Fixed side(WorldPos a, WorldPos b, WorldPos c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Strictly overlapping, not merely touching. A wall that ends exactly where a
// cell begins does not stand in that cell - and getting this wrong closes the
// gateway a wall was built around, because both halves of the wall touch the
// gap from either side and between them block every cell of it.
bool overlapsFlat(WorldPos aMin, WorldPos aMax, WorldPos bMin, WorldPos bMax) {
    return bMin.x.raw < aMax.x.raw && bMax.x.raw > aMin.x.raw && bMin.y.raw < aMax.y.raw &&
           bMax.y.raw > aMin.y.raw;
}

} // namespace

Fixed travelCost(Travel t) {
    switch (t) {
        case Travel::Walk: return core::kOne;
        case Travel::Scramble: return Fixed::fromInt(3);
        // A ford is slow and it is safe, and a cart uses it: dearer than a
        // scramble, cheaper than a face. Five times the open ground, so a body
        // will go a hundred metres round to find a crossing and not five
        // hundred - which is what a crossing is.
        case Travel::Ford: return Fixed::fromInt(5);
        case Travel::Climb: return Fixed::fromInt(8);
        // Swimming is a last resort and the numbers should say so. It is not
        // impassable, because a body does get across; it is thirty times the
        // cost of walking, so a path takes it only when there is no other way.
        case Travel::Swim: return Fixed::fromInt(30);
        case Travel::None: break;
    }
    return Fixed::fromInt(1 << 16);
}

bool NavPoly::contains(core::WorldPos p) const {
    if (p.x.raw < min.x.raw || p.x.raw > max.x.raw || p.y.raw < min.y.raw || p.y.raw > max.y.raw)
        return false;
    if (points.size() < 3) return true;
    // On the inner side of every edge, or on it. Which side is inner is the way
    // the polygon is wound, and every polygon in the mesh is wound the same way,
    // so it is read off the first edge pair rather than assumed.
    bool anyLeft = false, anyRight = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const core::WorldPos& a = points[i];
        const core::WorldPos& b = points[(i + 1) % points.size()];
        const core::Fixed which = side(a, b, p);
        if (which.raw > 0) anyRight = true;
        if (which.raw < 0) anyLeft = true;
    }
    return !(anyLeft && anyRight);
}

core::WorldPos NavPoly::centre() const {
    if (points.empty())
        return {(min.x + max.x) / core::Fixed::fromInt(2),
                (min.y + max.y) / core::Fixed::fromInt(2)};
    // The average of the corners. For a convex polygon that is inside it, which
    // is all anything here needs of it: it is where a portal is measured from
    // and where a corridor is strung through.
    core::Fixed x = core::kZero, y = core::kZero;
    for (const core::WorldPos& p : points) {
        x += p.x;
        y += p.y;
    }
    const core::Fixed count = core::Fixed::fromInt(static_cast<std::int64_t>(points.size()));
    return {x / count, y / count};
}

void NavMesh::build(const streaming::PageGround& ground_, ChunkId chunk,
                    const std::vector<Obstacle>& obstacles) {
    drop(chunk);

    const std::int64_t baseX = static_cast<std::int64_t>(chunk.x) * kCellsPerChunk;
    const std::int64_t baseY = static_cast<std::int64_t>(chunk.y) * kCellsPerChunk;

    // The page has to hold the whole chunk, its far corner included. A chunk
    // is sixty-four metres and a page five hundred and twelve, so one always
    // does - but a caller handed the wrong page would otherwise get a mesh of
    // the ground outside it, which is worse than none.
    const Fixed edge = Fixed::fromInt(kCellsPerChunk * kCellMetres);
    const WorldPos northWest{Fixed::fromInt(baseX * kCellMetres),
                             Fixed::fromInt(baseY * kCellMetres)};
    if (!ground_.covers(northWest) ||
        !ground_.covers({northWest.x + edge, northWest.y + edge}))
        return;

    // How each cell of this chunk is got over. The field answers for the
    // ground; the obstacles are what has been built on it.
    //
    // Not a yes or no: a mountain country is mostly ground that is crossed
    // rather than walked, and a navmesh that only knows level going would
    // answer "there is no way over this range" when what is true is "there is
    // a way over this range and it is four hours of scrambling" (D117).
    std::vector<Travel> ground(static_cast<std::size_t>(kCellsPerChunk) * kCellsPerChunk,
                               Travel::None);
    std::vector<Fixed> height(ground.size(), core::kZero);

    // Every corner of the grid, once.
    //
    // A cell is as crossable as its worst corner, not as its middle. Asked at
    // the centre alone, a cell with a stream two metres off centre is walkable
    // ground with a stream in it, and the navmesh says walk. It never showed
    // while the worlds were mostly land and the flattest cell around had no
    // water in it; on a world seven tenths sea the flattest cell is beside a
    // lake, and four of the points a test drops inside the polygons came out
    // under water. A body cannot walk on water.
    //
    // Asked per cell that is four extra queries each; asked per corner it is
    // one, because every corner belongs to four cells. Measured over a hundred
    // chunks: 7.0 ms a chunk for the centre alone, 24.8 ms asking every cell
    // about its own four corners, 11.8 ms asking each corner once. A navmesh is
    // built when a chunk is loaded rather than every frame, so eleven
    // milliseconds is affordable and twenty-five was worth not paying.
    constexpr std::int32_t kCorners = kCellsPerChunk + 1;
    std::vector<Travel> corner(static_cast<std::size_t>(kCorners) * kCorners, Travel::None);
    for (std::int32_t row = 0; row < kCorners; ++row)
        for (std::int32_t col = 0; col < kCorners; ++col)
            corner[static_cast<std::size_t>(row) * kCorners + col] = ground_.travelAt(
                    {Fixed::fromInt((baseX + col) * kCellMetres),
                     Fixed::fromInt((baseY + row) * kCellMetres)});

    for (std::int32_t row = 0; row < kCellsPerChunk; ++row) {
        for (std::int32_t col = 0; col < kCellsPerChunk; ++col) {
            const Fixed x = Fixed::fromInt((baseX + col) * kCellMetres);
            const Fixed y = Fixed::fromInt((baseY + row) * kCellMetres);
            const WorldPos middle{x + Fixed::ratio(kCellMetres, 2), y + Fixed::ratio(kCellMetres, 2)};
            const std::size_t at = static_cast<std::size_t>(row) * kCellsPerChunk + col;
            height[at] = ground_.heightAt(middle);
            Travel travel = ground_.travelAt(middle);
            int impassable = 0;
            for (int which = 0; which < 4 && travel != Travel::None; ++which) {
                const Travel there = corner[static_cast<std::size_t>(row + which / 2) * kCorners +
                                            col + which % 2];
                if (there == Travel::None) { ++impassable; continue; }
                if (static_cast<int>(there) > static_cast<int>(travel)) travel = there;
            }
            // Half of it, not a corner of it.
            //
            // Thrown out on any impassable corner, the mesh loses a line of
            // cells along every bank - measured, half a percent of the ground,
            // and every one of them because a corner was under water rather
            // than too steep. Half a percent sounds like nothing and is not: the
            // lines are continuous, so a ford or a strip between a river and a
            // crag closes, and three of the pathfinding tests stopped finding a
            // way at all. A cell with one wet corner is a bank - mostly dry
            // ground, walkable, and the thing riverside paths are made of; two
            // is a cell that is half in the water.
            if (impassable >= 2) travel = Travel::None;
            if (travel == Travel::None) continue;
            bool blocked = false;
            for (const Obstacle& o : obstacles)
                if (overlapsFlat({x, y},
                                 {x + Fixed::fromInt(kCellMetres), y + Fixed::fromInt(kCellMetres)},
                                 o.min, o.max)) {
                    blocked = true;
                    break;
                }
            if (!blocked) ground[at] = travel;
        }
    }

    // Merge the open ground into as few rectangles as it will make.
    //
    // Greedy: take the longest run of open cells in a row, then push it down as
    // far as every row below matches it. On a field that is one polygon; along
    // a bank it is a staircase of thin ones, which is exactly where a walker
    // needs the detail. The result is convex pieces of wildly different sizes,
    // which is the point - a navmesh that came out uniform would just be the
    // tile grid again wearing a different name.
    std::vector<Travel> taken = ground;
    for (std::int32_t row = 0; row < kCellsPerChunk; ++row) {
        for (std::int32_t col = 0; col < kCellsPerChunk;) {
            const std::size_t at = static_cast<std::size_t>(row) * kCellsPerChunk + col;
            if (taken[at] == Travel::None) {
                ++col;
                continue;
            }
            // Only ground of the same kind merges: the edge of a scree slope is
            // a boundary, so a path can decide to go round it rather than being
            // told it is all the same hillside.
            const Travel kind = taken[at];
            std::int32_t wide = 0;
            while (col + wide < kCellsPerChunk &&
                   taken[static_cast<std::size_t>(row) * kCellsPerChunk + col + wide] == kind)
                ++wide;
            std::int32_t tall = 1;
            for (std::int32_t next = row + 1; next < kCellsPerChunk; ++next) {
                bool wholeRow = true;
                for (std::int32_t i = 0; i < wide && wholeRow; ++i)
                    wholeRow = taken[static_cast<std::size_t>(next) * kCellsPerChunk + col + i] == kind;
                if (!wholeRow) break;
                ++tall;
            }
            for (std::int32_t r = row; r < row + tall; ++r)
                for (std::int32_t i = 0; i < wide; ++i)
                    taken[static_cast<std::size_t>(r) * kCellsPerChunk + col + i] = Travel::None;

            NavPoly poly;
            poly.id = spare_.empty() ? static_cast<std::uint32_t>(polys_.size()) : spare_.back();
            if (!spare_.empty()) spare_.pop_back();
            poly.chunk = chunk;
            poly.min = {Fixed::fromInt((baseX + col) * kCellMetres),
                        Fixed::fromInt((baseY + row) * kCellMetres)};
            poly.max = {Fixed::fromInt((baseX + col + wide) * kCellMetres),
                        Fixed::fromInt((baseY + row + tall) * kCellMetres)};
            // The same rectangle as four corners, wound the one way the whole
            // mesh is wound. Still a rectangle for now: this step is only about
            // the mesh being able to hold any convex shape at all.
            poly.points = {poly.min,
                           {poly.max.x, poly.min.y},
                           poly.max,
                           {poly.min.x, poly.max.y}};
            if (turnOf(poly.points) < 0)
                std::reverse(poly.points.begin(), poly.points.end());
            poly.height = height[static_cast<std::size_t>(row + tall / 2) * kCellsPerChunk + col +
                                 wide / 2];
            poly.travel = kind;
            if (poly.id >= polys_.size()) polys_.resize(poly.id + 1);
            polys_[poly.id] = std::move(poly);
            byChunk_[chunk].push_back(polys_[poly.id].id);
            col += wide;
        }
    }

    relink(chunk);
    rebuildRegions();
}

void NavMesh::drop(ChunkId chunk) {
    const auto it = byChunk_.find(chunk);
    if (it == byChunk_.end()) return;
    for (std::uint32_t id : it->second) {
        // Anything pointing at this polygon has to stop pointing at it. A
        // portal to a polygon that no longer exists is a path that walks into a
        // wall - and it is a neighbouring chunk that holds it, which is why
        // dropping a chunk is not a local operation.
        for (const Portal& gate : polys_[id].links) {
            std::vector<Portal>& theirs = polys_[gate.to].links;
            theirs.erase(std::remove_if(theirs.begin(), theirs.end(),
                                        [id](const Portal& p) { return p.to == id; }),
                         theirs.end());
        }
        polys_[id] = NavPoly{};
        spare_.push_back(id);
    }
    byChunk_.erase(it);
    rebuildRegions();
}

void NavMesh::relink(ChunkId chunk) {
    linkPair(chunk, chunk);
    for (std::int32_t dir = 0; dir < 4; ++dir) {
        const ChunkId neighbour{chunk.x + (dir == 0 ? 1 : dir == 1 ? -1 : 0),
                                chunk.y + (dir == 2 ? 1 : dir == 3 ? -1 : 0)};
        if (byChunk_.count(neighbour)) linkPair(chunk, neighbour);
    }
}

void NavMesh::linkPair(ChunkId a, ChunkId b) {
    const auto left = byChunk_.find(a);
    const auto right = byChunk_.find(b);
    if (left == byChunk_.end() || right == byChunk_.end()) return;

    for (std::uint32_t oneId : left->second) {
        NavPoly& one = polys_[oneId];
        for (std::uint32_t twoId : right->second) {
            if (oneId == twoId) continue;
            NavPoly& two = polys_[twoId];
            const bool already =
                    std::any_of(one.links.begin(), one.links.end(),
                                [twoId](const Portal& p) { return p.to == twoId; });
            if (already) continue;

            // Two rectangles are neighbours when they touch along a side and
            // the touching part is longer than nothing. The same test serves
            // inside a chunk and across a border: a polygon does not know or
            // care which chunk the one it touches was built by.
            WorldPos gateLeft{}, gateRight{};
            bool touching = false;
            if (one.max.x.raw == two.min.x.raw || two.max.x.raw == one.min.x.raw) {
                const Fixed x = one.max.x.raw == two.min.x.raw ? one.max.x : one.min.x;
                const Fixed lowY = Fixed::fromRaw(std::max(one.min.y.raw, two.min.y.raw));
                const Fixed highY = Fixed::fromRaw(std::min(one.max.y.raw, two.max.y.raw));
                if (highY.raw > lowY.raw) {
                    gateLeft = {x, lowY};
                    gateRight = {x, highY};
                    touching = true;
                }
            } else if (one.max.y.raw == two.min.y.raw || two.max.y.raw == one.min.y.raw) {
                const Fixed y = one.max.y.raw == two.min.y.raw ? one.max.y : one.min.y;
                const Fixed lowX = Fixed::fromRaw(std::max(one.min.x.raw, two.min.x.raw));
                const Fixed highX = Fixed::fromRaw(std::min(one.max.x.raw, two.max.x.raw));
                if (highX.raw > lowX.raw) {
                    gateLeft = {lowX, y};
                    gateRight = {highX, y};
                    touching = true;
                }
            }
            if (!touching) continue;

            // Each side keeps the gate with its own left and right, which is
            // what the funnel reads. Ordered by which side of the line between
            // the two centres the end falls on, so "left" means left to a body
            // walking that way.
            const auto oriented = [&](const NavPoly& from, const NavPoly& to) {
                const Fixed which = side(from.centre(), to.centre(), gateLeft);
                return which.raw >= 0 ? std::pair<WorldPos, WorldPos>{gateRight, gateLeft}
                                      : std::pair<WorldPos, WorldPos>{gateLeft, gateRight};
            };
            const auto forward = oriented(one, two);
            const auto backward = oriented(two, one);
            one.links.push_back({twoId, forward.first, forward.second});
            two.links.push_back({oneId, backward.first, backward.second});
        }
    }
}

void NavMesh::rebuildRegions() {
    regions_.clear();
    for (NavPoly& p : polys_) p.region = 0;

    // A region is one connected part of one chunk. Chunk-sized, because that is
    // what makes the coarse graph coarse: a walk across the world is planned on
    // a few dozen of these before a single polygon is opened.
    std::uint32_t next = 1;
    for (const auto& [chunk, ids] : byChunk_) {
        for (std::uint32_t start : ids) {
            if (polys_[start].region != 0) continue;
            const std::uint32_t id = next++;
            regions_.push_back({id, chunk, {}});
            std::vector<std::uint32_t> stack{start};
            polys_[start].region = id;
            while (!stack.empty()) {
                const std::uint32_t at = stack.back();
                stack.pop_back();
                for (const Portal& gate : polys_[at].links) {
                    NavPoly& other = polys_[gate.to];
                    if (other.chunk != chunk || other.region != 0) continue;
                    other.region = id;
                    stack.push_back(gate.to);
                }
            }
        }
    }

    // And which regions touch which, which is the coarse graph itself.
    for (const NavPoly& p : polys_) {
        if (p.region == 0) continue;
        for (const Portal& gate : p.links) {
            const std::uint32_t other = polys_[gate.to].region;
            if (other == 0 || other == p.region) continue;
            std::vector<std::uint32_t>& mine = regions_[p.region - 1].neighbours;
            if (std::find(mine.begin(), mine.end(), other) == mine.end()) mine.push_back(other);
        }
    }
}

const NavPoly* NavMesh::poly(std::uint32_t id) const {
    if (id >= polys_.size()) return nullptr;
    const NavPoly& p = polys_[id];
    return p.max.x.raw > p.min.x.raw ? &p : nullptr;
}

const NavPoly* NavMesh::polyAt(WorldPos p) const {
    // The chunk it stands in, and the ring around it: a point on a border
    // belongs to a polygon that may have been built by either side.
    for (ChunkId c : withNeighbours(chunkOf(p))) {
        const auto it = byChunk_.find(c);
        if (it == byChunk_.end()) continue;
        for (std::uint32_t id : it->second)
            if (polys_[id].contains(p)) return &polys_[id];
    }
    return nullptr;
}

std::optional<WorldPos> NavMesh::nearestWalkable(WorldPos p, Fixed within) const {
    if (polyAt(p) != nullptr) return p;
    const NavPoly* best = nullptr;
    Fixed bestDistance = within;
    WorldPos bestPoint{};
    for (ChunkId c : withNeighbours(chunkOf(p))) {
        const auto it = byChunk_.find(c);
        if (it == byChunk_.end()) continue;
        for (std::uint32_t id : it->second) {
            const NavPoly& poly = polys_[id];
            // The closest point of the rectangle to p, which is p clamped into
            // it - the nearest standable ground in that polygon, rather than
            // its middle.
            const WorldPos clamped{
                    Fixed::fromRaw(std::clamp(p.x.raw, poly.min.x.raw, poly.max.x.raw)),
                    Fixed::fromRaw(std::clamp(p.y.raw, poly.min.y.raw, poly.max.y.raw))};
            const Fixed distance = flatDistance(p, clamped);
            if (distance.raw < bestDistance.raw) {
                bestDistance = distance;
                best = &poly;
                bestPoint = clamped;
            }
        }
    }
    if (!best) return std::nullopt;
    return bestPoint;
}

std::vector<std::uint32_t> NavMesh::regionCorridor(std::uint32_t from, std::uint32_t to) const {
    if (from == 0 || to == 0) return {};
    if (from == to) return {from};
    // Breadth first over the coarse graph. There are a handful of regions to a
    // chunk, so this is cheap even over a country's worth of them, and the
    // corridor it returns is what keeps the polygon search from wandering.
    std::unordered_map<std::uint32_t, std::uint32_t> cameFrom;
    std::vector<std::uint32_t> queue{from};
    cameFrom[from] = from;
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::uint32_t at = queue[head];
        if (at == to) break;
        for (std::uint32_t next : regions_[at - 1].neighbours) {
            if (cameFrom.count(next)) continue;
            cameFrom[next] = at;
            queue.push_back(next);
        }
    }
    if (!cameFrom.count(to)) return {};
    std::vector<std::uint32_t> path;
    for (std::uint32_t at = to;; at = cameFrom[at]) {
        path.push_back(at);
        if (at == from) break;
    }
    std::reverse(path.begin(), path.end());
    return path;
}

std::optional<Path> NavMesh::findPath(WorldPos from, WorldPos to, Travel hardest) const {
    const NavPoly* start = polyAt(from);
    const NavPoly* goal = polyAt(to);
    if (!start || !goal) return std::nullopt;
    // What this traveller will not set foot on. Compared by cost rather than by
    // the order of the enum: the enum is a list of kinds of ground and the cost
    // is what says which is worse, and the two need not agree.
    const Fixed limit = travelCost(hardest);
    const auto refused = [&](const NavPoly& poly) {
        return travelCost(poly.travel).raw > limit.raw;
    };
    if (refused(*start) || refused(*goal)) return std::nullopt;
    if (start->id == goal->id) {
        Path direct;
        direct.points = {from, to};
        direct.corridor = {start->id};
        direct.length = flatDistance(from, to);
        direct.cost = direct.length * travelCost(start->travel);
        direct.worst = start->travel;
        return direct;
    }

    // The coarse pass. Long walks are planned on regions first and the polygon
    // search is then held inside that corridor; short ones skip it, because
    // opening every polygon within a chunk or two is cheaper than planning
    // twice.
    std::vector<std::uint32_t> allowed;
    const bool faraway = flatDistance(from, to).raw > Fixed::fromInt(kChunkMetres * 3).raw;
    if (faraway && start->region != goal->region) {
        allowed = regionCorridor(start->region, goal->region);
        if (allowed.empty()) return std::nullopt;
    }
    const auto insideCorridor = [&](std::uint32_t region) {
        if (allowed.empty()) return true;
        return std::find(allowed.begin(), allowed.end(), region) != allowed.end();
    };

    // A* over the polygons. The cost is the distance between the middles of the
    // gates walked through - not between polygon centres, which is the mistake
    // that makes a path prefer a chain of small polygons over one big one.
    struct Open {
        Fixed estimate;
        std::uint32_t poly;
        bool operator>(const Open& other) const { return estimate.raw > other.estimate.raw; }
    };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> queue;
    std::unordered_map<std::uint32_t, Fixed> best;
    std::unordered_map<std::uint32_t, std::uint32_t> cameFrom;
    std::unordered_map<std::uint32_t, WorldPos> arrivedAt;

    best[start->id] = core::kZero;
    arrivedAt[start->id] = from;
    queue.push({flatDistance(from, to), start->id});

    bool reached = false;
    while (!queue.empty()) {
        const Open node = queue.top();
        queue.pop();
        if (node.poly == goal->id) {
            reached = true;
            break;
        }
        const Fixed sofar = best[node.poly];
        if (node.estimate.raw > (sofar + flatDistance(arrivedAt[node.poly], to)).raw) continue;

        for (const Portal& gate : polys_[node.poly].links) {
            const NavPoly& next = polys_[gate.to];
            if (!insideCorridor(next.region)) continue;
            if (refused(next)) continue;
            const WorldPos through{(gate.left.x + gate.right.x) / Fixed::fromInt(2),
                                   (gate.left.y + gate.right.y) / Fixed::fromInt(2)};
            // Paid for by the ground it is crossing, not by the distance alone:
            // three metres of scree costs what nine metres of path costs, so
            // going round is worth it up to that much further.
            const Fixed step = flatDistance(arrivedAt[node.poly], through);
            const Fixed cost = sofar + step * travelCost(polys_[node.poly].travel);
            const auto known = best.find(gate.to);
            if (known != best.end() && known->second.raw <= cost.raw) continue;
            best[gate.to] = cost;
            cameFrom[gate.to] = node.poly;
            arrivedAt[gate.to] = through;
            queue.push({cost + flatDistance(through, to), gate.to});
        }
    }
    if (!reached) return std::nullopt;

    // The corridor, and the gates between its polygons.
    std::vector<std::uint32_t> corridor;
    for (std::uint32_t at = goal->id;; at = cameFrom[at]) {
        corridor.push_back(at);
        if (at == start->id) break;
    }
    std::reverse(corridor.begin(), corridor.end());

    std::vector<Portal> gates;
    for (std::size_t i = 0; i + 1 < corridor.size(); ++i) {
        for (const Portal& gate : polys_[corridor[i]].links)
            if (gate.to == corridor[i + 1]) {
                gates.push_back(gate);
                break;
            }
    }

    Path path;
    path.corridor = std::move(corridor);
    for (std::uint32_t id : path.corridor)
        if (travelCost(polys_[id].travel).raw > travelCost(path.worst).raw)
            path.worst = polys_[id].travel;
    path.points = pullString(gates, from, to);

    // And then straightened.
    //
    // The funnel gives the shortest line inside the corridor it was handed, and
    // the corridor is only as good as the graph search that chose it. Over open
    // country the polygons are enormous - a chunk of level ground is one
    // polygon - so a search whose cost is the distance between gate middles
    // finds an L round two sides of a square exactly as cheap as the diagonal,
    // takes whichever it happened to pop first, and the funnel then faithfully
    // pulls a string through the L. Which looks like what it is: a body walking
    // east and then south across an empty field.
    //
    // So the corners are dropped again wherever the ground allows walking
    // straight between what is left. The proper cure is an any-angle search
    // over the polygon graph; this costs a few hundred lookups on a path of
    // several hundred metres and gets the same line.
    if (path.points.size() > 2) {
        std::vector<WorldPos> straight{path.points.front()};
        std::size_t at = 0;
        while (at + 1 < path.points.size()) {
            std::size_t furthest = at + 1;
            for (std::size_t candidate = path.points.size() - 1; candidate > at + 1; --candidate) {
                const std::optional<Fixed> direct =
                        costBetween(path.points[at], path.points[candidate]);
                if (!direct) continue;
                // Only when it is not worse. A straight line across a scree
                // slope is shorter than the way round it and costs more, and
                // taking it would undo the choice the search just made.
                Fixed round = core::kZero;
                for (std::size_t i = at; i < candidate; ++i) {
                    const std::optional<Fixed> leg =
                            costBetween(path.points[i], path.points[i + 1]);
                    if (!leg) { round = Fixed::fromInt(1 << 20); break; }
                    round += *leg;
                }
                if (direct->raw > round.raw) continue;
                furthest = candidate;
                break;
            }
            straight.push_back(path.points[furthest]);
            at = furthest;
        }
        path.points = std::move(straight);
    }

    path.length = core::kZero;
    path.cost = core::kZero;
    for (std::size_t i = 0; i + 1 < path.points.size(); ++i) {
        path.length += flatDistance(path.points[i], path.points[i + 1]);
        path.cost += costBetween(path.points[i], path.points[i + 1])
                             .value_or(flatDistance(path.points[i], path.points[i + 1]));
    }
    return path;
}

bool NavMesh::clearBetween(WorldPos from, WorldPos to) const {
    return costBetween(from, to).has_value();
}

std::optional<Fixed> NavMesh::costBetween(WorldPos from, WorldPos to) const {
    const Fixed span = flatDistance(from, to);
    // A step of a metre: four times finer than the smallest polygon there can
    // be, so nothing narrower than the ground itself can be stepped over.
    const std::int64_t steps = std::max<std::int64_t>(2, span.toInt());
    Fixed cost = core::kZero;
    const Fixed each = span / Fixed::fromInt(steps);
    for (std::int64_t i = 0; i <= steps; ++i) {
        const Fixed t = Fixed::ratio(i, steps);
        const WorldPos on{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
        const NavPoly* poly = polyAt(on);
        if (poly == nullptr) return std::nullopt;
        if (i > 0) cost += each * travelCost(poly->travel);
    }
    return cost;
}

std::vector<WorldPos> NavMesh::pullString(const std::vector<Portal>& gates, WorldPos from,
                                          WorldPos to) {
    // The simple funnel. Walk the gates keeping a wedge from the last corner;
    // whenever one side would cross the other, the crossing side's end was a
    // corner the string has to bend round, so it is taken and the wedge starts
    // again from there.
    //
    // What this buys: a path that runs from where the body is to where it is
    // going in straight lines that graze the corners, instead of a tour of the
    // middles of the polygons it happens to pass through.
    std::vector<WorldPos> out{from};
    if (gates.empty()) {
        out.push_back(to);
        return out;
    }

    WorldPos apex = from;
    WorldPos left = gates[0].left, right = gates[0].right;
    std::size_t leftAt = 0, rightAt = 0;

    for (std::size_t i = 1; i <= gates.size(); ++i) {
        const WorldPos nextLeft = i < gates.size() ? gates[i].left : to;
        const WorldPos nextRight = i < gates.size() ? gates[i].right : to;

        // Tighten the right side if it does not cross the left.
        if (side(apex, right, nextRight).raw <= 0) {
            if (apex.x.raw == right.x.raw && apex.y.raw == right.y.raw) {
                right = nextRight;
                rightAt = i;
            } else if (side(apex, left, nextRight).raw > 0) {
                right = nextRight;
                rightAt = i;
            } else {
                // The right side crossed the left: the left end was a corner.
                out.push_back(left);
                apex = left;
                i = leftAt;
                left = right = apex;
                leftAt = rightAt = i;
                continue;
            }
        }
        // And the left side, the same way round.
        if (side(apex, left, nextLeft).raw >= 0) {
            if (apex.x.raw == left.x.raw && apex.y.raw == left.y.raw) {
                left = nextLeft;
                leftAt = i;
            } else if (side(apex, right, nextLeft).raw < 0) {
                left = nextLeft;
                leftAt = i;
            } else {
                out.push_back(right);
                apex = right;
                i = rightAt;
                left = right = apex;
                leftAt = rightAt = i;
                continue;
            }
        }
    }

    if (out.empty() || out.back().x.raw != to.x.raw || out.back().y.raw != to.y.raw)
        out.push_back(to);
    return out;
}

} // namespace world
