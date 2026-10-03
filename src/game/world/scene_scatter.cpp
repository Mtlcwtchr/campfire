#include "game/world/scene_scatter.hpp"
#include "engine/biomes/patch_field.hpp"
#include "engine/core/rng.hpp"
#include "game/world/object_id.hpp"
#include "../../../assets/shaders/environment_detail.hlsli"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace world::decor {
namespace {
std::int64_t firstCell(std::int64_t coordinate) {
    return coordinate/kCell-(coordinate%kCell<0);
}
std::int64_t pastCell(std::int64_t coordinate) {
    return coordinate/kCell+(coordinate%kCell>0);
}
double smooth(double a,double b,double x) {
    const double t=std::clamp((x-a)/(b-a),0.0,1.0); return t*t*(3-2*t);
}
double noise(std::uint64_t seed,double x,double y,double wavelength) {
    x/=wavelength;y/=wavelength;
    const auto ix=std::int64_t(std::floor(x)),iy=std::int64_t(std::floor(y));
    const double u=smooth(0,1,x-ix),v=smooth(0,1,y-iy);
    const auto at=[seed](std::int64_t a,std::int64_t b) {
        return double(core::splitmix64(seed^core::splitmix64(std::uint64_t(a))^
            core::splitmix64(std::uint64_t(b)+0x7135u))>>11)*0x1p-53;
    };
    return std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),u),
                     std::lerp(at(ix,iy+1),at(ix+1,iy+1),u),v);
}
// The scene model a plant or a prop names; kModels.size() when none.
std::uint32_t modelNamed(const std::string& name) {
    for (std::uint32_t i=0;i<kModels.size();++i) if (name==kModels[i]) return i;
    return std::uint32_t(kModels.size());
}
// One of a weighted list, by a number in [0, 1).
const std::string* pickWeighted(const engine::biomes::Weighted& list,double t) {
    double total=0;
    for (const auto& [name,w]:list) total+=std::max(0.0,w);
    if (!(total>0)) return nullptr;
    t*=total;
    for (const auto& [name,w]:list) if ((t-=std::max(0.0,w))<0) return &name;
    return &list.back().first;
}
// A hectare's count to a candidate's chance: one candidate an 8 m cell.
constexpr double kCellsPerHectare=10000.0/(kCell*kCell);

