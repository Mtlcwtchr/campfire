#include "game/render/forest_hierarchy.hpp"
#include "game/render/world_materials.hpp"
#include "engine/render/frame.hpp"
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

namespace game {
namespace {
using engine::render::ImpostorKey;
using engine::render::ImpostorAtlas;
using engine::render::ImpostorPlacement;
std::uint64_t mix(std::uint64_t h,std::uint64_t v) {
    h^=v+0x9e3779b97f4a7c15ULL+(h<<6)+(h>>2);
    h=(h^(h>>30))*0xbf58476d1ce4e5b9ULL;return h^(h>>31);
}
double cellOf(ImpostorKey key) { return double(engine::render::hierarchyCell(key.level)); }
// Far bakes are never urgent: the finer path already covers what they would
// replace. On macOS tell the scheduler so, or two bake threads compete with
// the frame thread for the performance cores and show up as frame spikes.
void backgroundWorker() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY,0);
#endif
}
}
ForestHierarchy::ForestHierarchy() : gpu_(kSlots,kResolution,48*1024*1024),store_(96*1024*1024) {}
ForestHierarchy::~ForestHierarchy() {
    for (auto& job:jobs_) job.cancel->store(true);
    for (auto& job:jobs_) if (job.work.valid()) job.work.wait();
}
bool ForestHierarchy::setup(engine::Device& device,engine::RenderPipeline& pipeline,const Leaves& leaves,
        SDL_GPUTextureSamplerBinding shadow,const engine::VertexLayout& layout) {
    leaves_=leaves;
    auto coarse=std::make_shared<Leaves>(leaves.size());
    bool any=false;
    for (std::size_t i=0;i<leaves.size();++i) if (leaves[i]) {
        // Baked once: a 128 m node at 32 px is ~4 m a texel, so a 16 px tree
        // loses nothing it could show and costs a sixty-fourth of the splats.
        (*coarse)[i]=engine::render::resampleImpostor(*leaves[i],kLeafResolution);
        any=any || bool((*coarse)[i]);
    }
    coarse_=std::move(coarse);
    if (!any || !gpu_.setup(device) || !material_.setup(device,pipeline,materials::sceneModels(true),layout)) return false;
    material_.textures(gpu_.bindings(shadow));ready_=true;return true;
}
void ForestHierarchy::clear() {
    ++generation_;
    for (auto& job:jobs_) job.cancel->store(true);
    gpu_.cache.clear();store_.clear();failed_.clear();drawn_.clear();keys_.clear();previous_.clear();
    ground_.clear();edits_.clear();ecologyRevision_=~std::uint64_t(0);
    world_.reset();field_.reset();delta_.reset();pressure_=1;haveCut_=false;lastCut_={};fade_.clear();
}
ForestHierarchy::Stats ForestHierarchy::stats() const {
    Stats s;s.bakes=bakes_;s.failed=failures_;s.uploads=gpu_.uploadedViews();s.drawnTotal=drawnTotal_;
    s.visited=visited_;s.wanted=wanted_;s.jobs=jobs_.size();s.storeBytes=store_.bytes();s.storeEntries=store_.size();
    s.pressure=pressure_;s.limited=limited_;s.updateMs=updateMs_;s.worstUpdateMs=worstMs_;s.uploadMs=uploadMs_;
    s.invisible=last_.invisible;s.below=last_.below;s.coarse=last_.coarse;s.pruned=last_.pruned;
    for (const auto& draw:drawn_) ++s.drawnAt[std::size_t(draw.key.level)];
    s.wantedAt=wantedAt_;s.bakedAt=bakedAt_;
    for (std::size_t i=0;i<4;++i) if (bakedAt_[i]) {s.totalError[i]=totalSum_[i]/bakedAt_[i];s.viewError[i]=viewSum_[i]/bakedAt_[i];s.bakeMs[i]=msSum_[i]/bakedAt_[i];}
    return s;
}
bool ForestHierarchy::covered(double x,double y) const { return coverage(x,y)>=1.0; }
double ForestHierarchy::coverage(double x,double y) const {
    double best=0;
    for (int level=1;level<engine::render::kHierarchyLevels;++level)
        if (const auto it=fade_.find(engine::render::hierarchyKey(level,x,y));it!=fade_.end()) best=std::max(best,it->second);
    return best;
}
bool ForestHierarchy::coveredOld(double x,double y) const {
    if (keys_.empty()) return false;
    for (int level=1;level<engine::render::kHierarchyLevels;++level)
        if (std::binary_search(keys_.begin(),keys_.end(),engine::render::hierarchyKey(level,x,y))) return true;
    return false;
}
void ForestHierarchy::refreshWorld(const world::WorldBuilder::Snapshot& world) {
    if (world!=world_) {
        clear();world_=world;
        if (world_) field_.emplace(world_->field());
    }
    if (!world_) return;
    delta_=world_->ecology().read();
    if (delta_->revision==ecologyRevision_) return;
    // Ecology edits are sparse: hash each one into the three node levels that
    // contain its 128 m region, once per ecology revision, not per node.
    edits_.clear();
    for (const auto& [region,revision]:delta_->regions) for (int level=1;level<engine::render::kHierarchyLevels;++level) {
        const auto key=engine::render::hierarchyKey(level,double(region.first)*128+1,double(region.second)*128+1);
        auto& h=edits_[key];h=mix(mix(mix(h,std::uint64_t(region.first)),std::uint64_t(region.second)),revision);
    }
    ecologyRevision_=delta_->revision;
}
std::uint64_t ForestHierarchy::revision(ImpostorKey key) const {
    const auto edit=edits_.find(key);
    return mix(mix(world_?world_->version()+1:0,std::uint64_t(key.level)),edit==edits_.end()?0:edit->second);
}
double ForestHierarchy::groundAt(ImpostorKey key) {
    if (const auto it=ground_.find(key);it!=ground_.end()) return it->second;
    // Sampling walks the drainage; a first look at a wide view must not pay
    // for thousands in one frame. Unsampled nodes use the world's mid height.
    if (!field_ || groundBudget_<=0) return groundFallback_;
    --groundBudget_;
    const double cell=cellOf(key);
    const auto stride=std::int64_t(std::max(4.0,cell/4));
    const auto sx=std::int64_t(std::floor((double(key.x)+cell*.5)/world::kSampleMetres));
    const auto sy=std::int64_t(std::floor((double(key.y)+cell*.5)/world::kSampleMetres));
    if (ground_.size()>200000) ground_.clear();
    return ground_[key]=std::max(0.0,field_->sampleHeight(sx,sy,stride).toDouble());
}
void ForestHierarchy::collectJobs() {
    for (auto job=jobs_.begin();job!=jobs_.end();) {
        if (job->work.wait_for(std::chrono::seconds(0))!=std::future_status::ready) {++job;continue;}
        Result result;
        try {result=job->work.get();} catch (...) {result={};}
        if (job->generation==generation_ && !job->cancel->load()) {
            if (result.ok && store_.put(job->key,job->revision,result.atlas,clock_)) {
                ++bakes_;
                if (result.atlas) {
                    const auto level=std::size_t(job->key.level);++bakedAt_[level];
                    if (const char* dump=std::getenv("ASR_FAR_DUMP"); dump && bakedAt_[level]==1) {
                        // Colour and alpha of all 21 views side by side, for eyes.
                        const auto& a=*result.atlas;const int n=int(a.resolution);
                        std::vector<engine::render::ImpostorPixel> strip(std::size_t(n)*n*21*2);
                        for (int v=0;v<21;++v) for (int y=0;y<n;++y) for (int x=0;x<n;++x) {
                            const auto& c=a.colour[std::size_t(v)*n*n+std::size_t(y)*n+x];
                            strip[std::size_t(y)*n*21+v*n+x]={c[0],c[1],c[2],255};
                            strip[std::size_t(y+n)*n*21+v*n+x]={c[3],c[3],c[3],255};
                        }
                        if (auto* s=SDL_CreateSurfaceFrom(n*21,n*2,SDL_PIXELFORMAT_RGBA32,strip.data(),n*21*4)) {
                            IMG_SavePNG(s,(std::string(dump)+"-level"+std::to_string(level)+".png").c_str());SDL_DestroySurface(s);
                        }
                        std::fprintf(stderr,"far-dump level=%zu side=%.1f centre=%.1f,%.1f,%.1f members=%u error=%.1f view=%.1f\n",
                            level,a.side,a.centre[0],a.centre[1],a.centre[2],a.members,a.error,a.viewError);
                    }
                    totalSum_[level]+=engine::render::impostorTotalError(*result.atlas);
                    viewSum_[level]+=std::max(0.0,result.atlas->viewError);msSum_[level]+=result.ms;
                }
            }
            else {failed_[job->key]=job->revision;++failures_;}
        }
        job=jobs_.erase(job);
    }
}
bool ForestHierarchy::need(ImpostorKey key,double priority,std::vector<std::pair<ImpostorKey,double>>& bakes,int budget) {
    const auto rev=revision(key);
    if (store_.find(key,rev)) {store_.touch(key,clock_);return true;}
    if (const auto f=failed_.find(key);f!=failed_.end() && f->second==rev) return false;
    if (std::any_of(jobs_.begin(),jobs_.end(),[&](const auto& job){return job.key==key;})) return false;
    if (int(bakes.size())>=budget) return false;
    if (key.level==1) {bakes.push_back({key,priority});return false;}
    bool all=true,broken=false;
    for (const auto child:engine::render::hierarchyChildren(key)) {
        all=need(child,priority*0.999,bakes,budget) && all;
        const auto f=failed_.find(child);
        broken=broken || (f!=failed_.end() && f->second==revision(child));
    }
    // A parent of an unbakeable child can never be complete: remember that
    // instead of asking again every frame.
    if (broken) {failed_[key]=rev;return false;}
    if (all) bakes.push_back({key,priority});
    return false;
}
bool ForestHierarchy::start(ImpostorKey key) {
    const auto rev=revision(key);
    if (key.level>1) {
        std::vector<ImpostorPlacement> children;
        for (const auto child:engine::render::hierarchyChildren(key)) {
            const auto* entry=store_.find(child,revision(child));
            if (!entry) return false;
            store_.touch(child,clock_);
            if (!entry->atlas) continue;
            const auto& atlas=entry->atlas;
            children.push_back({atlas,{atlas->centre[0]+double(child.x-key.x),atlas->centre[1]+double(child.y-key.y),atlas->centre[2]},1,0,1});
        }
        // Nothing below: an answer, not a job.
        if (children.empty()) {store_.put(key,rev,nullptr,clock_);return true;}
        auto cancel=std::make_shared<std::atomic_bool>(false);
        jobs_.push_back({key,rev,generation_,std::async(std::launch::async,[children=std::move(children),cancel] {
            backgroundWorker();
            const auto begun=std::chrono::steady_clock::now();
            engine::render::ImpostorBakeOptions options{kResolution,16,16,1};
            auto atlas=engine::render::withMeasuredError(
                engine::render::bakeRecursiveImpostor(children,options,cancel.get()),children,cancel.get());
            return Result{bool(atlas),std::move(atlas),
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count()};
        }),cancel});
        return true;
    }
    auto cancel=std::make_shared<std::atomic_bool>(false);
    jobs_.push_back({key,rev,generation_,std::async(std::launch::async,
        [world=world_,delta=delta_,leaves=coarse_,key,cancel] {
            backgroundWorker();
            const auto begun=std::chrono::steady_clock::now();
            // The same deterministic scatter the placement would publish for
            // this region, generated here so a far node never depends on the
            // near placement having visited it.
            auto query=world->field();
            const world::decor::ScatterBounds region{key.x,key.y,key.x+128,key.y+128};
            const auto scatter=world->scatter(region,*delta,query);
            if (cancel->load()) return Result{};
            std::vector<ImpostorPlacement> members;
            for (const auto& object:scatter.objects) {
                if (object.model>=leaves->size() || !(*leaves)[object.model]) continue;
                const auto& source=(*leaves)[object.model];
                members.push_back({source,{object.x-double(key.x),object.y-double(key.y),
                    object.z+source->centre[2]*object.scale},object.scale,object.yaw,object.tint});
            }
            if (std::getenv("ASR_FAR_DEBUG")) std::fprintf(stderr,"far-bake %lld,%lld objects=%zu members=%zu\n",
                (long long)key.x,(long long)key.y,scatter.objects.size(),members.size());
            if (members.empty()) return Result{true,nullptr};
            if (members.size()>kMaxRegionMembers) return Result{}; // never silently thinned
            engine::render::ImpostorBakeOptions options{kResolution,16,unsigned(kMaxRegionMembers),3};
            auto atlas=engine::render::withMeasuredError(
                engine::render::bakeRecursiveImpostor(members,options,cancel.get()),members,cancel.get());
            return Result{bool(atlas),std::move(atlas),
                std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count()};
        }),cancel});
    return true;
}
void ForestHierarchy::update(const engine::Frame& frame,const Inputs& in) {
    // Frame-thread cost of the whole far forest, for ASR_SCENE_DEBUG.
    struct Timed {
        double& last;double& worst;std::chrono::steady_clock::time_point begun=std::chrono::steady_clock::now();
        ~Timed() {last=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count();worst=std::max(worst,last);}
    } timed{updateMs_,worstMs_};
    previous_.clear();
    for (const auto& draw:drawn_) previous_.push_back(draw.key);
    drawn_.clear();keys_.clear(); // visited/wanted describe the last walk, not this frame
    if (!ready_) return;
    clock_+=std::clamp(frame.step,0.0,0.25);
    gpu_.cache.frame(clock_,frame.device->completedSubmission());
    refreshWorld(in.world);
    collectJobs();
    if (!in.enabled || !world_ || !in.view.valid() || !(in.drawDistance>0) || in.bounds.maxX<=in.bounds.minX) {
        gpu_.upload(*frame.device,kViewsPerFrame);return;
    }
    const auto& view=in.view;
    groundBudget_=32;groundFallback_=std::max(0.0,in.bounds.low);
    // Distance is measured on the ground from where the fog is measured from:
    // the eye in perspective, the focus under an orthographic camera.
    const double ex=view.orthographic?engine::cullEye(frame.scene)[0]:view.position[0];
    const double ey=view.orthographic?engine::cullEye(frame.scene)[1]:view.position[1];
    const double reach=in.drawDistance;
    const int top=engine::render::kHierarchyLevels-1;
    const double topCell=cellOf({0,0,top});
    std::vector<ImpostorKey> roots;
    const double x0=std::max(in.bounds.minX,ex-reach),x1=std::min(in.bounds.maxX,ex+reach);
    const double y0=std::max(in.bounds.minY,ey-reach),y1=std::min(in.bounds.maxY,ey+reach);
    if (x0<x1 && y0<y1) {
        for (double y=std::floor(y0/topCell)*topCell;y<y1;y+=topCell)
            for (double x=std::floor(x0/topCell)*topCell;x<x1;x+=topCell) roots.push_back(engine::render::hierarchyKey(top,x+1,y+1));
    }
    // Nearest first, so a visit budget cuts the far edge rather than a side.
    std::sort(roots.begin(),roots.end(),[&](const auto& a,const auto& b) {
        const auto d=[&](const ImpostorKey& k){const double c=topCell*.5,dx=double(k.x)+c-ex,dy=double(k.y)+c-ey;return dx*dx+dy*dy;};
        return d(a)!=d(b)?d(a)<d(b):a<b;
    });
    const auto* m=engine::cullMatrix(frame.scene);
    const auto frustum=[&](double minX,double minY,double minZ,double maxX,double maxY,double maxZ) {
        for (int plane=0;plane<4;++plane) {
            const int row=plane<2?0:4;const double sign=plane%2?-1:1;
            const double n[3]{m[12]+sign*m[row],m[13]+sign*m[row+1],m[14]+sign*m[row+2]};
            const double w=m[15]+sign*m[row+3];
            const double px=n[0]>=0?maxX:minX,py=n[1]>=0?maxY:minY,pz=n[2]>=0?maxZ:minZ;
            if (n[0]*px+n[1]*py+n[2]*pz+w<0) return false;
        }
        return true;
    };
    std::vector<ImpostorKey> resident;
    // Key -> slot for what is complete on the GPU, once per frame: the cut
    // asks it for every visited node, and a scan of every slot each time was
    // the largest part of the traversal.
    std::vector<std::pair<ImpostorKey,std::size_t>> slotOf;
    for (std::size_t i=0;i<gpu_.cache.slots().size();++i) {
        const auto& slot=gpu_.cache.slots()[i];
        if (slot.state!=engine::render::ImpostorCache::State::Resident) continue;
        resident.push_back(slot.key);slotOf.push_back({slot.key,i});
    }
    std::sort(slotOf.begin(),slotOf.end());
    const auto residentSlot=[&](ImpostorKey key,std::uint64_t rev)->const engine::render::ImpostorCache::Slot* {
        const auto it=std::lower_bound(slotOf.begin(),slotOf.end(),std::pair{key,std::size_t(0)});
        if (it==slotOf.end() || !(it->first==key)) return nullptr;
        const auto& slot=gpu_.cache.slots()[it->second];
        return slot.revision==rev?&slot:nullptr;
    };
    const auto info=[&](ImpostorKey key) {
        engine::render::HierarchyNodeInfo node;
        const double cell=cellOf(key);
        const double dx=std::max({double(key.x)-ex,0.0,ex-double(key.x)-cell});
        const double dy=std::max({double(key.y)-ey,0.0,ey-double(key.y)-cell});
        if (dx*dx+dy*dy>reach*reach ||
            !frustum(double(key.x),double(key.y),in.bounds.low,double(key.x)+cell,double(key.y)+cell,in.bounds.high+60)) {
            node.visible=false;return node;
        }
        const auto rev=revision(key);
        const auto* entry=store_.find(key,rev);
        const auto* slot=residentSlot(key,rev);
        if (entry && !entry->atlas) {node.empty=true;return node;}
        if (const auto f=failed_.find(key);!entry && !slot && f!=failed_.end() && f->second==rev) {node.mustRefine=true;return node;}
        if (in.density) for (int i=0;i<8;++i) {
            const auto* d=in.density[i];
            if (d[3]>0 && d[0]>=double(key.x) && d[0]<double(key.x)+cell && d[1]>=double(key.y) && d[1]<double(key.y)+cell) {
                node.mustRefine=true;return node; // terrain density owns part of it
            }
        }
        // A resident slot keeps its own atlas even after the store evicted it.
        const ImpostorAtlas* atlas=slot?slot->atlas.get():entry?entry->atlas.get():nullptr;
        if (atlas) {
            node.bounds={{atlas->centre[0]+double(key.x),atlas->centre[1]+double(key.y),atlas->centre[2]},atlas->side*.5};
            node.errorMetres=engine::render::impostorTotalError(*atlas);
            node.viewErrorIncluded=atlas->viewError>=0;
        } else {
            // Before the bake: the ground cell and a canopy above it, and only
            // the texel term. Optimistic on purpose: it decides whether to
            // bake, and the bake measures what drawing it would really cost.
            node.bounds={{double(key.x)+cell*.5,double(key.y)+cell*.5,groundAt(key)+15},cell*0.7071+30};
            node.errorMetres=4*cell/kResolution;node.viewErrorIncluded=true;
        }
        node.resident=slot!=nullptr;
        return node;
    };
    engine::render::HierarchyOptions options;
    // A sixth of the slots is never drawn from: a parent can only replace its
    // children once it is resident, and it cannot become resident while the
    // children it replaces fill every slot.
    options.allowanceScale=pressure_;options.maxDraws=kSlots-kSlots/6;options.maxVisits=20000;
    // The cut is a function of the view and of what is resident. A still or
    // slowly moving camera re-walks it every third frame; anything faster, or
    // a changed draw distance, walks it at once. Draw lists are revalidated
    // against residency every frame either way.
    const auto f0=engine::camera::normalized(view.forward),f1=engine::camera::normalized(lastView_.forward);
    const auto moved=engine::camera::subtract(view.position,lastView_.position);
    const bool fresh=!haveCut_ || view.orthographic!=lastView_.orthographic || reach!=lastReach_ ||
        engine::camera::dot(moved,moved)>25.0*25.0 || engine::camera::dot(f0,f1)<0.99996 ||
        std::abs(view.orthographicScale-lastView_.orthographicScale)>0.01*lastView_.orthographicScale ||
        ++sinceCut_>=3;
    if (fresh) {
        lastCut_=engine::render::selectImpostorHierarchy(view,roots,info,resident,previous_,options);
        lastView_=view;lastReach_=reach;sinceCut_=0;haveCut_=true;
        const auto& cut=lastCut_;
        visited_=cut.visited;wanted_=cut.wanted.size();limited_=cut.budgetLimited;
        last_.invisible=cut.invisible;last_.below=cut.below;last_.coarse=cut.coarse;last_.pruned=cut.pruned;
        wantedAt_={};for (const auto& [key,priority]:cut.wanted) ++wantedAt_[std::size_t(key.level)];
        // Budget pressure: more acceptable nodes than slots means the cut cannot
        // be published at this quality. Coarsen slowly, recover slower still.
        const auto demand=cut.draws.size()+cut.wanted.size();
        if (cut.budgetLimited || demand>kSlots*0.8) pressure_=std::min(4.0,pressure_*1.08);
        else if (demand<kSlots*0.5) pressure_=std::max(1.0,pressure_/1.02);
    }
    const auto& cut=lastCut_;
    // Transitions are dissolves, never switches: every node the cut selects
    // fades in over kFadeSeconds, every node it drops fades out, and while a
    // parent is between 0 and 1 its drawn descendants (and the finer paths
    // below it) take exactly the complementary pixels.
    const double fadeStep=std::clamp(frame.step,0.0,0.1)/kFadeSeconds;
    std::set<ImpostorKey> selected;
    for (const auto& key:cut.draws) if (gpu_.cache.resident(key,revision(key))) selected.insert(key);
    for (const auto& key:selected) fade_.try_emplace(key,0.0);
    for (auto it=fade_.begin();it!=fade_.end();) {
        const bool on=selected.contains(it->first);
        it->second=std::clamp(it->second+(on?fadeStep:-fadeStep),0.0,1.0);
        // A fading node must still be resident; if its slot is gone it is gone.
        const auto* current=gpu_.cache.resident(it->first,revision(it->first));
        if ((!on && it->second<=0.0) || !current) it=fade_.erase(it); else ++it;
    }
    for (const auto& [key,weight]:fade_) {
        const auto rev=revision(key);
        const auto* current=gpu_.cache.resident(key,rev);
        gpu_.cache.adopt(key,rev,current->atlas); // touch: same key+revision is never re-uploaded
        store_.touch(key,clock_);
        auto centre=current->atlas->centre;centre[0]+=double(key.x);centre[1]+=double(key.y);
        drawn_.push_back({key,std::size_t(current-gpu_.cache.slots().data()),centre,current->atlas->side,weight,weight});
        keys_.push_back(key);
    }
    std::sort(keys_.begin(),keys_.end());
    // Nested draws: a node under a partly faded-in ancestor is drawn as its
    // complement; under a fully opaque one it is not drawn at all.
    std::erase_if(drawn_,[&](Draw& draw) {
        double above=0;
        for (auto key=draw.key;key.level+1<engine::render::kHierarchyLevels;) {
            key=engine::render::hierarchyParent(key);
            if (const auto it=fade_.find(key);it!=fade_.end()) above=std::max(above,it->second);
        }
        if (above>=1.0) return true;
        if (above>0.0) draw.coverage=-above;
        return false;
    });
    auto wanted=cut.wanted;
    // On-screen size, then nearness to where the fog is measured from: under
    // an orthographic camera every node of a level is the same size and a key
    // order tie would start baking at the world's west edge.
    for (auto& [key,priority]:wanted) {
        const double c=cellOf(key)*.5,dx=double(key.x)+c-ex,dy=double(key.y)+c-ey;
        priority/=1+std::sqrt(dx*dx+dy*dy)/1000;
    }
    // Coarser first: a resident parent removes sixteen draws, a child adds one.
    // Two workers finish a few bakes a frame: ordering and asking the store
    // about thousands of wanted nodes every frame is time nothing can use.
    const auto first=std::min<std::size_t>(wanted.size(),128);
    std::partial_sort(wanted.begin(),wanted.begin()+std::ptrdiff_t(first),wanted.end(),[](const auto& a,const auto& b){
        return a.first.level!=b.first.level?a.first.level>b.first.level:
               a.second!=b.second?a.second>b.second:a.first<b.first;});
    wanted.resize(first);
    std::vector<std::pair<ImpostorKey,double>> bakes;
    int refused=0;
    for (const auto& [key,priority]:wanted) {
        const auto rev=revision(key);
        if (const auto* entry=store_.find(key,rev)) {
            store_.touch(key,clock_);
            // Resident but not drawn (the cut ran out of draws): not kept warm,
            // so its slot goes back to a node that can replace more.
            if (gpu_.cache.resident(key,rev)) continue;
            if (entry->atlas && refused<4 && gpu_.cache.adopt(key,rev,entry->atlas)<0) ++refused;
        } else need(key,priority,bakes,64);
    }
    std::stable_sort(bakes.begin(),bakes.end(),[](const auto& a,const auto& b){return a.second>b.second;});
    const std::size_t workers=2;
    for (const auto& [key,priority]:bakes) {
        if (jobs_.size()>=workers) break;
        if (std::any_of(jobs_.begin(),jobs_.end(),[&](const auto& job){return job.key==key;})) continue;
        start(key);
    }
    {
        const auto begun=std::chrono::steady_clock::now();
        gpu_.upload(*frame.device,kViewsPerFrame);
        uploadMs_=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begun).count();
    }
    drawnTotal_+=drawn_.size();
}
} // namespace game

