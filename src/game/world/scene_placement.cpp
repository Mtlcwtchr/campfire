#include "game/world/scene_placement.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace world {
ScenePlacement::~ScenePlacement() {
    published_.cancel();
    // Workers own a world lease, never a renderer pointer.
    for (auto& job : jobs_) if (job.work.valid()) job.work.wait();
}

unsigned ScenePlacement::workerLimit() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return std::clamp(hardware / 2, 1u, 4u);
}

void ScenePlacement::collectFinishedJobs() {
    for (auto job = jobs_.begin(); job != jobs_.end();) {
        if (!job->work.valid() ||
            job->work.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++job;
            continue;
        }
        try {
            for (auto& [region, scatter] : job->work.get()) {
                cache_[region] = {std::move(scatter), clock_};
                if (wanted_.contains(region)) dirty_ = true;
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
        job = jobs_.erase(job);
    }
}

void ScenePlacement::startJobs() {
    if (!enabled_ || !error_.empty() || !missing_) return;
    const auto workers = std::size_t(workerLimit());
    std::set<Region> assigned;
    for (const auto& job : jobs_) assigned.insert(job.regions.begin(), job.regions.end());
    // Larger batches amortise the job while a wide view is filling; a small
    // view keeps them short so the first regions publish quickly.
    const auto batch = std::clamp<std::size_t>(missing_ / (workers * 4), 8, 128);
    while (jobs_.size() < workers) {
        std::vector<Region> regions;
        for (const auto& region : order_) { // nearest-first priority order
            if (cache_.contains(region) || assigned.contains(region)) continue;
            regions.push_back(region);
            assigned.insert(region);
            if (regions.size() == batch) break;
        }
        if (regions.empty()) return;
        jobs_.push_back({std::async(std::launch::async,
            [world = world_, regions] {
                Completed done;
                done.reserve(regions.size());
                for (const auto& r : regions)
                    done.emplace_back(r, std::make_shared<const decor::Scatter>(world->scatter(r)));
                return done;
            }), std::move(regions)});
    }
}

void ScenePlacement::update(decor::ScatterBounds bounds, bool detailWanted) {
    std::vector<Region> regions;
    if (detailWanted) {
        if (!bounds.cells() || bounds.cells()>decor::kMaxScatterCells) {
            updateDemand({},bounds,false,false);
            error_="invalid scene scatter domain";
            return;
        }
        const auto first=[](std::int64_t x) { return (x/decor::kRegion-(x%decor::kRegion<0))*decor::kRegion; };
        for (auto y=first(bounds.minY);y<bounds.maxY;y+=decor::kRegion)
            for (auto x=first(bounds.minX);x<bounds.maxX;x+=decor::kRegion)
                regions.push_back({std::max(x,bounds.minX),std::max(y,bounds.minY),
                    std::min(x+decor::kRegion,bounds.maxX),std::min(y+decor::kRegion,bounds.maxY)});
    }
    updateDemand(std::move(regions),bounds,detailWanted,false);
}

void ScenePlacement::updateRegions(std::span<const Region> regions, bool enabled) {
    Region bounds;
    if (!regions.empty()) bounds=regions.front();
    for (const auto& r:regions) {
        if (r.minX%decor::kRegion || r.minY%decor::kRegion ||
            r.maxX-r.minX!=decor::kRegion || r.maxY-r.minY!=decor::kRegion) {
            updateDemand({}, {}, false, true);
            error_="invalid scene ownership region";
            return;
        }
        bounds.minX=std::min(bounds.minX,r.minX);bounds.minY=std::min(bounds.minY,r.minY);
        bounds.maxX=std::max(bounds.maxX,r.maxX);bounds.maxY=std::max(bounds.maxY,r.maxY);
    }
    updateDemand({regions.begin(),regions.end()},bounds,enabled,true);
}

void ScenePlacement::updateDemand(std::vector<Region> regions, Region bounds, bool enabled, bool incremental) {
    const std::set<Region> wanted(regions.begin(),regions.end());
    if (wanted!=wanted_ || bounds!=bounds_ || enabled!=enabled_ || incremental!=incremental_) {
        wanted_=wanted;bounds_=bounds;enabled_=enabled;incremental_=incremental;
        dirty_=true;error_.clear();
    }
    order_=std::move(regions); // priority can change without invalidating any work
    ++clock_;
    collectFinishedJobs();
    missing_=0;
    if (enabled_) for (const auto& r:wanted_) {
        const auto it=cache_.find(r);
        if (it==cache_.end()) ++missing_;
        else it->second.used=clock_;
    }
    // Visible regions are pinned. Keep a small warm fringe, not every region
    // visited in the session. Eviction can never truncate current visibility.
    if (cache_.size()>wanted_.size()+128) {
        std::vector<Region> unused;
        for (const auto& [r,entry]:cache_) if (!wanted_.contains(r)) unused.push_back(r);
        std::sort(unused.begin(),unused.end(),[&](const auto& a,const auto& b) {
            return cache_.at(a).used<cache_.at(b).used;
        });
        const auto remove=unused.size()>128?unused.size()-128:0;
        for (std::size_t i=0;i<remove;++i) cache_.erase(unused[i]);
    }
    if (enabled_ && dirty_ && (!missing_ || (incremental_ && missing_<wanted_.size()))) {
        auto result=std::make_shared<ScenePlacementSnapshot>();
        const auto ticket=published_.request();
        result->worldVersion=world_->version();result->version=ticket.version();
        result->bounds=bounds_;result->complete=missing_==0;
        for (const auto& r:wanted_) {
            const auto found=cache_.find(r);
            if (found==cache_.end()) continue;
            result->regions.push_back(r);
            const auto& part=*found->second.scatter;
            auto& out=result->scatter;
            out.objects.insert(out.objects.end(),part.objects.begin(),part.objects.end());
            out.sampled+=part.sampled;out.waterTilesSkipped+=part.waterTilesSkipped;
            for (std::size_t i=0;i<out.populations.size();++i) out.populations[i]+=part.populations[i];
        }
        // Stable identity order regardless of job completion or region priority.
        std::sort(result->scatter.objects.begin(),result->scatter.objects.end(),
                  [](const auto& a,const auto& b) { return a.id<b.id; });
        published_.publish(ticket,std::move(result));
        dirty_=false;
    }
    // Bound concurrency, not coverage. In-flight results stay useful after a pan.
    startJobs();
}
} // namespace world
