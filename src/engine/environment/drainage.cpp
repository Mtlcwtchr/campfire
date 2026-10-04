#include "engine/environment/drainage.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>

namespace engine::environment {

namespace {
constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
constexpr double kDist[8] = {1, 1.4142135623730951, 1, 1.4142135623730951, 1, 1.4142135623730951, 1, 1.4142135623730951};

std::vector<std::array<double, 2>> chaikin(const std::vector<std::array<double, 2>>& in, int passes) {
    auto pts = in;
    for (int p = 0; p < passes && pts.size() > 2; ++p) {
        std::vector<std::array<double, 2>> out;
        out.reserve(pts.size() * 2);
        out.push_back(pts.front());
        for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
            const auto& a = pts[i];
            const auto& b = pts[i + 1];
            out.push_back({a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25});
            out.push_back({a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75});
        }
        out.push_back(pts.back());
        pts = std::move(out);
    }
    return pts;
}
} // namespace

ChannelClass classifyChannel(double pastArea, double todayArea, const DrainageSettings& s) {
    if (todayArea >= s.permanentArea) return ChannelClass::PermanentRiver;
    const double ratio = pastArea > 0 ? todayArea / pastArea : 0;
    if (ratio >= s.seasonalRatio) return todayArea >= s.seasonalArea ? ChannelClass::SeasonalStream : ChannelClass::EphemeralChannel;
    if (ratio >= s.ephemeralRatio) return ChannelClass::EphemeralChannel;
    if (ratio >= s.abandonedRatio) return ChannelClass::AbandonedChannel;
    return ChannelClass::Paleochannel;
}

float DrainageNetwork::pastAt(double x, double y) const {
    const int c = int(std::floor((x - x0) / step)), r = int(std::floor((y - y0) / step));
    if (c < 0 || r < 0 || c >= columns || r >= rows) return 0;
    return pastArea[std::size_t(r) * columns + c];
}

