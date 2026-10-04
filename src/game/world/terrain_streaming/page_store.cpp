#include "game/world/terrain_streaming/page_store.hpp"

#include "engine/core/rng.hpp"
#include "engine/environment/environment.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/baked_page_cache.hpp"
#include "game/world/terrain_streaming/tile_layout.hpp"

namespace world::streaming {

PageStore::PageStore(const generation::WorldMapData& world, const HydrologyGraph& graph,
                     HsimQuantisation quantisation, Config config)
    : world_(world), graph_(graph), quantisation_(quantisation), config_(config),
      landMask_(terrain::makeLandMask64(world)) {
    if (config_.residentBytes == 0) config_.residentBytes = 1u << 20;
    if (!config_.diskCacheRoot.empty() && world_.terrainStage==generation::TerrainStage::Final)
        diskCache_ = std::make_unique<BakedPageCache>(config_.diskCacheRoot, world_, graph_,
                                                     quantisation_, config_.padding);
}

PageStore::PageStore(const generation::WorldMapData& world, const HydrologyGraph& graph,
                     HsimQuantisation quantisation)
    : PageStore(world, graph, quantisation, Config{}) {}

TerrainWorkerPool& PageStore::workerPool() {
    if (!workerPool_) {
        const auto budget = terrainWorkerBudget(std::thread::hardware_concurrency());
        workerPool_ = std::make_unique<TerrainWorkerPool>(
            config_.workerCount ? std::min(config_.workerCount, budget) : budget);
    }
    return *workerPool_;
}

std::shared_ptr<const BakedPage> PageStore::resident(TileKey key) const {
    // A page something has been dug into since it was baked is not the ground
    // any more, wherever it is kept: the caller is told there is none, and the
    // next page() bakes it again.
    const auto fresh = [&](const std::shared_ptr<const BakedPage>& page) {
        if (current(*page)) return true;
        const std::lock_guard<std::mutex> held(guard_);
        ++stats_.stale;
        return false;
    };
    // The prebaked world first. It is published whole and never changed after,
    // so a reader takes the map by pointer and looks in it without a lock.
    const auto pinned = std::atomic_load(&pinned_);
    if (const auto found = pinned->find(key); found != pinned->end() && fresh(found->second)) return found->second;
    std::shared_ptr<const BakedPage> page;
    {
        const std::lock_guard<std::mutex> held(guard_);
        const auto found = pages_.find(key);
        if (found == pages_.end()) return nullptr;
        order_.splice(order_.end(), order_, found->second.age);
        ++stats_.hits;
        page = found->second.page;
    }
    return fresh(page) ? page : nullptr;
}

bool PageStore::current(const BakedPage& page) const {
    if (page.environment != environmentGeneration_.load(std::memory_order_acquire)) return false;
    const auto* edits = config_.edits.get();
    return !edits || edits->revisionIn(reach(page.base.key)) <= page.groundRevision;
}

void PageStore::environment(std::shared_ptr<const engine::environment::Environment> environment) {
    const std::lock_guard<std::mutex> held(environmentGuard_);
    environment_ = std::move(environment);
    environmentGeneration_.store(environment_ ? environment_->generation() : 0, std::memory_order_release);
}

std::shared_ptr<const engine::environment::Environment> PageStore::environment() const {
    const std::lock_guard<std::mutex> held(environmentGuard_);
    return environment_;
}

std::size_t PageStore::footprintOf(const BakedPage& page) {
    const auto& base = page.base;
    const auto& water = page.water;
    return base.heightQuantized.size() * 2 + base.waterBodyId.size() * 2 +
           base.watershedId.size() * 2 + base.buildability.size() + base.walkability.size() +
           water.surfaceQuantized.size() * 2 + water.waterBodyId.size() * 2 +
           water.riverId.size() * 4 + water.shoreDecimetres.size() * 2 +
           water.coverage.size() + water.flowX.size() + water.flowY.size() +
           page.large.deltaQuantized.size() * 2 + page.medium.deltaQuantized.size() * 2 +
            page.materials.size() + page.featureCells.size() +
           (page.refinementDepth.size() + page.refinementAshore.size()) * sizeof(core::Fixed);
}

std::shared_ptr<const BakedPage> PageStore::keepLocked(TileKey key, std::shared_ptr<const BakedPage> page) {
    const auto found = pages_.find(key);
    if (repinLocked(key, page)) {
        // The pinned set holds it now; an older copy in the working set (the
        // prebake passes through it) must not be found in its place.
        if (found != pages_.end()) {
            bytes_ -= footprintOf(*found->second.page);
            order_.erase(found->second.age);
            pages_.erase(found);
        }
        return page;
    }
    if (found != pages_.end()) {
        order_.splice(order_.end(), order_, found->second.age);
        if (found->second.page->groundRevision >= page->groundRevision) {
            // Somebody else finished first. Their page and this one are the
            // same bytes, so the one already resident stays and nothing is
            // disturbed.
            ++stats_.rebaked;
            return found->second.page;
        }
        // Baked over newer ground than the one it replaces.
        bytes_ -= footprintOf(*found->second.page);
        bytes_ += footprintOf(*page);
        found->second.page = page;
        return page;
    }
    order_.push_back(key);
    auto age = order_.end();
    --age;
    bytes_ += footprintOf(*page);
    pages_.emplace(key, Entry{page, age});
    while (bytes_ > config_.residentBytes && order_.size() > 1) {
        const TileKey oldest = order_.front();
        order_.pop_front();
        const auto going = pages_.find(oldest);
        if (going != pages_.end()) {
            bytes_ -= footprintOf(*going->second.page);
            pages_.erase(going);
        }
        ++stats_.evicted;
    }
    return page;
}

bool PageStore::repinLocked(TileKey key, const std::shared_ptr<const BakedPage>& page) {
    const auto pinned = std::atomic_load(&pinned_);
    const auto found = pinned->find(key);
    if (found == pinned->end()) return false;
    if (found->second->groundRevision >= page->groundRevision) return true;
    // Copy on write, as the prebake publishes: a reader holding the old map
    // keeps reading it, whole.
    auto next = std::make_shared<Pinned>(*pinned);
    (*next)[key] = page;
    std::atomic_store(&pinned_, std::shared_ptr<const Pinned>(std::move(next)));
    return true;
}

std::shared_ptr<const BakedPage> PageStore::page(TileKey key) {
    if (auto already = resident(key)) return already;

    const std::int32_t spacing = sampleMetresAtLevel(key.level);
    if (spacing <= 0) return nullptr;   // no such level

    const bool persistent = diskCache_ && (key.level == 2 || key.level == 4);
    const auto cacheFailure = [&](const CacheResult& result) {
        const std::lock_guard<std::mutex> held(guard_);
        if (++stats_.diskErrors == 1)
            std::fprintf(stderr, "terrain cache: %s: %s; falling back to generated pages\n",
                config_.diskCacheRoot.string().c_str(), result.detail.c_str());
    };
    const auto* edits = config_.edits.get();
    const auto area = reach(key);
    // A stroke that lands inside the page while it bakes makes the bake a
    // picture of ground that is gone. It is baked again, a few times; a
    // stroke that goes on longer than that gets the last one, which is
    // current up to the moment it began and is found stale - and baked once
    // more - the next time anybody asks.
    constexpr int kAttempts = 3;
    for (int attempt = 1;; ++attempt) {
        // The ground as this attempt begins. Read before anything is sampled:
        // every edit at or below this revision is in what the attempt reads.
        const std::uint64_t revision = edits ? edits->revision() : 0;
        // The environment's fingerprint goes into the name too, and only when
        // it does anything: a world without one keeps its cached pages.
        const auto environment = this->environment();
        const std::uint64_t environmentId = environment ? environment->generation() : 0;
        const std::uint64_t ground = [&] {
            const std::uint64_t dug = edits ? edits->fingerprint(area) : 0;
            const std::uint64_t made = environment ? environment->fingerprint() : 0;
            if (!made) return dug;
            const auto mixed = core::splitmix64(dug ^ core::splitmix64(made));
            return mixed ? mixed : std::uint64_t{1};
        }();
        const auto unchanged = [&] { return !edits || edits->revisionIn(area) <= revision; };
        const auto stale = [&] {
            const std::lock_guard<std::mutex> held(guard_);
            ++stats_.stale;
        };

        if (persistent) {
            BakedPage stored;
            const auto result = diskCache_->read(key, stored, ground);
            if (result) {
                stored.groundRevision = revision;
                stored.environment = environmentId;
                auto loaded = std::make_shared<const BakedPage>(std::move(stored));
                if (unchanged() || attempt == kAttempts) {
                    const std::lock_guard<std::mutex> held(guard_);
                    ++stats_.diskLoaded;
                    return keepLocked(key, std::move(loaded));
                }
                stale();
                continue;
            }
            { const std::lock_guard<std::mutex> held(guard_); ++stats_.diskMisses; }
            if (result.status != CacheStatus::NotFound) cacheFailure(result);
        }

        // Runtime detail is a real chain, not an H4 bake behind H8 topology. Do
        // not wait on a sibling task in this pool: missing parents are made here.
        std::shared_ptr<const BakedPage> parent;
        if (key.level < 2) parent = page({key.x, key.y, static_cast<std::uint8_t>(key.level + 1)});
        else if (key.level == 2) parent = resident({key.x, key.y, 4});
        // A parent's samples are reused as they are, so a parent older than
        // the ground this page reaches would carry the old ground into it.
        if (parent && edits && edits->revisionIn(area) > parent->groundRevision) parent.reset();
        if (parent && parent->environment != environmentId) parent.reset();

        // Baked outside the residency lock. A bake is tens of milliseconds and
        // every other page in the store would otherwise wait on it; the cost of
        // two workers baking one page at the same moment is one wasted bake of an
        // identical answer, which is counted rather than prevented.
        auto& baker = bakerForThisThread();
        baker.environment(environment.get());
        BakedPage made = baker.bakePage(key, spacing, config_.padding, {}, parent.get());
        if (!made.base.valid()) return nullptr;
        made.groundRevision = revision;
        made.environment = environmentId;
        made.ground = ground;
        auto baked = std::make_shared<const BakedPage>(std::move(made));
        const bool current = unchanged();
        if (!current && attempt < kAttempts) { stale(); continue; }

        // Never file a page under a name its bytes may not answer to.
        if (persistent && current) {
            const auto saved = diskCache_->write(*baked);
            if (saved) { const std::lock_guard<std::mutex> held(guard_); ++stats_.diskSaved; }
            else cacheFailure(saved);
        }

        const std::lock_guard<std::mutex> held(guard_);
        ++stats_.baked;
        stats_.evaluatedSamples += baked->evaluatedSamples;
        stats_.reusedSamples += baked->reusedSamples;
        return keepLocked(key, std::move(baked));
    }
}

BaseTileBaker& PageStore::bakerForThisThread() const {
    // One baker to a worker: constructing one resolves the macro map's
    // hydrology, and a HeightField keeps a drainage neighbourhood cache
    // inside itself that only stays correct while one thread uses it. The
    // unique_ptr keeps the baker put even when the map rehashes, so the
    // reference stays good outside this lock.
    const std::lock_guard<std::mutex> held(bakerGuard_);
    auto& slot = bakers_[std::this_thread::get_id()];
    if (!slot) {
        slot = std::make_unique<BaseTileBaker>(world_, graph_, quantisation_, config_.edits.get());
        slot->landMask(&landMask_);
    }
    return *slot;
}

std::vector<TileKey> PageStore::keysOverlapping(core::WorldRect area, std::uint8_t level) const {
    std::vector<TileKey> keys;
    if (!area.valid()) return keys;
    const auto first = tileAt(area.min, level);
    const auto last = tileAt(area.max, level);
    const auto wide = static_cast<std::size_t>(last.x - first.x + 1);
    const auto high = static_cast<std::size_t>(last.y - first.y + 1);
    keys.reserve(wide * high);
    for (std::int32_t y = first.y; y <= last.y; ++y)
        for (std::int32_t x = first.x; x <= last.x; ++x) keys.push_back({x, y, level});
    return keys;
}

bool PageStore::containsLand(TileKey key) const {
    const std::int32_t metres = pageMetresAtLevel(key.level);
    // A coarse page's padding reaches two of its own samples out.
    const std::int32_t halo = std::max(terrain::kFoundationMetres, 2 * (kSampleMetres << key.level));
    const auto minX = key.x * metres - halo;
    const auto minY = key.y * metres - halo;
    return landMask_.anyLandInWorldRect(minX, minY, minX + metres + 2 * halo, minY + metres + 2 * halo);
}

std::size_t PageStore::pinnedBytes() const { return pinnedBytes_.load(); }

void PageStore::prebakeInBackground(std::initializer_list<std::uint8_t> requested) {
    if (world_.width <= 0 || world_.height <= 0 || prebakeRunning_.exchange(true)) return;
    auto& pool = workerPool();
    pool.remove(prebake_);
    const std::int64_t metres = generation::kMetresPerCell;
    const std::int32_t wide = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(world_.width) * metres + kPageMetres - 1) / kPageMetres);
    const std::int32_t high = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(world_.height) * metres + kPageMetres - 1) / kPageMetres);

    // Coarsest first, and one level at a time.
    //
    // This used to be one list of every page at every level, baked in any
    // order and published once at the end. Which meant that for the whole of
    // the bake - seconds, on a large world - there was not a single pinned
    // page, so the coarse backdrop and the foundation, both of which are
    // gated on the world being in memory, could not start until the finest
    // level they do not even read was finished. The coarse level is the
    // cheapest: bake it first and reuse it for H16. The pool's exclusive
    // phase remains active between levels and until the final publication.
    std::vector<std::uint8_t> selected(requested);
    if (world_.terrainFoundation) selected = {4}; // H16/H8 are bounded, on-demand products
    std::erase_if(selected, [](std::uint8_t level) { return level > kFinestLevels; });
    std::sort(selected.begin(), selected.end(), std::greater<>());
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    auto levels = std::make_shared<std::vector<std::vector<TileKey>>>();
    std::size_t total = 0;
    for (const std::uint8_t level : selected) {
        std::vector<TileKey> layer;
        layer.reserve(static_cast<std::size_t>(wide) * high);
        for (std::int32_t y = 0; y < high; ++y)
            for (std::int32_t x = 0; x < wide; ++x)
                if (containsLand({x, y, level})) layer.push_back({x, y, level});
        total += layer.size();
        if (!layer.empty()) levels->push_back(std::move(layer));
    }
    prebakeTotal_ = total;
    prebakeDone_ = 0;
    prebakeFailed_ = 0;
    finestPinned_ = kNothingPinned;

    if (levels->empty()) {
        prebakeRunning_ = false;
        return;
    }
    struct Preparation {
        std::mutex mutex;
        Pinned built;
        std::size_t level = 0, next = 0, busy = 0, bytes = 0;
#if ASR_ENABLE_DIAGNOSTICS
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#endif
    };
    auto state = std::make_shared<Preparation>();
    prebake_ = pool.add([this, levels, state](std::size_t) {
        TileKey key;
        {
            std::lock_guard lock(state->mutex);
            if (prebakeStop_ || state->level == levels->size()) return false;
            const auto& layer = (*levels)[state->level];
            if (state->next == layer.size()) return false;
            key = layer[state->next++];
            ++state->busy;
        }
        // One page per turn, with no coordinator waiting on this same pool.
        auto baked = page(key);
        {
            std::lock_guard lock(state->mutex);
            if (baked) {
                state->bytes += footprintOf(*baked);
                state->built.emplace(key, std::move(baked));
            } else {
                ++prebakeFailed_;
            }
            ++prebakeDone_;
            --state->busy;
            if (state->busy == 0 && state->next == (*levels)[state->level].size()) {
                if (!prebakeStop_) {
                    // Publish only a complete, immutable level, coarsest first.
                    pinnedBytes_ = state->bytes;
                    std::atomic_store(&pinned_, std::make_shared<const Pinned>(state->built));
                    finestPinned_ = key.level;
                }
                ++state->level;
                state->next = 0;
                if (state->level == levels->size()) {
                    state->built.clear();
#if ASR_ENABLE_DIAGNOSTICS
                    if (diskCache_) {
                        const auto cache = stats();
                        const double seconds = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - state->started).count();
                        std::fprintf(stderr, "terrain cache: foundation ready in %.3fs (finest pinned H%d); loaded=%zu baked=%zu saved=%zu misses=%zu errors=%zu root=%s\n",
                            seconds, sampleMetresAtLevel(key.level), cache.diskLoaded, cache.baked, cache.diskSaved, cache.diskMisses,
                            cache.diskErrors, config_.diskCacheRoot.string().c_str());
                    }
#endif
                    prebakeRunning_ = false;
                }
            }
        }
        return true;
    }, TerrainWorkerPool::Task::Preparation, [this] { return prebakeRunning_.load(); });
}

