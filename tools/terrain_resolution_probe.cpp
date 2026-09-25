// Offline source-resolution experiment. Does not change runtime terrain policy.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "nlohmann/json.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_adaptive.hpp"
#include "game/world/terrain_streaming/base_tile_baker.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"

namespace {
using Json = nlohmann::json;
using core::Fixed;
using namespace world::streaming;
using Clock = std::chrono::steady_clock;
constexpr std::array<int,4> steps{4,8,16,64};
int referenceStep = 2;
constexpr int window = 256;
struct Distribution {
    std::vector<double> values;
    double squares = 0, sum = 0;
    void add(double x) { x = std::abs(x); values.push_back(x); squares += x*x; sum += x; }
    Json json() const {
        if (values.empty()) return {{"n",0}};
        auto sorted = values; std::sort(sorted.begin(),sorted.end());
        const auto q = [&](double p) { return sorted[std::size_t(p*(sorted.size()-1))]; };
        const auto above = [&](double x) { return 100.0*std::count_if(sorted.begin(),sorted.end(),[&](double v){return v>x;})/sorted.size(); };
        return {{"n",sorted.size()},{"mean",sum/sorted.size()},{"rmse",std::sqrt(squares/sorted.size())},
            {"p50",q(0.5)},{"p95",q(0.95)},{"p99",q(0.99)},{"max",sorted.back()},
            {"percent_gt_025",above(0.25)},{"percent_gt_05",above(0.5)},{"percent_gt_1",above(1)}};
    }
};
struct Grid {
    int step = 0, side = 0, padding = 0;
    std::array<std::vector<float>,4> height; // base, base+large, native, raw wet flag
    float at(double x,double y,int channel,bool bilinear=false) const {
        const double gx = x/step+padding, gy = y/step+padding;
        const int ix = std::clamp(int(std::floor(gx)),0,side-2), iy = std::clamp(int(std::floor(gy)),0,side-2);
        if (gx<0 || gy<0 || gx>side-1 || gy>side-1) throw std::runtime_error("probe outside padded page");
        const double fx=gx-ix, fy=gy-iy;
        const auto& h=height[channel]; const auto i=std::size_t(iy)*side+ix;
        if (bilinear) return float(std::lerp(std::lerp(double(h[i]),double(h[i+1]),fx),
            std::lerp(double(h[i+side]),double(h[i+side+1]),fx),fy));
        // Same NE--SW source triangles used by the regular terrain path.
        return float(fx+fy<=1 ? h[i]*(1-fx-fy)+h[i+1]*fx+h[i+side]*fy :
            h[i+side+1]*(fx+fy-1)+h[i+1]*(1-fy)+h[i+side]*(1-fx));
    }
};
Grid decode(const BakedPage& page) {
    Grid g; g.step=page.base.sampleMetres; g.padding=page.base.padding;
    g.side=page.base.width+2*g.padding;
    const double low=page.base.elevationMin.toDouble(), range=(page.base.elevationMax-page.base.elevationMin).toDouble();
    for (auto& h:g.height) h.resize(page.base.heightQuantized.size());
    for (std::size_t i=0;i<g.height[0].size();++i) {
        g.height[0][i]=float(low+page.base.heightQuantized[i]*range/65535.0);
        g.height[1][i]=g.height[0][i]+(page.large.deltaQuantized.empty()?0:page.large.deltaQuantized[i]*0.01f);
        g.height[2][i]=g.height[1][i]+(page.medium.deltaQuantized.empty()?0:page.medium.deltaQuantized[i]*0.01f);
        // Raw wetness, before the five-sample coverage blur. This measures
        // source support for a bank, not the complete water fragment shader.
        g.height[3][i]=page.water.surfaceQuantized[i]!=0 || page.water.waterBodyId[i]!=0 ? 1.0f : 0.0f;
    }
    return g;
}
struct Reference {
    std::array<double,3> height; // base, common large band, full large+medium
    double head = 0, bank = 0;
    bool river = false, wet = false;
};
Reference direct(const world::HeightField& field,const GraphCarver& carver,double x,double y) {
    const core::WorldPos p{Fixed::fromDoubleForContent(x),Fixed::fromDoubleForContent(y)};
    const auto pieces=field.piecesAt(p.x,p.y);
    const auto carved=carver.carve(p,pieces.country,pieces.moved);
    const auto residual=field.residualAt(p);
    const double ashore=std::clamp(carved.bankDistance.toDouble()/8.0,0.0,1.0);
    const double base=carved.floor.toDouble(), large=base+residual.large.toDouble()*ashore;
    return {{base,large,large+residual.medium.toDouble()*ashore},carved.surface.toDouble(),
        carved.bankDistance.toDouble(),carved.reach!=kInvalidRiverId && carved.body==kInvalidWaterBodyId,carved.wet};
}
struct Site {
    std::string kind;
    double x = 0,y = 0,width = 0,nx = 0,ny = 0;
};
std::vector<Site> choose(const generation::WorldMapData& map,const HydrologyGraph& graph,int perKind) {
    struct Candidate { Site site; double score; };
    std::vector<Candidate> land,rough;
    for (int y=1;y<map.height-1;++y) for (int x=1;x<map.width-1;++x) {
        const auto index=std::size_t(y)*map.width+x;
        if (map.cells[index].sea) continue;
        const double wx=(x+0.5)*generation::kMetresPerCell, wy=(y+0.5)*generation::kMetresPerCell;
        const auto h=map.cells[index].elevation;
        double relief=0;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx)
            relief=std::max(relief,double(std::abs(int(h)-int(map.cells[std::size_t(y+dy)*map.width+x+dx].elevation))));
        land.push_back({{"land",wx,wy},double(core::splitmix64(map.seed^index)>>11)});
        rough.push_back({{"rough",wx,wy},relief});
    }
    std::vector<Site> sites; std::set<std::pair<int,int>> pages;
    const auto take = [&](Site s) {
        if (pages.emplace(int(s.x/512),int(s.y/512)).second) { sites.push_back(std::move(s)); return true; }
        return false;
    };
    for (auto* candidates:{&land,&rough}) {
        std::stable_sort(candidates->begin(),candidates->end(),[](const auto& a,const auto& b){return a.score>b.score;});
        int n=0; for (const auto& c:*candidates) if (take(c.site) && ++n==perKind) break;
    }
    std::vector<Site> rivers;
    for (const auto& segment:graph.segments) for (std::size_t i=1;i+1<segment.course.size();++i) {
        const auto& p=segment.course[i];
        const double x=p.position.x.toDouble(), y=p.position.y.toDouble(), width=p.halfWidth.toDouble()*2;
        const double lx=x-std::floor(x/512)*512, ly=y-std::floor(y/512)*512;
        if (x<0 || y<0 || x>=map.width*540 || y>=map.height*540 || lx<96 || lx>416 || ly<96 || ly>416 || width>64) continue;
        const auto& a=segment.course[i-1].position; const auto& b=segment.course[i+1].position;
        const double dx=(b.x-a.x).toDouble(), dy=(b.y-a.y).toDouble(), length=std::hypot(dx,dy);
        if (length>0) rivers.push_back({"river",x,y,width,-dy/length,dx/length});
    }
    std::stable_sort(rivers.begin(),rivers.end(),[](const auto& a,const auto& b){return a.width<b.width;});
    // Equal width quantile bins, not just the widest trunk or the easiest stream.
    for (int bin=0;bin<perKind;++bin) {
        const auto lo=rivers.size()*bin/perKind, hi=rivers.size()*(bin+1)/perKind;
        for (auto i=lo;i<hi;++i) if (take(rivers[i])) break;
    }
    return sites;
}
struct Metrics {
    Distribution base,common,native,adaptive,delta4common,delta4native;
    Distribution trianglesCommon,trianglesNative,bakeMs;
    Distribution widthError,bankError,depthError;
    std::size_t profiles=0,missed=0,unbounded=0;
    Json json() const {
        return {{"base_error_m",base.json()},{"common_large_error_m",common.json()},
            {"native_vs_full_error_m",native.json()},{"adaptive_native_error_m",adaptive.json()},
            {"difference_from_H4_common_m",delta4common.json()},{"difference_from_H4_native_m",delta4native.json()},
            {"page_triangles_common",trianglesCommon.json()},{"page_triangles_native",trianglesNative.json()},
            {"bake_ms",bakeMs.json()},{"river_width_error_m",widthError.json()},
            {"river_bank_error_m",bankError.json()},{"river_depth_error_m",depthError.json()},
            {"river_profiles",profiles},{"river_missed",missed},{"river_unbounded",unbounded}};
    }
};
using Group = std::array<Metrics,4>;
void profiles(const Site& site,const std::array<Grid,4>& grids,const world::HeightField& field,
              const GraphCarver& carver,Group& metrics,Json& rows) {
    if (site.kind!="river") return;
    const double ox=std::floor(site.x/512)*512, oy=std::floor(site.y/512)*512;
    const auto centre=direct(field,carver,site.x,site.y);
    if (!centre.wet || !centre.river) return;
    const double extent=std::min({128.0,site.x-ox-1,511-site.x+ox,site.y-oy-1,511-site.y+oy});
    constexpr double increment=0.5;
    const int half=int(extent/increment);
    std::vector<double> truth(2*half+1);
    std::array<std::vector<double>,4> heights;
    for (auto& h:heights) h.resize(truth.size());
    for (int i=-half;i<=half;++i) {
        const double x=site.x+i*increment*site.nx, y=site.y+i*increment*site.ny;
        truth[i+half]=direct(field,carver,x,y).wet ? 0.0 : 1.0;
        for (int s=0;s<4;++s) heights[s][i+half]=1.0-grids[s].at(x-ox,y-oy,3,true);
    }
    const auto banks=[&](const auto& h) -> std::array<double,2> {
        constexpr double level=0.5;
        if (h[half]>=level) return {0,0};
        int left=half,right=half;
        while (left>0 && h[left-1]<level) --left;
        while (right<2*half && h[right+1]<level) ++right;
        if (left==0 || right==2*half) return {-1,-1};
        const double a=left-1+(h[left-1]-level)/(h[left-1]-h[left]);
        const double b=right+(level-h[right])/(h[right+1]-h[right]);
        return {(a-half)*increment,(b-half)*increment};
    };
    const auto ref=banks(truth);
    if (ref[1]<=ref[0]) return;
    Json row{{"graph_width_m",site.width},{"reference_width_m",ref[1]-ref[0]},{"x",site.x},{"y",site.y}};
    for (int s=0;s<4;++s) {
        const auto measured=banks(heights[s]); auto& m=metrics[s]; ++m.profiles;
        const double bed=grids[s].at(site.x-ox,site.y-oy,1);
        m.depthError.add(bed-centre.height[1]);
        if (measured[0]==-1 && measured[1]==-1) {
            ++m.unbounded;
            row["H"+std::to_string(steps[s])]={{"width_m",nullptr},{"unbounded",true},
                {"depth_m",std::max(0.0,centre.head-bed)}};
            continue;
        }
        const double width=std::max(0.0,measured[1]-measured[0]);
        m.missed+=width==0; m.widthError.add(width-(ref[1]-ref[0]));
        if (width>0) { m.bankError.add(measured[0]-ref[0]); m.bankError.add(measured[1]-ref[1]); }
        row["H"+std::to_string(steps[s])]={{"width_m",width},{"depth_m",std::max(0.0,centre.head-bed)}};
    }
    rows.push_back(std::move(row));
}
Json synthetic() {
    Json rows=Json::array();
    // An isolated 8 m terrace riser, multiple alignments against each lattice.
    // Separate analytic experiment, not a claim that the generator emits these.
    for (double width:{2.0,4.0,8.0,16.0,32.0}) for (int step:steps) {
        Distribution error,peakRatio;
        for (int phase=0;phase<32;++phase) {
            const double offset=step*(phase+0.5)/32;
            const auto h=[&](double x){return 8*std::clamp((x-offset)/width,0.0,1.0);};
            double peak=0;
            for (double x=-step;x<=width+2*step;x+=0.25) {
                const double a=std::floor(x/step)*step, lo=h(a), hi=h(a+step);
                error.add(std::lerp(lo,hi,(x-a)/step)-h(x));
                peak=std::max(peak,(hi-lo)/step);
            }
            peakRatio.add(peak/(8/width));
        }
        rows.push_back({{"riser_run_m",width},{"step_m",step},{"height_error_m",error.json()},
                        {"peak_slope_retained_fraction",peakRatio.json()}});
    }
    return rows;
}
void checkInterpolation() {
    Grid g; g.step=4; g.side=3;
    for (auto& h:g.height) for (int y=0;y<3;++y) for (int x=0;x<3;++x) h.push_back(float(10+2*x*4+3*y*4));
    for (double y=0;y<=8;y+=0.25) for (double x=0;x<=8;x+=0.25)
        if (std::abs(g.at(x,y,0)-(10+2*x+3*y))>1e-6) throw std::runtime_error("plane interpolation failed");
    g.height[0]={50,200,0,800,100,0,0,0,0};
    if (g.at(2,2,0)!=500) throw std::runtime_error("wrong triangle diagonal");
}
}
int main(int argc,char** argv) {
    try {
        int worldSize=64,perKind=6; std::vector<std::uint64_t> seeds{11,71,2026};
        for (int i=1;i<argc;++i) {
            const std::string a=argv[i];
            if (a=="--world" && i+1<argc) worldSize=std::stoi(argv[++i]);
            else if (a=="--reference-step" && i+1<argc) referenceStep=std::stoi(argv[++i]);
            else if (a=="--sites-per-kind" && i+1<argc) perKind=std::stoi(argv[++i]);
            else if (a=="--seed" && i+1<argc) seeds={std::stoull(argv[++i])};
            else if (a=="--self-test") { checkInterpolation(); std::cout<<"interpolation checks passed\n"; return 0; }
            else throw std::runtime_error("usage: terrain_resolution_probe [--world N] [--sites-per-kind N] [--seed N] [--reference-step 1|2] [--self-test]");
        }
        if (worldSize<8 || perKind<1 || (referenceStep!=1 && referenceStep!=2)) throw std::runtime_error("invalid probe size");
        checkInterpolation();
        std::map<std::string,Group> groups; Distribution medium;
        Json sitesJson=Json::array(),riverRows=Json::array(),worlds=Json::array();
        const auto started=Clock::now();
        for (const auto seed:seeds) {
            generation::WorldMapParams params; params.seed=seed; params.width=params.height=worldSize;
            const auto map=generation::generateWorldMap(params);
            const auto graph=buildHydrologyGraph(map);
            const auto quant=hsimQuantisationFor(map);
            BaseTileBaker baker(map,graph,quant);
            const auto sites=choose(map,graph,perKind);
            worlds.push_back({{"seed",seed},{"sites",sites.size()},{"river_segments",graph.segments.size()},
                {"quantisation_half_step_m",quant.resolution().toDouble()}});
            for (const auto& site:sites) {
                const int px=int(site.x/512),py=int(site.y/512);
                const double ox=px*512,oy=py*512;
                const int x0=std::clamp(int(site.x-ox)-window/2,0,512-window)/2*2;
                const int y0=std::clamp(int(site.y-oy)-window/2,0,512-window)/2*2;
                std::cerr<<"seed="<<seed<<" "<<site.kind<<" page="<<px<<","<<py<<"\n";
                std::array<Grid,4> grids;
                std::array<std::shared_ptr<const world::terrain::AdaptiveMesh>,4> adaptive;
                BakedPage parent;
                Json row{{"seed",seed},{"kind",site.kind},{"page",{px,py}},{"window_origin",{ox+x0,oy+y0}}};
                for (int s=3;s>=0;--s) {
                    const int step=steps[s]; const auto begin=Clock::now();
                    auto page=baker.bakePage({px,py,std::uint8_t(step==64?4:step==16?2:step==8?1:0)},step,2,{},s==3?nullptr:&parent);
                    if (!page.base.valid()) throw std::runtime_error("invalid baked page");
                    const double ms=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
                    grids[s]=decode(page); parent=std::move(page);
                    const auto& g=grids[s];
                    auto mesh=[&](int channel) { return world::terrain::makeAdaptiveMesh(512/step,step,0.25,
                        [&](double x,double y){return std::array{g.at(x,y,channel),0.0f};}); };
                    const auto common=mesh(1); adaptive[s]=mesh(2);
                    for (const auto& name:{std::string("all"),site.kind}) {
                        auto& m=groups[name][s]; m.bakeMs.add(ms);
                        m.trianglesCommon.add(common->surfaceIndices/3); m.trianglesNative.add(adaptive[s]->surfaceIndices/3);
                    }
                    row["H"+std::to_string(step)]={{"bake_ms",ms},{"evaluated",parent.evaluatedSamples},
                        {"reused",parent.reusedSamples},{"gpu_page_bytes",g.height[0].size()*34},
                        {"source_triangles",2*(512/step)*(512/step)},
                        {"adaptive_common_triangles",common->surfaceIndices/3},{"adaptive_native_triangles",adaptive[s]->surfaceIndices/3}};
                }
                GraphCarver carver(graph,{{Fixed::fromInt(px*512),Fixed::fromInt(py*512)},
                                         {Fixed::fromInt((px+1)*512),Fixed::fromInt((py+1)*512)}},900);
                const int n=window/referenceStep;
                std::vector<Reference> reference(n*n);
                for (int y=0;y<n;++y) for (int x=0;x<n;++x)
                    reference[y*n+x]=direct(baker.field(),carver,ox+x0+1+x*referenceStep,oy+y0+1+y*referenceStep);
                // Sanity check the oracle against the actual baked source, not HeightField::heightAt's H4 interpolation.
                for (int y=(y0+3)/4*4;y<=y0+window;y+=4) for (int x=(x0+3)/4*4;x<=x0+window;x+=4) {
                    const auto ref=direct(baker.field(),carver,ox+x,oy+y);
                    if (std::abs(grids[0].at(x,y,2)-ref.height[2])>quant.resolution().toDouble()+0.012) {
                        std::cerr<<"oracle mismatch at "<<ox+x<<","<<oy+y<<" source="<<grids[0].at(x,y,2)<<" direct="<<ref.height[2]<<"\n";
                        throw std::runtime_error("direct oracle disagrees with H4 source");
                    }
                }
                std::array<double,4> worst{};
                for (int y=1;y<n-1;++y) for (int x=1;x<n-1;++x) {
                    const auto& r=reference[y*n+x];
                    const double lx=x0+1+x*referenceStep,ly=y0+1+y*referenceStep;
                    const auto h=[&](int dx,int dy){return reference[(y+dy)*n+x+dx].height[1];};
                    const double bend=std::max(std::abs(h(-1,0)-2*h(0,0)+h(1,0)),std::abs(h(0,-1)-2*h(0,0)+h(0,1)))/referenceStep;
                    const double slope=std::hypot(h(1,0)-h(-1,0),h(0,1)-h(0,-1))/(2*referenceStep);
                    std::vector<std::string> names{"all",site.kind};
                    if (!r.wet) { names.push_back("dry"); if (bend>0.25) names.push_back("dry_breaks"); if(slope>1) names.push_back("dry_steep"); }
                    if (r.river && std::abs(r.bank)<8) names.push_back("river_bank_8m");
                    medium.add(r.height[2]-r.height[1]);
                    for (int s=0;s<4;++s) {
                        const double base=grids[s].at(lx,ly,0), common=grids[s].at(lx,ly,1), native=grids[s].at(lx,ly,2);
                        const double simplified=adaptive[s]->sample(lx,ly)[0];
                        if (std::abs(native-r.height[2])>worst[s]) {
                            worst[s]=std::abs(native-r.height[2]);
                            row["worst"]["H"+std::to_string(steps[s])]={{"x",ox+lx},{"y",oy+ly},
                                {"error_m",worst[s]},{"source_height_m",native},{"reference_height_m",r.height[2]},
                                {"bank_distance_m",r.bank},{"wet",r.wet},{"river",r.river}};
                        }
                        for (const auto& name:names) {
                            auto& m=groups[name][s]; m.base.add(base-r.height[0]); m.common.add(common-r.height[1]);
                            m.native.add(native-r.height[2]); m.adaptive.add(simplified-r.height[2]);
                            m.delta4common.add(common-grids[0].at(lx,ly,1)); m.delta4native.add(native-grids[0].at(lx,ly,2));
                        }
                    }
                }
                // Keep a directly inspectable cross through the worst H4 point.
                // A percentile alone must not hide sub-grid incisions or jumps.
                const double wx=row["worst"]["H4"]["x"].get<double>();
                const double wy=row["worst"]["H4"]["y"].get<double>();
                for (int axis=0;axis<2;++axis) for (int delta=-4;delta<=4;++delta) {
                    const double x=wx+(axis==0?delta:0), y=wy+(axis==1?delta:0);
                    const auto ref=direct(baker.field(),carver,x,y);
                    const core::WorldPos p{Fixed::fromDoubleForContent(x),Fixed::fromDoubleForContent(y)};
                    const auto pieces=baker.field().piecesAt(p.x,p.y);
                    row["worst_H4_cross"].push_back({{"axis",axis},{"offset_m",delta},{"x",x},{"y",y},
                        {"uncarved_m",(pieces.country+pieces.moved).toDouble()},{"direct_base_m",ref.height[0]},
                        {"direct_full_m",ref.height[2]},{"H4_m",grids[0].at(x-ox,y-oy,2)},
                        {"H8_m",grids[1].at(x-ox,y-oy,2)},{"bank_m",ref.bank},{"wet",ref.wet}});
                }
                const auto previousProfiles=riverRows.size();
                profiles(site,grids,baker.field(),carver,groups["river"],riverRows);
                if (riverRows.size()!=previousProfiles) riverRows.back()["seed"]=seed;
                sitesJson.push_back(std::move(row));
            }
        }
        Json out{{"world_size",worldSize},{"worlds",worlds},{"reference_step_m",referenceStep},{"reference_offset_m",1},
            {"patch_window_m",window},{"adaptive_tolerance_m",0.25},{"medium_band_magnitude_m",medium.json()},
            {"sites",sitesJson},{"river_profiles",riverRows},{"synthetic_terrace",synthetic()},
            {"seconds",std::chrono::duration<double>(Clock::now()-started).count()}};
        for (const auto& [name,group]:groups) for (int s=0;s<4;++s) out["groups"][name]["H"+std::to_string(steps[s])]=group[s].json();
        std::cout<<out.dump(2)<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}

