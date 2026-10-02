#include "game/world/scene_placement.hpp"

#include "engine/biomes/registry.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace world {
ScenePlacement::ScenePlacement(WorldBuilder::Snapshot world, Limits limits)
    : world_(std::move(world)), limits_(limits) {
    if (!world_ || !limits_.regions || !limits_.startsPerUpdate || !limits_.regionsPerJob ||
        !limits_.collectsPerUpdate || !limits_.publishesPerUpdate)
        throw std::invalid_argument("invalid scene placement limits or world");
}
ScenePlacement::~ScenePlacement() {
    published_.cancel();
    for (auto& job : jobs_) job.cancel->store(true);
    // Workers own a world lease, never a renderer pointer.
    for (auto& job : jobs_) if (job.work.valid()) job.work.wait();
}

unsigned ScenePlacement::workerLimit() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp(hardware / 2, 1u, 4u);
}

bool ScenePlacement::groundCurrent(const Region& r, const decor::Scatter& scatter) const {
    const auto* edits = world_->edits();
    if (!edits) return true;
    // The region and the ecology cell around its edge: a site reads the
    // slope over the cell it stands in.
    constexpr std::int64_t reach = ecology::kCellMetres;
    const core::WorldRect area{{core::Fixed::fromInt(r.minX - reach), core::Fixed::fromInt(r.minY - reach)},
                               {core::Fixed::fromInt(r.maxX + reach), core::Fixed::fromInt(r.maxY + reach)}};
    return edits->revisionIn(area) <= scatter.ground;
}

void ScenePlacement::collectFinishedJobs() {
    const auto delta = world_->ecology().read();
    for (auto job = jobs_.begin(); job != jobs_.end();) {
        if (job->work.valid() &&
            job->work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++job;
            continue;
        }
        try {
            if (job->work.valid()) job->ready=job->work.get();
            while (job->next<job->ready.size() && stats_.collected<limits_.collectsPerUpdate) {
                auto& [region, scatter]=job->ready[job->next++];
                ++stats_.collected;
                if (scatter->revision != delta->region(double(region.minX), double(region.minY))) continue;
                // Dug under while it was being placed: an older entry stays
                // stale and is placed again.
                if (!groundCurrent(region, *scatter)) continue;
                cache_[region] = {std::move(scatter), clock_};
                if (resident_.contains(region)) dirty_ = true;
            }
        } catch (const std::exception& e) {
            // Only a region still demanded may report; a pan past a failing
            // region must not leave a permanent error on the channel.
            if (enabled_ && std::any_of(job->regions.begin(), job->regions.end(),
                                        [&](const auto& r) { return wanted_.contains(r); })) {
                error_ = e.what();
                std::cerr << "Scene scatter: " << error_ << '\n';
            }
        }
        if (job->next<job->ready.size()) { ++job;continue; }
        job = jobs_.erase(job);
    }
}

void ScenePlacement::startJobs() {
    if (!enabled_ || !error_.empty() || (!missing_ && !stale_)) return;
    const auto workers = std::size_t(workerLimit());
    std::unordered_set<Region,RegionHash> assigned;
    for (const auto& job : jobs_) assigned.insert(job.regions.begin(), job.regions.end());
    while (jobs_.size() < workers && stats_.started<limits_.startsPerUpdate) {
        const auto batch=std::min(limits_.regionsPerJob,limits_.startsPerUpdate-stats_.started);
        std::vector<Region> regions;
        for (const auto& region : order_) { // nearest-first priority order
            const auto cached = cache_.find(region);
            if ((cached != cache_.end() && !cached->second.stale) || assigned.contains(region)) continue;
            regions.push_back(region);
            assigned.insert(region);
            if (regions.size() == batch) break;
        }
        if (regions.empty()) return;
        stats_.started+=regions.size();
        auto cancel=std::make_shared<std::atomic_bool>(false);
        jobs_.push_back({std::async(std::launch::async,
            [world = world_, regions, delta = world_->ecology().read(), cancel] {
                Completed done;
                done.reserve(regions.size());
                auto query = world->field(); // one macro attachment per batch, not per region
                for (const auto& r : regions) {
                    if (cancel->load()) break;
                    done.emplace_back(r, std::make_shared<const decor::Scatter>(world->scatter(r, *delta, query)));
                }
                return done;
            }), std::move(regions),{},0,std::move(cancel)});
    }
}

