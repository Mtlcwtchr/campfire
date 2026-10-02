#pragma once
#include "game/world/scene_placement.hpp"
#include "game/world/surface_reconstruction.hpp"

namespace world {
// CPU state for one consumer's detail requests. Separate consumers never cancel
// each other's work. GPU resources do not live here.
struct WorldPreparation {
    const WorldBuilder::Snapshot world;
    SurfaceReconstruction surface;
    ScenePlacement placement;
    explicit WorldPreparation(WorldBuilder::Snapshot snapshot)
        : world(std::move(snapshot)), surface(world), placement(world, ScenePlacement::Limits::streaming()) {}
};

class WorldSystem {
public:
    explicit WorldSystem(streaming::PageStore::Config config = WorldBuilder::defaultPageConfig())
        : builder_(std::move(config)) {}
    void build(const generation::WorldMapParams& params) { builder_.build(params); retire(); }
    void publish(generation::WorldMapData map) { builder_.publish(std::move(map)); retire(); }
    WorldBuilder::Snapshot read() const { return builder_.read(); }
    // A world built off the thread that asks for it: `request` here, then
    // generation and `complete` on any thread (publication is safe across
    // threads); the next read() is the new world once it lands. Only the
    // latest request may publish.
    WorldBuilder::Ticket request() { return builder_.request(); }
    bool complete(WorldBuilder::Ticket ticket, generation::WorldMapData map) {
        return builder_.complete(ticket, std::move(map));
    }
    // See WorldBuilder::raise and publish: a world made ready before it is shown.
    std::shared_ptr<const WorldSnapshot> raise(WorldBuilder::Ticket ticket, generation::WorldMapData map) {
        return builder_.raise(ticket, std::move(map));
    }
    bool publish(WorldBuilder::Ticket ticket, std::shared_ptr<const WorldSnapshot> raised) {
        return builder_.publish(ticket, std::move(raised));
    }
    bool current(WorldBuilder::Ticket ticket) const { return builder_.current(ticket); }
    void cancel() { builder_.cancel(); }
    // See WorldBuilder::attach.
    void attach(std::shared_ptr<ecology::Store> edits, std::shared_ptr<const EditLayer> heights = {}) {
        builder_.attach(std::move(edits), std::move(heights));
    }
    // Owner-thread channel registry. Leases may outlive both registry and world
    // publication; destruction drains jobs before releasing the borrowed world.
    std::shared_ptr<WorldPreparation> prepare(WorldBuilder::Snapshot snapshot) {
        if (!snapshot || snapshot != read()) return {};
        retire();
        auto channel = std::make_shared<WorldPreparation>(std::move(snapshot));
        channels_.push_back(channel);
        return channel;
    }
private:
    void retire() {
        const auto current = read();
        std::erase_if(channels_, [&](const auto& channel) {
            return channel->world != current || channel.use_count() == 1;
        });
    }
    WorldBuilder builder_;
    std::vector<std::shared_ptr<WorldPreparation>> channels_;
};
} // namespace world
