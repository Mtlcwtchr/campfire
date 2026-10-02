#include "game/world/terrain_detail.hpp"
#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/page_store.hpp"

namespace world::terrain {
namespace {
using Pair=std::array<float,2>;
Pair blend(Pair a,Pair b,double t) {
    return {float(std::lerp(double(a[0]),double(b[0]),t)),float(std::lerp(double(a[1]),double(b[1]),t))};
}
double smooth(double t) { t=std::clamp(t,0.0,1.0); return t*t*(3-2*t); }
}
bool LocalHeightWindow::contains(double wx,double wy) const {
    return wx>=x && wy>=y && wx<=x+metres && wy<=y+metres;
}
Pair LocalHeightWindow::sample(double wx,double wy) const {
    const double gx=std::clamp((wx-x)/step,0.0,double(cells)), gy=std::clamp((wy-y)/step,0.0,double(cells));
    const int ix=std::min(cells-1,int(gx)), iy=std::min(cells-1,int(gy));
    const double fx=gx-ix,fy=gy-iy;
    const auto a=heights[iy*side+ix], b=heights[iy*side+ix+1], c=heights[(iy+1)*side+ix], d=heights[(iy+1)*side+ix+1];
    Pair result{};
    for (int k=0;k<2;++k) result[k]=float(fx+fy<=1 ? a[k]*(1-fx-fy)+b[k]*fx+c[k]*fy : d[k]*(fx+fy-1)+b[k]*(1-fy)+c[k]*(1-fx));
    return result;
}
struct TerrainDetail::Impl {
    const generation::WorldMapData& world;
    const streaming::PageStore& pages;
    int chunkCells;
    std::array<int,kGeometryLevels> chunkMetres;
    std::unique_ptr<HeightField> field;
    std::unique_ptr<streaming::GraphCarver> carver;
    int carverX=0,carverY=0;
    std::unique_ptr<LocalHeightWindow> local;
    double amount=0,target=0;
    std::size_t localEvaluated=0,featureEvaluated=0;
    // Fixed budget independent of the world's size. H16 masks remain in PageStore.
    static constexpr std::size_t maxFeatureSamples=65536;
    using SampleKey=std::pair<int,int>;
    std::list<SampleKey> ages;
    struct CachedSample { Pair value; std::list<SampleKey>::iterator age; };
    std::map<SampleKey,CachedSample> feature;
    struct Mask { std::shared_ptr<const streaming::BakedPage> page; };
    mutable std::map<std::pair<int,int>,Mask> masks;
    std::uint64_t groundRevision=0;
    Impl(const generation::WorldMapData& w,const streaming::PageStore& p,int cells,std::array<int,kGeometryLevels> metres)
        :world(w),pages(p),chunkCells(cells),chunkMetres(metres) {}
    std::int64_t metres(TileId tile) const {
        return chunkMetres[0]?chunkMetres[tile.lod]:tileMetresAt(tile.lod,chunkCells);
    }
    // Everything here is sampled from the ground; after a dig none of it is
    // trusted. Coarse, and this is the CPU comparison path, not the runtime one.
    bool observeGround() {
        const auto* edits=pages.edits();
        if (!edits || edits->revision()==groundRevision) return false;
        groundRevision=edits->revision();
        feature.clear(); ages.clear(); masks.clear(); local.reset();
        return true;
    }
    Pair direct(int x,int y,bool medium) {
        using core::Fixed;
        if (!field) { field=std::make_unique<HeightField>(&world,world.seed); field->edits(pages.edits()); }
        const int px=int(floorDiv(x,512)),py=int(floorDiv(y,512));
        if (!carver || px!=carverX || py!=carverY) {
            carver=std::make_unique<streaming::GraphCarver>(pages.graph(),
                core::WorldRect{{Fixed::fromInt(px*512),Fixed::fromInt(py*512)},
                                {Fixed::fromInt((px+1)*512),Fixed::fromInt((py+1)*512)}},900);
            carverX=px;carverY=py;
        }
        const core::WorldPos p{Fixed::fromInt(x),Fixed::fromInt(y)};
        const auto pieces=field->piecesAt(p.x,p.y);
        const auto h=carver->carve(p,pieces.country,pieces.moved);
        const auto r=field->residualAt(p);
        const double ashore=std::clamp(h.bankDistance.toDouble()/8.0,0.0,1.0);
        return {float(h.floor.toDouble()+ashore*(r.large.toDouble()+(medium?r.medium.toDouble():0))),
                h.wet?float(h.surface.toDouble()):0.0f};
    }
    std::uint8_t flag(int cx,int cy) const {
        const int px=int(floorDiv(cx,32)),py=int(floorDiv(cy,32));
        auto& stored=masks[{px,py}].page;
        if (!stored) stored=pages.resident({px,py,2});
        if (!stored || stored->featureCells.size()!=1024) return 0;
        return stored->featureCells[(cy-py*32)*32+cx-px*32];
    }
    double featureWeight(double x,double y) const {
        const int cx=int(std::floor(x/16)),cy=int(std::floor(y/16));
        double weight=0;
        for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) if (flag(cx+dx,cy+dy)) {
            const double lx=(cx+dx)*16,ly=(cy+dy)*16;
            const double distance=std::max({lx-x,x-lx-16,ly-y,y-ly-16,0.0});
            weight=std::max(weight,1-smooth(distance/16));
        }
        return weight;
    }
};
TerrainDetail::TerrainDetail(const generation::WorldMapData& world,const streaming::PageStore& pages,int chunkCells,
                             std::array<int,kGeometryLevels> chunkMetres)
    :impl_(std::make_unique<Impl>(world,pages,chunkCells,chunkMetres)) {}
