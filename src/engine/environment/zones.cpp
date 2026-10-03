#include "engine/environment/zones.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/environment/random.hpp"

namespace engine::environment {

void ZoneWeights::fromDense(std::span<const float> dense) {
    type.fill(kNoZone);
    weight.fill(0);
    for (std::size_t t = 0; t < dense.size(); ++t) {
        const float w = dense[t];
        if (w <= 0) continue;
        for (std::size_t s = 0; s < kZoneSlots; ++s) {
            if (w > weight[s]) {
                for (std::size_t m = kZoneSlots - 1; m > s; --m) { type[m] = type[m - 1]; weight[m] = weight[m - 1]; }
                type[s] = ZoneTypeId(t);
                weight[s] = w;
                break;
            }
        }
    }
    const float sum = std::accumulate(weight.begin(), weight.end(), 0.0f);
    if (sum > 0) for (auto& w : weight) w /= sum;
}

EnvironmentZone ZoneGrid::at(double x, double y) const {
    if (cells.empty()) return {};
    const double gx = std::clamp((x - x0) / step, 0.0, double(columns - 1));
    const double gy = std::clamp((y - y0) / step, 0.0, double(rows - 1));
    const int c0 = std::min(int(gx), columns - 2 < 0 ? 0 : columns - 2);
    const int r0 = std::min(int(gy), rows - 2 < 0 ? 0 : rows - 2);
    const int c1 = std::min(c0 + 1, columns - 1), r1 = std::min(r0 + 1, rows - 1);
    const double fx = gx - c0, fy = gy - r0;
    const double wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    const EnvironmentZone* corner[4] = {&cell(c0, r0), &cell(c1, r0), &cell(c0, r1), &cell(c1, r1)};
    std::array<float, kMaxZoneTypes> dense{};
    EnvironmentZone out;
    for (int k = 0; k < 4; ++k) {
        for (std::size_t s = 0; s < kZoneSlots; ++s)
            dense[corner[k]->weights.type[s]] += float(wts[k]) * corner[k]->weights.weight[s];
        for (std::size_t i = 0; i < kZoneScalars; ++i)
            scalarAt(out.scalars, i) += float(wts[k]) * scalarAt(corner[k]->scalars, i);
    }
    out.weights.fromDense(dense);
    return out;
}

ZoneField::ZoneField(const FieldSource& fields, const ZoneClassifier& classifier, ZoneSettings settings)
    : fields_(fields), classifier_(classifier), settings_(settings) {}

ZoneGrid ZoneField::build(double x0, double y0, int columns, int rows) const {
    const auto types = classifier_.types().size();
    const double step = settings_.step;
    // A halo wide enough for the kernel, so the smoothed border is the same
    // number the neighbouring page works out.
    const int halo = int(std::ceil(std::max(settings_.alongMetres, settings_.acrossMetres) * 2.0 / step));
    const int wc = columns + 2 * halo, wr = rows + 2 * halo;
    std::vector<float> raw(std::size_t(wc) * wr * types, 0.0f);
    std::vector<ZoneScalars> scalars(std::size_t(wc) * wr);
    std::vector<float> across(std::size_t(wc) * wr * 2, 0.0f);   // unit gradient direction
    std::vector<FieldSample> samples(std::size_t(wc) * wr);
    const double bx = x0 - halo * step, by = y0 - halo * step;
    const auto seed = settings_.seed ^ 0x5a0e5a0eULL;
    for (int r = 0; r < wr; ++r) {
        for (int c = 0; c < wc; ++c) {
            double x = bx + c * step, y = by + r * step;
            if (settings_.breakupMetres > 0) {
                x += (valueNoise(seed, x / 70.0, y / 70.0) - 0.5) * 2 * settings_.breakupMetres;
                y += (valueNoise(seed + 1, x / 70.0, y / 70.0) - 0.5) * 2 * settings_.breakupMetres;
            }
            auto& f = samples[std::size_t(r) * wc + c];
            fields_.sample(x, y, f);
        }
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        classifier_.classify(samples[i], std::span<float>(raw.data() + i * types, types), scalars[i]);
        for (std::size_t t = 0; t < types; ++t) raw[i * types + t] = std::max(0.0f, raw[i * types + t]);
        const float aspect = samples[i].get(field::Aspect, 0.0f);
        const float slope = samples[i].get(field::Slope, 0.0f);
        // Flat ground has no "across"; the kernel is then round.
        const float k = std::min(1.0f, slope * 8.0f);
        across[i * 2] = std::cos(aspect) * k;
        across[i * 2 + 1] = std::sin(aspect) * k;
    }
    ZoneGrid grid;
    grid.x0 = x0; grid.y0 = y0; grid.step = step; grid.columns = columns; grid.rows = rows;
    grid.cells.resize(std::size_t(columns) * rows);
    const double sa = settings_.alongMetres, sc = settings_.acrossMetres;
    std::vector<float> dense(types);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            const int wcx = c + halo, wry = r + halo;
            const std::size_t centre = std::size_t(wry) * wc + wcx;
            const float ax = across[centre * 2], ay = across[centre * 2 + 1];
            const float k = std::hypot(ax, ay);
            std::fill(dense.begin(), dense.end(), 0.0f);
            ZoneScalars sum{};
            double total = 0;
            for (int dy = -halo; dy <= halo; ++dy) {
                for (int dx = -halo; dx <= halo; ++dx) {
                    const double ox = dx * step, oy = dy * step;
                    double d2;
                    if (k > 0) {
                        const double ux = ax / k, uy = ay / k;
                        const double acrossD = ox * ux + oy * uy;
                        const double alongD = -ox * uy + oy * ux;
                        // Interpolate between round (flat) and stretched (slope).
                        const double sAcross = sa + (sc - sa) * k;
                        d2 = alongD * alongD / (sa * sa) + acrossD * acrossD / (sAcross * sAcross);
                    } else {
                        d2 = (ox * ox + oy * oy) / (sa * sa);
                    }
                    if (d2 > 4.0) continue;
                    const double w = std::exp(-d2);
                    const std::size_t i = std::size_t(wry + dy) * wc + (wcx + dx);
                    for (std::size_t t = 0; t < types; ++t) dense[t] += float(w) * raw[i * types + t];
                    for (std::size_t s = 0; s < kZoneScalars; ++s)
                        scalarAt(sum, s) += float(w) * scalarAt(scalars[i], s);
                    total += w;
                }
            }
            auto& cell = grid.cells[std::size_t(r) * columns + c];
            cell.weights.fromDense(dense);
            if (total > 0) for (std::size_t s = 0; s < kZoneScalars; ++s) scalarAt(cell.scalars, s) = float(scalarAt(sum, s) / total);
        }
    }
    return grid;
}

std::shared_ptr<const ZoneGrid> ZoneField::page(std::int64_t pageX, std::int64_t pageY) const {
    const Key key{pageX, pageY};
    {
        std::lock_guard guard(lock_);
        if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    }
    // One cell past the page on each side, so a point on the far edge
    // interpolates inside the grid.
    const int side = int(settings_.pageMetres / settings_.step) + 2;
    auto grid = std::make_shared<const ZoneGrid>(
            build(double(pageX) * settings_.pageMetres - settings_.step,
                  double(pageY) * settings_.pageMetres - settings_.step, side, side));
    std::lock_guard guard(lock_);
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    cache_.emplace(key, grid);
    order_.push_back(key);
    if (order_.size() > kKept) {
        cache_.erase(order_.front());
        order_.erase(order_.begin());
    }
    return grid;
}

EnvironmentZone ZoneField::at(double x, double y) const {
    const auto px = std::int64_t(std::floor(x / settings_.pageMetres));
    const auto py = std::int64_t(std::floor(y / settings_.pageMetres));
    return page(px, py)->at(x, y);
}

void ZoneField::clear() {
    std::lock_guard guard(lock_);
    cache_.clear();
    order_.clear();
}

} // namespace engine::environment
