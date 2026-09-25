#include "game/generation/world_map_gen.hpp"
#include "game/generation/mountain_shape.hpp"
#include "game/world/height_field.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

namespace {
bool skeletonPreview(const char* path,std::uint64_t seed) {
    using core::Fixed;
    using namespace generation::mountains;
    std::ofstream out(path);
    if (!out) return false;
    const auto s=makeSkeleton(seed);
    out<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1000\" height=\"530\" viewBox=\"0 0 1000 530\">\n"
           "<rect width=\"1000\" height=\"530\" fill=\"#17232c\"/>\n";
    int extent=5000;
    for (std::size_t i=0;i<s.nodeCount;++i) {
        const auto& n=s.nodes[i];
        const auto reach=std::max(core::abs(n.x),core::abs(n.y))+s.nodes[0].width;
        extent=std::max(extent,static_cast<int>((reach.toInt()/500+1)*500));
    }
    const auto coordinate=[&](Fixed v) { return 10+(v.toDouble()+extent)*240/extent; };
    for (std::size_t i=0;i<s.edgeCount;++i) {
        const auto& e=s.edges[i]; const auto& a=s.nodes[e.from]; const auto& b=s.nodes[e.to];
        out<<"<line x1=\""<<coordinate(a.x)<<"\" y1=\""<<coordinate(a.y)<<"\" x2=\""<<coordinate(b.x)
           <<"\" y2=\""<<coordinate(b.y)<<"\" stroke=\"#d5c39b\" stroke-width=\""<<(kBranchGenerations-e.order)<<"\"/>\n";
    }
    for (std::size_t i=0;i<s.nodeCount;++i) {
        const auto& n=s.nodes[i]; if (n.degree<3) continue;
        out<<"<circle cx=\""<<coordinate(n.x)<<"\" cy=\""<<coordinate(n.y)<<"\" r=\""<<2+n.height.toDouble()*5
           <<"\" fill=\"#ef9f63\"/>\n";
    }
    for (int y=0;y<240;++y) for (int x=0;x<240;++x) {
        const auto wx=Fixed::ratio(x*2*extent,240)-Fixed::fromInt(extent);
        const auto wy=Fixed::ratio(y*2*extent,240)-Fixed::fromInt(extent);
        const auto h=sample(seed,wx,wy).height().toDouble();
        const auto shade=std::clamp(0.75+(h-sample(seed,wx+Fixed::fromInt(24),wy+Fixed::fromInt(24)).height().toDouble())*8,0.25,1.1);
        const auto c=[&](int a,int b) { return std::clamp(int((a+(b-a)*h)*shade),0,255); };
        out<<"<rect x=\""<<510+x*2<<"\" y=\""<<10+y*2<<"\" width=\"2\" height=\"2\" fill=\"rgb("
           <<c(65,245)<<','<<c(96,240)<<','<<c(61,227)<<")\"/>\n";
    }
    out<<"<g fill=\"white\" font-family=\"sans-serif\" font-size=\"15\">"
           "<text x=\"10\" y=\"516\">"<<s.edgeCount<<" edges / "<<kBranchGenerations
       <<" orders / "<<extent*2<<" m across</text>"
           "<text x=\"510\" y=\"516\">Distance-field relief / raised junctions</text></g></svg>\n";
    return bool(out);
}
}

// Same location/seed before and after a change. SVG is a shaded height-field
// diagnostic, not a screenshot. Metrics count maxima at a stated resolution.
int main(int argc, char** argv) {
    generation::WorldMapParams params;
    params.seed=42; params.width=params.height=96;
    if (argc>2) params.terrainReferenceRoot=argv[2];
    if (argc>3 && !skeletonPreview(argv[3],params.seed)) return 2;
    const auto map=generation::generateWorldMap(params);
    world::HeightField field(&map,map.seed);
    constexpr int side=257,step=32,cx=35640,cy=31320;
    std::vector<double> heights(side*side);
    double low=1e9,high=-1e9;
    for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
        const auto wx=cx+(x-side/2)*step,wy=cy+(y-side/2)*step;
        auto& h=heights[y*side+x];
        h=field.sampleHeight(wx/world::kSampleMetres,wy/world::kSampleMetres).toDouble();
        low=std::min(low,h); high=std::max(high,h);
    }
    int peaks=0; double curvature=0;
    for (int y=1;y<side-1;++y) for (int x=1;x<side-1;++x) {
        const auto h=heights[y*side+x];
        double neighbour=-1e9;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx)
            if (dx || dy) neighbour=std::max(neighbour,heights[(y+dy)*side+x+dx]);
        peaks+=h>neighbour+0.25;
        curvature+=std::abs(h-(heights[y*side+x-1]+heights[y*side+x+1]+
            heights[(y-1)*side+x]+heights[(y+1)*side+x])/4);
    }
    if (argc>1) {
        std::ofstream svg(argv[1]);
        if (!svg) return 2;
        svg<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"768\" height=\"768\" viewBox=\"0 0 256 256\">\n";
        for (int y=0;y<side-1;++y) for (int x=0;x<side-1;++x) {
            const double h=heights[y*side+x],t=std::clamp(h/2200,0.0,1.0);
            const double dx=(heights[y*side+x+1]-h)/step,dy=(heights[(y+1)*side+x]-h)/step;
            const double light=std::clamp(0.4+0.6*(0.55*dx+0.55*dy+0.63)/std::sqrt(1+dx*dx+dy*dy),0.2,1.0);
            const auto colour=[&](int a,int b) { return int((a+(b-a)*t)*light); };
            svg<<"<rect x=\""<<x<<"\" y=\""<<y<<"\" width=\"1\" height=\"1\" fill=\"rgb("
                <<colour(105,245)<<','<<colour(136,241)<<','<<colour(80,234)<<")\"/>\n";
        }
        svg<<"</svg>\n";
    }
    const auto skeleton=generation::mountains::makeSkeleton(params.seed);
    double length=0,radius=0;
    for (std::size_t i=0;i<skeleton.edgeCount;++i) {
        const auto& a=skeleton.nodes[skeleton.edges[i].from];
        const auto& b=skeleton.nodes[skeleton.edges[i].to];
        length+=core::hypot(b.x-a.x,b.y-a.y).toDouble();
        radius=std::max(radius,core::hypot(b.x,b.y).toDouble());
    }
    std::cout<<nlohmann::json{{"seed",map.seed},{"centre_metres",{cx,cy}},{"step_metres",step},
        {"skeleton",{{"nodes",skeleton.nodeCount},{"edges",skeleton.edgeCount},
            {"branch_generations",generation::mountains::kBranchGenerations},
            {"total_length_metres",length},{"maximum_radius_metres",radius}}},
        {"prominence_threshold_metres",0.25},{"local_peaks",peaks},{"minimum_metres",low},
        {"maximum_metres",high},{"mean_laplacian_metres",curvature/((side-2)*(side-2))}}.dump(2)<<'\n';
}
