#include "game/render/forest_proxy_cache.hpp"
#include "game/render/world_materials.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/representation_selector.hpp"
#include "engine/render/impostor_hierarchy.hpp"
#include <optional>
#include <set>

namespace game {
namespace {
world::decor::ScatterBounds parent(engine::render::ImpostorKey key) {
    const auto x=std::int64_t(std::floor(double(key.x)/128))*128,y=std::int64_t(std::floor(double(key.y)/128))*128;
    return {x,y,x+128,y+128};
}
}
engine::render::ImpostorKey ForestProxyCache::key(double x,double y) {
    return {std::int64_t(std::floor(x/kCell))*kCell,std::int64_t(std::floor(y/kCell))*kCell};
}
bool ForestProxyCache::setup(engine::Device& device,engine::RenderPipeline& pipeline,
        std::vector<std::shared_ptr<const engine::render::ImpostorAtlas>> sources,
        SDL_GPUTextureSamplerBinding shadow,const engine::VertexLayout& layout) {
    sources_=std::move(sources);
    if (std::none_of(sources_.begin(),sources_.end(),[](const auto& source){return bool(source);})) return false;
    if (!gpu_.setup(device) || !material_.setup(device,pipeline,materials::sceneModels(true),layout)) return false;
    material_.textures(gpu_.bindings(shadow));ready_=true;return true;
}
void ForestProxyCache::clear() {gpu_.cache.clear();groups_.clear();regionRevisions_.clear();drawn_.clear();fade_.clear();objectsVersion_=~std::uint64_t(0);}
void ForestProxyCache::rebuild(const world::ScenePlacementSnapshot* placement) {
    if (!placement) {clear();return;}
    if (objectsVersion_==placement->objectsVersion) return;
    objectsVersion_=placement->objectsVersion;
    std::unordered_set<engine::render::ImpostorKey,engine::render::ImpostorKeyHash> unchanged;
    unchanged.reserve(groups_.size());
    for (auto it=groups_.begin();it!=groups_.end();) {
        const auto revision=placement->regionRevisions.find(parent(it->first));
        if (revision!=placement->regionRevisions.end() && revision->second==it->second.revision) {unchanged.insert(it->first);++it;}
        else {gpu_.cache.invalidate(it->first);it=groups_.erase(it);}
    }
    // Region by region: a part at the revision last grouped adds nothing new
    // (its cells are all in `unchanged`), so it is not even walked.
    auto previous=std::move(regionRevisions_);
    regionRevisions_.clear();
    for (std::size_t i=0;i<placement->regions.size() && i<placement->parts.size();++i) {
        const auto& part=*placement->parts[i];
        regionRevisions_[placement->regions[i]]=part.revision;
        if (const auto was=previous.find(placement->regions[i]); was!=previous.end() && was->second==part.revision) continue;
        for (const auto& object:part.objects) {
            if (object.model>=sources_.size() || !sources_[object.model]) continue;
            const auto cell=key(object.x,object.y);if (unchanged.contains(cell)) continue;
            const auto revision=placement->regionRevisions.find(parent(cell));if (revision==placement->regionRevisions.end()) continue;
            auto& group=groups_[cell];group.revision=revision->second;
            const auto& source=sources_[object.model];
            group.members.push_back({source,{object.x-cell.x,object.y-cell.y,object.z+source->centre[2]*object.scale},object.scale,object.yaw,object.tint});
        }
    }
    for (auto& [cell,group]:groups_) if (!unchanged.contains(cell)) {
        engine::camera::Vec3 low{1e30,1e30,1e30},high{-1e30,-1e30,-1e30};
        for (const auto& member:group.members) for (int k=0;k<3;++k) {
            const auto r=member.atlas->side*member.scale*.5;low[k]=std::min(low[k],member.centre[k]-r);high[k]=std::max(high[k],member.centre[k]+r);
        }
        for (int k=0;k<3;++k) group.centre[k]=(low[k]+high[k])*.5;
        group.radius=0;
        for (const auto& member:group.members) {
            const auto d=engine::camera::subtract(member.centre,group.centre);
            group.radius=std::max(group.radius,std::sqrt(engine::camera::dot(d,d))+member.atlas->side*member.scale*.5);
        }
    }
}
void ForestProxyCache::update(const engine::Frame& frame,const world::ScenePlacementSnapshot* placement,
        const engine::camera::ViewState& view,bool enabled,const std::function<bool(double,double)>& covered) {
    const auto previous=drawn_;drawn_.clear();if (!ready_) return;
    clock_+=std::clamp(frame.step,0.0,0.25);
    gpu_.cache.frame(clock_,frame.device->completedSubmission());
    // Disabled proxies cost nothing: no regrouping of every placed object on
    // each placement publication. The version stays stale, so turning them
    // back on regroups once.
    if (!enabled || !view.valid()) {gpu_.cache.poll();return;}
    rebuild(placement);
    struct Wanted {engine::render::ImpostorKey key;double score;};
    std::vector<Wanted> wanted;
    for (const auto& [cell,group]:groups_) {
        if (group.members.size()<6 || group.members.size()>64) continue;
        if (covered && covered(double(cell.x)+kCell*.5,double(cell.y)+kCell*.5)) continue;
        auto centre=group.centre;centre[0]+=cell.x;centre[1]+=cell.y;
        const auto p=parent(cell);bool density=false;
        for (const auto& entry:frame.scene.vegetationDensity) if (entry[3]>0 &&
            std::abs(entry[0]-(p.minX+64))<0.01 && std::abs(entry[1]-(p.minY+64))<0.01) density=true;
        if (density) continue;
        const auto* m=engine::cullMatrix(frame.scene);bool visible=true;
        for (int plane=0;plane<4 && visible;++plane) {
            const int row=plane<2?0:4;const double sign=plane%2?-1:1;
            engine::camera::Vec3 n{m[12]+sign*m[row],m[13]+sign*m[row+1],m[14]+sign*m[row+2]};
            visible=engine::camera::dot(n,centre)+m[15]+sign*m[row+3]+group.radius*std::sqrt(engine::camera::dot(n,n))>=0;
        }
        if (!visible) continue;
        const double scale=view.pixelsPerMetre(centre,group.radius);
        // Prefetch only where a 64 px atlas might become useful. No distance bands.
        if (scale<=0 || group.radius*2*scale>64 || group.radius*2*scale<1) continue;
        const auto hemisphere=engine::render::selectHemisphere(view.directionFrom(centre));if (!hemisphere) continue;
        double leafError=0;
        for (const auto& member:group.members) leafError=std::max(leafError,member.atlas->error*member.scale);
        // Before the bake only the texel term is known; the bake measures the
        // rest (view selection, merging) and the selector then decides.
        const double projectedLowerBound=(leafError+group.radius*2*(2.0/64))*scale;
        if (projectedLowerBound>view.quality.impostorErrorPx*1.25) continue;
        wanted.push_back({cell,double(group.members.size())/std::max(0.25,projectedLowerBound)});
    }
    std::sort(wanted.begin(),wanted.end(),[](const auto& a,const auto& b){return a.score!=b.score?a.score>b.score:a.key<b.key;});
    if (wanted.size()>engine::render::ImpostorGpuCache::kSlots) wanted.resize(engine::render::ImpostorGpuCache::kSlots);
    for (const auto& want:wanted) {
        const auto& group=groups_.at(want.key);gpu_.cache.request(want.key,group.revision,group.members);
    }
    gpu_.cache.poll();gpu_.upload(*frame.device);
    std::set<engine::render::ImpostorKey> selected;
    std::map<engine::render::ImpostorKey,Draw> candidates;
    for (const auto& want:wanted) {
        const auto& group=groups_.at(want.key);const auto* slot=gpu_.cache.resident(want.key,group.revision);if (!slot) continue;
        auto centre=slot->atlas->centre;centre[0]+=want.key.x;centre[1]+=want.key.y;
        const auto hemisphere=engine::render::selectHemisphere(view.directionFrom(centre));if (!hemisphere) continue;
        std::array<engine::render::RepresentationCandidate,2> representation;
        representation[0].estimatedCost=group.members.size()*8;
        auto& proxy=representation[1];proxy.kind=engine::render::RepresentationKind::Aggregate;
        const auto side=slot->atlas->side;
        const double scale=view.pixelsPerMetre(centre,side*.5);
        proxy.estimatedCost=16+side*scale*side*scale*.08;
        // Measured at bake time against the members themselves (as the far
        // hierarchy does); the analytic angular bound only for old atlases.
        const auto& atlas=*slot->atlas;
        proxy.errorMetres=atlas.viewError>=0?engine::render::impostorTotalError(atlas):
            atlas.error+side*std::sin(hemisphere->maxAngle*.5);
        proxy.residualDepth=side;
        const bool was=fade_.contains(want.key);
        const auto choice=engine::render::selectRepresentation(view,{centre,side*.5},representation,was?1:0);
        const Draw draw{want.key,std::size_t(slot-gpu_.cache.slots().data()),centre,side};
        candidates[want.key]=draw;
        if (choice.candidate==1) selected.insert(want.key);
    }
    // Dissolve, never switch: the proxy fades in over kFadeSeconds while its
    // members take the complementary pixels (SceneModelsPass reads fade()).
    const double step=std::clamp(frame.step,0.0,0.1)/kFadeSeconds;
    for (const auto& key:selected) fade_.try_emplace(key,0.0);
    for (auto it=fade_.begin();it!=fade_.end();) {
        const auto found=candidates.find(it->first);
        const bool on=selected.contains(it->first);
        it->second=std::clamp(it->second+(on?step:-step),0.0,1.0);
        std::optional<Draw> draw;
        if (found!=candidates.end()) draw=found->second;
        else if (const auto group=groups_.find(it->first);group!=groups_.end()) {
            // Fading out after the cut moved on: still resident, still drawn,
            // and kept warm until it has gone.
            gpu_.cache.request(it->first,group->second.revision,group->second.members);
            if (const auto* slot=gpu_.cache.resident(it->first,group->second.revision)) {
                auto centre=slot->atlas->centre;centre[0]+=it->first.x;centre[1]+=it->first.y;
                draw=Draw{it->first,std::size_t(slot-gpu_.cache.slots().data()),centre,slot->atlas->side};
            }
        }
        if (!draw || (!on && it->second<=0.0)) { it=fade_.erase(it); continue; }
        draw->weight=it->second;
        if (!(covered && covered(double(it->first.x)+kCell*.5,double(it->first.y)+kCell*.5))) drawn_.push_back(*draw);
        ++it;
    }
    drawnTotal_+=drawn_.size();
}
double ForestProxyCache::fade(engine::render::ImpostorKey key) const {
    const auto it=fade_.find(key);return it==fade_.end()?0.0:it->second;
}
} // namespace game