void ScenePlacement::update(decor::ScatterBounds bounds, bool detailWanted) {
    stats_.requested=0;
    std::vector<Region> regions;
    if (detailWanted) {
        if (!bounds.cells() || bounds.cells()>decor::kMaxScatterCells) {
            updateDemand({},bounds,false,false);
            error_="invalid scene scatter domain";
            return;
        }
        const auto first=[](std::int64_t x) { return (x/decor::kRegion-(x%decor::kRegion<0))*decor::kRegion; };
        for (auto y=first(bounds.minY);y<bounds.maxY;y+=decor::kRegion)
            for (auto x=first(bounds.minX);x<bounds.maxX;x+=decor::kRegion) {
                if (regions.size()==limits_.regions) {
                    updateDemand({},bounds,false,false);
                    error_="scene scatter request exceeds region budget";
                    return;
                }
                regions.push_back({std::max(x,bounds.minX),std::max(y,bounds.minY),
                    std::min(x+decor::kRegion,bounds.maxX),std::min(y+decor::kRegion,bounds.maxY)});
            }
    }
    stats_.requested=regions.size();
    updateDemand(std::move(regions),bounds,detailWanted,false);
}

void ScenePlacement::updateRegions(std::span<const Region> regions, bool enabled) {
    stats_.requested=regions.size();
    const bool limited=regions.size()>limits_.regions;
    regions=regions.first(std::min(regions.size(),limits_.regions));
    Region bounds;
    if (!regions.empty()) bounds=regions.front();
    for (const auto& r:regions) {
        if (r.minX%decor::kRegion || r.minY%decor::kRegion ||
            r.maxX<=r.minX || r.maxY<=r.minY ||
            std::uint64_t(r.maxX)-std::uint64_t(r.minX)!=decor::kRegion ||
            std::uint64_t(r.maxY)-std::uint64_t(r.minY)!=decor::kRegion) {
            updateDemand({}, {}, false, true);
            error_="invalid scene ownership region";
            return;
        }
        bounds.minX=std::min(bounds.minX,r.minX);bounds.minY=std::min(bounds.minY,r.minY);
        bounds.maxX=std::max(bounds.maxX,r.maxX);bounds.maxY=std::max(bounds.maxY,r.maxY);
    }
    updateDemand({regions.begin(),regions.end()},bounds,enabled,true,limited);
}

