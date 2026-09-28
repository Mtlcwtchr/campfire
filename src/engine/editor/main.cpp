#include "engine/editor/module_panel.hpp"
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <charconv>
#include <memory>
#if ASR_ENGINE_EDITOR_GPU
#include "engine/render/dag_viewport.hpp"
#endif

int main(int argc,char** argv) {
    std::filesystem::path settings=".cache/engine-editor.json",shot,profile;
    bool useGpu=ASR_ENGINE_EDITOR_GPU;
    int benchmarkFrames=0;
    for (int i=1;i<argc;++i) {
        const std::string arg=argv[i];
        if (arg=="--shot" && i+1<argc) shot=argv[++i];
        else if (arg=="--settings" && i+1<argc) settings=argv[++i];
        else if (arg=="--cpu-preview") useGpu=false;
        else if (arg=="--profile" && i+1<argc) profile=argv[++i];
        else if (arg=="--bench" && i+1<argc) {
            const std::string count=argv[++i];
            const auto parsed=std::from_chars(count.data(),count.data()+count.size(),benchmarkFrames);
            if (parsed.ec!=std::errc{} || parsed.ptr!=count.data()+count.size() || benchmarkFrames<1 || benchmarkFrames>100000) {
                std::cerr<<"--bench expects 1..100000 frames\n";return 2;
            }
        }
        else if (arg=="--help") {
            std::cout<<"Campfire engine module editor\n"
                <<"  --settings FILE  Camera/terrain workspace settings\n"
                <<"  --shot FILE      Render a deterministic preview to PNG and exit\n"
                <<"  --cpu-preview    Use the legacy CPU inspection renderer\n"
                <<"  --bench N        Headless orbit benchmark (30 warmup frames)\n"
                <<"  --profile FILE   Save benchmark timings and upload counters\n"
                <<"  RMB orbit, MMB pan, wheel zoom, WASD/QE free flight, F5 rebuild\n";
            return 0;
        } else { std::cerr<<"Unknown or incomplete argument: "<<arg<<'\n';return 2; }
    }
    if (!profile.empty() && !benchmarkFrames) { std::cerr<<"--profile requires --bench\n";return 2; }
    const bool automated=!shot.empty() || benchmarkFrames>0;
    engine::editor::ModulePanel panel;
    try {
        std::ifstream in(settings);
        if (in) {
            nlohmann::json j;in>>j;
            panel.seed=j.value("seed",std::uint64_t(11));
            panel.relief=std::clamp(j.value("relief",1.0),0.25,4.0);
            panel.quality.geometryErrorPx=std::clamp(j.value("geometryErrorPx",1.5),0.25,8.0);
            panel.wireframe=j.value("wireframe",false);
            panel.camera.verticalFov=std::clamp(j.value("fovDegrees",60.0),20.0,120.0)*std::numbers::pi/180;
        }
    } catch (const std::exception& e) { std::cerr<<"Workspace settings: "<<e.what()<<'\n';return 2; }
    if (!SDL_Init(SDL_INIT_VIDEO)) { std::cerr<<SDL_GetError()<<'\n';return 1; }
    SDL_Window* window=nullptr;SDL_Renderer* renderer=nullptr;
    SDL_Surface* uiSurface=nullptr;
    const auto flags=automated?SDL_WINDOW_HIDDEN:SDL_WINDOW_RESIZABLE;
    if (useGpu) window=SDL_CreateWindow("Campfire - engine modules / GPU",1440,900,flags);
    else SDL_CreateWindowAndRenderer("Campfire - engine modules / CPU",1440,900,flags,&window,&renderer);
    if (!window || (!useGpu && !renderer)) {
        std::cerr<<SDL_GetError()<<'\n';SDL_Quit();return 1;
    }
    SDL_SetWindowMinimumSize(window,900,760);
#if ASR_ENGINE_EDITOR_GPU
    auto gpu=useGpu?std::make_unique<engine::DagViewport>():nullptr;
    if (gpu && !gpu->open(automated?nullptr:window,"assets/sprites")) {
        std::cerr<<gpu->error()<<'\n';gpu.reset();SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
#endif
    if (useGpu) {
        uiSurface=SDL_CreateSurface(1440,900,SDL_PIXELFORMAT_RGBA32);
        if (uiSurface) renderer=SDL_CreateSoftwareRenderer(uiSurface);
    }
    ui::Ui gui;
    if (!renderer || !gui.init(renderer)) {
        std::cerr<<"GUI font initialization failed\n";
#if ASR_ENGINE_EDITOR_GPU
        gpu.reset();
#endif
        SDL_DestroyRenderer(renderer);SDL_DestroySurface(uiSurface);SDL_DestroyWindow(window);SDL_Quit();return 1;
    }
    gui.theme().barTop=ui::Colour::rgb(29,35,45);
    gui.theme().barEdge=ui::Colour::rgb(65,78,96);
    gui.theme().slot=ui::Colour::rgb(42,52,66);
    gui.theme().slotHot=ui::Colour::rgb(59,75,94);
    gui.theme().label=ui::Colour::rgb(225,233,242);
    gui.theme().labelSoft=ui::Colour::rgb(164,181,200);
    ui::Input pointer;
    SDL_Process* sceneProcess=nullptr;
    bool running=true;int result=0,frames=0;
    std::vector<double> timings;
    if (automated) panel.wait();
    auto previous=SDL_GetTicksNS();
    while (running) {
        const auto frameStart=SDL_GetTicksNS();
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type==SDL_EVENT_QUIT) running=false;
            if (event.type==SDL_EVENT_KEY_DOWN) {
                if (event.key.key==SDLK_ESCAPE) running=false;
                if (event.key.key==SDLK_F5) panel.regenerate=true;
            }
            if (event.type==SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button==SDL_BUTTON_LEFT) pointer.pressed=true;
            if (event.type==SDL_EVENT_MOUSE_BUTTON_UP && event.button.button==SDL_BUTTON_LEFT) pointer.released=true;
            if (event.type==SDL_EVENT_MOUSE_WHEEL) pointer.wheel+=event.wheel.y;
        }
        const auto buttons=SDL_GetMouseState(&pointer.mouseX,&pointer.mouseY);
        int width=1440,height=900;
        if (useGpu) {
            if (!automated && (SDL_GetWindowFlags(window)&SDL_WINDOW_MINIMIZED)) { SDL_Delay(16);continue; }
            SDL_GetWindowSizeInPixels(window,&width,&height);
            int logicalWidth,logicalHeight;SDL_GetWindowSize(window,&logicalWidth,&logicalHeight);
            pointer.mouseX*=float(width)/std::max(1,logicalWidth);
            pointer.mouseY*=float(height)/std::max(1,logicalHeight);
            if (width<=0 || height<=0) { SDL_Delay(16);continue; }
            if (uiSurface->w!=width || uiSurface->h!=height) {
                gui.shutdown();SDL_DestroyRenderer(renderer);SDL_DestroySurface(uiSurface);
                uiSurface=SDL_CreateSurface(width,height,SDL_PIXELFORMAT_RGBA32);
                renderer=uiSurface?SDL_CreateSoftwareRenderer(uiSurface):nullptr;
                if (!renderer || !gui.init(renderer)) { result=1;break; }
                panel.invalidateUi();
            }
        } else SDL_GetRenderOutputSize(renderer,&width,&height);
        pointer.down=(buttons&SDL_BUTTON_LMASK)!=0;
        pointer.rightDown=(buttons&SDL_BUTTON_RMASK)!=0;
        pointer.middleDown=(buttons&SDL_BUTTON_MMASK)!=0;
        const auto now=SDL_GetTicksNS();
        const double dt=std::clamp(double(now-previous)/1e9,0.0,0.1);previous=now;
        if (panel.camera.mode==engine::camera::Camera::Mode::Free && SDL_GetKeyboardFocus()==window) {
            const auto keys=SDL_GetKeyboardState(nullptr);
            const auto state=panel.camera.viewState();
            const double forward=double(keys[SDL_SCANCODE_W])-double(keys[SDL_SCANCODE_S]);
            const double right=double(keys[SDL_SCANCODE_D])-double(keys[SDL_SCANCODE_A]);
            const double up=double(keys[SDL_SCANCODE_E])-double(keys[SDL_SCANCODE_Q]);
            const double speed=panel.camera.flightSpeed*dt;
            panel.camera.centreX+=speed*(state.forward[0]*forward+std::sin(panel.camera.yaw)*right);
            panel.camera.centreY+=speed*(state.forward[1]*forward-std::cos(panel.camera.yaw)*right);
            panel.camera.focusHeight+=speed*(state.forward[2]*forward+up);
        }
        if (benchmarkFrames) panel.camera.orbit(0.002,0);
        if (!useGpu) { SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer); }
        gui.begin(pointer,width,height);panel.draw(gui,useGpu);gui.end();
        // Play: the world scene is the game's explorer in its own window,
        // with its graphics window (O) and scene view (F).
        if (panel.launchScene) {
            panel.launchScene=false;
            if (!sceneProcess) {
                const char* base=SDL_GetBasePath();
                const std::string client=std::string(base?base:"")+"campfire_client";
                const std::string seedText=std::to_string(panel.seed);
                const char* args[]{client.c_str(),"--explore","--seed",seedText.c_str(),"--camera","orbit",nullptr};
                sceneProcess=SDL_CreateProcess(args,false);
                panel.sceneStatus=sceneProcess?"Scene: seed "+seedText+" (O graphics, F scene view)":
                    std::string("Scene failed: ")+SDL_GetError();
            }
        }
        if (sceneProcess) {
            int exitCode=0;
            if (SDL_WaitProcess(sceneProcess,false,&exitCode)) {
                SDL_DestroyProcess(sceneProcess);sceneProcess=nullptr;
                panel.sceneStatus="Scene closed (exit "+std::to_string(exitCode)+")";
            }
        }
        panel.sceneRunning=sceneProcess!=nullptr;
        bool frameOk=SDL_FlushRenderer(renderer);
