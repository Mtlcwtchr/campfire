#pragma once
#include "game/world/terrain_plan.hpp"
#include "game/world/world_builder.hpp"

namespace world {
// One independent detail channel. Inputs are values, never a camera or GPU
// object. Its world lease outlives planning jobs and all borrowed page data.
class SurfaceReconstruction {
public:
    explicit SurfaceReconstruction(WorldBuilder::Snapshot world) : world_(std::move(world)) {}
    void configure(const terrain::TerrainConfig& config);
    bool request(terrain::TerrainView view, std::shared_ptr<const terrain::TerrainResidency> residency,
                 double time, bool restart = false) {
        if (!residency) return false;
        if (!planner_) configure(view.config);
        return planner_->request(std::move(view), std::move(residency), time, restart);
    }
    std::shared_ptr<const terrain::TerrainPlan> collect() {
        if (!planner_) return {};
        auto next = planner_->collect();
        if (next) published_ = next;
        return next;
    }
    std::shared_ptr<const terrain::TerrainPlan> read() const { return published_; }
    bool busy() const { return planner_ && planner_->busy(); }
    terrain::TerrainPlanner::Stats stats() const { return planner_ ? planner_->stats() : terrain::TerrainPlanner::Stats{}; }
private:
    WorldBuilder::Snapshot world_;
    std::unique_ptr<terrain::TerrainPlanner> planner_;
    std::shared_ptr<const terrain::TerrainPlan> published_;
};
} // namespace world