std::uint64_t detailId(std::uint64_t seed,PlacementStage stage,int cell,std::int64_t gx,std::int64_t gy) {
    const auto perPage=kPlacementPageMetres/cell;
    const auto px=gx/perPage-(gx%perPage<0),py=gy/perPage-(gy%perPage<0);
    return objectId(seed,stage,1,px,py,std::uint32_t((gx-px*perPage)+(gy-py*perPage)*perPage));
}
// Separate stages keep old removals and species choices valid. This runs in
// the existing page traversal; extra height queries are only for admitted
// clumps or cliff supports, never for every point of the finer lattice.
void environmentCell(Scatter& out,std::uint64_t seed,std::int64_t gx,std::int64_t gy,
        double x,double y,const Site& site,ScatterBounds bounds,double width,double height,
        const SiteSample& sample,const WetTest& wet) {
    if (!site.hasEcology || !site.hasMaterials) return;
    if (!std::isfinite(site.ecology.moisture+site.ecology.canopy+site.ecology.disturbance+
        site.ecology.shrubs+site.detailDensity+site.rock+site.snow+site.sand)) return;
    const auto inside=[&](double a,double b) {
        return a>=0 && b>=0 && a<width && b<height && a>=bounds.minX && a<bounds.maxX && b>=bounds.minY && b<bounds.maxY;
    };
    const auto read=[&](double a,double b) { ++out.sampled; return sample(a,b); };
    const auto usable=[](const Site& s) { return std::isfinite(s.height+s.water+s.slope) && s.height>s.water+0.4 && s.height>=0; };
    const auto& e=site.ecology;
    const double habitat=environmentMossHabitat(e.moisture,e.canopy,e.disturbance,float(site.snow));
    const double painted=std::clamp(site.detailDensity,0.0,4.0);
    if (painted<=0) return;
    // Outcrops occupy a 16 m lattice. No pairwise/iteration-order collision
    // solver: the bounded footprint and lattice separation give stable spacing.
    if (gx%2==0 && gy%2==0 && site.slope>0.8 && site.rock>0.22 && site.snow<0.4) {
        const auto id=detailId(seed,PlacementStage::Cliff,16,gx/2,gy/2);
        auto hash=id;
        const auto roll=[&]() { hash=core::splitmix64(hash); return double(hash>>11)*0x1p-53; };
        const double outcrop=smooth(0.35,0.78,noise(seed^0x7b381u,x,y,64));
        const double chance=0.38*smooth(0.8,1.8,site.slope)*smooth(0.22,0.45,site.rock)*outcrop*painted;
        if (inside(x,y) && roll()<chance) {
            // Sample the real relief and normal; coarse biome slope alone does
            // not tell which way a face looks or whether a lip has a landing.
            const auto east=read(std::min(width-0.01,x+4),y),west=read(std::max(0.0,x-4),y);
            const auto north=read(x,std::min(height-0.01,y+4)),south=read(x,std::max(0.0,y-4));
            const double dx=(east.height-west.height)/8,dy=(north.height-south.height)/8;
            const double slope=std::hypot(dx,dy);
            if (std::isfinite(slope) && slope>0.65) {
                const double downhillX=-dx/slope,downhillY=-dy/slope;
                const auto model=e.moisture<0.35f && site.boreal<0.3?kCliffWarm:kCliffGrey;
                const auto shape=kEnvironmentBounds[model-kCliffGrey];
                // Fit the scan to a fraction of the local relief, in metres.
                // Keep its horizontal footprint below the lattice separation.
                const double relief=std::max({east.height,west.height,north.height,south.height})-
                                    std::min({east.height,west.height,north.height,south.height});
                const float scale=float(std::clamp(relief*0.6/shape.height,0.55,
                    std::min(1.65,11.0/shape.width))*(0.90+roll()*0.1));
                // The face is at -Y; the scan's broad uphill back is embedded.
                // Using its full frame radius as the landing would bury the
                // whole face on a steep slope rather than just its roots.
                const double radius=shape.front*scale;
                const double fx=std::clamp(x+downhillX*radius,0.0,width-0.01);
                const double fy=std::clamp(y+downhillY*radius,0.0,height-0.01);
                const auto foot=read(fx,fy);
                const double bury=shape.height*scale*0.16;
                const double z=std::min(site.height-shape.height*scale*0.30,foot.height-bury);
                if (usable(foot) && z+shape.height*scale>foot.height+shape.height*scale*0.3 &&
                    (!wet || (!wet(x,y) && !wet(fx,fy)))) {
                    const float yaw=float(std::atan2(downhillY,downhillX)+std::acos(-1.0)*0.5+(roll()-0.5)*0.16);
                    out.objects.push_back({id,x,y,z,scale,yaw,0,float(0.94+roll()*0.1),model,float(habitat)});
                    ++out.populations[model];
                }
            }
        }
    }
    // Four independent candidates per 8 m cell. The low-frequency habitat
    // and a 12 m patch mask make a thicket with openings, rather than a lawn
    // of evenly spaced specimens. Canopy suppresses tall shrubs, not ferns.
    const double support=(1-smooth(0.12,0.45,site.sand))*(1-smooth(0.30,0.70,site.rock))*
                         (1-smooth(0.10,0.4,site.snow))*(1-smooth(0.32,0.72,site.slope));
    if (support<=0 || site.slope>0.72 ||
        (site.forestBiome && site.forestBiome->undergrowth=="none")) return;
    for (int oy=0;oy<2;++oy) for (int ox=0;ox<2;++ox) {
        const auto sx=gx*2+ox,sy=gy*2+oy;
        const auto id=detailId(seed,PlacementStage::Undergrowth,4,sx,sy);
        auto hash=id;
        const auto roll=[&]() { hash=core::splitmix64(hash); return double(hash>>11)*0x1p-53; };
        const double px=(sx+0.12+roll()*0.76)*4,py=(sy+0.12+roll()*0.76)*4;
        if (!inside(px,py)) continue;
        const double patch=smooth(0.50,0.62,engine::biomes::patchField(px,py));
        const double edge=smooth(0.06,0.25,e.canopy)*(1-smooth(0.55,0.85,e.canopy));
        const double richness=smooth(0.18,0.65,e.moisture)*(1-smooth(0.35,0.8,e.disturbance));
        const double chance=painted*support*patch*richness*(0.08+0.50*e.canopy+0.60*edge+0.40*e.shrubs);
        if (roll()>=chance) continue;
        const auto local=read(px,py);
        if (!usable(local) || local.slope>0.55 || local.sand>0.35 || local.snow>0.2 ||
            local.detailDensity<=0 || (wet && (wet(px+0.6,py) || wet(px-0.6,py) || wet(px,py+0.6) || wet(px,py-0.6)))) continue;
        const double kind=roll();
        const auto model=kind<habitat*0.28 && local.slope<0.16?kMoss:
            kind<0.38+e.canopy*0.4?kFern:kind<0.83?kLowShrub:kDenseShrub;
        const double sizeRoll=roll();
        // Match the role's target height, independent of the source pack's
        // dimensions. Keep each model's proportions and avoid metre-high litter.
        const float scale=float(model==kDenseShrub?(0.75+sizeRoll*0.55)/kEnvironmentBounds[model-kCliffGrey].height:
                                model==kLowShrub?(0.25+sizeRoll*0.25)/kEnvironmentBounds[model-kCliffGrey].height:
                                0.72+sizeRoll*0.65);
        const double sink=groundSink(model,scale);
        out.objects.push_back({id,px,py,local.height-sink,scale,float(roll()*6.283185307),
            float(roll()*6.283185307),float(0.94+roll()*0.12),model});
        ++out.populations[model];
    }
}
}