void ScenePlacement::updateDemand(std::vector<Region> regions, Region bounds, bool enabled, bool incremental,
                                 bool limited) {
    stats_.started=stats_.collected=stats_.published=0;
    stats_.limited=limited;
    std::unordered_set<Region,RegionHash> wanted;
    wanted.reserve(regions.size());
    wanted.insert(regions.begin(),regions.end());
    if (wanted!=wanted_ || bounds!=bounds_ || enabled!=enabled_ || incremental!=incremental_ || limited!=limited_) {
        wanted_=std::move(wanted);bounds_=bounds;enabled_=enabled;incremental_=incremental;
        limited_=limited;
        dirty_=true;error_.clear();
    }
    order_=std::move(regions); // priority can change without invalidating any work
    ++clock_;
    for (auto& job:jobs_)
        if (!enabled_ || std::none_of(job.regions.begin(),job.regions.end(),
                                     [&](const auto& r) { return wanted_.contains(r); }))
            job.cancel->store(true);
    collectFinishedJobs();
    // The terrain categories' registry swapped (a live edit): every forest
    // and every prop is placed again from the new one.
    if (const auto generation = engine::biomes::activeGeneration(); generation != biomesGeneration_) {
        biomesGeneration_ = generation;
        for (auto& [r, entry] : cache_) entry.stale = true;
        dirty_ = true;
    }
    const auto delta = world_->ecology().read();
    if (delta->revision != ecologyRevision_) {
        std::erase_if(cache_, [&](const auto& pair) {
            const auto& [r, entry] = pair;
            const bool stale = entry.scatter->revision != delta->region(double(r.minX), double(r.minY));
            if (stale && wanted_.contains(r)) dirty_ = true;
            return stale;
        });
        ecologyRevision_ = delta->revision;
    }
    if (const auto* edits = world_->edits(); edits && edits->revision() != groundRevision_) {
        // Only what lies near where the ground moved is asked about.
        std::vector<core::WorldRect> changed;
        groundRevision_ = edits->changedSince(groundRevision_, changed);
        const auto reach = core::Fixed::fromInt(ecology::kCellMetres);
        for (auto& [r, entry] : cache_) {
            if (entry.stale) continue;
            const core::WorldRect area{{core::Fixed::fromInt(r.minX) - reach, core::Fixed::fromInt(r.minY) - reach},
                                       {core::Fixed::fromInt(r.maxX) + reach, core::Fixed::fromInt(r.maxY) + reach}};
            if (std::any_of(changed.begin(), changed.end(), [&](const auto& c) { return c.overlaps(area); }) &&
                !groundCurrent(r, *entry.scatter))
                entry.stale = true;
        }
    }
    missing_=0;
    stale_=0;
    if (enabled_) for (const auto& r:wanted_) {
        const auto it=cache_.find(r);
        if (it==cache_.end()) ++missing_;
        else { it->second.used=clock_; stale_+=it->second.stale; }
    }
    // Only the bounded admitted set is pinned. A warm fringe cannot grow with
    // the visible world or with the distance travelled in this session.
    if (cache_.size()>wanted_.size()+limits_.warmRegions) {
        std::vector<Region> unused;
        for (const auto& [r,entry]:cache_) if (!wanted_.contains(r) && !resident_.contains(r)) unused.push_back(r);
        const auto remove=unused.size()>limits_.warmRegions?unused.size()-limits_.warmRegions:0;
        if (remove && remove<unused.size())
            std::nth_element(unused.begin(),unused.begin()+std::ptrdiff_t(remove),unused.end(),
                [&](const auto& a,const auto& b) {
                    return cache_.at(a).used!=cache_.at(b).used?
                        cache_.at(a).used<cache_.at(b).used:a<b;
                });
        for (std::size_t i=0;i<remove;++i) cache_.erase(unused[i]);
    }
    // Regions out of the view linger (bounded) before they leave the
    // publication: a camera turning back and forth must not republish.
    if (std::erase_if(resident_,[&](const auto& r) {
            const auto found=cache_.find(r);
            if (found==cache_.end()) return true;
            if (wanted_.contains(r)) return false;
            // An explicitly empty demand is a clear, not a camera turn.
            return wanted_.empty() || !incremental_ || limits_.lingerUpdates==0 ||
                   clock_-found->second.used>limits_.lingerUpdates;
        }))
        dirty_=true;
    const auto capResident=[&] {
        if (resident_.size()<=limits_.regions) return;
        std::vector<Region> lingering;
        for (const auto& r:resident_) if (!wanted_.contains(r)) lingering.push_back(r);
        std::sort(lingering.begin(),lingering.end(),[&](const auto& a,const auto& b) {
            return cache_.at(a).used!=cache_.at(b).used?cache_.at(a).used<cache_.at(b).used:a<b;
        });
        for (const auto& r:lingering) {
            if (resident_.size()<=limits_.regions) break;
            resident_.erase(r);dirty_=true;
        }
    };
    capResident();
    if (enabled_ && incremental_) {
        // Publication admission is separate from result collection: a pan into
        // an already-warm set must not publish hundreds of regions in one frame.
        for (const auto& r:order_) {
            if (stats_.published==limits_.publishesPerUpdate) break;
            if (cache_.contains(r) && resident_.insert(r).second) { ++stats_.published;dirty_=true; }
        }
        capResident();
        missing_=0;
        for (const auto& r:wanted_) missing_+=!resident_.contains(r);
    } else if (enabled_ && !missing_) {
        // Rectangle compatibility remains atomic and bounded by limits_.regions.
        resident_=wanted_;
    }
    // While regions are still streaming in, batch them: each publication
    // copies and sorts every object and makes the renderer regroup them.
    const bool due=missing_==0 || !published_.read().value || clock_-lastPublish_>=limits_.publishEvery;
    if (enabled_ && dirty_ && due && (!missing_ || (incremental_ &&
        (!resident_.empty() || published_.read().value)))) {
        lastPublish_=clock_;
        auto result=std::make_shared<ScenePlacementSnapshot>();
        const auto ticket=published_.request();
        result->worldVersion=world_->version();result->version=ticket.version();
        result->bounds=bounds_;result->complete=missing_==0;
        result->capacityLimited=limited_;
        std::size_t readyObjects=0;
        for (const auto& r:resident_) readyObjects+=cache_.at(r).scatter->objects.size();
        result->scatter.objects.reserve(readyObjects);
        result->regions.reserve(resident_.size());
        result->regionRevisions.reserve(resident_.size());
        for (const auto& r:resident_) {
            const auto found=cache_.find(r);
            if (found==cache_.end()) continue;
            result->regions.push_back(r);
            const auto& part=*found->second.scatter;
            result->regionRevisions[r] = part.revision;
            auto& out=result->scatter;
            out.objects.insert(out.objects.end(),part.objects.begin(),part.objects.end());
            out.sampled+=part.sampled;out.waterTilesSkipped+=part.waterTilesSkipped;
            for (std::size_t i=0;i<out.populations.size();++i) out.populations[i]+=part.populations[i];
        }
        // Stable identity order regardless of job completion or region priority.
        std::sort(result->regions.begin(),result->regions.end());
        std::sort(result->scatter.objects.begin(),result->scatter.objects.end(),
                  [](const auto& a,const auto& b) { return a.id<b.id; });
        const auto prior=published_.read().value;
        result->objectsVersion=prior && prior->scatter.objects==result->scatter.objects
            ? prior->objectsVersion : ticket.version();
        published_.publish(ticket,std::move(result));
        dirty_=false;
    }
    startJobs();
    stats_.admitted=wanted_.size();stats_.cached=cache_.size();stats_.resident=resident_.size();
    stats_.inFlight=0;
    for (const auto& job:jobs_) stats_.inFlight+=job.regions.size();
}
} // namespace world
