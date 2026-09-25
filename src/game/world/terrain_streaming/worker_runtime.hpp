#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "game/world/terrain_streaming/base_tile.hpp"

namespace world::streaming {

// The content identity of a streamed product.  `viewEpoch` intentionally does
// not live here: moving a camera may invalidate a presentation request, but it
// never makes an already generated world tile incorrect.
struct TileProductKey {
    std::uint64_t worldRevision = 0;
    TileKey tile{};
    std::uint8_t products = 0;

    friend constexpr bool operator==(TileProductKey a, TileProductKey b) {
        return a.worldRevision == b.worldRevision && a.tile == b.tile &&
               a.products == b.products;
    }
};

struct TileProductKeyHash {
    std::size_t operator()(const TileProductKey& key) const noexcept {
        const auto tileHash = std::hash<TileKey>{}(key.tile);
        const auto revisionHash = std::hash<std::uint64_t>{}(key.worldRevision);
        return tileHash ^ (revisionHash + 0x9e3779b9u + (tileHash << 6u) + (tileHash >> 2u)) ^
               std::hash<std::uint8_t>{}(key.products);
    }
};

enum class TileProduct : std::uint8_t {
    Base = 1u << 0u,
    LargeResidual = 1u << 1u,
    MediumResidual = 1u << 2u,
    FineResidual = 1u << 3u,
    DerivedFields = 1u << 4u,
};

constexpr std::uint8_t productMask(TileProduct product) {
    return static_cast<std::uint8_t>(product);
}

constexpr bool containsProduct(std::uint8_t products, TileProduct product) {
    return (products & productMask(product)) != 0;
}

// Derived data belongs to a tile result, never to the main thread.  The first
// rollout only needs compact fields; more render fields can be added here
// without changing the request and completion contract.
struct DerivedTile {
    TileKey key{};
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t padding = 0;
    std::vector<std::uint32_t> normalOctahedral;
    std::vector<std::uint8_t> slope;
    std::vector<std::uint8_t> curvature;
    std::vector<std::uint16_t> shorelineDistance;
    std::vector<std::uint32_t> materialWeights;
};

struct TerrainBuildResult {
    TileProductKey key{};
    std::shared_ptr<const BaseTile> base;
    std::shared_ptr<const ResidualTile> largeResidual;
    std::shared_ptr<const ResidualTile> mediumResidual;
    std::shared_ptr<const ResidualTile> fineResidual;
    std::shared_ptr<const DerivedTile> derived;
};

struct TerrainStreamTicket {
    std::uint64_t value = 0;
    friend constexpr bool operator==(TerrainStreamTicket a, TerrainStreamTicket b) {
        return a.value == b.value;
    }
    explicit constexpr operator bool() const { return value != 0; }
};

// `viewEpoch` is only a consumer label.  It lets the caller discard stale
// presentation work while allowing a still useful shared tile job to finish.
struct TerrainStreamRequest {
    TileProductKey key{};
    std::uint64_t viewEpoch = 0;
    std::int32_t priority = 0; // Higher runs first; ties retain submission order.
};

struct TerrainStreamConsumer {
    TerrainStreamTicket ticket{};
    std::uint64_t viewEpoch = 0;
};

struct TerrainStreamCompletion {
    TerrainBuildResult result;
    std::vector<TerrainStreamConsumer> consumers;
};

class TerrainCancellation {
public:
    bool cancelled() const noexcept;

private:
    friend class TerrainWorkerRuntime;
    TerrainCancellation(std::weak_ptr<std::atomic_bool> cancelled,
                        std::weak_ptr<std::atomic_bool> stopping)
        : cancelled_(std::move(cancelled)), stopping_(std::move(stopping)) {}

    std::weak_ptr<std::atomic_bool> cancelled_;
    std::weak_ptr<std::atomic_bool> stopping_;
};

class TerrainWorkerRuntime {
public:
    struct Config {
        std::size_t workerCount = 1;
        std::size_t maxQueued = 64;
        std::size_t maxReady = 32;
    };

    // Every call to BuildFunction happens on a worker.  It must periodically
    // inspect cancellation.cancelled() in long decode/generation passes and
    // return std::nullopt for work which it abandoned.
    using BuildFunction = std::function<std::optional<TerrainBuildResult>(
        const TerrainStreamRequest& request, const TerrainCancellation& cancellation)>;

    TerrainWorkerRuntime(Config config, BuildFunction build);
    ~TerrainWorkerRuntime();

    TerrainWorkerRuntime(const TerrainWorkerRuntime&) = delete;
    TerrainWorkerRuntime& operator=(const TerrainWorkerRuntime&) = delete;

    // Thread-safe, non-blocking entry point for the frame.  It only enqueues
    // metadata.  A zero ticket means that the bounded queue rejected a new job.
    TerrainStreamTicket submit(TerrainStreamRequest request);

    // Cancels one consumer.  A coalesced worker task stops only after every
    // consumer of that content key has been cancelled.
    bool cancel(TerrainStreamTicket ticket);
    void cancelViewEpoch(std::uint64_t viewEpoch);

    // Thread-safe, non-blocking handoff to the frame.  All returned payloads
    // are immutable shared snapshots made by workers.
    std::vector<TerrainStreamCompletion> takeReady(std::size_t limit);
    bool hasReady() const;

    struct Stats {
        std::size_t workers = 0;
        std::size_t queued = 0;
        std::size_t running = 0;
        std::size_t ready = 0;
        std::size_t rejected = 0;
        std::size_t cancelled = 0;
    };
    Stats stats() const;

private:
    struct Job;

    void work();
    void eraseTicketLocked(TerrainStreamTicket ticket, const std::shared_ptr<Job>& job);
    void discardCancelledFrontLocked();

    Config config_;
    BuildFunction build_;
    std::shared_ptr<std::atomic_bool> stopping_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::thread> workers_;
    std::vector<std::shared_ptr<Job>> queue_;
    std::vector<TerrainStreamCompletion> ready_;
    std::unordered_map<TileProductKey, std::shared_ptr<Job>, TileProductKeyHash> jobs_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Job>> tickets_;
    std::uint64_t nextTicket_ = 1;
    std::uint64_t nextSequence_ = 1;
    std::size_t running_ = 0;
    std::size_t rejected_ = 0;
    std::size_t cancelled_ = 0;
    bool closing_ = false;
};

} // namespace world::streaming