TerrainDetail::~TerrainDetail()=default;
bool TerrainDetail::updateLocal(bool enabled,double x,double y,double seconds,double morphSeconds) {
    auto& s=*impl_;
    const double previous=s.amount;
    bool changed=s.observeGround();
    s.target=enabled?1:0;
    const double change=morphSeconds>0?std::max(0.0,seconds)/morphSeconds:1.0;
    s.amount=enabled?std::min(1.0,s.amount+change):std::max(0.0,s.amount-change);
    changed=changed || previous!=s.amount;
    if (enabled) {
        const int ox=int(std::floor(x/16))*16-100,oy=int(std::floor(y/16))*16-100;
        if (!s.local || s.local->x!=ox || s.local->y!=oy) {
            auto next=std::make_unique<LocalHeightWindow>(); next->x=ox;next->y=oy;
            for (int row=0;row<LocalHeightWindow::side;++row) for (int col=0;col<LocalHeightWindow::side;++col) {
                const int wx=ox+col*4,wy=oy+row*4;
                if (s.local && s.local->contains(wx,wy)) next->heights[row*LocalHeightWindow::side+col]=s.local->sample(wx,wy);
                else { next->heights[row*LocalHeightWindow::side+col]=s.direct(wx,wy,true); ++s.localEvaluated; }
            }
            s.local=std::move(next); changed=true;
        }
    }
    if (!enabled && s.amount==0) s.local.reset();
    return changed;
}
bool TerrainDetail::localTouches(TileId tile) const {
    if (!impl_->local || impl_->amount==0) return false;
    const auto& w=*impl_->local; const double side=impl_->metres(tile);
    return tile.x*side<w.x+w.metres && (tile.x+1)*side>w.x && tile.y*side<w.y+w.metres && (tile.y+1)*side>w.y;
}
bool TerrainDetail::hasFeatures(TileId tile) const {
    const int side=int(impl_->metres(tile));
    // Read only permanent H16 metadata, even for a coarse ancestor. Fine
    // sampling is deferred until an actual visible feature block is built.
    for (int y=tile.y*side/16-1;y<=(tile.y+1)*side/16;++y)
        for (int x=tile.x*side/16-1;x<=(tile.x+1)*side/16;++x)
            if (impl_->flag(x,y)) return true;
    return false;
}
double TerrainDetail::tolerance(double x,double y) const {
    const int cx=int(std::floor(x/16)),cy=int(std::floor(y/16));
    std::uint8_t flags=0;
    for (int dy=-1;dy<=1;++dy) for (int dx=-1;dx<=1;++dx) flags|=impl_->flag(cx+dx,cy+dy);
    return flags&6?0.125:flags&1?0.5:1.0;
}
Pair TerrainDetail::sample(double x,double y,Pair base,bool local) {
    auto& s=*impl_;
    const double weight=s.featureWeight(x,y);
    if (weight>0) {
        const auto key=std::pair{int(std::lround(x)),int(std::lround(y))};
        auto it=s.feature.find(key);
        if (it==s.feature.end()) {
            const auto value=s.direct(key.first,key.second,false);
            if (s.feature.size()>=s.maxFeatureSamples) {
                s.feature.erase(s.ages.front());s.ages.pop_front();
            }
            s.ages.push_back(key);
            it=s.feature.emplace(key,Impl::CachedSample{value,std::prev(s.ages.end())}).first; ++s.featureEvaluated;
        } else s.ages.splice(s.ages.end(),s.ages,it->second.age);
        base=blend(base,it->second.value,weight);
    }
    if (local && s.local && s.local->contains(x,y)) {
        const auto& w=*s.local;
        const double border=std::min({x-w.x,y-w.y,w.x+w.metres-x,w.y+w.metres-y});
        // A camera-local residual must never reshape permanent banks/cliffs.
        base=blend(base,w.sample(x,y),(1-weight)*s.amount*smooth(border/16));
    }
    return base;
}
bool TerrainDetail::transitioning() const { return impl_->amount!=impl_->target; }
TerrainDetail::Stats TerrainDetail::stats() const {
    return {impl_->local?impl_->local->heights.size():0,impl_->localEvaluated,impl_->feature.size(),impl_->featureEvaluated,impl_->amount};
}
} // namespace world::terrain

