#include "framework.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "game/generation/terrain_foundation.hpp"

namespace {
using core::Fixed;
using generation::TerrainFoundation;
using generation::TerrainStage;

// A foundation built by hand, so the reconstruction can be measured against a
// grid whose answer is known.
TerrainFoundation gridOf(int columns, int rows, const std::function<double(int, int)>& height) {
    TerrainFoundation f;
    f.columns = columns;
    f.rows = rows;
    for (auto& plane : f.heightDm) {
        plane.resize(std::size_t(columns) * rows);
        for (int y = 0; y < rows; ++y)
            for (int x = 0; x < columns; ++x)
                plane[std::size_t(y) * columns + x] = std::int32_t(std::lround(height(x, y) * 10));
    }
    return f;
}

double read(const TerrainFoundation& f, double x, double y) {
    return f.sample(Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y),
                    TerrainStage::Final)
            .toDouble();
}
}

TEST(foundation_reconstruction_has_no_crease_along_its_own_grid_lines) {
    // Between the samples of a sixty-four metre grid there is nothing but this
    // function, and bilinear between them gives flat facets with a crease along
    // every grid line. No amount of detail laid on top hides a crease in what
    // carries it - which is why the ground in this world reads as folded paper
    // at any distance where the grid shows.
    //
    // Measured as the second difference across a line, which is what a crease
    // is: a jump in slope.
    const auto hill = gridOf(9, 9, [](int x, int y) {
        return 400.0 + 120.0 * std::sin(x * 0.7) * std::cos(y * 0.5) + 6.0 * x;
    });
    const int step = TerrainFoundation::kStep;
    double worstBend = 0;
    // Walk across three grid lines, taking the curvature at each metre.
    for (double x = step * 1.5; x < step * 4.5; x += 1.0) {
        const double a = read(hill, x - 1, step * 3 + 7);
        const double b = read(hill, x, step * 3 + 7);
        const double c = read(hill, x + 1, step * 3 + 7);
        worstBend = std::max(worstBend, std::abs(a - 2 * b + c));
    }
    // A bilinear reconstruction of this grid bends by more than a third of a
    // metre in one metre at every grid line; a cubic one does not.
    CHECK(worstBend < 0.06);
}

TEST(foundation_reconstruction_invents_no_peak_between_two_samples) {
    // The reason it is monotone Hermite and not an ordinary cubic. An ordinary
    // one overshoots, and an overshoot in a height field is a bump in the
    // middle of a plain or a dip in the middle of a ridge - shape that nothing
    // asked for and that no later stage can tell from shape that was meant.
    const auto ramp = gridOf(7, 7, [](int x, int) { return 100.0 + 45.0 * x; });
    const int step = TerrainFoundation::kStep;
    for (double x = 0; x <= step * 6; x += 2.0) {
        const int cell = std::min(5, int(x) / step);
        const double low = 100.0 + 45.0 * cell, high = low + 45.0;
        const double got = read(ramp, x, step * 3);
        CHECK(got >= low - 0.05);
        CHECK(got <= high + 0.05);
    }

    // And a plateau beside a step stays a plateau: no dip before the rise.
    const auto shelf = gridOf(7, 7, [](int x, int) { return x < 3 ? 100.0 : 300.0; });
    // Samples 0..2 are the low shelf and 3..6 the high one, so the rise lives
    // in the cell between them and nowhere else.
    for (double x = 0; x <= step * 2; x += 2.0) {
        CHECK(read(shelf, x, step * 3) <= 100.05);
        CHECK(read(shelf, x, step * 3) >= 99.95);
    }
    for (double x = step * 3; x <= step * 6; x += 2.0) {
        CHECK(read(shelf, x, step * 3) >= 299.95);
        CHECK(read(shelf, x, step * 3) <= 300.05);
    }
}

TEST(foundation_reconstruction_is_the_grid_at_the_grid) {
    // Whatever it does between them, it has to pass through the samples - or
    // the surface is not the one the stages built.
    const auto rough = gridOf(8, 8, [](int x, int y) {
        return 250.0 + 70.0 * std::sin(x * 1.3 + y * 0.4) - 30.0 * std::cos(y * 0.9);
    });
    const int step = TerrainFoundation::kStep;
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const double wanted =
                    std::lround((250.0 + 70.0 * std::sin(x * 1.3 + y * 0.4) -
                                 30.0 * std::cos(y * 0.9)) * 10) / 10.0;
            CHECK(std::abs(read(rough, x * step, y * step) - wanted) < 0.05);
        }
}
