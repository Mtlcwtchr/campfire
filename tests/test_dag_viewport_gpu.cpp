#include "framework.hpp"
#include "engine/render/dag_viewport.hpp"
#include "engine/editor/module_panel.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace {
struct Video {
    Video() { SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP,"1"); CHECK(SDL_Init(SDL_INIT_VIDEO)); }
    ~Video() { SDL_Quit(); }
};
struct Probe {
    Video video;
    engine::DagViewport viewport;
    engine::camera::Camera camera;
    bool ready;
    Probe() : ready(viewport.open(nullptr,std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites") && viewport.resize(96,96)) {
        if (!ready) std::cerr<<viewport.error()<<'\n';
        camera.viewportWidth=camera.viewportHeight=96;
        camera.pixelsPerTile=2;
    }
    std::vector<std::uint8_t> pixels(int width=96,int height=96) {
        std::vector<std::uint8_t> result(std::size_t(width)*height*4);
        CHECK(viewport.readPixels(result));return result;
    }
};
engine::geometry::ClusterDag mesh(float z=0) {
    engine::geometry::ClusterDag dag;
    dag.positions={-18,-18,z,18,-18,z,0,18,z};
    dag.indices={0,1,2};
    engine::geometry::MeshCluster cluster;cluster.indices={0,3};
    dag.clusters.push_back(cluster);
    return dag;
}
bool background(const std::vector<std::uint8_t>& p,std::size_t pixel) {
    const auto at=pixel*4;
    return p[at]==22 && p[at+1]==30 && p[at+2]==40;
}
using Surface=std::unique_ptr<SDL_Surface,decltype(&SDL_DestroySurface)>;
}

TEST(viewport_depth_is_independent_of_cluster_draw_order) {
    Probe p;CHECK(p.ready);if (!p.ready) return;
    auto dag=mesh(1000);
    const auto lower=mesh();
    dag.positions.insert(dag.positions.end(),lower.positions.begin(),lower.positions.end());
    dag.indices.insert(dag.indices.end(),{3,4,5});
    auto cluster=dag.clusters.front();cluster.indices={3,3};dag.clusters.push_back(cluster);
    CHECK(p.viewport.mesh(dag,1));
    const std::array<std::uint32_t,1> upper{0};
    CHECK(p.viewport.draw(p.camera,{0,0,96,96},upper,false));const auto reference=p.pixels();
    CHECK(!background(reference,48*96+48));
    for (auto cut:{std::array<std::uint32_t,2>{0,1},std::array<std::uint32_t,2>{1,0}}) {
        CHECK(p.viewport.draw(p.camera,{0,0,96,96},cut,false));CHECK(p.pixels()==reference);
    }
}

TEST(viewport_uses_shared_camera_projection_and_scissor) {
    Probe p;CHECK(p.ready);if (!p.ready) return;
    CHECK(p.viewport.mesh(mesh(),1));
    const std::array<std::uint32_t,1> cut{0};
    using Mode=engine::camera::Camera::Mode;
    for (auto mode:{Mode::Map,Mode::Orbit,Mode::Free}) {
        p.camera.setMode(mode);
        p.camera.viewportWidth=p.camera.viewportHeight=64;
        CHECK(p.viewport.draw(p.camera,{16,16,64,64},cut,false));
        const auto pixels=p.pixels();std::size_t drawn=0;
        for (int y=0;y<96;++y) for (int x=0;x<96;++x) {
            if (x<16 || x>=80 || y<16 || y>=80) CHECK(background(pixels,y*96+x));
            else drawn+=!background(pixels,y*96+x);
        }
        CHECK(drawn>50);
    }
    CHECK(!p.viewport.draw(p.camera,{-1,0,96,96},cut,false));
    CHECK(!p.viewport.draw(p.camera,{0,0,96,96},std::array<std::uint32_t,1>{99},false));
}