#if ASR_ENGINE_EDITOR_GPU
        if (gpu) frameOk=frameOk && gpu->resize(width,height) && gpu->mesh(panel.dag(),panel.revision()) &&
            gpu->draw(panel.camera,panel.viewportArea(),panel.cut(),panel.wireframe,uiSurface);
        if (gpu && benchmarkFrames) gpu->wait(); // completed frames, not CPU queue throughput
#endif
        if (!frameOk || !panel.error().empty()) {
            std::cerr<<"Frame failed: "<<panel.error()<<" "<<SDL_GetError()<<'\n';
#if ASR_ENGINE_EDITOR_GPU
            if (gpu) std::cerr<<gpu->error()<<'\n';
#endif
            result=1;break;
        }
        ++frames;
        if (benchmarkFrames && frames>30) timings.push_back(double(SDL_GetTicksNS()-frameStart)/1e6);
        const bool finished=automated && frames>=(benchmarkFrames?benchmarkFrames+30:2);
        if (!shot.empty() && finished) {
            if (!shot.parent_path().empty()) std::filesystem::create_directories(shot.parent_path());
            bool saved=false;
#if ASR_ENGINE_EDITOR_GPU
            if (gpu) saved=gpu->capture(shot.string());
            else
#endif
            {
                auto* surface=SDL_RenderReadPixels(renderer,nullptr);
                saved=surface && IMG_SavePNG(surface,shot.string().c_str());
                SDL_DestroySurface(surface);
            }
            if (!saved) {
                std::cerr<<"Preview failed: "<<panel.error()<<" "<<SDL_GetError()<<'\n';result=1;
            } else std::cout<<"Engine preview: "<<shot<<'\n';
        }
        if (finished) running=false;
        if (!useGpu) SDL_RenderPresent(renderer);
        pointer.pressed=pointer.released=false;pointer.wheel=0;
        if (!automated && !useGpu) SDL_Delay(8);
    }
    if (benchmarkFrames && result==0 && !timings.empty()) {
        std::sort(timings.begin(),timings.end());
        const auto percentile=[&](double q) { return timings[std::min(timings.size()-1,std::size_t(std::ceil(q*timings.size())-1))]; };
        nlohmann::json report{{"backend",useGpu?"gpu":"cpu"},{"frames",timings.size()},
            {"warmupFrames",30},{"medianMs",percentile(0.5)},{"p99Ms",percentile(0.99)},{"maxMs",timings.back()},
            {"uiPaints",panel.uiPaints()}};
#if ASR_ENGINE_EDITOR_GPU
        if (gpu) { report["meshUploads"]=gpu->meshUploads();report["overlayUploads"]=gpu->overlayUploads();report["residentMeshBytes"]=gpu->residentBytes(); }
#endif
        std::cout<<report.dump(2)<<'\n';
        if (!profile.empty()) {
            if (!profile.parent_path().empty()) std::filesystem::create_directories(profile.parent_path());
            std::ofstream out(profile);out<<report.dump(2)<<'\n';
            if (!out) { std::cerr<<"Cannot save profile\n";result=1; }
        }
    }
    if (!automated) {
        try {
            if (!settings.parent_path().empty()) std::filesystem::create_directories(settings.parent_path());
            std::ofstream out(settings);
            if (!out) throw std::runtime_error("cannot write settings");
            out<<nlohmann::json{{"seed",panel.seed},{"relief",panel.relief},
                {"geometryErrorPx",panel.quality.geometryErrorPx},{"wireframe",panel.wireframe},
                {"fovDegrees",panel.camera.verticalFov*180/std::numbers::pi}}.dump(2)<<'\n';
        } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';result=1; }
    }
    gui.shutdown();SDL_DestroyRenderer(renderer);SDL_DestroySurface(uiSurface);
#if ASR_ENGINE_EDITOR_GPU
    gpu.reset();
#endif
    SDL_DestroyWindow(window);SDL_Quit();
    return result;
}