DrainageNetwork buildDrainage(const HeightAt& height, const DrainageClimate& climate,
                              double x0, double y0, int columns, int rows, const DrainageSettings& s) {
    DrainageNetwork net;
    net.x0 = x0; net.y0 = y0; net.step = s.step; net.columns = columns; net.rows = rows;
    const std::size_t n = std::size_t(columns) * rows;
    if (n == 0) return net;
    std::vector<double> z(n), filled(n);
    std::vector<double> past(n, 1.0), today(n, 1.0), erode(n, 0.5);
    const double half = 0.5 * s.step;
    if (climate.heights) {
        climate.heights(x0 + half, y0 + half, s.step, columns, rows, z);
    } else {
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) z[std::size_t(r) * columns + c] = height(x0 + (c + 0.5) * s.step, y0 + (r + 0.5) * s.step);
    }
    // The climate on a grid four cells to a step, read bilinearly.
    constexpr int kCoarse = 4;
    const int cc = columns / kCoarse + 2, cr = rows / kCoarse + 2;
    const auto coarse = [&](const std::function<double(double, double)>& f, std::vector<double>& out, double lo, double hi) {
        if (!f) return;
        std::vector<double> g(std::size_t(cc) * cr);
        for (int r = 0; r < cr; ++r)
            for (int c = 0; c < cc; ++c)
                g[std::size_t(r) * cc + c] = std::clamp(f(x0 + half + c * kCoarse * s.step, y0 + half + r * kCoarse * s.step), lo, hi);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c) {
                const double gx = double(c) / kCoarse, gy = double(r) / kCoarse;
                const int ix = std::min(int(gx), cc - 2), iy = std::min(int(gy), cr - 2);
                const double fx = gx - ix, fy = gy - iy;
                const auto at = [&](int a, int b) { return g[std::size_t(b) * cc + a]; };
                out[std::size_t(r) * columns + c] = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) +
                                                    (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
            }
    };
    coarse(climate.pastRain, past, 0.0, 1e9);
    coarse(climate.rainToday, today, 0.0, 1e9);
    coarse(climate.erodibility, erode, 0.0, 1.0);
    // Priority flood from the border: every pit drains, with a tiny rise so the
    // flats have a direction. Ties broken by index so the order is the same
    // on every machine.
    using Item = std::pair<double, std::size_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    std::vector<char> done(n, 0);
    net.down.assign(n, -1);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            if (r == 0 || c == 0 || r == rows - 1 || c == columns - 1) {
                const std::size_t i = std::size_t(r) * columns + c;
                filled[i] = z[i];
                done[i] = 1;
                open.push({z[i], i});
            }
    const double rise = 1e-4 * s.step;
    std::vector<std::size_t> order;
    order.reserve(n);
    while (!open.empty()) {
        const auto [h, i] = open.top();
        open.pop();
        order.push_back(i);
        const int c = int(i % columns), r = int(i / columns);
        for (int k = 0; k < 8; ++k) {
            const int nc = c + kDx[k], nr = r + kDy[k];
            if (nc < 0 || nr < 0 || nc >= columns || nr >= rows) continue;
            const std::size_t j = std::size_t(nr) * columns + nc;
            if (done[j]) continue;
            done[j] = 1;
            filled[j] = std::max(z[j], filled[i] + rise);
            net.down[j] = std::int8_t((k + 4) % 8);   // j drains towards i
            open.push({filled[j], j});
        }
    }
    // Where an unfilled cell has a steeper way down than the one the flood
    // found, take it: the flood only decided the pits and the flats.
    for (std::size_t i = 0; i < n; ++i) {
        if (filled[i] > z[i] + rise * 0.5) continue;
        const int c = int(i % columns), r = int(i / columns);
        double best = 0;
        int bestK = net.down[i];
        for (int k = 0; k < 8; ++k) {
            const int nc = c + kDx[k], nr = r + kDy[k];
            if (nc < 0 || nr < 0 || nc >= columns || nr >= rows) continue;
            const std::size_t j = std::size_t(nr) * columns + nc;
            const double drop = (filled[i] - filled[j]) / kDist[k];
            if (drop > best) { best = drop; bestK = k; }
        }
        net.down[i] = std::int8_t(bestK);
    }
    // Accumulate from the highest down: the reverse of the flood's order.
    net.pastArea.assign(n, 0.0f);
    net.todayArea.assign(n, 0.0f);
    std::vector<double> pa(n), ta(n);
    const double cell = s.step * s.step;
    for (std::size_t i = 0; i < n; ++i) { pa[i] = cell * past[i]; ta[i] = cell * today[i]; }
    std::vector<std::size_t> byHeight(n);
    std::iota(byHeight.begin(), byHeight.end(), 0);
    std::stable_sort(byHeight.begin(), byHeight.end(), [&](std::size_t a, std::size_t b) { return filled[a] > filled[b]; });
    std::vector<int> upstreamChannels(n, 0);
    const auto downOf = [&](std::size_t i) -> std::ptrdiff_t {
        const int k = net.down[i];
        if (k < 0) return -1;
        const int c = int(i % columns) + kDx[k], r = int(i / columns) + kDy[k];
        if (c < 0 || r < 0 || c >= columns || r >= rows) return -1;
        return std::ptrdiff_t(std::size_t(r) * columns + c);
    };
    for (std::size_t i : byHeight) {
        const auto j = downOf(i);
        if (j >= 0) { pa[std::size_t(j)] += pa[i]; ta[std::size_t(j)] += ta[i]; }
    }
    for (std::size_t i = 0; i < n; ++i) { net.pastArea[i] = float(pa[i]); net.todayArea[i] = float(ta[i]); }
    // A channel: enough water over enough slope for ground this soft.
    std::vector<char> channel(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const auto j = downOf(i);
        const double slope = j >= 0 ? std::max(s.minSlope, (filled[i] - filled[std::size_t(j)]) / s.step) : s.minSlope;
        const double power = pa[i] * std::sqrt(slope / 0.05) * (0.4 + 1.2 * erode[i]);
        channel[i] = power >= s.channelArea;
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!channel[i]) continue;
        const auto j = downOf(i);
        if (j >= 0 && channel[std::size_t(j)]) ++upstreamChannels[std::size_t(j)];
    }
    // Trace reaches from every head and every confluence to the next confluence
    // or the edge of the window.
    std::vector<char> started(n, 0);
    for (std::size_t head : byHeight) {
        if (!channel[head] || started[head]) continue;
        if (upstreamChannels[head] == 1) continue;   // the middle of a reach
        started[head] = 1;
        ChannelReach reach;
        std::size_t i = head;
        std::vector<std::size_t> cells{i};
        while (true) {
            const auto j = downOf(i);
            if (j < 0 || !channel[std::size_t(j)]) break;
            cells.push_back(std::size_t(j));
            if (upstreamChannels[std::size_t(j)] != 1) break;   // a confluence ends it
            i = std::size_t(j);
        }
        std::vector<std::array<double, 2>> pts;
        for (auto c : cells) pts.push_back({x0 + (double(c % columns) + 0.5) * s.step, y0 + (double(c / columns) + 0.5) * s.step});
        pts = chaikin(pts, int(s.smoothing));
        double length = 0;
        for (std::size_t k = 1; k < pts.size(); ++k) length += std::hypot(pts[k][0] - pts[k - 1][0], pts[k][1] - pts[k - 1][1]);
        if (length < s.minReach) continue;
        reach.points = std::move(pts);
        reach.length = length;
        for (std::size_t k = 0; k < reach.points.size(); ++k) {
            const std::size_t c = cells[std::min(cells.size() - 1, k * cells.size() / reach.points.size())];
            reach.pastArea.push_back(pa[c]);
            reach.todayArea.push_back(ta[c]);
        }
        const std::size_t last = cells.back();
        reach.kind = classifyChannel(pa[last], ta[last], s);
        const double root = std::sqrt(std::sqrt(pa[last]));
        reach.outerWidth = s.outerWidthPerRootArea * root * 4;
        reach.innerWidth = s.innerWidthPerRootArea * root * 4;
        reach.waterWidth = reach.kind <= ChannelClass::EphemeralChannel ? s.waterWidthPerRootArea * std::sqrt(std::sqrt(ta[last])) * 4 : 0;
        const auto& a = reach.points.front();
        const auto& b = reach.points.back();
        const double chord = std::hypot(b[0] - a[0], b[1] - a[1]);
        reach.sinuosity = chord > 1 ? length / chord : 1;
        // Bends: where three points a reach's width apart turn hard.
        const std::size_t span = std::max<std::size_t>(2, std::size_t(reach.outerWidth / (s.step * 0.5)));
        for (std::size_t k = span; k + span < reach.points.size(); k += span) {
            const auto& p0 = reach.points[k - span];
            const auto& p1 = reach.points[k];
            const auto& p2 = reach.points[k + span];
            const double ax = p1[0] - p0[0], ay = p1[1] - p0[1], bx = p2[0] - p1[0], by = p2[1] - p1[1];
            const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
            if (la < 1e-6 || lb < 1e-6) continue;
            const double turn = std::abs(ax * by - ay * bx) / (la * lb);
            if (turn > 0.6) {
                const double side = (ax * by - ay * bx) > 0 ? 1 : -1;
                const double radius = (la + lb) * 0.25;
                // The hollow lies on the inside of the bend.
                net.meanders.push_back({p1[0] - ay / la * radius * side, p1[1] + ax / la * radius * side, radius, reach.kind});
            }
        }
        net.reaches.push_back(std::move(reach));
    }
    return net;
}

} // namespace engine::environment
