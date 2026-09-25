// Measures tessellation, not whether an erosion reference has been reproduced.
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_adaptive.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace world::terrain;
using Json=nlohmann::json;

void wireframe(const AdaptiveMesh& mesh,const std::string& path) {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write subdivision SVG");
    const double extent=mesh.cells*mesh.step;
    const auto [low,high]=std::minmax_element(mesh.bed.begin(),mesh.bed.end());
    out<<"<svg xmlns='http://www.w3.org/2000/svg' width='1024' height='1024' viewBox='0 0 "
       <<extent<<' '<<extent<<"'><title>Adaptive triangles; colour is source height</title>\n";
    for (std::size_t i=0;i<mesh.surfaceIndices;i+=3) {
        double h=0;
        for (int c=0;c<3;++c) h+=mesh.vertices[mesh.indices[i+c]].sourceBed/3.0;
        const int shade=50+int(170*std::clamp((h-*low)/std::max(1.0,double(*high-*low)),0.0,1.0));
        out<<"<polygon fill='rgb("<<shade<<','<<shade<<','<<shade<<")' stroke='#66bbaa' stroke-width='"
           <<extent/2500<<"' points='";
        for (int c=0;c<3;++c) {
            const auto& v=mesh.vertices[mesh.indices[i+c]];
            out<<v.x*mesh.step<<','<<v.y*mesh.step<<' ';
        }
        out<<"'/>\n";
    }
    out<<"</svg>\n";
    if (!out) throw std::runtime_error("subdivision SVG write failed");
}

struct ErrorStats {
    std::size_t samples=0,overTolerance=0;
    double maximum=0,squared=0;
    std::array<double,2> worst{};
    void add(double error,double x,double y,double tolerance) {
        ++samples;
        overTolerance+=error>tolerance;
        squared+=error*error;
        if (samples==1 || error>maximum) { maximum=error;worst={x,y}; }
    }
    Json json() const {
        return {{"samples",samples},{"max_error_m",maximum},
            {"rms_error_m",samples?std::sqrt(squared/samples):0},
            {"over_tolerance_samples",overTolerance},{"worst_local_m",worst}};
    }
};