TEST(viewport_clips_crossing_triangles_instead_of_dropping_them) {
    Probe p;CHECK(p.ready);if (!p.ready) return;
    p.camera.mode=engine::camera::Camera::Mode::Free;
    p.camera.yaw=p.camera.pitch=0;
    p.camera.nearPlane=1;p.camera.farPlane=100;
    auto dag=mesh();dag.positions={-0.5f,0,0.8f,-4,2,-1,-4,-2,-1};
    const std::array<std::uint32_t,1> cut{0};
    CHECK(p.viewport.mesh(dag,1));CHECK(p.viewport.draw(p.camera,{0,0,96,96},cut,false));
    auto pixels=p.pixels();std::size_t visible=0;
    for (std::size_t i=0;i<96*96;++i) visible+=!background(pixels,i);
    CHECK(visible>100);
    for (int i=0;i<3;++i) dag.positions[i*3]=-0.5f;
    CHECK(p.viewport.mesh(dag,2));CHECK(p.viewport.draw(p.camera,{0,0,96,96},cut,false));
    pixels=p.pixels();visible=0;
    for (std::size_t i=0;i<96*96;++i) visible+=!background(pixels,i);
    CHECK(visible==0);
}

TEST(viewport_camera_motion_keeps_mesh_resident_and_revisions_replace_it) {
    Probe p;CHECK(p.ready);if (!p.ready) return;
    auto dag=mesh();
    CHECK(p.viewport.mesh(dag,1));
    const auto bytes=p.viewport.residentBytes();CHECK(bytes==48);
    for (int i=0;i<8;++i) {
        p.camera.centreX=i;
        CHECK(p.viewport.mesh(dag,1));
        CHECK(p.viewport.draw(p.camera,{0,0,96,96},std::array<std::uint32_t,1>{0},false));
    }
    CHECK(p.viewport.meshUploads()==1);
    dag.positions[2]=500;
    CHECK(p.viewport.mesh(dag,2));CHECK(p.viewport.meshUploads()==2);
    dag.indices[0]=999;
    CHECK(!p.viewport.mesh(dag,3));CHECK(p.viewport.meshUploads()==2);
    CHECK(p.viewport.draw(p.camera,{0,0,96,96},std::array<std::uint32_t,1>{0},true));
    CHECK(p.viewport.mesh({},4));CHECK(p.viewport.residentBytes()==0);
    CHECK(p.viewport.draw(p.camera,{0,0,96,96},{},false));
    const auto empty=p.pixels();CHECK(background(empty,48*96+48));
}

TEST(viewport_overlay_uploads_only_changes_and_survives_resize) {
    Probe p;CHECK(p.ready);if (!p.ready) return;
    Surface surface(SDL_CreateSurface(96,96,SDL_PIXELFORMAT_RGBA32),SDL_DestroySurface);
    CHECK(bool(surface));if (!surface) return;
    CHECK(SDL_FillSurfaceRect(surface.get(),nullptr,SDL_MapSurfaceRGBA(surface.get(),0,0,0,0)));
    const SDL_Rect red{0,0,24,24};
    CHECK(SDL_FillSurfaceRect(surface.get(),&red,SDL_MapSurfaceRGBA(surface.get(),255,0,0,255)));
    for (int i=0;i<4;++i) CHECK(p.viewport.draw(p.camera,{0,0,96,96},{},false,surface.get()));
    CHECK(p.viewport.overlayUploads()==1);
    auto pixels=p.pixels();CHECK(pixels[0]==255);CHECK(pixels[1]==0);CHECK(background(pixels,48*96+48));
    CHECK(SDL_FillSurfaceRect(surface.get(),&red,SDL_MapSurfaceRGBA(surface.get(),0,255,0,255)));
    CHECK(p.viewport.draw(p.camera,{0,0,96,96},{},false,surface.get()));
    CHECK(p.viewport.overlayUploads()==2);
    pixels=p.pixels();CHECK(pixels[0]==0);CHECK(pixels[1]==255);
    CHECK(p.viewport.resize(64,80));CHECK(p.viewport.draw(p.camera,{0,0,64,80},{},false));
    pixels=p.pixels(64,80);CHECK(background(pixels,0));
    CHECK(p.viewport.resize(96,96));
    CHECK(p.viewport.draw(p.camera,{0,0,96,96},{},false,surface.get()));
    CHECK(p.viewport.overlayUploads()==3);
}

