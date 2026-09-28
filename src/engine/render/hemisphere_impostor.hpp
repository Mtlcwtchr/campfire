#pragma once
#include "engine/render/impostor_depth.hpp"

namespace engine::render {
inline constexpr unsigned kHemisphereViews = 21;
inline constexpr const char* kHemisphereLayout = "rings-8-8-4-1-v1";
struct ImpostorBasis { camera::Vec3 right, up, eye; };
inline std::optional<ImpostorBasis> hemisphereBasis(unsigned view) {
    if (view >= kHemisphereViews) return {};
    static const auto frames=[] {
        std::array<ImpostorBasis,kHemisphereViews> result{};
        constexpr unsigned first[]{0,8,16,20}, count[]{8,8,4,1};
        constexpr double elevation[]{0,45,70,90};
        for (unsigned i=0;i<kHemisphereViews;++i) {
            const unsigned ring=i<8?0:i<16?1:i<20?2:3;
            const double angle=(i-first[ring])*2*std::numbers::pi/count[ring];
            const double e=elevation[ring]*std::numbers::pi/180;
            const double c=std::cos(angle),s=std::sin(angle),ce=std::cos(e),se=std::sin(e);
            result[i]={{c,s,0},{s*se,-c*se,ce},{-s*ce,c*ce,se}};
        }
        return result;
    }();
    return frames[view];
}
struct HemisphereSelection {
    std::array<unsigned,4> views{};
    std::array<double,4> weights{};
    double maxAngle = 0;
};
inline std::optional<HemisphereSelection> selectHemisphere(camera::Vec3 direction, double yaw=0) {
    for (double v:direction) if (!std::isfinite(v)) return {};
    const double lengthSquared=camera::dot(direction,direction);
    if (!std::isfinite(yaw) || !std::isfinite(lengthSquared) || lengthSquared<1e-12 || direction[2]<0) return {};
    direction=camera::normalized(direction);
    const double c=std::cos(yaw),s=std::sin(yaw);
    direction={c*direction[0]+s*direction[1],-s*direction[0]+c*direction[1],direction[2]};
    const double elevation=std::asin(std::clamp(direction[2],0.0,1.0))*180/std::numbers::pi;
    const unsigned low=elevation<45?0:elevation<70?1:2, high=low+1;
    constexpr double elevations[]{0,45,70,90};
    constexpr unsigned first[]{0,8,16,20}, count[]{8,8,4,1};
    const double blend=std::clamp((elevation-elevations[low])/(elevations[high]-elevations[low]),0.0,1.0);
    const double angle=(std::atan2(direction[1],direction[0])-std::numbers::pi/2)/(2*std::numbers::pi);
    const double turn=angle-std::floor(angle);
    HemisphereSelection result;
    for (unsigned band=0;band<2;++band) {
        const auto ring=band?high:low;
        const double at=turn*count[ring], fraction=at-std::floor(at), weight=band?blend:1-blend;
        const auto index=unsigned(std::floor(at))%count[ring];
        result.views[band*2]=first[ring]+index;
        result.views[band*2+1]=first[ring]+(index+1)%count[ring];
        result.weights[band*2]=weight*(1-fraction);
        result.weights[band*2+1]=weight*fraction;
    }
    double worstDot=1;
    for (unsigned i=0;i<4;++i) if (result.weights[i]>1e-8)
        worstDot=std::min(worstDot,camera::dot(direction,hemisphereBasis(result.views[i])->eye));
    result.maxAngle=std::acos(std::clamp(worstDot,-1.0,1.0));
    return result;
}
inline std::optional<ImpostorHit> intersectHemisphereDepth(camera::Vec3 plane,
        camera::Vec3 towardEye,double depth,double side,unsigned view) {
    const auto basis=hemisphereBasis(view);
    if (!basis || !(side>0) || !std::isfinite(side+depth)) return {};
    for (double v:plane) if (!std::isfinite(v)) return {};
    for (double v:towardEye) if (!std::isfinite(v)) return {};
    const double denominator=camera::dot(towardEye,basis->eye);
    if (denominator<=0.2) return {};
    const double t=(depth-camera::dot(plane,basis->eye))/denominator;
    ImpostorHit hit;
    for (unsigned i=0;i<3;++i) hit.position[i]=plane[i]+towardEye[i]*t;
    hit.uv={camera::dot(hit.position,basis->right)/side+0.5,
            0.5-camera::dot(hit.position,basis->up)/side};
    return hit;
}
} // namespace engine::render
