#include "engine/environment/fields.hpp"

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace engine::environment {

namespace {
struct Names {
    std::mutex lock;
    std::vector<std::string> names{
            "elevation", "slope", "aspect", "curvature", "plan_curvature", "convergence",
            "tpi_small", "tpi_large", "exposure", "wetness", "moisture", "fertility",
            "soil_depth", "drainage", "temperature", "canopy", "rockiness", "disturbance",
            "flow_accum", "dist_water", "age", "sand", "snow", "water"};
};
Names& names() {
    static Names n;
    return n;
}
} // namespace

std::string_view fieldName(FieldId id) {
    auto& n = names();
    std::lock_guard guard(n.lock);
    return id < n.names.size() ? std::string_view(n.names[id]) : std::string_view();
}

std::optional<FieldId> fieldId(std::string_view name, bool add) {
    auto& n = names();
    std::lock_guard guard(n.lock);
    for (std::size_t i = 0; i < n.names.size(); ++i)
        if (n.names[i] == name) return FieldId(i);
    if (!add || n.names.size() >= kMaxFields) return std::nullopt;
    n.names.emplace_back(name);
    return FieldId(n.names.size() - 1);
}

std::size_t fieldCount() {
    auto& n = names();
    std::lock_guard guard(n.lock);
    return n.names.size();
}

void FieldSource::sampleGrid(double x0, double y0, double step, int columns, int rows,
                             std::span<FieldSample> out) const {
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            sample(x0 + c * step, y0 + r * step, out[std::size_t(r) * columns + c]);
}

void deriveShape(const HeightAt& height, double x, double y, const ShapeSettings& s, FieldSample& out) {
    const double h = s.step;
    const double z = height(x, y);
    const double e = height(x + h, y), w = height(x - h, y);
    const double n = height(x, y + h), so = height(x, y - h);
    const double ne = height(x + h, y + h), nw = height(x - h, y + h);
    const double se = height(x + h, y - h), sw = height(x - h, y - h);
    // Zevenbergen-Thorne on the 3x3 window.
    const double p = (e - w) / (2 * h);
    const double q = (n - so) / (2 * h);
    const double r = (e - 2 * z + w) / (h * h);
    const double t = (n - 2 * z + so) / (h * h);
    const double sxy = (ne - nw - se + sw) / (4 * h * h);
    const double g2 = p * p + q * q;
    out.set(field::Elevation, float(z));
    out.set(field::Slope, float(std::sqrt(g2)));
    out.set(field::Aspect, float(std::atan2(-q, -p)));
    if (g2 > 1e-10) {
        const double profile = -(p * p * r + 2 * p * q * sxy + q * q * t) / (g2 * std::pow(1 + g2, 1.5));
        const double plan = -(q * q * r - 2 * p * q * sxy + p * p * t) / std::pow(g2, 1.5);
        out.set(field::Curvature, float(profile));
        out.set(field::PlanCurvature, float(std::clamp(plan, -1.0, 1.0)));
    } else {
        out.set(field::Curvature, float(-(r + t) * 0.5));
        out.set(field::PlanCurvature, 0.0f);
    }
    // Convergence: how many of eight neighbours slope down towards the centre,
    // weighted by how directly they point at it.
    const double nb[8] = {e, ne, n, nw, w, sw, so, se};
    const double dx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
    const double dy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
    double towards = 0;
    for (int i = 0; i < 8; ++i) {
        const double dist = std::hypot(dx[i], dy[i]) * h;
        const double drop = (nb[i] - z) / dist;
        if (drop > 0) towards += std::min(1.0, drop * 4.0);
    }
    out.set(field::Convergence, float(towards / 8.0));
    // Topographic position against two rings of eight.
    const auto ring = [&](double radius) {
        double sum = 0;
        for (int i = 0; i < 8; ++i) {
            const double a = i * 0.7853981633974483;
            sum += height(x + std::cos(a) * radius, y + std::sin(a) * radius);
        }
        return z - sum / 8.0;
    };
    out.set(field::TpiSmall, float(ring(s.smallRing)));
    out.set(field::TpiLarge, float(ring(s.largeRing)));
    // Exposure: the share of the slope that faces into the vector, lifted by
    // height above the large ring (a crest is exposed whichever way it faces).
    const double slope = std::sqrt(g2);
    double facing = 0.5;
    if (slope > 1e-6) facing = 0.5 + 0.5 * ((-p / slope) * s.exposureX + (-q / slope) * s.exposureY) * std::min(1.0, slope * 2);
    const double crest = std::clamp(out[field::TpiLarge] / 30.0, -0.5, 0.5);
    out.set(field::Exposure, float(std::clamp(facing + crest, 0.0, 1.0)));
}

void deriveShapeGrid(std::span<const double> heights, int columns, int rows, int margin, double step,
                     const ShapeSettings& s, std::span<FieldSample> out) {
    const int wc = columns + 2 * margin;
    const auto H = [&](int c, int r) {
        c = std::clamp(c, -margin, columns - 1 + margin);
        r = std::clamp(r, -margin, rows - 1 + margin);
        return heights[std::size_t(r + margin) * wc + (c + margin)];
    };
    const int small = std::clamp(int(std::lround(s.smallRing / step)), 1, std::max(1, margin));
    const int large = std::clamp(int(std::lround(s.largeRing / step)), 1, std::max(1, margin));
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < columns; ++c) {
            // The same arithmetic as deriveShape, read off the grid.
            const HeightAt grid = [&](double x, double y) {
                return H(c + int(std::lround(x / step)), r + int(std::lround(y / step)));
            };
            ShapeSettings local = s;
            local.step = step;
            local.smallRing = small * step;
            local.largeRing = large * step;
            deriveShape(grid, 0, 0, local, out[std::size_t(r) * columns + c]);
        }
    }
}

ShapedSource::ShapedSource(const FieldSource* base, HeightAt height, ShapeSettings settings)
    : base_(base), height_(std::move(height)), settings_(settings) {}

void ShapedSource::sample(double x, double y, FieldSample& out) const {
    if (base_) base_->sample(x, y, out);
    constexpr std::uint32_t shape = (1u << field::Slope) | (1u << field::Aspect) | (1u << field::Curvature) |
                                    (1u << field::PlanCurvature) | (1u << field::Convergence) |
                                    (1u << field::TpiSmall) | (1u << field::TpiLarge) | (1u << field::Exposure);
    if ((out.known & shape) == shape || !height_) return;
    FieldSample derived;
    deriveShape(height_, x, y, settings_, derived);
    for (FieldId id = 0; id < field::Wetness; ++id)
        if (!out.has(id) && derived.has(id)) out.set(id, derived[id]);
}

} // namespace engine::environment
