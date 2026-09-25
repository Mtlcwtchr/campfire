#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/height_field.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using core::Fixed;

void preview(const generation::WorldMapData& world, const std::filesystem::path& file) {
    std::ofstream out(file);
    if (!out) throw std::runtime_error("cannot write preview: " + file.string());
    constexpr int side=192, scale=4;
    world::HeightField field(&world,world.seed);
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"768\" height=\"800\" viewBox=\"0 0 768 800\">\n"
           "<rect width=\"768\" height=\"800\" fill=\"#18232c\"/>\n";
    for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
        const auto sx=std::int64_t(x)*world.width*generation::kMetresPerCell/(side*world::kSampleMetres);
        const auto sy=std::int64_t(y)*world.height*generation::kMetresPerCell/(side*world::kSampleMetres);
        const double height=field.sampleHeight(sx,sy).toDouble();
        const double slope=(field.sampleHeight(sx-8,sy-8)-field.sampleHeight(sx+8,sy+8)).toDouble()/128;
        const double shade=std::clamp(0.85+slope*0.4,0.4,1.2);
        const double high=std::clamp(height/1800,0.0,1.0);
        const auto channel=[&](double low,double top) { return std::clamp(int((low+(top-low)*high)*shade),0,255); };
        int r=channel(85,220),g=channel(119,218),b=channel(66,211);
        if (height<=0) { r=30; g=76; b=108; }
        out << "<rect x=\""<<x*scale<<"\" y=\""<<y*scale<<"\" width=\"4\" height=\"4\" fill=\"rgb("
            <<r<<','<<g<<','<<b<<")\"/>\n";
    }
    if (world.hybridTerrain) for (const auto& p : world.hybridTerrain->patches) {
        const double x=double(p.originX+p.radius)*768/(world.width*generation::kMetresPerCell);
        const double y=double(p.originY+p.radius)*768/(world.height*generation::kMetresPerCell);
        out << "<circle cx=\""<<x<<"\" cy=\""<<y<<"\" r=\"4\" fill=\"none\" stroke=\""
            <<(p.family==generation::LandformFamily::Volcanic ? "#ff9570" : "#fff2bd")<<"\"/>\n";
    }
    out << "<text x=\"12\" y=\"789\" fill=\"white\" font-family=\"sans-serif\" font-size=\"14\">"
        <<"Hybrid terrain / seed "<<world.seed<<" / red: volcanoes / yellow: DEM landforms</text></svg>\n";
    if (!out) throw std::runtime_error("cannot finish preview");
}
void formPreviews(const generation::WorldMapData& world,const std::filesystem::path& file) {
    if (!world.hybridTerrain) throw std::runtime_error("form previews require hybrid terrain");
    const auto& terrain=*world.hybridTerrain;
    constexpr int side=192,scale=2,rowHeight=416;
    std::ofstream out(file);
    if (!out) throw std::runtime_error("cannot write form previews");
    const auto height=std::max<std::size_t>(1,terrain.patches.size())*rowHeight;
    out<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"768\" height=\""<<height
       <<"\"><rect width=\"768\" height=\""<<height<<"\" fill=\"#18232c\"/>\n";
    for (std::size_t n=0;n<terrain.patches.size();++n) {
        const auto& p=terrain.patches[n];
        for (int y=0;y<side;++y) for (int x=0;x<side;++x) {
            const auto wx=Fixed::fromInt(p.originX)+Fixed::ratio(std::int64_t(x)*2*p.radius,side-1);
            const auto wy=Fixed::fromInt(p.originY)+Fixed::ratio(std::int64_t(y)*2*p.radius,side-1);
            const auto h=terrain.sample(wx,wy).delta;
            const auto offset=Fixed::fromInt(64);
            const auto slope=(terrain.sample(wx-offset,wy-offset).delta-
                              terrain.sample(wx+offset,wy+offset).delta).toDouble()/256;
            // This is the unwarped baked height contribution, NOT a material
            // map or the final river-carved HeightField. Both panels align.
            const int grey=std::clamp(int(145+h.toDouble()*0.055+slope*90),20,240);
            const double cut=std::clamp(terrain.incisionAt(wx,wy).toDouble()/150,0.0,1.0);
            out<<"<rect x=\""<<x*scale<<"\" y=\""<<n*rowHeight+y*scale
               <<"\" width=\"2\" height=\"2\" fill=\"rgb("<<grey<<','<<grey<<','<<grey<<")\"/>\n";
            out<<"<rect x=\""<<384+x*scale<<"\" y=\""<<n*rowHeight+y*scale
               <<"\" width=\"2\" height=\"2\" fill=\"rgb("<<int(22+cut*233)<<','<<int(30+cut*125)<<','<<int(40-cut*20)<<")\"/>\n";
        }
        out<<"<text x=\"8\" y=\""<<n*rowHeight+405<<"\" fill=\"white\" font-family=\"sans-serif\" font-size=\"13\">"
           <<generation::landformFamilyName(p.family)<<(p.island ? " island" : "")
           <<" / "<<2*p.radius<<" m / left: baked relief / right: incision 0-150 m</text>\n";
    }
    out<<"</svg>\n";
    if (!out) throw std::runtime_error("cannot finish form previews");
}
}

