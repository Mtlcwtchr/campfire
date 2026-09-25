#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace engine {
struct SmartSpriteMesh {
    struct Vertex { float corner[2]; float uv[2]; };
    float halfWidth = 1, height = 1;
    unsigned views = 1;
    std::array<Vertex, 4> vertices() const {
        if (!(halfWidth > 0) || !(height > 0) || !std::isfinite(halfWidth + height))
            throw std::invalid_argument("invalid sprite mesh extent");
        return {{{{-halfWidth, 0}, {0, 1}}, {{halfWidth, 0}, {1, 1}},
                 {{-halfWidth, height}, {0, 0}}, {{halfWidth, height}, {1, 0}}}};
    }
    static constexpr std::array<std::uint16_t, 6> indices{0, 2, 3, 0, 3, 1};
    unsigned viewIndex(double cameraAngle, double yaw) const {
        if (!views || views > 256 || !std::isfinite(cameraAngle) || !std::isfinite(yaw))
            throw std::invalid_argument("invalid impostor views/angle");
        constexpr double turn = 6.2831853071795864769;
        double angle = std::fmod(cameraAngle - yaw, turn);
        if (angle < 0) angle += turn;
        return static_cast<unsigned>(std::floor(angle * views / turn + 0.5)) % views;
    }
};
} // namespace engine
