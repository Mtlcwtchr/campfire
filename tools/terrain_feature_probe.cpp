// Headless measurement of the production planner; no GPU/FPS claims.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include "nlohmann/json.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_plan.hpp"
#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/terrain_streaming/hydrology_builder.hpp"

namespace {
using namespace world::terrain;
using namespace world::streaming;
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
std::shared_ptr<SurfacePage> decode(const BakedPage& p) {
    auto out=std::make_shared<SurfacePage>();
    out->step=p.base.sampleMetres; out->padding=p.base.padding;
    out->side=p.base.width+2*out->padding;
    const HsimQuantisation q{p.base.elevationMin,p.base.elevationMax};
    const double low=p.base.elevationMin.toDouble(),range=(p.base.elevationMax-p.base.elevationMin).toDouble();
    for (std::size_t i=0;i<p.base.heightQuantized.size();++i) {
        out->bed.push_back(float(low+p.base.heightQuantized[i]*range/65535.0+
            (p.large.deltaQuantized.empty()?0:p.large.deltaQuantized[i]*0.01)+
            (p.medium.deltaQuantized.empty()?0:p.medium.deltaQuantized[i]*0.01)));
        out->head.push_back(float(low+(p.water.wet(i)?p.water.surfaceQuantized[i]:q.quantise(core::kZero))*range/65535.0));
    }
    return out;
}
TerrainView viewAt(double x,double y,double half) {
    TerrainView v;
    v.width=1600;v.height=1000;v.x=v.lookX=x;v.y=v.lookY=y;v.radius=half*2;
    v.matrix[0]=v.matrix[5]=float(1/half);v.matrix[3]=float(-x/half);v.matrix[7]=float(-y/half);
    v.matrix[10]=v.matrix[15]=1;v.prediction=v.matrix;
    v.target=geometryLevelFor(half*2/v.width);
    return v;
}
std::shared_ptr<const TerrainPlan> settle(TerrainPlanner& planner,const TerrainView& view,
    std::shared_ptr<const TerrainResidency> resident,double& time,double& work) {
    std::shared_ptr<const TerrainPlan> plan;
    for (int tick=0;tick<100;++tick) {
        if (!planner.request(view,resident,time+=0.1)) throw std::runtime_error("unexpected idle request");
        const auto deadline=Clock::now()+std::chrono::seconds(120);
        plan.reset();
        while (!(plan=planner.collect()) && Clock::now()<deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!plan) throw std::runtime_error("planner timed out");
        work+=plan->buildMs;
        if (!plan->needsUpdate) return plan;
    }
    throw std::runtime_error("planner did not settle");
}
std::vector<float> profile(const TerrainPlan& plan,double x,double y) {
    std::vector<float> out;
    for (int dy=-64;dy<=64;dy+=4) for (int dx=-64;dx<=64;dx+=2) {
        const double wx=x+dx,wy=y+dy;
        bool found=false;
        for (const auto& b:plan.coverage) {
            const double side=b.metres(),ox=b.tile.x*side,oy=b.tile.y*side;
            if (b.mesh && wx>=ox && wy>=oy && wx<ox+side && wy<oy+side) {
                out.push_back(b.mesh->sample(wx-ox,wy-oy)[0]);found=true;break;
            }
        }
        if (!found) throw std::runtime_error("profile has no covering mesh");
    }
    return out;
}
Json measure(const TerrainPlan& plan) {
    std::size_t triangles=0,gpu=0,source=0,payload=0,blocks=0;
    std::set<const AdaptiveMesh*> seen;
    for (const auto& b:plan.coverage) if (b.mesh && seen.insert(b.mesh.get()).second) payload+=b.mesh->bytes();
    for (const auto& b:plan.drawing) if (b.mesh) {
        triangles+=b.mesh->surfaceIndices/3;
        gpu+=b.mesh->vertices.size()*sizeof(AdaptiveVertex)+b.mesh->indices.size()*sizeof(std::uint32_t);
        source+=b.mesh->bed.size()*sizeof(float)*2;
        blocks+=b.mesh->step==2;
    }
    return {{"draw_blocks",plan.drawing.size()},{"h2_blocks",blocks},{"surface_triangles",triangles},
        {"draw_gpu_payload_bytes_including_skirts",gpu},{"draw_source_height_bytes",source},
        {"coverage_mesh_payload_bytes",payload},{"feature_samples",plan.detail.featureSamples},
        {"feature_evaluated",plan.detail.featureEvaluated},{"full_h4_samples",plan.detail.localSamples},
        {"view_detail_percent",plan.viewMesh.percent()},{"view_data_percent",plan.viewPages.percent()},
        {"capacity_limited",plan.capacityLimited},{"missing_pages",plan.missing.size()}};
}
Json simplification() {
    Json rows=Json::array();
    for (bool cliff:{false,true}) {
        const auto field=[=](double x,double y) {
            return std::array{float(100+0.02*x+0.01*y+0.04*std::sin(x/16)*std::cos(y/16)+
                (cliff?40*std::clamp((x-245)/4,0.0,1.0):0)),0.0f};
        };
        const auto parent=makeAdaptiveMesh(128,4,0.001,field);
        const auto target=[&](double x,double y){return parent->sample(x,y);};
        const auto strict=makeAdaptiveMesh(256,2,0.25,field,target);
        const auto relaxed=makeAdaptiveMesh(256,2,0.25,field,target,0.25);
        const auto spatial=makeAdaptiveMesh(256,2,1.0,field,{},0.25,[=](double x,double) {
            return cliff && x>=224 && x<=272?0.125:1.0;
        });
        double error=0;
        for (int y=0;y<=512;y+=2) for (int x=0;x<=512;x+=2)
            error=std::max(error,std::abs(double(relaxed->sample(x,y)[0])-field(x,y)[0]));
        rows.push_back({{"kind",cliff?"synthetic_cliff":"synthetic_flat"},
            {"strict_triangles",strict->surfaceIndices/3},{"relaxed_triangles",relaxed->surfaceIndices/3},
            {"height_error_max_m",error},{"strict_bytes",strict->bytes()},{"relaxed_bytes",relaxed->bytes()}});
        rows.back()["spatial_triangles"]=spatial->surfaceIndices/3;
    }
    return rows;
}
}
int main(int argc,char** argv) {
    try {
        int size=16,sites=2;std::uint64_t seed=11;bool stable=true;
        for (int i=1;i<argc;++i) {
            const std::string a=argv[i];
            if (a=="--world" && i+1<argc) size=std::stoi(argv[++i]);
            else if (a=="--seed" && i+1<argc) seed=std::stoull(argv[++i]);
            else if (a=="--sites" && i+1<argc) sites=std::stoi(argv[++i]);
            else if (a=="--regional") stable=false;
            else throw std::runtime_error("usage: terrain_feature_probe [--world N] [--seed N] [--sites N] [--regional]");
        }
        if (size<8 || size>128 || sites<1 || sites>20) throw std::runtime_error("invalid probe size");
        const auto policy=stable?kStableFeatureDataLodPolicy:kRegionalDataLodPolicy;
        int rootLod=0;
        while (world::tileMetresAt(rootLod,policy.chunkCells)<2048) ++rootLod;
        generation::WorldMapParams params;params.width=params.height=size;params.seed=seed;
        const auto world=generation::generateWorldMap(params);
        const auto graph=buildHydrologyGraph(world);
        PageStore pages(world,graph,hsimQuantisationFor(world),{512u<<20,2,2});
        Json output{{"seed",seed},{"world",size},{"policy",stable?"stable":"regional"},
            {"synthetic",simplification()},{"sites",Json::array()}};
        std::vector<std::pair<double,double>> locations;
        std::set<std::pair<int,int>> used;
        for (const auto& river:graph.segments) for (const auto& p:river.course) {
            const double x=p.position.x.toDouble(),y=p.position.y.toDouble();
            if (locations.size()>=std::size_t(sites)) break;
            if (p.halfWidth.toDouble()<3 || x<1024 || y<1024 || x>size*540-1024 || y>size*540-1024) continue;
            if (std::fmod(x,2048)<128 || std::fmod(x,2048)>1920 ||
                std::fmod(y,2048)<128 || std::fmod(y,2048)>1920) continue;
            if (used.emplace(int(x/2048),int(y/2048)).second) locations.emplace_back(x,y);
        }
        if (locations.empty()) locations.emplace_back(size*270.0,size*270.0);
        for (const auto [x,y]:locations) {
            std::cerr<<"site "<<x<<","<<y<<"\n";
            // One 2 km root plus all ancestors' H64 dependencies. H16 metadata
            // is complete before planning, just like runtime permanent preparation.
            const world::TileId root{int(x/2048),int(y/2048),rootLod};
            const int n=(size*540+511)/512;
            auto resident=std::make_shared<TerrainResidency>();resident->revision=1;
            resident->capacity={0,4096};
            std::size_t flags=0,cells=0;
            const auto start=Clock::now();
            for (std::uint8_t level:{4,2,1}) for (int py=-2;py<n+2;++py) for (int px=-2;px<n+2;++px) {
                if (level==1 && (px<root.x*4-2 || px>root.x*4+5 || py<root.y*4-2 || py>root.y*4+5)) continue;
                if (level==2 && (px<(root.x/2)*8-2 || px>(root.x/2)*8+9 ||
                    py<(root.y/2)*8-2 || py>(root.y/2)*8+9)) continue;
                const TileKey key{px,py,level};
                if (!pages.containsLand(key)) continue;
                auto p=pages.page(key);resident->pages.insert(key);resident->surfaces.emplace(key,decode(*p));
                if (level==2) { cells+=p->featureCells.size();flags+=std::count_if(p->featureCells.begin(),p->featureCells.end(),[](auto f){return f!=0;}); }
            }
            Json row{{"x",x},{"y",y},{"root",{root.x,root.y,root.lod}},{"mask_cells",cells},{"feature_cells",flags},
                {"page_preparation_ms",std::chrono::duration<double,std::milli>(Clock::now()-start).count()},
                {"views",Json::array()}};
            TerrainPlanner planner(world,pages,{root},policy);
            double time=0;std::vector<float> previous;
            for (double half:{256.0,2048.0,8192.0,256.0}) {
                const auto begin=Clock::now();double work=0;
                const auto plan=settle(planner,viewAt(x,y,half),resident,time,work);
                auto metrics=measure(*plan);const auto values=profile(*plan,x,y);
                double delta=0;
                if (!previous.empty()) for (std::size_t i=0;i<values.size();++i) delta=std::max(delta,std::abs(double(values[i])-previous[i]));
                metrics["half_view_m"]=half;metrics["profile_change_max_m"]=delta;
                metrics["worker_ms"]=work;metrics["settle_wall_ms"]=std::chrono::duration<double,std::milli>(Clock::now()-begin).count();
                previous=values;row["views"].push_back(std::move(metrics));
            }
            auto without=std::make_shared<TerrainResidency>(*resident);++without->revision;without->capacity[1]=0;
            std::erase_if(without->pages,[](auto k){return k.level==1;});
            std::erase_if(without->surfaces,[](const auto& e){return e.first.level==1;});
            double work=0;
            const auto lost=settle(planner,viewAt(x,y,256),without,time,work);
            auto metrics=measure(*lost);const auto values=profile(*lost,x,y);double delta=0;
            for (std::size_t i=0;i<values.size();++i) delta=std::max(delta,std::abs(double(values[i])-previous[i]));
            metrics["profile_change_max_m"]=delta;metrics["worker_ms"]=work;row["h8_eviction"]=std::move(metrics);
            row["page_store_payload_bytes"]=pages.stats().residentBytes;
            output["sites"].push_back(std::move(row));
        }
        std::cout<<output.dump(2)<<'\n';
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

