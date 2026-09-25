#include "engine/core/geometry.hpp"

#include <array>

namespace core {

namespace {

using Wide = Fixed::Wide;

constexpr Wide orient(WorldPos a, WorldPos b, WorldPos c) {
    return Wide(b.x.raw - a.x.raw) * Wide(c.y.raw - a.y.raw) -
           Wide(b.y.raw - a.y.raw) * Wide(c.x.raw - a.x.raw);
}

constexpr bool between(Fixed a, Fixed b, Fixed x) {
    return x >= (a < b ? a : b) && x <= (a < b ? b : a);
}

constexpr bool onSegment(WorldPos a, WorldPos b, WorldPos p) {
    return orient(a, b, p) == 0 && between(a.x, b.x, p.x) && between(a.y, b.y, p.y);
}

bool segmentsIntersect(WorldPos a, WorldPos b, WorldPos c, WorldPos d) {
    const Wide o1 = orient(a, b, c);
    const Wide o2 = orient(a, b, d);
    const Wide o3 = orient(c, d, a);
    const Wide o4 = orient(c, d, b);
    if (o1 == 0 && onSegment(a, b, c)) return true;
    if (o2 == 0 && onSegment(a, b, d)) return true;
    if (o3 == 0 && onSegment(c, d, a)) return true;
    if (o4 == 0 && onSegment(c, d, b)) return true;
    return ((o1 < 0) != (o2 < 0)) && ((o3 < 0) != (o4 < 0));
}

} // namespace

std::vector<TilePos> tilesWithin(TilePos centre, std::int32_t radius) {
    std::vector<TilePos> out;
    if (radius < 0) return out;
    const std::int32_t span = 2 * radius + 1;
    out.reserve(static_cast<std::size_t>(span) * static_cast<std::size_t>(span));
    // Row by row, so iteration order is the storage order and stays stable.
    for (std::int32_t dy = -radius; dy <= radius; ++dy)
        for (std::int32_t dx = -radius; dx <= radius; ++dx)
            out.push_back({centre.x + dx, centre.y + dy});
    return out;
}

std::vector<TilePos> tileRing(TilePos centre, std::int32_t radius) {
    std::vector<TilePos> out;
    if (radius <= 0) { out.push_back(centre); return out; }
    out.reserve(static_cast<std::size_t>(8) * static_cast<std::size_t>(radius));

    // Walk the four sides from the north-west corner, clockwise, each side
    // taking its leading corner and stopping short of the next one.
    TilePos p{centre.x - radius, centre.y - radius};
    const std::array<TilePos, 4> along{{{+1, 0}, {0, +1}, {-1, 0}, {0, -1}}};
    for (int side = 0; side < 4; ++side)
        for (std::int32_t i = 0; i < 2 * radius; ++i) {
            out.push_back(p);
            p.x += along[static_cast<std::size_t>(side)].x;
            p.y += along[static_cast<std::size_t>(side)].y;
        }
    return out;
}

std::vector<WorldPos> discOutline(WorldPos centre, Fixed radius) {
    static constexpr std::array<std::pair<std::int32_t, std::int32_t>, 16> kUnit{{
            {1024, 0}, {946, 392}, {724, 724}, {392, 946},
            {0, 1024}, {-392, 946}, {-724, 724}, {-946, 392},
            {-1024, 0}, {-946, -392}, {-724, -724}, {-392, -946},
            {0, -1024}, {392, -946}, {724, -724}, {946, -392},
    }};
    std::vector<WorldPos> out;
    out.reserve(kUnit.size());
    for (const auto& [ux, uy] : kUnit)
        out.push_back({centre.x + radius * ux / 1024, centre.y + radius * uy / 1024});
    return out;
}

bool pointInPolygon(const std::vector<WorldPos>& polygon, WorldPos p) {
    if (polygon.size() < 3) return false;
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        const WorldPos a = polygon[j];
        const WorldPos b = polygon[i];
        if (onSegment(a, b, p)) return true;
        const bool crosses = (a.y.raw > p.y.raw) != (b.y.raw > p.y.raw);
        if (!crosses) continue;
        const Wide lhs = Wide(p.x.raw - a.x.raw) * Wide(b.y.raw - a.y.raw);
        const Wide rhs = Wide(b.x.raw - a.x.raw) * Wide(p.y.raw - a.y.raw);
        const bool hit = (b.y.raw > a.y.raw) ? (lhs < rhs) : (lhs > rhs);
        if (hit) inside = !inside;
    }
    return inside;
}

bool segmentIntersectsRect(WorldPos a, WorldPos b, const WorldRect& rect) {
    if (!rect.valid()) return false;
    if (rect.contains(a) || rect.contains(b)) return true;
    const WorldPos bl{rect.min.x, rect.min.y};
    const WorldPos br{rect.max.x, rect.min.y};
    const WorldPos tr{rect.max.x, rect.max.y};
    const WorldPos tl{rect.min.x, rect.max.y};
    return segmentsIntersect(a, b, bl, br) || segmentsIntersect(a, b, br, tr) ||
           segmentsIntersect(a, b, tr, tl) || segmentsIntersect(a, b, tl, bl);
}

bool polygonIntersectsRect(const std::vector<WorldPos>& polygon, const WorldRect& rect) {
    if (polygon.size() < 3 || !rect.valid()) return false;
    for (WorldPos p : polygon)
        if (rect.contains(p)) return true;

    const std::array<WorldPos, 4> corners{{
            {rect.min.x, rect.min.y}, {rect.max.x, rect.min.y},
            {rect.max.x, rect.max.y}, {rect.min.x, rect.max.y},
    }};
    for (WorldPos p : corners)
        if (pointInPolygon(polygon, p)) return true;

    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
        if (segmentIntersectsRect(polygon[j], polygon[i], rect)) return true;
    return false;
}

} // namespace core
