#include "game/generation/land_coverage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace generation {

bool WorldRect::valid() const {
    return std::isfinite(minX) && std::isfinite(minY) && std::isfinite(maxX) &&
           std::isfinite(maxY) && maxX >= minX && maxY >= minY;
}

WorldRect WorldRect::grown(double metres) const {
    return {minX - metres, minY - metres, maxX + metres, maxY + metres};
}

LandCoverage::LandCoverage(std::shared_ptr<const MacroOverview> overview)
    : overview_(std::move(overview)),
      field_(overview_ ? overview_->descriptor : WorldDescriptor{}) {
    if (!overview_ || !overview_->columns || !overview_->rows ||
        overview_->floors.size() != overview_->cells() ||
        overview_->ceilings.size() != overview_->cells())
        throw std::invalid_argument("land coverage needs a built macro overview");

    levels_.push_back({overview_->columns, overview_->rows, overview_->floors, overview_->ceilings});
    // Halving both axes each time, rounding up, so the last level is one cell
    // and a query over the whole world reads exactly one pair of numbers.
    while (levels_.back().columns > 1 || levels_.back().rows > 1) {
        const auto& fine = levels_.back();
        Level coarse;
        coarse.columns = (fine.columns + 1) / 2;
        coarse.rows = (fine.rows + 1) / 2;
        coarse.floors.assign(std::size_t(coarse.columns) * coarse.rows,
                             std::numeric_limits<float>::infinity());
        coarse.ceilings.assign(coarse.floors.size(), -std::numeric_limits<float>::infinity());
        for (std::uint32_t row = 0; row < fine.rows; ++row)
            for (std::uint32_t column = 0; column < fine.columns; ++column) {
                const auto from = std::size_t(row) * fine.columns + column;
                const auto to = std::size_t(row / 2) * coarse.columns + column / 2;
                coarse.floors[to] = std::min(coarse.floors[to], fine.floors[from]);
                coarse.ceilings[to] = std::max(coarse.ceilings[to], fine.ceilings[from]);
            }
        levels_.push_back(std::move(coarse));
    }
}

std::size_t LandCoverage::bytes() const {
    std::size_t total = 0;
    for (const auto& level : levels_) total += (level.floors.size() + level.ceilings.size()) * sizeof(float);
    return total;
}

std::array<float, 2> LandCoverage::bounds(double minX, double minY, double maxX, double maxY) const {
    const auto& base = levels_.front();
    // Clamping rather than rejecting: a halo may reach past the edge, and the
    // world does not stop being water because the query did.
    const auto span = [](double low, double high, double cell, std::uint32_t count) {
        const auto first = std::clamp(std::int64_t(std::floor(low / cell)), std::int64_t(0),
                                      std::int64_t(count) - 1);
        const auto last = std::clamp(std::int64_t(std::floor(high / cell)), first,
                                     std::int64_t(count) - 1);
        return std::array<std::int64_t, 2>{first, last};
    };
    auto columns = span(minX, maxX, overview_->cellWidthMetres, base.columns);
    auto rows = span(minY, maxY, overview_->cellHeightMetres, base.rows);

    // Climb until the rectangle is a handful of cells wide. A level's cell
    // covers exactly the fine cells that fold into it, so reading it is reading
    // their bound - never a neighbour's, which would be unsound in both
    // directions at once.
    std::size_t level = 0;
    while (level + 1 < levels_.size() &&
           ((columns[1] - columns[0] + 1) > 2 || (rows[1] - rows[0] + 1) > 2)) {
        ++level;
        columns = {columns[0] / 2, columns[1] / 2};
        rows = {rows[0] / 2, rows[1] / 2};
    }
    const auto& chosen = levels_[level];
    float floor = std::numeric_limits<float>::infinity();
    float ceiling = -std::numeric_limits<float>::infinity();
    for (auto row = rows[0]; row <= rows[1]; ++row)
        for (auto column = columns[0]; column <= columns[1]; ++column) {
            const auto index = std::size_t(row) * chosen.columns + std::size_t(column);
            floor = std::min(floor, chosen.floors[index]);
            ceiling = std::max(ceiling, chosen.ceilings[index]);
        }
    return {floor, ceiling};
}

