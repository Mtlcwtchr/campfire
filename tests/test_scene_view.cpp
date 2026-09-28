#include "framework.hpp"
#include <limits>
#include <set>
#include "game/client/camera.hpp"
#include "game/world/scene_view.hpp"

namespace {
constexpr world::decor::SceneRegionBudget fullView{8192,32768};
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
    const auto regions=world::decor::visibleSceneRegions(view,fullView);
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
    const auto regions=world::decor::visibleSceneRegions(view,fullView);
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
    const auto regions=world::decor::visibleSceneRegions(view,fullView);
    for (double z:{0.0,600.0,1220.0}) for (int x:{0,640,1280}) for (int y:{0,400,800}) {
        double wx,wy;
        camera.worldOfScreenAtHeight(x,y,z,wx,wy);
        if (wx>=0 && wy>=0 && wx<8192 && wy<8192) CHECK(covers(regions,wx,wy));
    }
}

TEST(scene_view_bounded_query_is_the_nearest_deterministic_prefix) {
    client::Camera camera;
    camera.isometric=false;camera.centreX=camera.centreY=4096;
    camera.viewportWidth=camera.viewportHeight=1000;camera.pixelsPerTile=0.2;
    auto view=viewOf(camera);
    view.priorityX=4093;view.priorityY=4001;
    auto expected=world::decor::visibleSceneRegions(view,fullView);
    const auto distance=[&](const auto& r) {
        return std::hypot(view.priorityX-std::clamp(view.priorityX,double(r.minX),double(r.maxX)),
                          view.priorityY-std::clamp(view.priorityY,double(r.minY),double(r.maxY)));
    };
    std::sort(expected.begin(),expected.end(),[&](const auto& a,const auto& b) {
        return distance(a)!=distance(b)?distance(a)<distance(b):a<b;
    });
    CHECK(expected.size()>512);
    for (const std::size_t count:{0u,1u,7u,64u,512u}) {
        world::decor::SceneRegionStats stats;
        const auto actual=world::decor::visibleSceneRegions(view,{count,8192},&stats);
        CHECK_EQ(actual.size(),count);
        CHECK_EQ(actual,(std::vector<world::decor::ScatterBounds>(expected.begin(),expected.begin()+count)));
        CHECK(stats.limited);CHECK(stats.visited<=8192);
        CHECK_EQ(actual,world::decor::visibleSceneRegions(view,{count,8192}));
    }
}

TEST(scene_view_huge_world_bounds_work_before_enumerating_the_far_field) {
    world::decor::SceneView view;
    view.world={-1e9,-1e9,1e9,1e9,0,100};
    view.matrix={1e-9f,0,0,0,0,1e-9f,0,0,0,0,1,0,0,0,0,1};
    view.maxExtent=20;view.pixelsPerMetre=1;view.viewportWidth=1000;
    view.priorityX=13;view.priorityY=-39;
    world::decor::SceneRegionStats stats;
    const auto regions=world::decor::visibleSceneRegions(view,{},&stats);
    CHECK_EQ(regions.size(),std::size_t(512));
    CHECK(stats.visited<=8192);CHECK(stats.limited);
    CHECK(covers(regions,view.priorityX,view.priorityY));
    CHECK_EQ(std::set<world::decor::ScatterBounds>(regions.begin(),regions.end()).size(),regions.size());
    CHECK(world::decor::visibleSceneRegions(view,{512,1},&stats).empty());
    CHECK_EQ(stats.visited,std::size_t(1));CHECK(stats.limited);
    CHECK(world::decor::visibleSceneRegions(view,{512,0},&stats).empty());
    CHECK_EQ(stats.visited,std::size_t(0));CHECK(stats.limited);
    view.priorityX=std::numeric_limits<double>::quiet_NaN();
    CHECK(world::decor::visibleSceneRegions(view,{},&stats).empty());
    CHECK_EQ(stats.visited,std::size_t(0));CHECK(!stats.limited);
    view.priorityX=0;view.world.maxX=1e30;
    CHECK(world::decor::visibleSceneRegions(view).empty());
}

TEST(scene_view_map_budget_tracks_focus_not_the_distant_orthographic_eye) {
    client::Camera camera;
    camera.centreX=camera.centreY=4096;camera.pixelsPerTile=0.02;
    auto view=viewOf(camera);view.maxExtent=80;
    const auto state=camera.viewState({100,200,0});
    CHECK(state.orthographic);
    CHECK(std::hypot(state.position[0]-camera.centreX,state.position[1]-camera.centreY)>10000);
    view.predictPriority(state);
    CHECK_EQ(view.priorityX,camera.centreX);CHECK_EQ(view.priorityY,camera.centreY);
    const auto regions=world::decor::visibleSceneRegions(view,{8,8192});
    CHECK(covers(regions,camera.centreX,camera.centreY));
    camera.mode=client::Camera::Mode::Free;
    const auto perspective=camera.viewState({20,-12,0});
    view.predictPriority(perspective);
    CHECK_EQ(view.priorityX,perspective.predictedPosition()[0]);
    CHECK_EQ(view.priorityY,perspective.predictedPosition()[1]);
}

