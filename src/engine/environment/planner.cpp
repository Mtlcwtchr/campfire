#include "engine/environment/planner.hpp"

#include <algorithm>
#include <cmath>

#include "engine/environment/random.hpp"

namespace engine::environment {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint64_t nameHash(const std::string& s) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

bool needsSpline(const FeatureRecipe& r) {
    for (const auto& op : r.terrain)
        if (op.kind == TerrainOpKind::CarveProfile || op.kind == TerrainOpKind::Step) return true;
    for (const auto& m : r.meshes)
        if (m.attach == MeshAttach::Spline || m.attach == MeshAttach::Edge) return true;
    for (const auto& s : r.scatter)
        if (s.primitive == ScatterPrimitive::AlongChannel || s.primitive == ScatterPrimitive::Edge) return true;
    return false;
}

std::int64_t floorDiv(double v, double cell) { return std::int64_t(std::floor(v / cell)); }

} // namespace

core::Fixed quantised(double metres) {
    return core::Fixed::fromRaw(core::Fixed::Raw(std::llround(metres * 256.0)) << (core::Fixed::kFracBits - 8));
}

FeaturePlanner::FeaturePlanner(PlannerContext context) : context_(std::move(context)) {
    const auto& recipes = context_.catalogue->recipes();
    order_.assign(recipes.size(), 0);
    std::int32_t next = 0;
    for (int s = int(FeatureScale::Macro); s >= 0; --s)
        for (auto r : context_.catalogue->ofScale(FeatureScale(s))) order_[r] = next++;
}

std::int32_t FeaturePlanner::order(std::uint32_t recipe) const { return order_[recipe]; }

void FeaturePlanner::clear() {
    candidates_.clear();
    cells_.clear();
    drainage_.clear();
}

bool FeaturePlanner::passes(const FeatureRecipe& r, double x, double y) const {
    const auto& p = r.placement;
    // The zone first: it is read off a cached page, while the fields at a
    // point cost a dozen height and climate queries, and most candidates of
    // a zone-bound recipe fall outside its zones.
    if (!p.zoneIds.empty() || !p.excludeZoneIds.empty() || p.source == PlacementSource::Edge) {
        if (!context_.zones) return p.zoneIds.empty() && p.source != PlacementSource::Edge;
        const auto zone = context_.zones->at(x, y);
        if (!p.zoneIds.empty()) {
            float w = 0;
            for (auto z : p.zoneIds) w += zone.weights.of(z);
            if (w < p.minZoneWeight) return false;
        }
        for (auto z : p.excludeZoneIds)
            if (zone.weights.of(z) > 0.5f) return false;
        if (p.source == PlacementSource::Edge && !(zone.weights.weight[0] < 0.75f && zone.weights.weight[1] > 0.25f))
            return false;
    }
    const bool needsFields = p.avoidWater || !p.fields.empty() || p.source == PlacementSource::Ridge ||
                             p.source == PlacementSource::CliffFoot;
    if (!needsFields || !context_.fields) return true;
    FieldSample f;
    context_.fields->sample(x, y, f);
    if (p.avoidWater && (f.get(field::Water, 0) > 0.3f || f.get(field::DistWater, 1e9f) < 4.0f)) return false;
    for (const auto& range : p.fields) {
        if (!f.has(range.field)) return false;
        const float v = f[range.field];
        if (v < range.min || v > range.max) return false;
    }
    switch (p.source) {
        case PlacementSource::Ridge:
            if (f.get(field::TpiLarge, 0) < 4.0f || f.get(field::Curvature, 0) < 0) return false;
            break;
        case PlacementSource::CliffFoot: {
            if (f.get(field::Slope, 0) > 0.6f) return false;
            // Up the slope, within twenty metres, the ground must stand steep.
            const float aspect = f.get(field::Aspect, 0);
            FieldSample up;
            context_.fields->sample(x - std::cos(aspect) * 20, y - std::sin(aspect) * 20, up);
            if (up.get(field::Slope, 0) < 1.0f) return false;
            break;
        }
        default: break;
    }
    return true;
}

