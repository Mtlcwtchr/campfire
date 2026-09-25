#pragma once
// Dropping what the ground is standing in front of.
//
// Frustum culling answers "is it in the view". This answers the other question
// nobody had been asking: "is there a hill in the way". On a landscape that is
// most of the world - a valley's far side hides everything behind it, and a
// forest on the wrong side of a ridge was being transformed, sorted, levelled
// and drawn so the depth test could throw every pixel of it away.
//
// The occluder is the ground itself, described as a HORIZON: for each direction
// from the eye, the highest angle the terrain reaches and how far out it
// reaches it. A thing further away than that, and lower than that angle, cannot
// be seen from here. That is the whole test, and it costs one array lookup.
//
// It is built to be wrong in one direction only. Every sample taken is the
// LOWEST ground in its neighbourhood, and the angle recorded is the lowest of
// the samples across a bin's wedge, so the horizon is at or below the real
// skyline. Something may therefore survive that could have been culled; nothing
// can be culled that could have been seen. A visible thing vanishing is a bug
// anyone can see and nobody can explain, and it is not worth the last few per
// cent of a cull.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace engine::render {

struct Horizon {
    // Five hundred and twelve directions: a wedge is a bit over a degree, which
    // at a kilometre is eleven metres - narrower than the ground samples the
    // horizon is built from, so a wedge is covered by the samples marched
    // through it rather than falling between them.
    static constexpr int kBins = 512;

    // tan of the highest elevation angle in this direction, and the distance to
    // the ground that set it. Both stay at their empty values until something
    // is raised, and an empty bin hides nothing.
    std::array<float, kBins> rise{};
    std::array<float, kBins> reach{};
    float eyeX = 0, eyeY = 0, eyeZ = 0;
    bool active = false;

    void clear() {
        rise.fill(-1e9f);
        reach.fill(0.0f);
        active = false;
    }

    static int binOf(double x, double y) {
        // atan2 is in [-pi, pi]; the bin is its fraction of a turn.
        const double turn = std::atan2(y, x) * (0.5 / 3.14159265358979323846) + 0.5;
        const int bin = int(turn * kBins);
        return bin < 0 ? 0 : bin >= kBins ? kBins - 1 : bin;
    }

    // Raise the skyline in one direction, at a distance. Only ever raises: the
    // horizon is a maximum over everything the ground does out there.
    void raise(double dx, double dy, double height) {
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (!(distance > 1.0)) return;
        const auto slope = float((height - eyeZ) / distance);
        const int bin = binOf(dx, dy);
        if (slope > rise[bin]) {
            rise[bin] = slope;
            reach[bin] = float(distance);
        }
        active = true;
    }

    // Whether a sphere is behind the skyline. Its TOP is what is tested, and
    // every bin its angular width touches has to hide it, so a thing straddling
    // two directions is kept unless both of them bury it.
    [[nodiscard]] bool hides(float x, float y, float z, float radius) const {
        if (!active) return false;
        const double dx = double(x) - eyeX, dy = double(y) - eyeY;
        const double distance = std::sqrt(dx * dx + dy * dy);
        if (!(distance > radius + 1.0)) return false;
        const double top = (double(z) + radius - eyeZ) / distance;
        // How many bins the sphere spans, plus one either side for the sampling
        // the horizon itself was built at.
        const double halfTurn = std::asin(std::min(1.0, radius / distance)) *
                                (0.5 / 3.14159265358979323846);
        const int spread = int(halfTurn * kBins) + 1;
        const int middle = binOf(dx, dy);
        for (int offset = -spread; offset <= spread; ++offset) {
            const int bin = ((middle + offset) % kBins + kBins) % kBins;
            // Nothing recorded here, or the ground that was recorded is further
            // away than the sphere: it cannot be in front of it.
            if (reach[bin] <= 0.0f || double(reach[bin]) >= distance - radius) return false;
            if (top >= double(rise[bin])) return false;
        }
        return true;
    }
};

// Where the eye is, from the view-projection alone.
//
// Three of its rows are three planes through the eye - the two screen axes and
// the depth - so the eye is where they meet, and a three by three solve finds
// it. Worth having here rather than passed in: every caller that has a matrix
// has one of these, and a horizon built from the wrong eye is worse than no
// horizon at all. Returns false for a matrix with no single centre of
// projection, which is what an orthographic frame is.
inline bool eyeFrom(const float rowX[4], const float rowY[4], const float rowW[4], double out[3]) {
    const double a[3][4]{{rowX[0], rowX[1], rowX[2], -double(rowX[3])},
                         {rowY[0], rowY[1], rowY[2], -double(rowY[3])},
                         {rowW[0], rowW[1], rowW[2], -double(rowW[3])}};
    const double determinant =
            a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) -
            a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) +
            a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(std::abs(determinant) > 1e-12)) return false;
    const auto column = [&](int c) {
        double m[3][3];
        for (int r = 0; r < 3; ++r)
            for (int k = 0; k < 3; ++k) m[r][k] = k == c ? a[r][3] : a[r][k];
        return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
               m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
               m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    };
    for (int i = 0; i < 3; ++i) out[i] = column(i) / determinant;
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

} // namespace engine::render
