#pragma once
#include <algorithm>
#include <cmath>
#include <queue>
#include "engine/camera/view_state.hpp"
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
    void predictPriority(const engine::camera::ViewState& camera) {
        // An orthographic eye can be kilometres away from its visible focus.
        // Panning/zooming a map must not spend its bounded demand around that eye.
        if (camera.orthographic) return;
        const auto predicted=camera.predictedPosition();
        priorityX=predicted[0];priorityY=predicted[1];
    }
};

struct SceneRegionBudget {
    std::size_t regions=512, nodes=8192;
};
struct SceneRegionStats {
    std::size_t visited=0;
    bool limited=false; // traversal stopped with unexplored candidates, not an exact omitted count
};

// Stable, disjoint 128 m ownership regions, nearest-first. The bounded query
// returns a priority prefix, not a claim of complete world visibility.
inline std::vector<ScatterBounds> visibleSceneRegions(const SceneView& view,
        SceneRegionBudget budget={}, SceneRegionStats* stats=nullptr) {
    SceneRegionStats query;
    if (stats) *stats=query;
    std::vector<ScatterBounds> regions;
    const auto& m=view.matrix;
    const auto& world=view.world;
    if (!(world.maxX>world.minX && world.maxY>world.minY && view.maxExtent>0) ||
        !std::isfinite(world.minX+world.minY+world.maxX+world.maxY+world.low+world.high+
                       view.maxExtent+view.viewportWidth+view.pixelsPerMetre) ||
        !std::isfinite(view.priorityX) || !std::isfinite(view.priorityY)) return regions;
    // Keep floor/ceil conversions, subdivision differences and metre products
    // representable even for invalid caller-supplied world extents.
    constexpr double coordinateLimit=0x1p62;
    for (double value:{world.minX,world.minY,world.maxX,world.maxY})
        if (std::abs(value)>=coordinateLimit) return regions;
    for (float value:m) if (!std::isfinite(value)) return regions;
    const bool perspective=m[12]!=0 || m[13]!=0 || m[14]!=0;
    const double focal=std::hypot(std::hypot(double(m[0]),double(m[1])),double(m[2]))*
                       view.viewportWidth*0.5;
    if (!perspective && view.maxExtent*view.pixelsPerMetre<=kImpostorFloorPixels) return regions;
    const auto minX=std::int64_t(std::floor(world.minX/kRegion));
    const auto minY=std::int64_t(std::floor(world.minY/kRegion));
    const auto maxX=std::int64_t(std::ceil(world.maxX/kRegion));
    const auto maxY=std::int64_t(std::ceil(world.maxY/kRegion));
    std::array<std::array<double,4>,6> planes{};
    const int planeCount=perspective?6:4;
    for (int plane=0;plane<planeCount;++plane) {
        const int row=(plane/2)*4;
        const double sign=plane%2?-1.0:1.0;
        const double w=plane==4?0.0:(plane<4?1.05:1.0);
        for (int axis=0;axis<4;++axis) planes[plane][axis]=w*m[12+axis]+sign*m[row+axis];
    }
    const auto visible=[&](const terrain::ViewBounds& box) {
        for (int i=0;i<planeCount;++i) {
            const auto& p=planes[i];
            const double high=p[3]+p[0]*(p[0]>=0?box.maxX:box.minX)+
                p[1]*(p[1]>=0?box.maxY:box.minY)+p[2]*(p[2]>=0?box.high:box.low);
            if (high<0) return false;
        }
        return true;
    };
    struct Node { double distance; ScatterBounds cells; };
    const auto farther=[](const Node& a,const Node& b) {
        return a.distance!=b.distance?a.distance>b.distance:a.cells>b.cells;
    };
    std::priority_queue<Node,std::vector<Node>,decltype(farther)> pending(farther);
    const auto push=[&](std::int64_t x0,std::int64_t y0,std::int64_t x1,std::int64_t y1) {
        const double dx=view.priorityX-std::clamp(view.priorityX,double(x0*kRegion),double(x1*kRegion));
        const double dy=view.priorityY-std::clamp(view.priorityY,double(y0*kRegion),double(y1*kRegion));
        pending.push({std::hypot(dx,dy),{x0,y0,x1,y1}});
    };
    push(minX,minY,maxX,maxY);
    while (!pending.empty() && regions.size()<budget.regions && query.visited<budget.nodes) {
        const auto [x0,y0,x1,y1]=pending.top().cells;
        pending.pop();++query.visited;
        const double pad=view.maxExtent+2; // horizontal crown, wind and sunk roots
        terrain::ViewBounds box{double(x0*kRegion)-pad,double(y0*kRegion)-pad,
            double(x1*kRegion)+pad,double(y1*kRegion)+pad,world.low-pad,world.high+pad};
        if (!visible(box)) continue;
        if (perspective) {
            double nearest=m[15];
            const double lo[]{box.minX,box.minY,box.low},hi[]{box.maxX,box.maxY,box.high};
            for (int i=0;i<3;++i) nearest+=std::min(m[12+i]*lo[i],m[12+i]*hi[i]);
            if (nearest>0 && view.maxExtent*focal/nearest<=kImpostorFloorPixels) continue;
        }
        if (x1-x0==1 && y1-y0==1) {
            regions.push_back({x0*kRegion,y0*kRegion,x1*kRegion,y1*kRegion});
        } else if (x1-x0>=y1-y0) {
            const auto mid=x0+(x1-x0)/2;
            push(x0,y0,mid,y1);push(mid,y0,x1,y1);
        } else {
            const auto mid=y0+(y1-y0)/2;
            push(x0,y0,x1,mid);push(x0,mid,x1,y1);
        }
    }
    query.limited=!pending.empty();
    if (stats) *stats=query;
    return regions;
}
} // namespace world::decor