double FeaturePlanner::alignment(const FeatureRecipe& r, double x, double y, double fallback) const {
    FieldSample f;
    switch (r.placement.align) {
        case Align::None:
        case Align::Random: return fallback;
        case Align::Wind: return std::atan2(context_.windY, context_.windX);
        default: break;
    }
    if (context_.fields) context_.fields->sample(x, y, f);
    if (!f.has(field::Aspect) || f.get(field::Slope, 0) < 1e-3f) return fallback;
    const double aspect = f[field::Aspect];
    switch (r.placement.align) {
        case Align::Contour: return aspect + kPi * 0.5;
        case Align::Slope:
        case Align::Flow: return aspect;
        default: return fallback;
    }
}

std::vector<std::array<double, 2>> FeaturePlanner::march(const FeatureRecipe& r, double x, double y, double length,
                                                         double yaw) const {
    // Walk out both ways from the anchor, turning with the alignment field, so
    // a cliff band follows the contour of the hill it is on.
    const double step = std::clamp(length / 12.0, 4.0, 32.0);
    const int steps = std::max(1, int(std::round(length * 0.5 / step)));
    std::vector<std::array<double, 2>> back, fore;
    for (int dir : {-1, 1}) {
        double px = x, py = y;
        double hx = std::cos(yaw) * dir, hy = std::sin(yaw) * dir;
        auto& out = dir < 0 ? back : fore;
        for (int i = 0; i < steps; ++i) {
            const double a = alignment(r, px, py, std::atan2(hy, hx));
            double nx = std::cos(a), ny = std::sin(a);
            if (nx * hx + ny * hy < 0) { nx = -nx; ny = -ny; }
            // Turn at most a third of the way per step: a field that jumps
            // must not fold the line back on itself.
            hx = hx * 0.67 + nx * 0.33;
            hy = hy * 0.67 + ny * 0.33;
            const double len = std::hypot(hx, hy);
            hx /= len; hy /= len;
            px += hx * step;
            py += hy * step;
            out.push_back({px, py});
        }
    }
    std::vector<std::array<double, 2>> line;
    for (auto it = back.rbegin(); it != back.rend(); ++it) line.push_back(*it);
    line.push_back({x, y});
    for (const auto& p : fore) line.push_back(p);
    return line;
}

std::shared_ptr<const DrainageNetwork> FeaturePlanner::drainage(FeatureScale scale, std::int64_t cx, std::int64_t cy) const {
    // One network serves a tile of 4 x 4 cells, with a cell of halo all round:
    // a window a cell's own would be worked out sixteen times over.
    constexpr std::int64_t kTile = 4;
    const auto tx = cx >= 0 ? cx / kTile : (cx - kTile + 1) / kTile;
    const auto ty = cy >= 0 ? cy / kTile : (cy - kTile + 1) / kTile;
    const Key key{0xffffffffu - std::uint32_t(scale), tx, ty};
    if (auto hit = drainage_.find(key)) return hit;
    const double cell = kPlanningCell[int(scale)];
    DrainageSettings s = context_.drainage;
    s.step = cell / 32.0;
    s.minReach = std::max(s.minReach, s.step * 3);
    // Scaled with the step, so a network's channels begin at the same share of
    // the window whatever the scale.
    const double area = (s.step / 16.0) * (s.step / 16.0);
    s.channelArea *= area;
    s.seasonalArea *= area;
    s.permanentArea *= area;
    const int side = int(32 * (kTile + 2));
    auto net = std::make_shared<const DrainageNetwork>(
            buildDrainage(context_.height, context_.climate, double(tx * kTile - 1) * cell, double(ty * kTile - 1) * cell,
                          side, side, s));
    return drainage_.put(key, net);
}

