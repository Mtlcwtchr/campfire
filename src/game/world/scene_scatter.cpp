#include "game/world/scene_scatter.hpp"
#include "engine/core/rng.hpp"
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
                const LandTest& land,const SiteSample& sample) {
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
            const auto id=hash;
            const auto random=[&]() { hash=core::splitmix64(hash);return double(hash>>11)*0x1p-53; };
            const double x=(gx+0.15+random()*0.7)*kCell,y=(gy+0.15+random()*0.7)*kCell;
            if (x>=width || y>=height || x<bounds.minX || x>=bounds.maxX ||
                y<bounds.minY || y>=bounds.maxY) continue;
            const Site site=sample(x,y); ++result.sampled;
            if (!std::isfinite(site.height+site.water+site.slope+site.forest+site.boreal) ||
                site.height<=site.water+0.4 || site.height<0 || site.slope>1.8) continue;
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
            const double trees=(site.hasEcology ? site.ecology.canopy*0.94*(1-smooth(0.45,0.95,site.slope))
                                                : habitat*0.94*density)*bare*soil;
            const double scrub=noise(seed^0x529du,x,y,40);
            // Favor the transition belt, not the empty centre of a clearing.
            // The independent scrub field still breaks that belt into patches.
            const double edge=smooth(0.04,0.2,density)*(1-smooth(0.45,0.8,density));
            const double bushes=(site.hasEcology ? site.ecology.shrubs*0.28 : habitat*(0.018+0.23*edge))
                *smooth(0.25,0.75,scrub)*(0.35+0.65*bare);
            const double mushrooms=(site.hasEcology ? site.ecology.canopy*site.ecology.moisture*
                (0.01+site.ecology.deadwood*0.08) : habitat*density*0.028)*(1-smooth(0.25,0.5,site.slope))*bare;
            const double outcrop=noise(seed^0xe157u,x,y,112);
            const double rocks=(0.004+0.05*smooth(0.45,0.8,outcrop))*(1-0.7*trees)*
                (site.hasEcology ? (1-site.ecology.fertility*0.8)*(1+std::min(site.slope,1.0)*4) : 1)*
                (site.hasMaterials ? 1+3*smooth(0.30,0.70,site.rock) : 1);
            // Deadwood fills existing candidates, never adds another world
            // traversal or changes a site's stable ID. Keep logs off cliffs.
            const double deadwood=(site.hasEcology ? site.ecology.deadwood*0.2 : habitat*density*0.055)
                *(1-smooth(0.10,0.30,site.slope))*bare;
            std::uint32_t model;
            if (choice<trees)
                model=random()<std::clamp(site.boreal+0.18,0.0,1.0)?1:0;
            else if (choice<trees+bushes) model=2;
            else if (choice<trees+bushes+mushrooms) model=4;
            else if (choice<trees+bushes+mushrooms+rocks) model=3;
            else if (choice<trees+bushes+mushrooms+rocks+deadwood) {
                const double kind=(choice-trees-bushes-mushrooms-rocks)/deadwood;
                model=kind<0.4?5:(kind<0.65?6:7);
            }
            else continue;
            ++result.populations[model];
            result.objects.push_back({id,x,y,site.height-(model==3?0.25:0.08),
                float(0.75+random()*0.65),float(random()*2*std::acos(-1.0)),
                float(random()*2*std::acos(-1.0)),float(0.88+random()*0.20),model});
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