PageStore::Progress PageStore::prebakeProgress() const {
    return {prebakeDone_.load(), prebakeTotal_.load(), prebakeRunning_.load(),
            finestPinned_.load(), std::atomic_load(&pinned_)->size(), prebakeFailed_.load()};
}

PageStore::~PageStore() {
    prebakeStop_ = true;
    if (workerPool_) workerPool_->remove(prebake_);
}

PageStore::Stats PageStore::stats() const {
    const std::lock_guard<std::mutex> held(guard_);
    Stats out = stats_;
    out.resident = pages_.size();
    out.residentBytes = bytes_;
    return out;
}

std::size_t PageStore::forget(core::WorldRect area) {
    const std::lock_guard<std::mutex> held(guard_);
    std::size_t dropped = 0;
    for (auto it = pages_.begin(); it != pages_.end();) {
        if (!reach(it->first).overlaps(area)) {
            ++it;
            continue;
        }
        bytes_ -= footprintOf(*it->second.page);
        order_.erase(it->second.age);
        it = pages_.erase(it);
        ++dropped;
    }
    return dropped;
}

void PageStore::clear() {
    const std::lock_guard<std::mutex> held(guard_);
    pages_.clear();
    order_.clear();
    bytes_ = 0;
}

} // namespace world::streaming