std::shared_ptr<const std::vector<FeaturePlanner::Candidate>>
FeaturePlanner::candidates(std::uint32_t recipe, std::int64_t cx, std::int64_t cy) const {
    const Key key{recipe, cx, cy};
    if (auto hit = candidates_.find(key)) return hit;
    const auto& r = context_.catalogue->recipes()[recipe];
    const auto& p = r.placement;
    const double cell = kPlanningCell[int(r.scale)];
    const double x0 = double(cx) * cell, y0 = double(cy) * cell;
    Rng rng(hashOf(context_.seed ^ nameHash(r.name), cx, cy, 0xca4d));
    auto out = std::make_shared<std::vector<Candidate>>();
    const bool spline = needsSpline(r);
    const auto finish = [&](Candidate c) {
        c.priority = rng.unit();
        // A scale the source already decided (a channel's width) stands; otherwise draw one.
        const double drawn = rng.range(p.scaleMin, p.scaleMax);
        if (c.scale <= 0) c.scale = drawn;
        if (!passes(r, c.x, c.y) || rng.unit() > p.chance) return;
        out->push_back(std::move(c));
    };
    if (p.source == PlacementSource::Channel) {
        const auto net = drainage(r.scale, cx, cy);
        if (spline) {
            for (const auto& reach : net->reaches) {
                if (!p.channelClasses.empty() &&
                    std::find(p.channelClasses.begin(), p.channelClasses.end(), reach.kind) == p.channelClasses.end())
                    continue;
                // Cut the reach into pieces of the recipe's length; a piece
                // belongs to the cell its middle is in.
                std::size_t i = 0;
                Rng cut(hashOf(context_.seed ^ nameHash(r.name), std::int64_t(reach.points.front()[0] * 4),
                               std::int64_t(reach.points.front()[1] * 4), 0x9ece));
                while (i + 1 < reach.points.size()) {
                    const double want = cut.range(p.lengthMin, p.lengthMax);
                    std::vector<std::array<double, 2>> piece{reach.points[i]};
                    double got = 0;
                    while (i + 1 < reach.points.size() && got < want) {
                        got += std::hypot(reach.points[i + 1][0] - reach.points[i][0], reach.points[i + 1][1] - reach.points[i][1]);
                        ++i;
                        piece.push_back(reach.points[i]);
                    }
                    if (got < p.lengthMin * 0.5 || piece.size() < 2) break;
                    const auto& mid = piece[piece.size() / 2];
                    if (floorDiv(mid[0], cell) != cx || floorDiv(mid[1], cell) != cy) continue;
                    // The rarer of the reach's own candidates and the recipe's density.
                    if (cut.unit() > std::min(1.0, p.densityPerKm2 * got / 1000.0 * 0.25)) continue;
                    Candidate c;
                    c.x = mid[0]; c.y = mid[1];
                    c.spline = std::move(piece);
                    c.channel = reach.kind;
                    // Scale the carve to the channel: a recipe's widths are for scale 1.
                    double recipeWidth = 0;
                    for (const auto& op : r.terrain)
                        if (op.kind == TerrainOpKind::CarveProfile) recipeWidth = std::max(recipeWidth, op.outerWidth);
                    c.scale = recipeWidth > 0 ? std::clamp(reach.outerWidth / recipeWidth, p.scaleMin, p.scaleMax) : 0;
                    const auto& a = c.spline.front();
                    const auto& b = c.spline.back();
                    c.yaw = std::atan2(b[1] - a[1], b[0] - a[0]);
                    finish(std::move(c));
                }
            }
        } else {
            for (const auto& m : net->meanders) {
                if (floorDiv(m.x, cell) != cx || floorDiv(m.y, cell) != cy) continue;
                if (!p.channelClasses.empty() &&
                    std::find(p.channelClasses.begin(), p.channelClasses.end(), m.kind) == p.channelClasses.end())
                    continue;
                Candidate c;
                c.x = m.x; c.y = m.y;
                c.channel = m.kind;
                c.yaw = rng.range(0, 2 * kPi);
                finish(std::move(c));
            }
        }
        return candidates_.put(key, out);
    }
    // Jittered points: the expected count, its fraction decided by a draw.
    const double expected = p.densityPerKm2 * (cell / 1000.0) * (cell / 1000.0);
    int count = int(std::floor(expected));
    if (rng.unit() < expected - count) ++count;
    count = std::min(count, 4096);
    // Stratified over a grid of strata so candidates do not clump by chance.
    const int strata = std::max(1, int(std::ceil(std::sqrt(double(count)))));
    for (int i = 0; i < count; ++i) {
        const int sx = i % strata, sy = (i / strata) % strata;
        Candidate c;
        c.x = x0 + (sx + rng.unit()) * cell / strata;
        c.y = y0 + (sy + rng.unit()) * cell / strata;
        c.yaw = alignment(r, c.x, c.y, rng.range(0, 2 * kPi));
        if (spline) c.spline = march(r, c.x, c.y, rng.range(p.lengthMin, p.lengthMax), c.yaw);
        finish(std::move(c));
    }
    return candidates_.put(key, out);
}