int main(int argc,char** argv) {
    try {
        generation::WorldMapParams params;
        std::filesystem::path svg,formsSvg;
        bool validate=false;
        for (int i=1;i<argc;++i) {
            const std::string flag=argv[i];
            const auto value=[&]() -> std::string {
                if (++i>=argc) throw std::runtime_error("missing value for "+flag);
                return argv[i];
            };
            if (flag=="--seed") params.seed=std::stoull(value());
            else if (flag=="--cells") params.width=params.height=std::stoi(value());
            else if (flag=="--references") params.terrainReferenceRoot=value();
            else if (flag=="--svg") svg=value();
            else if (flag=="--forms-svg") formsSvg=value();
            else if (flag=="--validate-library") validate=true;
            else if (flag=="--legacy") params.hybridTerrain=false;
            else throw std::runtime_error("usage: hybrid_terrain_probe [--seed N] [--cells 24..256] "
                                          "[--references DIR] [--validate-library] [--legacy] [--svg FILE] [--forms-svg FILE]");
        }
        if (params.width<24 || params.width>256) throw std::runtime_error("cells must be in 24..256");
        nlohmann::json report;
        if (validate) {
            const auto root=params.terrainReferenceRoot.empty() ? generation::defaultTerrainReferenceRoot() : params.terrainReferenceRoot;
            std::ifstream in(root/"index.json"); nlohmann::json index; in>>index;
            std::size_t count=0;
            for (const auto& entry : index.at("regions")) {
                const auto id=entry.at("id").get<std::string>();
                if (id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=std::string::npos)
                    throw std::runtime_error("invalid reference ID");
                (void)generation::loadTerrainReference(root/id/"metadata.json"); ++count;
            }
            report["validated_references"]=count;
        }
        const auto start=std::chrono::steady_clock::now();
        const auto world=generation::generateWorldMap(params);
        report["generation_ms"]=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        report["seed"]=world.seed; report["cells"]=world.width; report["hybrid"]=bool(world.hybridTerrain);
        report["land_cells"]=std::count_if(world.cells.begin(),world.cells.end(),[](const auto& c) { return !c.sea; });
        report["river_cells"]=std::count_if(world.cells.begin(),world.cells.end(),[](const auto& c) { return c.river; });
        report["forms"]=nlohmann::json::array();
        std::size_t bytes=0,clipped=0;
        world::HeightField field(&world,world.seed);
        const auto envelope=world::streaming::hsimQuantisationFor(world);
        if (world.hybridTerrain) {
            report["fingerprint"]=world.hybridTerrain->fingerprint();
            report["diagnostics"]=world.hybridTerrain->diagnostics;
            const auto& cuts=world.hybridTerrain->incision;
            report["incision_step_metres"]=world.hybridTerrain->incisionStep;
            report["incision_bytes"]=cuts.size()*sizeof(std::uint16_t);
            report["incised_samples"]=std::count_if(cuts.begin(),cuts.end(),[](auto d) { return d>0; });
            report["max_incision_metres"]=cuts.empty() ? 0.0 : *std::max_element(cuts.begin(),cuts.end())/10.0;
            for (const auto& p : world.hybridTerrain->patches) {
                const int x=p.originX+p.radius,y=p.originY+p.radius;
                report["forms"].push_back({{"reference",p.referenceId},{"family",generation::landformFamilyName(p.family)},
                    {"island",p.island},{"centre_metres",{x,y}},{"radius_metres",p.radius},{"freshness",p.freshness}});
                bytes+=p.delta.size()*sizeof(std::int16_t);
                for (int dy=-p.radius;dy<=p.radius;dy+=128) for (int dx=-p.radius;dx<=p.radius;dx+=128) {
                    const Fixed height=field.sampleHeight((x+dx)/world::kSampleMetres,(y+dy)/world::kSampleMetres);
                    if (height<envelope.low || height>envelope.high) ++clipped;
                }
            }
        }
        report["baked_form_bytes"]=bytes; report["out_of_height_envelope"]=clipped;
        if (!svg.empty()) { preview(world,svg); report["preview"]=svg.string(); }
        if (!formsSvg.empty()) { formPreviews(world,formsSvg); report["forms_preview"]=formsSvg.string(); }
        std::cout<<report.dump(2)<<'\n';
        return clipped ? 2 : 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
