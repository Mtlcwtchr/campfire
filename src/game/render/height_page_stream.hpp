#pragma once
#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <tuple>
#include <unordered_set>

#include "game/world/terrain_streaming/page_store.hpp"
#include "game/world/terrain_adaptive.hpp"

namespace game {

// Page fields, not vertices. Packed once on a worker, shared by every grid
// reading this page, including geometry-only 32/128/256 m levels.
struct PackedHeightPage {
    std::shared_ptr<const world::streaming::BakedPage> source;
    // RGBA16_UNORM: residuals/water, weights0, weights1/coverage, flow/kind.
    std::array<std::vector<std::uint16_t>, 4> fields;
    // False proves the entire stored page (including filtering padding) cannot
    // survive WaterPS's coverage clip. Unknown/manually built pages stay wet.
    bool mayHaveWater = true;
    std::shared_ptr<const world::terrain::SurfacePage> surface;
};
PackedHeightPage packHeightPage(std::shared_ptr<const world::streaming::BakedPage> page);

class HeightPageStream {
public:
    using Key = world::streaming::TileKey;
    explicit HeightPageStream(world::streaming::PageStore& pages);
    ~HeightPageStream();
    // The caller and upload staging use the same order. wants() replaces the
    // pending queue each frame, so a moved focus never waits for an old FIFO.
    static bool nearer(Key a, Key b, double focusX, double focusY) {
        const auto rank = [&](Key key) {
            const double x = key.x * 512.0, y = key.y * 512.0;
            const double dx = focusX - std::clamp(focusX, x, x + 512.0);
            const double dy = focusY - std::clamp(focusY, y, y + 512.0);
            return std::tuple{dx * dx + dy * dy, -int(key.level), key.y, key.x};
        };
        return rank(a) < rank(b);
    }
    void wants(const std::vector<Key>& draw, const std::vector<Key>& preload);
    std::vector<PackedHeightPage> collect();
    void frozen(bool value);
    struct Stats {
        std::size_t workers = 0, busy = 0, queued = 0, ready = 0, failed = 0;
        double lastBuildMs = 0, longestBuildMs = 0;
        double longestFetchMs = 0, longestPackMs = 0;
        Key longestPage{};
    };
    Stats stats() const;
    static constexpr std::size_t kCompletionBytes = 8u << 20;
    static constexpr std::size_t uploadBytes(Key key) {
        const auto side = world::streaming::storedSamples(4 << key.level);
        return std::size_t(side) * side * 34;
    }
private:
    bool work(bool preload);
    world::streaming::PageStore& pages_;
    world::streaming::TerrainWorkerPool& pool_;
    world::streaming::TerrainWorkerPool::Handle source_, preloadSource_;
    mutable std::mutex mutex_;
    std::deque<Key> queue_, preloadQueue_;
    std::unordered_set<Key> wanted_, inFlight_;
    std::vector<PackedHeightPage> done_;
    std::size_t doneBytes_ = 0, inFlightBytes_ = 0;
    std::size_t failed_ = 0;
    double lastBuildMs_ = 0, longestBuildMs_ = 0;
    double longestFetchMs_ = 0, longestPackMs_ = 0;
    Key longestPage_{};
    bool closing_ = false, frozen_ = false;
};

} // namespace game