bool FeaturePlanner::clearOfEarlier(std::uint32_t recipe, double x, double y) const {
    const auto& cat = *context_.catalogue;
    const double clearance = cat.recipes()[recipe].placement.clearance;
    if (clearance <= 0) return true;
    for (std::uint32_t other = 0; other < cat.recipes().size(); ++other) {
        if (order(other) >= order(recipe)) continue;
        const auto& o = cat.recipes()[other];
        const double cell = kPlanningCell[int(o.scale)];
        const double reach = cat.reach(other) + clearance;
        const auto x0 = floorDiv(x - reach, cell), x1 = floorDiv(x + reach, cell);
        const auto y0 = floorDiv(y - reach, cell), y1 = floorDiv(y + reach, cell);
        for (auto cy = y0; cy <= y1; ++cy)
            for (auto cx = x0; cx <= x1; ++cx)
                for (const auto& in : *this->cell(other, cx, cy)) {
                    const auto& b = in.bounds;
                    if (x >= b.min.x.toDouble() - clearance && x <= b.max.x.toDouble() + clearance &&
                        y >= b.min.y.toDouble() - clearance && y <= b.max.y.toDouble() + clearance)
                        return false;
                }
    }
    return true;
}

FeatureInstance FeaturePlanner::instance(std::uint32_t recipe, const Candidate& c, std::uint64_t id, bool secondary) const {
    const auto& r = context_.catalogue->recipes()[recipe];
    FeatureInstance in;
    in.id = id;
    in.recipe = recipe;
    in.seed = mix64(id ^ context_.seed);
    in.anchor = {quantised(c.x), quantised(c.y)};
    in.anchorHeight = quantised(context_.height ? context_.height(c.x, c.y) : 0.0);
    in.yawCos = core::Fixed::fromRaw(core::Fixed::Raw(std::llround(std::cos(c.yaw) * 65536.0)) << 16);
    in.yawSin = core::Fixed::fromRaw(core::Fixed::Raw(std::llround(std::sin(c.yaw) * 65536.0)) << 16);
    in.scale = quantised(c.scale);
    for (const auto& p : c.spline) in.spline.points.push_back({quantised(p[0]), quantised(p[1])});
    in.spline.finish();
    in.age = r.age;
    in.channel = c.channel;
    in.secondary = secondary;
    in.hand = (in.seed & 1) ? 1 : -1;
    const auto margin = quantised(r.reach() * c.scale);
    if (!in.spline.empty()) {
        in.bounds = in.spline.bounds(margin);
    } else {
        in.bounds = {{in.anchor.x - margin, in.anchor.y - margin}, {in.anchor.x + margin, in.anchor.y + margin}};
    }
    return in;
}