TEST(viewport_window_swapchain_survives_resize) {
    Video video;
    std::unique_ptr<SDL_Window,decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Viewport smoke",320,240,SDL_WINDOW_RESIZABLE),SDL_DestroyWindow);
    CHECK(bool(window));if (!window) return;
    engine::DagViewport viewport;
    CHECK(viewport.open(window.get(),std::filesystem::path(__FILE__).parent_path().parent_path()/"assets/sprites"));
    CHECK(viewport.mesh(mesh(),1));
    engine::camera::Camera camera;camera.pixelsPerTile=3;
    for (const auto size:{std::array<int,2>{320,240},std::array<int,2>{400,300},std::array<int,2>{240,180}}) {
        CHECK(SDL_SetWindowSize(window.get(),size[0],size[1]));SDL_PumpEvents();
        int w,h;CHECK(SDL_GetWindowSizeInPixels(window.get(),&w,&h));
        camera.viewportWidth=w;camera.viewportHeight=h;
        CHECK(viewport.resize(w,h));
        CHECK(viewport.draw(camera,{0,0,w,h},std::array<std::uint32_t,1>{0},false));
        viewport.wait();
    }
    CHECK(viewport.meshUploads()==1);
}

TEST(engine_panel_gpu_mode_leaves_viewport_transparent) {
    Video video;
    Surface surface(SDL_CreateSurface(900,760,SDL_PIXELFORMAT_RGBA32),SDL_DestroySurface);
    CHECK(bool(surface));if (!surface) return;
    auto* renderer=SDL_CreateSoftwareRenderer(surface.get());CHECK(renderer!=nullptr);if (!renderer) return;
    ui::Ui gui;CHECK(gui.init(renderer));
    engine::editor::ModulePanel panel;panel.wait();CHECK(panel.error().empty());
    SDL_SetRenderDrawColor(renderer,0,0,0,0);SDL_RenderClear(renderer);
    ui::Input input;gui.begin(input,900,760);panel.draw(gui,true);gui.end();
    CHECK(SDL_FlushRenderer(renderer));
    const auto* pixels=static_cast<const std::uint8_t*>(surface->pixels);
    CHECK(pixels[400*surface->pitch+500*4+3]==0);
    CHECK(pixels[5*surface->pitch+5*4+3]==255);
    CHECK(!panel.cut().empty());CHECK(panel.revision()==1);
    const auto draw=[&] {
        gui.begin(input,900,760);panel.draw(gui,true);gui.end();CHECK(SDL_FlushRenderer(renderer));
    };
    draw(); // publishes the selected-cluster counters from the preceding frame
    const auto paints=panel.uiPaints();
    std::vector<std::uint8_t> previous(pixels,pixels+surface->pitch*surface->h);
    panel.camera.orbit(0.001,0);draw();
    CHECK(panel.uiPaints()==paints);
    CHECK(std::memcmp(previous.data(),surface->pixels,previous.size())==0);
    const double fov=panel.camera.verticalFov;
    input.mouseX=50;input.mouseY=170;input.pressed=input.down=true;draw();
    input.pressed=input.down=false;input.released=true;draw();
    CHECK(panel.camera.verticalFov!=fov);
    CHECK(panel.uiPaints()>paints);
    input.released=false;draw();
    const auto beforeInvalidation=panel.uiPaints();
    panel.invalidateUi();draw();CHECK(panel.uiPaints()==beforeInvalidation+1);
    gui.shutdown();SDL_DestroyRenderer(renderer);
}
