#include "framework.hpp"
#include <set>
#include "game/client/camera.hpp"
#include "game/world/scene_view.hpp"

namespace {
world::decor::SceneView viewOf(const client::Camera& camera) {
    world::decor::SceneView view;
    camera.viewProjection(view.matrix.data(),0,1e-5);
    view.world={0,0,8192,8192,0,100};
    view.maxExtent=20;
    view.viewportWidth=camera.viewportWidth;
    view.pixelsPerMetre=camera.pixelsPerTile;
    view.priorityX=camera.centreX;view.priorityY=camera.centreY;
    return view;
}
bool covers(const std::vector<world::decor::ScatterBounds>& regions,double x,double y) {
    return std::any_of(regions.begin(),regions.end(),[&](const auto& r) {
        return x>=r.minX && x<r.maxX && y>=r.minY && y<r.maxY;
    });
}
}

TEST(scene_view_wide_coverage_is_not_a_focus_circle_or_single_job_budget) {
    client::Camera camera;
    camera.isometric=false;camera.centreX=camera.centreY=4096;
    camera.viewportWidth=camera.viewportHeight=1000;camera.pixelsPerTile=0.2;
    auto view=viewOf(camera);
    const auto regions=world::decor::visibleSceneRegions(view);
    CHECK(regions.size()*256>world::decor::kMaxScatterCells);
    CHECK(covers(regions,1800,1800));
    CHECK(covers(regions,6300,6300));
    CHECK(covers(regions,4096,4096));
    CHECK(!covers(regions,0,0));
    CHECK_EQ(std::set<world::decor::ScatterBounds>(regions.begin(),regions.end()).size(),regions.size());
    // All these objects are billboards below the mesh floor, but still visible.
    CHECK(view.maxExtent*view.pixelsPerMetre<world::decor::kMeshFloorPixels);
    view.pixelsPerMetre=0.01;
    CHECK(world::decor::visibleSceneRegions(view).empty());
}

TEST(scene_view_perspective_horizon_keeps_near_and_far_visible_regions) {
    client::Camera camera;
    camera.mode=client::Camera::Mode::Free;
    camera.centreX=7000;camera.centreY=4096;camera.focusHeight=100;
    camera.yaw=camera.pitch=0;
    camera.viewportWidth=1280;camera.viewportHeight=800;
    camera.farPlane=6000;
    auto view=viewOf(camera);
    const auto regions=world::decor::visibleSceneRegions(view);
    CHECK(covers(regions,6500,4096));
    CHECK(covers(regions,2000,4096)); // five kilometres from the eye
    CHECK(!covers(regions,7800,4096)); // behind the eye
    CHECK(!covers(regions,100,4096)); // beyond far clip
    CHECK(regions.size()>256);
    for (int sx:{20,640,1260}) for (int sy:{500,650,780}) {
        const auto ray=camera.screenRay(sx,sy);
        const double t=-ray.origin[2]/ray.direction[2];
        if (t<=camera.nearPlane || t>=camera.farPlane) continue;
        const double x=ray.origin[0]+ray.direction[0]*t,y=ray.origin[1]+ray.direction[1]*t;
        if (x>=0 && y>=0 && x<8192 && y<8192) CHECK(covers(regions,x,y));
    }
}

TEST(scene_view_crowns_can_enter_the_view_without_their_roots) {
    client::Camera camera;
    camera.centreX=camera.centreY=4096;
    camera.isometric=true;camera.pixelsPerTile=2;
    camera.yaw=0.9;camera.pitch=0.4;
    auto view=viewOf(camera);
    view.world.high=1200;
    const auto regions=world::decor::visibleSceneRegions(view);
    for (double z:{0.0,600.0,1220.0}) for (int x:{0,640,1280}) for (int y:{0,400,800}) {
        double wx,wy;
        camera.worldOfScreenAtHeight(x,y,z,wx,wy);
        if (wx>=0 && wy>=0 && wx<8192 && wy<8192) CHECK(covers(regions,wx,wy));
    }
}