std::shared_ptr<const InstanceList> FeaturePlanner::cell(std::uint32_t recipe, std::int64_t cx, std::int64_t cy) const {
    const Key key{recipe, cx, cy};
    if (auto hit = cells_.find(key)) return hit;
    const auto& r = context_.catalogue->recipes()[recipe];
    const auto& p = r.placement;
    const double cellSize = kPlanningCell[int(r.scale)];
    const auto mine = candidates(recipe, cx, cy);
    auto out = std::make_shared<InstanceList>();
    const int ring = std::max(1, int(std::ceil(p.minSpacing / cellSize)));
    const double spacing2 = p.minSpacing * p.minSpacing;
    const auto beats = [](const Candidate& a, const Candidate& b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        return a.x != b.x ? a.x > b.x : a.y > b.y;
    };
    const auto base = nameHash(r.name) ^ context_.seed;
    for (std::size_t i = 0; i < mine->size(); ++i) {
        const auto& c = (*mine)[i];
        bool kept = true;
        for (std::int64_t ny = cy - ring; ny <= cy + ring && kept; ++ny)
            for (std::int64_t nx = cx - ring; nx <= cx + ring && kept; ++nx) {
                const auto theirs = (nx == cx && ny == cy) ? mine : candidates(recipe, nx, ny);
                for (const auto& o : *theirs) {
                    if (&o == &c) continue;
                    const double dx = o.x - c.x, dy = o.y - c.y;
                    if (dx * dx + dy * dy < spacing2 && beats(o, c)) { kept = false; break; }
                }
            }
        if (!kept || !clearOfEarlier(recipe, c.x, c.y)) continue;
        const auto id = hashOf(base, cx, cy, i + 1);
        // Removed by hand: it still took its place in the spacing above, so
        // taking it away does not let another feature move in.
        if (context_.removed && context_.removed(id, c.x, c.y)) continue;
        out->push_back(instance(recipe, c, id, false));
        // The composition's secondaries: smaller relatives round the anchor.
        const auto& comp = r.composition;
        if (comp.secondaries > 0) {
            Rng rng(id ^ 0x5ec0ULL);
            for (int k = 0; k < comp.secondaries; ++k) {
                const double a = rng.range(0, 2 * kPi);
                const double d = comp.secondaryRadius * rng.range(0.35, 1.0);
                Candidate s;
                s.x = c.x + std::cos(a) * d * comp.elongation;
                s.y = c.y + std::sin(a) * d;
                // Elongation stretches the group along the anchor's own line.
                const double ca = std::cos(c.yaw), sa = std::sin(c.yaw);
                const double lx = s.x - c.x, ly = s.y - c.y;
                s.x = c.x + lx * ca - ly * sa;
                s.y = c.y + lx * sa + ly * ca;
                if (!passes(r, s.x, s.y)) continue;
                s.scale = c.scale * comp.secondaryScale * rng.range(0.8, 1.2);
                s.yaw = c.yaw + rng.range(-0.5, 0.5);
                s.channel = c.channel;
                if (needsSpline(r) && !c.spline.empty()) {
                    double len = 0;
                    for (std::size_t q = 1; q < c.spline.size(); ++q)
                        len += std::hypot(c.spline[q][0] - c.spline[q - 1][0], c.spline[q][1] - c.spline[q - 1][1]);
                    s.spline = march(r, s.x, s.y, len * comp.secondaryScale, s.yaw);
                }
                out->push_back(instance(recipe, s, hashOf(id, k, 0, 0x5ec), true));
            }
        }
    }
    return cells_.put(key, out);
}

void FeaturePlanner::instancesIn(const core::WorldRect& area, std::vector<FeatureInstance>& out) const {
    const auto& cat = *context_.catalogue;
    const double ax0 = area.min.x.toDouble(), ay0 = area.min.y.toDouble();
    const double ax1 = area.max.x.toDouble(), ay1 = area.max.y.toDouble();
    for (std::uint32_t recipe = 0; recipe < cat.recipes().size(); ++recipe) {
        const double size = kPlanningCell[int(cat.recipes()[recipe].scale)];
        const double reach = cat.reach(recipe) + cat.recipes()[recipe].composition.secondaryRadius;
        const auto x0 = floorDiv(ax0 - reach, size), x1 = floorDiv(ax1 + reach, size);
        const auto y0 = floorDiv(ay0 - reach, size), y1 = floorDiv(ay1 + reach, size);
        for (auto cy = y0; cy <= y1; ++cy)
            for (auto cx = x0; cx <= x1; ++cx)
                for (const auto& in : *cell(recipe, cx, cy))
                    if (in.bounds.overlaps(area)) out.push_back(in);
    }
}

} // namespace engine::environment