// Orthographic, unit vertical scale, identical bounds and light for all panels.
// These are CPU geometry previews, not screenshots of the in-game renderer.
void comparison(const AdaptiveMesh& source,const AdaptiveMesh& coarse,
                const AdaptiveMesh& adaptive,const std::string& path) {
    using Point=std::array<double,3>;
    struct Face { std::array<Point,3> points;double depth;int shade; };
    const std::array<const AdaptiveMesh*,3> meshes{&source,&coarse,&adaptive};
    const std::array<const char*,3> names{"Probe-lattice surface","Coarse mesh","Adaptive mesh"};
    std::array<std::vector<Face>,3> faces;
    double minX=std::numeric_limits<double>::infinity(),minY=minX,maxX=-minX,maxY=-minX;
    const double base=*std::min_element(source.bed.begin(),source.bed.end());
    const auto project=[](const Point& p) {
        return std::array{(p[0]-p[1])/std::sqrt(2.0),(p[0]+p[1]-2*p[2])/std::sqrt(6.0)};
    };
    for (std::size_t panel=0;panel<meshes.size();++panel) {
        const auto& mesh=*meshes[panel];
        for (std::size_t i=0;i<mesh.surfaceIndices;i+=3) {
            Face face{};
            for (int c=0;c<3;++c) {
                const auto& v=mesh.vertices[mesh.indices[i+c]];
                face.points[c]={v.x*mesh.step,v.y*mesh.step,v.sourceBed-base};
                const auto& p=face.points[c];
                face.depth+=(p[0]+p[1]+p[2])/3;
                const auto screen=project(p);
                minX=std::min(minX,screen[0]);maxX=std::max(maxX,screen[0]);
                minY=std::min(minY,screen[1]);maxY=std::max(maxY,screen[1]);
            }
            const auto& a=face.points[0];const auto& b=face.points[1];const auto& c=face.points[2];
            const Point u{b[0]-a[0],b[1]-a[1],b[2]-a[2]},v{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
            const Point n{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
            const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            const double light=std::max(0.0,(-n[0]-n[1]+2*n[2])/(length*std::sqrt(6.0)));
            face.shade=55+int(190*std::min(light,1.0));
            faces[panel].push_back(face);
        }
        std::stable_sort(faces[panel].begin(),faces[panel].end(),
            [](const Face& a,const Face& b){return a.depth<b.depth;});
    }
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write subdivision comparison SVG");
    out<<"<svg xmlns='http://www.w3.org/2000/svg' width='1500' height='700' viewBox='0 0 1500 700'>\n"
       <<"<title>CPU terrain geometry comparison; vertical scale 1:1</title>"
       <<"<rect width='1500' height='700' fill='#20252a'/>"
       <<"<text x='24' y='28' fill='white' font-family='sans-serif' font-size='16'>"
       <<"Same camera, light and scale; no materials or skirts. CPU preview, not a game screenshot.</text>\n";
    const double scale=std::min(452/std::max(1.0,maxX-minX),560/std::max(1.0,maxY-minY));
    for (std::size_t panel=0;panel<meshes.size();++panel) {
        out<<"<g id='panel-"<<panel<<"'><text x='"<<panel*500+24
           <<"' y='62' fill='white' font-family='sans-serif' font-size='16'>"<<names[panel]
           <<" ("<<meshes[panel]->surfaceIndices/3<<" triangles)</text>\n";
        for (const auto& face:faces[panel]) {
            out<<"<polygon fill='rgb("<<face.shade<<','<<face.shade<<','<<face.shade
               <<")' stroke='#304c47' stroke-width='0.3' points='";
            for (const auto& p:face.points) {
                const auto s=project(p);
                out<<panel*500+250+(s[0]-(minX+maxX)/2)*scale<<','
                   <<390+(s[1]-(minY+maxY)/2)*scale<<' ';
            }
            out<<"'/>\n";
        }
        out<<"</g>\n";
    }
    out<<"</svg>\n";
    if (!out) throw std::runtime_error("subdivision comparison SVG write failed");
}

Json measure(const char* name,int extent,int probeStep,int nominalStep,double tolerance,
             const SurfaceSample& field,const std::string& svg) {
    const auto coarse=makeAdaptiveMesh(extent/nominalStep,nominalStep,tolerance,field);
    const auto adaptive=makeAdaptiveMesh(extent/probeStep,probeStep,tolerance,field,{},0.001,{},{},{},
        {nominalStep/probeStep,10,0.25});
    const auto source=makeAdaptiveMesh(extent/probeStep,probeStep,0,field);
    double coarseError=0,adaptiveError=0;
    for (int y=0;y<=extent;y+=probeStep) for (int x=0;x<=extent;x+=probeStep) {
        const auto truth=field(x,y)[0];
        coarseError=std::max(coarseError,double(std::abs(coarse->sample(x,y)[0]-truth)));
        adaptiveError=std::max(adaptiveError,double(std::abs(adaptive->sample(x,y)[0]-truth)));
    }
    // Holdout points never participated in splitting. Keep their statistics
    // separate from the training lattice, including samples on chunk edges.
    constexpr int subdivisions=4;
    const int validationCells=extent/probeStep*subdivisions;
    const double validationStep=double(probeStep)/subdivisions;
    ErrorStats coarseOffGrid,adaptiveOffGrid,sourceOffGrid;
    for (int iy=0;iy<=validationCells;++iy) for (int ix=0;ix<=validationCells;++ix) {
        if (ix%subdivisions==0 && iy%subdivisions==0) continue;
        const double x=ix*validationStep,y=iy*validationStep,truth=field(x,y)[0];
        coarseOffGrid.add(std::abs(coarse->sample(x,y)[0]-truth),x,y,tolerance);
        adaptiveOffGrid.add(std::abs(adaptive->sample(x,y)[0]-truth),x,y,tolerance);
        sourceOffGrid.add(std::abs(source->sample(x,y)[0]-truth),x,y,tolerance);
    }
    const std::string comparisonSvg=svg.empty()?std::string{}:svg.substr(0,svg.size()-4)+"-comparison.svg";
    if (!svg.empty()) {
        wireframe(*adaptive,svg);
        comparison(*source,*coarse,*adaptive,comparisonSvg);
    }
    return {{"case",name},{"probe_step_m",probeStep},{"nominal_step_m",nominalStep},
        {"stored_step_m",adaptive->step},{"dense_triangles",2*(extent/probeStep)*(extent/probeStep)},
        {"adaptive_triangles",adaptive->surfaceIndices/3},{"coarse_triangles",coarse->surfaceIndices/3},
        {"coarse_max_error_m",coarseError},{"adaptive_max_error_m",adaptiveError},
        {"tolerance_m",tolerance},{"mesh_bytes",adaptive->bytes()},
        {"error_domain","probe lattice, not unsampled sub-grid terrain"},{"svg",svg},
        {"comparison_svg",comparisonSvg},
        {"off_grid",{{"step_m",validationStep},{"coarse",coarseOffGrid.json()},
            {"adaptive",adaptiveOffGrid.json()},{"probe_lattice_surface",sourceOffGrid.json()},
            {"error_domain","quarter-step holdouts excluding construction nodes; not a continuous error bound"}}}};
}
}

int main(int argc,char** argv) {
    try {
        generation::WorldMapParams params;params.width=params.height=128;params.seed=42;
        std::string prefix;
        for (int i=1;i<argc;++i) {
            const std::string arg=argv[i];
            if (arg=="--seed" && i+1<argc) params.seed=std::stoull(argv[++i]);
            else if (arg=="--world" && i+1<argc) params.width=params.height=std::stoi(argv[++i]);
            else if (arg=="--svg-prefix" && i+1<argc) prefix=argv[++i];
            else throw std::runtime_error("usage: terrain_subdivision_probe [--seed N] [--world 32..256] [--svg-prefix PATH]");
        }
        if (params.width<32 || params.width>256) throw std::runtime_error("world must be 32..256 cells");
        const auto svg=[&](const char* name){return prefix.empty()?std::string{}:prefix+'-'+name+".svg";};
        const auto plane=[](double x,double y){return std::array{float(100+2*x+0.5*y),-6000.0f};};
        const auto crest=[](double x,double y) {
            const double centre=280+std::abs(y-512)*0.25;
            return std::array{float(100+80*std::max(0.0,1-std::abs(x-centre)/16)),-6000.0f};
        };
        Json rows=Json::array();
        rows.push_back(measure("synthetic steep plane",1024,8,64,0.5,plane,svg("plane")));
        rows.push_back(measure("synthetic branching crest",1024,8,64,0.5,crest,svg("crest")));
        const auto world=generation::generateWorldMap(params);
        if (!world.terrainFoundation) throw std::runtime_error("missing H64 foundation");
        const auto& f=*world.terrainFoundation;
        const auto& heights=f.heightDm[std::size_t(generation::TerrainStage::Slopes)];
        const auto peak=std::size_t(std::max_element(heights.begin(),heights.end())-heights.begin());
        const int ox=std::clamp(int(peak%f.columns)*64-512,0,params.width*generation::kMetresPerCell-1024)/256*256;
        const int oy=std::clamp(int(peak/f.columns)*64-512,0,params.height*generation::kMetresPerCell-1024)/256*256;
        const auto mountain=[&](double x,double y) {
            return std::array{float(f.sample(core::Fixed::fromDoubleForContent(x+ox),
                core::Fixed::fromDoubleForContent(y+oy),generation::TerrainStage::Slopes).toDouble()),-6000.0f};
        };
        rows.push_back(measure("generated H64 slope-stage peak",1024,32,256,8,mountain,svg("mountain")));
        std::cout<<Json{{"seed",params.seed},{"world_cells",params.width},
            {"mountain_origin_m",{ox,oy}},{"peak_height_m",heights[peak]/10.0},
            {"foundation",{{"grid_samples",heights.size()},
                {"geographic_pages",f.detailPages.size()},
                {"shape_detail_candidates",std::count(f.detailPages.begin(),f.detailPages.end(),std::uint8_t(1))},
                {"detail_domain","H64 bend/incision evidence; excludes planner water halo and camera admission"},
                {"stage_milliseconds",f.milliseconds}}},{"measurements",rows}}.dump(2)<<'\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';return 1;
    }
}
