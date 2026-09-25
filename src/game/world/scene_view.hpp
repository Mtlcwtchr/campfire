#pragma once
#include <cmath>
#include "game/world/scene_scatter.hpp"
#include "game/world/terrain_view.hpp"

namespace world::decor {
// Maximum extent already includes the largest placement scale. Height bounds
// include the terrain's detail envelope, not just the height at camera focus.
struct SceneView {
    std::array<float,16> matrix{};
    terrain::ViewBounds world;
    double viewportWidth=1, pixelsPerMetre=0, maxExtent=0;
    double priorityX=0, priorityY=0;
};

// Stable, disjoint 128 m ownership regions. Work limits belong to the streamer,
// never to visibility: a large far field must not cancel the nearby demand.
inline std::vector<ScatterBounds> visibleSceneRegions(const SceneView& view) {
    std::vector<ScatterBounds> regions;
    const auto& m=view.matrix;
    const auto& world=view.world;
    if (!(world.maxX>world.minX && world.maxY>world.minY && view.maxExtent>0) ||
        !std::isfinite(world.minX+world.minY+world.maxX+world.maxY+world.low+world.high+
                       view.maxExtent+view.viewportWidth+view.pixelsPerMetre)) return regions;
    for (float value:m) if (!std::isfinite(value)) return regions;
    const bool perspective=m[12]!=0 || m[13]!=0 || m[14]!=0;
    const double focal=std::hypot(std::hypot(double(m[0]),double(m[1])),double(m[2]))*
                       view.viewportWidth*0.5;
    if (!perspective && view.maxExtent*view.pixelsPerMetre<=kImpostorFloorPixels) return regions;
    const auto minX=std::int64_t(std::floor(world.minX/kRegion));
    const auto minY=std::int64_t(std::floor(world.minY/kRegion));
    const auto maxX=std::int64_t(std::ceil(world.maxX/kRegion));
    const auto maxY=std::int64_t(std::ceil(world.maxY/kRegion));
    const auto visit=[&](auto&& self,std::int64_t x0,std::int64_t y0,
                         std::int64_t x1,std::int64_t y1)->void {
        const double pad=view.maxExtent+2; // horizontal crown, wind and sunk roots
        terrain::ViewBounds box{double(x0*kRegion)-pad,double(y0*kRegion)-pad,
            double(x1*kRegion)+pad,double(y1*kRegion)+pad,world.low-pad,world.high+pad};
        if (!terrain::intersectsView(box,m,0.05,0.05)) return;
        if (perspective) {
            double nearest=m[15];
            const double lo[]{box.minX,box.minY,box.low},hi[]{box.maxX,box.maxY,box.high};
            for (int i=0;i<3;++i) nearest+=std::min(m[12+i]*lo[i],m[12+i]*hi[i]);
            if (nearest>0 && view.maxExtent*focal/nearest<=kImpostorFloorPixels) return;
        }
        if (x1-x0==1 && y1-y0==1) {
            regions.push_back({x0*kRegion,y0*kRegion,x1*kRegion,y1*kRegion});
        } else if (x1-x0>=y1-y0) {
            const auto mid=x0+(x1-x0)/2;
            self(self,x0,y0,mid,y1);self(self,mid,y0,x1,y1);
        } else {
            const auto mid=y0+(y1-y0)/2;
            self(self,x0,y0,x1,mid);self(self,x0,mid,x1,y1);
        }
    };
    visit(visit,minX,minY,maxX,maxY);
    const auto distance=[&](const ScatterBounds& b) {
        const double dx=view.priorityX-std::clamp(view.priorityX,double(b.minX),double(b.maxX));
        const double dy=view.priorityY-std::clamp(view.priorityY,double(b.minY),double(b.maxY));
        return dx*dx+dy*dy;
    };
    std::sort(regions.begin(),regions.end(),[&](const auto& a,const auto& b) {
        const auto da=distance(a),db=distance(b);
        return da!=db?da<db:a<b;
    });
    return regions;
}
} // namespace world::decor

