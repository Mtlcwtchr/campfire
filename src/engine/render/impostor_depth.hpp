#pragma once
#include "engine/camera/view_state.hpp"
#include <cstdint>
#include <optional>

namespace engine::render {
// RG: big-endian UNORM16 signed depth/width; B: azimuth index; A: coverage.
// Depth is positive towards the baked eye (-sin(angle), cos(angle), 0).
// Point-sample this encoding: averaging its bytes destroys the depth value.
inline double impostorDepth(std::uint8_t high, std::uint8_t low, double width) {
    return (double((std::uint32_t(high)<<8)|low)/65535.0-0.5)*width;
}
struct ImpostorHit { camera::Vec3 position; std::array<double,2> uv; };
inline std::optional<ImpostorHit> intersectImpostorDepth(camera::Vec3 plane,
        camera::Vec3 towardEye, double depth, double width, double height, unsigned view) {
    if (!(width>0 && height>0) || !std::isfinite(width+height+depth) || view>=8) return {};
    for (double v:plane) if (!std::isfinite(v)) return {};
    for (double v:towardEye) if (!std::isfinite(v)) return {};
    const double angle=view*std::numbers::pi/4, c=std::cos(angle), s=std::sin(angle);
    const camera::Vec3 right{c,s,0}, eye{-s,c,0};
    const double denominator=camera::dot(towardEye,eye);
    // A side atlas cannot describe a grazing/top-down ray or its own back side.
    if (denominator<=0.2) return {};
    const double t=(depth-camera::dot(plane,eye))/denominator;
    ImpostorHit hit;
    for (int i=0;i<3;++i) hit.position[i]=plane[i]+towardEye[i]*t;
    hit.uv={camera::dot(hit.position,right)/width+0.5,1-hit.position[2]/height};
    return hit;
}
} // namespace engine::render