double forestDensity(std::uint64_t seed,double x,double y) {
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x)>1e12 || std::abs(y)>1e12) return 0;
    const double mass=0.7*noise(seed^0x1741u,x,y,512)+0.3*noise(seed^0x294bu,x,y,192);
    const double clearing=smooth(0.58,0.79,noise(seed^0xc731u,x,y,80));
    const double grove=0.72+0.4*noise(seed^0x831du,x,y,32);
    return std::clamp(smooth(0.43,0.66,mass)*(1-0.97*clearing)*grove,0.0,1.0);
}
Lod objectLod(double pixels,double distance,double meshStartPixels) {
    if (!std::isfinite(pixels) || !std::isfinite(distance) || !std::isfinite(meshStartPixels) ||
        pixels<=0 || distance<0 || meshStartPixels<kMeshFloorPixels) return {};
    return {float(smooth(meshStartPixels,meshStartPixels*(38.0/22),pixels)),
            float(smooth(kImpostorFloorPixels,2,pixels)*(1-smooth(480,600,distance)))};
}
double meshPixelThreshold(std::span<const MeshDemand> demands,std::size_t budget) {
    std::vector<MeshDemand> ordered;
    ordered.reserve(demands.size());
    for (const auto& demand:demands)
        if (std::isfinite(demand.pixels) && demand.pixels>kMeshFloorPixels && demand.triangles)
            ordered.push_back(demand);
    std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b) { return a.pixels>b.pixels; });
    std::size_t used=0;
    for (const auto& demand:ordered) {
        // A strict pixel cutoff also excludes ties, so ordering cannot overspend.
        if (demand.triangles>budget-used) return demand.pixels;
        used+=demand.triangles;
    }
    // Everything asked for fits. The crossover goes as low as it is allowed to
    // rather than stopping at a number: a budget with room to spare and a frame
    // full of impostors is the threshold refusing to do its job.
    return kMeshFloorPixels;
}
int impostorView(double angle,double yaw) {
    if (!std::isfinite(angle) || !std::isfinite(yaw)) return 0;
    const double turns=(angle-yaw)/(2*std::acos(-1.0));
    return int(std::floor((turns-std::floor(turns))*8+0.5))%8;
}
ImpostorPair impostorPair(double angle,double yaw) {
    if (!std::isfinite(angle) || !std::isfinite(yaw)) return {};
    const double turns=(angle-yaw)/(2*std::acos(-1.0));
    // The eight views are baked AT k/8 of a turn, so the two nearest are the
    // one below and the one above - not a pair straddling the object. At blend
    // zero this is exactly the view impostorView would pick, which is what
    // makes the blended path a strict improvement on the snapping one.
    const double exact=(turns-std::floor(turns))*8;
    const int first=int(std::floor(exact))%8;
    return {first,(first+1)%8,float(exact-std::floor(exact))};
}
std::size_t ScatterBounds::cells() const {
    if (maxX<=minX || maxY<=minY) return 0;
    const auto lowX=firstCell(minX),lowY=firstCell(minY);
    const auto highX=pastCell(maxX),highY=pastCell(maxY);
    const auto wide=std::uint64_t(highX-lowX),high=std::uint64_t(highY-lowY);
    return wide && high<=std::numeric_limits<std::size_t>::max()/wide
        ? std::size_t(wide*high):std::numeric_limits<std::size_t>::max();
}
Scatter scatter(std::uint64_t seed,ScatterBounds bounds,double width,double height,
                const LandTest& land,const SiteSample& sample,const WetTest& wet,bool environment) {
    const auto cells=bounds.cells();
    if (!land || !sample || !(width>0 && height>0) || !std::isfinite(width+height) ||
        !cells || cells>kMaxScatterCells)
        throw std::invalid_argument("invalid scene scatter domain");
    Scatter result;
    result.objects.reserve(std::min<std::size_t>(cells,16384));
    std::map<std::pair<int,int>,bool> mask;
    const auto minGx=firstCell(bounds.minX),minGy=firstCell(bounds.minY);
    const auto maxGx=pastCell(bounds.maxX),maxGy=pastCell(bounds.maxY);
    for (auto gy=minGy;gy<maxGy;++gy)
        for (auto gx=minGx;gx<maxGx;++gx) {
            if (gx<0 || gy<0 || gx*kCell>=width || gy*kCell>=height) continue;
            const auto key=std::pair{int(gx*kCell/512),int(gy*kCell/512)};
            auto [entry,fresh]=mask.try_emplace(key);
            if (fresh) { entry->second=land(key.first,key.second); result.waterTilesSkipped+=!entry->second; }
            if (!entry->second) continue; // no point-level height/water/biome work over proven ocean
            auto hash=core::splitmix64(seed^core::splitmix64(std::uint64_t(gx))^
                                      core::splitmix64(std::uint64_t(gy)+0x7135u));
            // The name the delta knows it by: its page and its slot in the
            // page, not where the jitter below happens to put it (object_id.hpp).
            // The jitter keeps its own stream, so no object moved when the id
            // stopped being that stream's first value.
            constexpr std::int64_t perPage=kPlacementPageMetres/kCell;
            static_assert(kPlacementPageMetres%kCell==0 && kPlacementPageMetres==kRegion);
            const auto pageX=gx/perPage-(gx%perPage<0),pageY=gy/perPage-(gy%perPage<0);
            const auto id=objectId(seed,PlacementStage::Decor,kDecorStableDomain,pageX,pageY,
                std::uint32_t((gx-pageX*perPage)+(gy-pageY*perPage)*perPage));
            const auto random=[&]() { hash=core::splitmix64(hash);return double(hash>>11)*0x1p-53; };
            const double x=(gx+0.15+random()*0.7)*kCell,y=(gy+0.15+random()*0.7)*kCell;
            if (x>=width || y>=height) continue;
            const bool owns=x>=bounds.minX && x<bounds.maxX && y>=bounds.minY && y<bounds.maxY;
            if (!owns && !environment) continue;
            const Site site=sample(x,y); ++result.sampled;
            if (!std::isfinite(site.height+site.water+site.slope+site.forest+site.boreal) ||
                site.height<=site.water+0.4 || site.height<0) continue;
            if (environment) environmentCell(result,seed,gx,gy,x,y,site,bounds,width,height,sample,wet);
            if (!owns) continue;
            if (site.slope>1.8) continue;
            const double choice=random(),forest=std::clamp(site.forest,0.0,1.0);
            const double density=forestDensity(seed,x,y);
            // What the ground is. Sand, bare rock, standing water and snow do
            // not carry a wood; poor soil carries a thin one.
            const double bare=site.hasMaterials
                ? (1-smooth(0.20,0.55,site.sand))*(1-smooth(0.30,0.65,site.rock))*
                  (1-smooth(0.35,0.75,site.marsh)*0.7)*(1-smooth(0.30,0.60,site.snow))
                : 1.0;
            const double soil=site.hasEcology ? 0.25+0.75*smooth(0.12,0.55,site.ecology.fertility) : 1.0;
            const double habitat=forest*(1-smooth(0.45,0.95,site.slope))*(1-smooth(1800,2400,site.height));
            double trees=(site.hasEcology ? site.ecology.canopy*0.94*(1-smooth(0.45,0.95,site.slope))
                                                : habitat*0.94*density)*bare*soil;
            const double scrub=noise(seed^0x529du,x,y,40);
            // Favor the transition belt, not the empty centre of a clearing.
            // The independent scrub field still breaks that belt into patches.
            const double edge=smooth(0.04,0.2,density)*(1-smooth(0.45,0.8,density));
            double bushes=(site.hasEcology ? site.ecology.shrubs*0.28 : habitat*(0.018+0.23*edge))
                *smooth(0.25,0.75,scrub)*(0.35+0.65*bare);
            const double vegetationIsland=smooth(0.50,0.62,engine::biomes::patchField(x,y));
            bushes*=vegetationIsland*(1.4+1.6*vegetationIsland);
            double mushrooms=(site.hasEcology ? site.ecology.canopy*site.ecology.moisture*
                (0.01+site.ecology.deadwood*0.08) : habitat*density*0.028)*(1-smooth(0.25,0.5,site.slope))*bare;
            const double outcrop=noise(seed^0xe157u,x,y,112);
            // Stones lie in heaps and spills, not salted evenly: a tighter
            // field gathers the outcrop's share into clusters.
            const double heap=0.25+1.6*smooth(0.45,0.72,noise(seed^0x3a9fu,x,y,24));
            double rocks=heap*(0.004+0.05*smooth(0.45,0.8,outcrop))*(1-0.7*trees)*
                (site.hasEcology ? (1-site.ecology.fertility*0.8)*(1+std::min(site.slope,1.0)*4) : 1)*
                (site.hasMaterials ? 1+3*smooth(0.30,0.70,site.rock) : 1);
            // Where the terrain categories rule, loose boulders are rare: the decor
            // biomes place stones where they belong, in groups.
            if (site.decorBiome || site.forestBiome) rocks*=0.2;
            // Deadwood fills existing candidates, never adds another world
            // traversal or changes a site's stable ID. Keep logs off cliffs.
            double deadwood=(site.hasEcology ? site.ecology.deadwood*0.2 : habitat*density*0.055)
                *(1-smooth(0.10,0.30,site.slope))*bare;
            std::uint32_t model;
            float sizeBy=1,tintBy=1;
            if (site.detailDensity!=1.0) {
                // The hand's density brush over whatever would grow here.
                const double k=std::max(0.0,site.detailDensity);
                trees=std::min(1.0,trees*k);
                bushes*=k; mushrooms*=k; rocks*=k; deadwood*=k;
            }
            if (site.forestBiome || site.decorBiome) {
                // The terrain category's biomes (engine/biomes): which trees,
                // shrubs and props, and how many. Only here - a candidate
                // with no biome takes the engine's own branch below, the same
                // numbers in the same order.
                const auto* f=site.forestBiome;
                double woods=trees,shrubs=bushes;
                if (f) {
                    // The engine's own wood is a fertility of 0.8 at forest_bias 0.5.
                    double k=f->fertility/0.8;
                    if (!f->density.points.empty())
                        k*=f->density.at(site.forestBias)/std::max(1e-3,f->density.at(0.5));
                    if (f->clearings && f->clearingShare>0) {
                        const double open=noise(seed^0x6c1eu,x,y,std::max(8.0,f->clearings->metres));
                        k*=smooth(f->clearingShare-0.06,f->clearingShare+0.06,open);
                    }
                    woods=std::clamp(trees*k,0.0,1.0);
                    shrubs=f->shrubs.empty()?0.0:bushes*f->shrubDensity/0.3*(woods>0||trees<=0?1.0:0.4);
                    // The mantle: a wood's edge is where the shrubs are, where
                    // light reaches the ground beside the trees.
                    shrubs*=1.0+2.2*smooth(0.06,0.28,woods)*(1-smooth(0.45,0.8,woods));
                }
                // Props a hectare: the forest's follow its trees, the decor's lie anywhere.
                double forestProps=0,decorProps=0;
                if (f) for (const auto& [n,c]:f->props) forestProps+=std::max(0.0,c);
                if (site.decorBiome) for (const auto& [n,c]:site.decorBiome->props) decorProps+=std::max(0.0,c);
                forestProps*=std::min(1.0,woods*1.6)/kCellsPerHectare*(1-smooth(0.10,0.30,site.slope))*bare;
                decorProps*=1.0/kCellsPerHectare*(1-smooth(0.25,0.6,site.slope));
                // Stones lie in groups, where a noise of its own says, and not
                // sprinkled over every metre.
                {
                    const double clump=smooth(0.60,0.82,noise(seed^0x57a1u,x,y,70.0));
                    decorProps*=0.08+3.0*clump;
                    forestProps*=0.4+1.4*smooth(0.45,0.7,noise(seed^0x71c3u,x,y,55.0));
                }
                const auto* registry=site.registry;
                // Which species: mostly the grove's, a field ~50 m across,
                // with some strays - a wood of one tree in stands, not a
                // shuffle of every kind a tree apart.
                const double grove=smooth(0.22,0.78,noise(seed^0x9d31u,x,y,48));
                const auto plantModel=[&](const engine::biomes::Weighted& list,std::uint32_t fallback) {
                    const double stray=random();
                    const double pick=stray<0.22?random():std::clamp(grove+(random()-0.5)*0.12,0.0,0.999);
                    const auto* name=pickWeighted(list,pick);
                    const auto i=name&&registry?registry->plantIndex(*name):std::nullopt;
                    if (!i) return fallback;
                    const auto& p=registry->plants()[*i];
                    const auto m=modelNamed(p.model);
                    if (m>=kModels.size()) return fallback;
                    sizeBy=float(std::lerp(p.heightMin,p.heightMax,random()));
                    tintBy=float((p.tint[0]+p.tint[1]+p.tint[2])/3.0);
                    return m;
                };
                const auto propModel=[&](const engine::biomes::Weighted& list) {
                    const auto* name=pickWeighted(list,random());
                    const auto i=name&&registry?registry->propIndex(*name):std::nullopt;
                    if (!i) return std::uint32_t(kModels.size());
                    const auto& p=registry->props()[*i];
                    sizeBy=float(std::lerp(p.scaleMin,p.scaleMax,random()));
                    return modelNamed(p.model);
                };
                if (choice<woods)
                    model=f&&!f->trees.empty()?plantModel(f->trees,random()<std::clamp(site.boreal+0.18,0.0,1.0)?1u:0u)
                                             :(random()<std::clamp(site.boreal+0.18,0.0,1.0)?1u:0u);
                else if (choice<woods+shrubs) model=f&&!f->shrubs.empty()?plantModel(f->shrubs,2):2;
                else if (choice<woods+shrubs+forestProps) model=propModel(f->props);
                else if (choice<woods+shrubs+forestProps+decorProps) model=propModel(site.decorBiome->props);
                else if (choice<woods+shrubs+forestProps+decorProps+rocks) model=3;
                else if (!f && choice<woods+shrubs+decorProps+rocks+mushrooms+deadwood) {
                    const double kind=(choice-woods-shrubs-decorProps-rocks)/(mushrooms+deadwood);
                    model=kind<mushrooms/(mushrooms+deadwood)?4:(kind<0.6?5:(kind<0.8?6:7));
                }
                else if (f && woods>0.25) {
                    // The floor of a closed wood: fallen logs, stumps and
                    // branches, and fungi on the damp ones - what a forest
                    // biome's own props list leaves out.
                    const double base=woods+shrubs+forestProps+decorProps+rocks;
                    const double floor=(mushrooms+deadwood)*smooth(0.25,0.6,woods);
                    if (!(floor>0) || choice>=base+floor) continue;
                    const double kind=(choice-base)/floor;
                    model=kind<mushrooms/(mushrooms+deadwood)?4:(kind<0.6?5:(kind<0.8?6:7));
                }
                else continue;
                if (model>=kModels.size()) continue;
            }
            else if (choice<trees)
                model=random()<std::clamp(site.boreal+0.18,0.0,1.0)?1:0;
            else if (choice<trees+bushes) model=2;
            else if (choice<trees+bushes+mushrooms) model=4;
            else if (choice<trees+bushes+mushrooms+rocks) model=3;
            else if (choice<trees+bushes+mushrooms+rocks+deadwood) {
                const double kind=(choice-trees-bushes-mushrooms-rocks)/deadwood;
                model=kind<0.4?5:(kind<0.65?6:7);
            }
            else continue;
            // Nothing wooded grows out of a lake or a river: its footprint,
            // and a margin for the drawn shore, has to be dry ground. Asked
            // only now, of the few candidates that got this far - four carve
            // queries each rather than four for every sample. The random
            // stream is already spent above, so dropping one here moves no
            // other object.
            // Boulders may stand in shallow water; nothing else does.
            const bool boulder=model==3 || (model>=kFirstRock2 && model<=kLastRock2);
            if (wet && !boulder) {
                const double r=kShoreClearance*(treeModel(model)?1.0:0.6);
                if (wet(x+r,y) || wet(x-r,y) || wet(x,y+r) || wet(x,y-r) ||
                    wet(x+r*0.7,y+r*0.7) || wet(x-r*0.7,y-r*0.7)) continue;
            }
            ++result.populations[model];
            const float scale=float(0.75+random()*0.65)*sizeBy;
            const float yaw=float(random()*2*std::acos(-1.0));
            const float phase=float(random()*2*std::acos(-1.0));
            const float tint=float(0.88+random()*0.20)*tintBy;
            const float moss=rockModel(model) && site.hasEcology?environmentMossHabitat(site.ecology.moisture,
                site.ecology.canopy,site.ecology.disturbance,float(site.snow)):0;
            // Stones of a river bed carry moss where they stand wet: a patch of
            // it fixed to the place (not to the random stream), most on the
            // quieter reaches.
            float mossed=moss;
            if (boulder && site.decorBiome && site.decorBiome->name=="river_stones") {
                const double patch=noise(seed^0x4d05u,x,y,9.0);
                mossed=std::max(mossed,float(0.35+0.65*smooth(0.25,0.75,patch)));
            }
            result.objects.push_back({id,x,y,site.height-plantSink(model),scale,yaw,phase,tint,model,mossed});
        }
    return result;
}
Scatter scatter(std::uint64_t seed,int rx,int ry,double width,double height,
                const LandTest& land,const SiteSample& sample,int radiusMetres) {
    if (radiusMetres<=0 || radiusMetres%kCell || radiusMetres>kRadius)
        throw std::invalid_argument("invalid scene scatter domain");
    const std::int64_t cx=std::int64_t(rx)*kRegion,cy=std::int64_t(ry)*kRegion;
    return scatter(seed,{cx-radiusMetres,cy-radiusMetres,cx+radiusMetres,cy+radiusMetres},
                   width,height,land,sample);
}
} // namespace world::decor
