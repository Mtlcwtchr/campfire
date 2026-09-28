#include "engine/editor/module_panel.hpp"
#include "engine/terrain/plate_field.hpp"
#include "engine/terrain/range_field.hpp"
#include "engine/terrain/field_noise.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace engine::editor {
namespace {
std::string number(double value, int precision = 2) {
    std::ostringstream out; out << std::fixed << std::setprecision(precision) << value; return out.str();
}
}
ModulePanel::ModulePanel() { resetCamera(); }
void ModulePanel::resetCamera() {
    camera = {}; camera.mode = camera::Camera::Mode::Orbit;
    camera.isometric = true; camera.centreX = camera.centreY = 0;
    camera.focusHeight = 400; camera.pixelsPerTile = 0.028; camera.minZoom = 0.002;
    camera.farPlane = 200000; camera.flightSpeed = 600;
    quality = camera::ViewQualityProfile::orbital();
}
void ModulePanel::start() {
    if (job_.valid() || !regenerate) return;
    regenerate = false; error_.clear();
    const auto requestedSeed = seed; const auto requestedRelief = relief;
    job_ = std::async(std::launch::async, [requestedSeed, requestedRelief] {
        terrain::PlateFieldSettings settings;
        settings.plateMetres = 18000; settings.marginMetres = 6500;
        settings.shelfMetres = 2000; settings.warpMetres = 2800;
        terrain::PlateField plates(requestedSeed, settings);
        terrain::RangeSettings mountain;
        mountain.halfWidthMetres = 3000; mountain.wanderMetres = 1200;
        terrain::RangeField ranges(requestedSeed, plates, mountain);
        constexpr int side = 33;
        std::vector<float> positions;
        std::vector<std::uint32_t> indices;
        positions.reserve(side*side*3); indices.reserve((side-1)*(side-1)*6);
        for (int y=0; y<side; ++y) for (int x=0; x<side; ++x) {
            const double wx=(x-16)*600.0, wy=(y-16)*600.0;
            const double z=requestedRelief*(plates.at(wx,wy).crust*700 + ranges.at(wx,wy).metres +
                terrain::noise::ridged(requestedSeed,wx,wy,5000,3)*450);
            positions.insert(positions.end(),{float(wx),float(wy),float(z)});
            if (x+1<side && y+1<side) {
                const auto i=std::uint32_t(y*side+x);
                indices.insert(indices.end(),{i,i+1,i+side, i+1,i+side+1,i+side});
            }
        }
        auto options=geometry::naniteProfile(); options.thinCards=false;
        Preview result;
        result.seed=requestedSeed; result.relief=requestedRelief;
        result.dag=geometry::buildClusterDag(positions,indices,options);
        return result;
    });
}
void ModulePanel::poll() {
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        try {
            auto result=job_.get();
            if (result.seed==seed && result.relief==relief) { preview_=std::move(result); ++revision_; }
            else regenerate=true; // an obsolete worker must not replace newer settings
        } catch (const std::exception& e) { error_=e.what(); }
    }
    start();
}
void ModulePanel::wait() {
    poll();
    while (job_.valid()) { job_.wait(); poll(); }
}
void ModulePanel::viewport(ui::Ui& gui, ui::Rect area, bool gpuPreview) {
    if (!gpuPreview) gui.rect(area, ui::Colour::rgb(22,30,40));
    viewportArea_={int(area.x),int(area.y),std::max(1,int(area.w)),std::max(1,int(area.h))};
    camera.viewportWidth=std::max(1,int(area.w)); camera.viewportHeight=std::max(1,int(area.h));
    const auto& input=gui.input();
    if (area.contains(input.mouseX,input.mouseY) && !input.takenByUi) {
        if (input.rightDown) camera.orbit(-(input.mouseX-lastMouseX_)*0.006, (input.mouseY-lastMouseY_)*0.006);
        if (input.middleDown) {
            const double scale=std::max(0.005,camera.pixelsPerTile);
            camera.pan(-(input.mouseX-lastMouseX_)/scale,(input.mouseY-lastMouseY_)/scale);
        }
        if (input.wheel!=0) camera.zoomAt(std::pow(1.12,input.wheel),int(input.mouseX-area.x),int(input.mouseY-area.y));
    }
    lastMouseX_=input.mouseX; lastMouseY_=input.mouseY;
    if (preview_.dag.clusters.empty()) {
        gui.text(area.x+24,area.y+30,error_.empty()?"Building terrain and cluster DAG...":error_,gui.theme().label);
        return;
    }
    auto state=camera.viewState(); state.quality=quality;
    const auto& dag=preview_.dag;
    const double scale=state.pixelsPerMetre({0,0,400},14000);
    chosen_=geometry::cutAt(dag,quality.geometryErrorPx/std::max(scale,1e-9));
    const auto& chosen=chosen_;
    selectedClusters_=chosen.size(); selectedTriangles_=0;
    for (auto id:chosen) selectedTriangles_+=dag.clusters[id].indices.count/3;
    if (gpuPreview) {
        gui.text(area.x+16,area.y+12,"Terrain + virtual geometry / GPU depth viewport",gui.theme().label);
        gui.text(area.x+16,area.bottom()-26,"RMB orbit  |  MMB pan  |  wheel zoom  |  WASD/QE free flight",gui.theme().labelSoft);
        return;
    }
    struct Triangle { std::array<SDL_FPoint,3> screen; double depth; SDL_FColor colour; };
    std::vector<Triangle> triangles;
    for (auto id:chosen) {
        const auto& cluster=dag.clusters[id];
        for (std::uint32_t at=cluster.indices.first; at<cluster.indices.first+cluster.indices.count; at+=3) {
            Triangle t{}; bool visible=true; double height=0;
            for (int corner=0;corner<3;++corner) {
                const auto vertex=std::size_t(dag.indices[at+corner])*3;
                camera::Vec3 p{dag.positions[vertex],dag.positions[vertex+1],dag.positions[vertex+2]};
                const double depth=camera::dot(camera::subtract(p,state.position),state.forward);
                if (!state.orthographic && depth<=state.nearPlane) { visible=false;break; }
                float x,y; camera.worldToScreen3D(p[0],p[1],p[2],x,y);
                t.screen[corner]={area.x+x,area.y+y}; t.depth+=depth; height+=p[2]/3;
            }
            if (!visible) continue;
            const float h=float(std::clamp(height/3500.0,0.0,1.0));
            t.colour={0.20f+h*0.48f,0.38f+h*0.26f,0.25f+h*0.38f,1};
            triangles.push_back(t);
        }
    }
    std::sort(triangles.begin(),triangles.end(),[](const auto& a,const auto& b){return a.depth>b.depth;});
    SDL_Rect clip{int(area.x),int(area.y),int(area.w),int(area.h)};
    SDL_SetRenderClipRect(gui.renderer(),&clip);
    std::vector<SDL_Vertex> vertices; vertices.reserve(triangles.size()*3);
    for (const auto& t:triangles) for (auto p:t.screen) vertices.push_back({p,t.colour,{0,0}});
    if (!vertices.empty()) SDL_RenderGeometry(gui.renderer(),nullptr,vertices.data(),int(vertices.size()),nullptr,0);
    if (wireframe) for (const auto& t:triangles) for (int i=0;i<3;++i) {
        const auto a=t.screen[i],b=t.screen[(i+1)%3];
        gui.line(a.x,a.y,b.x,b.y,ui::Colour::rgb(10,18,25,0.65f));
    }
    SDL_SetRenderClipRect(gui.renderer(),nullptr);
    gui.text(area.x+16,area.y+12,"Terrain + virtual geometry / CPU preview",gui.theme().label);
    gui.text(area.x+16,area.bottom()-26,"RMB orbit  |  MMB pan  |  wheel zoom  |  WASD/QE free flight",gui.theme().labelSoft);
}
void ModulePanel::draw(ui::Ui& gui, bool gpuPreview) {
    poll();
    auto& theme=gui.theme();
    const float panelWidth=std::min(350.0f,float(gui.width())*0.40f);
    if (gpuPreview) {
        const auto& input=gui.input();
        const bool overPanel=input.mouseX<panelWidth;
        std::ostringstream key;
        key<<std::setprecision(17)<<gui.width()<<' '<<gui.height()<<' '<<gui.renderer()<<' '
            <<seed<<' '<<relief<<' '<<revision_<<' '<<busy()<<' '<<regenerate<<' '<<error_<<' '
            <<int(camera.mode)<<' '<<camera.verticalFov<<' '<<quality.geometryErrorPx<<' '
            <<quality.impostorErrorPx<<' '<<quality.parallaxErrorPx<<' '<<wireframe<<' '
            <<selectedClusters_<<' '<<selectedTriangles_<<' '
            <<(overPanel?input.mouseX:-1)<<' '<<(overPanel?input.mouseY:-1)<<' '
            <<input.down<<input.pressed<<input.released<<' '<<sceneRunning<<' '<<sceneStatus;
        const auto next=key.str();
        const bool repaint=next!=lastUiKey_;
        lastUiKey_=next;
        gui.painting(repaint);
        if (repaint) {
            SDL_SetRenderDrawColor(gui.renderer(),0,0,0,0);
            SDL_RenderClear(gui.renderer());
            ++uiPaints_;
        }
    } else invalidateUi();
    const auto background=ui::Colour::rgb(17,21,28);
    if (!gpuPreview) gui.rect({0,0,float(gui.width()),float(gui.height())},background);
    else {
        gui.rect({0,0,float(gui.width()),68},background);
        gui.rect({0,68,panelWidth+6,float(gui.height())-68},background);
        gui.rect({float(gui.width())-12,68,12,float(gui.height())-68},background);
        gui.rect({panelWidth+6,float(gui.height())-12,float(gui.width())-panelWidth-6,12},background);
    }
    gui.text(18,16,"CAMPFIRE / ENGINE WORKSPACE",theme.label,1.5f);
    gui.text(18,42,"Camera  /  Representations  /  Terrain  /  Virtual geometry",theme.labelSoft);
    const ui::Rect panel{12,68,panelWidth-18,float(gui.height())-80};
    gui.panel(panel); gui.claim(panel);
    float y=82; const float x=24,w=panelWidth-44;
    const auto label=[&](const std::string& text) { gui.text(x,y,gui.fit(text,w),theme.labelSoft); y+=23; };
    const auto button=[&](const char* id,const std::string& text) {
        const bool pressed=gui.button(ui::widgetId(id),{x,y,w,25},text); y+=31; return pressed;
    };
    label("CAMERA / Campfire::Camera");
    if (button("engine.mode",camera.modeName())) {
        const auto next=camera.mode==camera::Camera::Mode::Map?camera::Camera::Mode::Orbit:
            camera.mode==camera::Camera::Mode::Orbit?camera::Camera::Mode::Free:camera::Camera::Mode::Map;
        camera.setMode(next);
        quality=next==camera::Camera::Mode::Free?camera::ViewQualityProfile::person():camera::ViewQualityProfile::orbital();
    }
    label("FOV: "+number(camera.verticalFov*180/std::numbers::pi,0)+" deg");
    if (button("engine.fov","Cycle FOV: 40 / 60 / 80")) {
        const double deg=camera.verticalFov*180/std::numbers::pi;
        camera.verticalFov=(deg<50?60:deg<70?80:40)*std::numbers::pi/180;
    }
    if (button("engine.reset","Reset camera")) resetCamera();
    label("REPRESENTATION / shared error policy");
    label("Geometry error: "+number(quality.geometryErrorPx)+" px");
    if (button("engine.quality","Cycle quality: 0.5 / 1 / 2 / 4 px"))
        quality.geometryErrorPx=quality.geometryErrorPx>=4?0.5:std::min(4.0,quality.geometryErrorPx*2);
    label("Impostor: "+number(quality.impostorErrorPx)+" px | parallax: "+number(quality.parallaxErrorPx));
    label("TERRAIN / Campfire::Terrain");
    label("Seed: "+std::to_string(seed)+" | relief: "+number(relief,1));
    if (button("engine.seed","Next seed + rebuild")) { ++seed;regenerate=true; }
    if (button("engine.relief","Cycle relief: 0.5 / 1 / 2")) { relief=relief>=2?0.5:relief*2;regenerate=true; }
    if (button("engine.rebuild",busy()?"Building in worker...":"Rebuild terrain (F5)")) regenerate=true;
    label("VIRTUAL GEOMETRY / cluster DAG");
    gui.checkbox(ui::widgetId("engine.wire"),{x,y,w,24},"Wireframe",wireframe);y+=30;
    label("Selected clusters: "+std::to_string(selectedClusters_));
    label("Selected triangles: "+std::to_string(selectedTriangles_));
    label("Resident DAG levels: "+std::to_string(preview_.dag.levels));
    if (!error_.empty()) label(error_);
    label("SCENE / game world");
    if (button("engine.play",sceneRunning?"World scene running...":"Play world scene (explore)")) launchScene=true;
    if (!sceneStatus.empty()) label(sceneStatus);
    viewport(gui,{panelWidth+6,68,std::max(1.0f,float(gui.width())-panelWidth-18),std::max(1.0f,float(gui.height())-80)},gpuPreview);
    gui.painting(true);
}
} // namespace engine::editor