std::array<float, 2> LandCoverage::sampledBounds(const WorldRect& rect) const {
    // Five by five over the rectangle, so the allowance is the spacing between
    // samples rather than the size of the rectangle. Twenty-five evaluations of
    // a closed-form field is microseconds, and it is the difference between a
    // dry inland page knowing it is dry and every page in a large world being
    // told to prepare for the sea.
    // Two stages, because a single global constant has to assume a mountain
    // front everywhere and then charges a rectangle of open ocean for one. The
    // crust is bounded first - it is far smoother than the elevation that rides
    // on it - and the elevation bound is then allowed to know that this
    // rectangle is abyssal plain, or interior, and that the terms which make
    // the global constant large are multiplied by zero here.
    constexpr int kSamples = 5;
    const double width = rect.maxX - rect.minX, height = rect.maxY - rect.minY;
    const double spacing = std::hypot(width / (kSamples - 1), height / (kSamples - 1)) * 0.5;
    double crustLow = 1, crustHigh = 0;
    double low = std::numeric_limits<double>::infinity(), high = -low;
    for (int row = 0; row < kSamples; ++row)
        for (int column = 0; column < kSamples; ++column) {
            const auto sample = field_.at(rect.minX + width * column / (kSamples - 1),
                                          rect.minY + height * row / (kSamples - 1));
            crustLow = std::min(crustLow, double(sample.continent));
            crustHigh = std::max(crustHigh, double(sample.continent));
            low = std::min(low, double(sample.elevationMetres));
            high = std::max(high, double(sample.elevationMetres));
        }
    const double crustReach = spacing * field_.crustLipschitz();
    const double reach = spacing * field_.elevationLipschitz(crustLow - crustReach,
                                                             crustHigh + crustReach);
    return {float(low - reach), float(high + reach)};
}

Coverage LandCoverage::classify(const WorldRect& rect) const {
    if (!rect.valid()) return Coverage::Unknown;
    const auto& domain = overview_->descriptor.domain;
    // A query entirely outside the domain is not water anyone has decided about.
    // One that merely overhangs is answered for the part that exists, because
    // every halo does that and refusing them all would leave the coast Unknown.
    if (rect.maxX <= 0 || rect.maxY <= 0 || rect.minX >= double(domain.widthMetres()) ||
        rect.minY >= double(domain.heightMetres()))
        return Coverage::Unknown;

    auto [floor, ceiling] = bounds(rect.minX, rect.minY, rect.maxX, rect.maxY);
    if (!std::isfinite(floor) || !std::isfinite(ceiling)) return Coverage::Unknown;
    const double sea = overview_->seaLevelMetres;
    // The pyramid answers first because it is a table lookup. Only an
    // undecided answer on a rectangle smaller than the cell it sits in is
    // worth paying the field for, and the two bounds are both sound, so taking
    // the tighter of them cannot turn a right answer into a wrong one.
    // A cost guard, not a fit: sampling is sound at any size, it is just not
    // worth twenty-five field evaluations for a rectangle that already spans
    // several cells. Written as a margin rather than an exact comparison
    // because a rectangle built as (column+1)*cell - column*cell is a hair
    // wider than `cell` in floating point, and an exact test silently switched
    // the sharper bound off for every overview whose cell was not a binary
    // fraction - which cut the proven open water of a small world from a half
    // to a tenth without changing a line of the composition.
    if (ceiling >= sea && floor <= sea &&
        rect.maxX - rect.minX <= overview_->cellWidthMetres * 2 &&
        rect.maxY - rect.minY <= overview_->cellHeightMetres * 2) {
        const auto sampled = sampledBounds(rect);
        floor = std::max(floor, sampled[0]);
        ceiling = std::min(ceiling, sampled[1]);
    }
    if (ceiling < sea) return Coverage::OpenWater;
    if (floor > sea) return Coverage::Land;
    return Coverage::Mixed;
}

bool LandCoverage::mayHaveWater(const WorldRect& rect, double haloMetres) const {
    if (!rect.valid() || !std::isfinite(haloMetres) || haloMetres < 0) return true;
    // Dry means dry with room to spare: a page whose neighbour holds the sea
    // still needs its own water surface where the two meet. Lakes and rivers
    // are not decided here, so an inland rectangle is only provisionally dry
    // and the hydrology stage may put water back into it.
    return classify(rect.grown(haloMetres)) != Coverage::Land;
}

} // namespace generation
