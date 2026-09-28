#pragma once
#include "engine/render/recursive_impostor.hpp"
#include <future>
#include <functional>

namespace engine::render {
struct ImpostorKey {
    std::int64_t x=0,y=0;
    int level=0; // hierarchy level; 0 = the 32 m runtime cell
    auto operator<=>(const ImpostorKey&) const = default;
};
struct ImpostorKeyHash {
    std::size_t operator()(const ImpostorKey& k) const noexcept {
        std::uint64_t h=std::uint64_t(k.x)*0x9e3779b97f4a7c15ULL^(std::uint64_t(k.y)+0x632be59bd9b4e019ULL)*0xbf58476d1ce4e5b9ULL^
                        std::uint64_t(k.level)*0x94d049bb133111ebULL;
        h^=h>>31;return std::size_t(h);
    }
};
class ImpostorCache {
public:
    enum class State { Empty, Queued, Baking, Ready, Uploading, Resident, Failed };
    struct Slot {
        ImpostorKey key{};
        std::uint64_t revision=0,ticket=0,lastSubmission=0;
        double requested=0;
        State state=State::Empty;
        std::shared_ptr<const ImpostorAtlas> atlas;
        unsigned uploadedViews=0;
    };
    using Baker=std::function<std::shared_ptr<const ImpostorAtlas>(
        std::span<const ImpostorPlacement>,const ImpostorBakeOptions&,const std::atomic_bool*)>;
    ImpostorCache(std::size_t capacity=8,std::size_t byteBudget=16*1024*1024,
                  ImpostorBakeOptions options={},Baker baker=bakeRecursiveImpostor);
    ~ImpostorCache();
    ImpostorCache(const ImpostorCache&)=delete;
    ImpostorCache& operator=(const ImpostorCache&)=delete;
    void frame(double seconds,std::uint64_t completedSubmission);
    // Returns a slot, not ownership of the objects it will eventually replace.
    int request(ImpostorKey key,std::uint64_t revision,std::span<const ImpostorPlacement> members);
    // An atlas that was already baked elsewhere (a persistent store): goes
    // straight to Ready without a worker. Same slot/fence/grace rules as
    // request(); calling it again for the same key+revision only keeps it warm.
    int adopt(ImpostorKey key,std::uint64_t revision,std::shared_ptr<const ImpostorAtlas> atlas);
    void invalidate(ImpostorKey key);
    void clear();
    void poll(); // nonblocking: adopts at most one result, starts at most one job
    int uploadCandidate() const;
    void uploaded(std::size_t slot,unsigned views,bool success,std::uint64_t uploadSerial=0);
    const Slot* resident(ImpostorKey key,std::uint64_t revision) const;
    void protect(std::size_t slot,std::uint64_t submission);
    const std::vector<Slot>& slots() const { return slots_; }
    std::size_t bytes() const { return bytes_; }
    bool busy() const { return job_.valid(); }
    std::uint64_t completed() const { return completed_; }
    std::uint64_t completedBakes() const { return completedBakes_; }
    std::uint64_t staleBakes() const { return staleBakes_; }
private:
    void discard(std::size_t slot);
    // Finds the slot for key: -2 = already current (touched), -1 = none free.
    int claim(ImpostorKey key,std::uint64_t revision);
    struct Result {std::size_t slot;std::uint64_t ticket;std::shared_ptr<const ImpostorAtlas> atlas;};
    std::vector<Slot> slots_;
    std::vector<std::vector<ImpostorPlacement>> pending_;
    std::future<Result> job_;
    std::shared_ptr<std::atomic_bool> cancel_;
    ImpostorBakeOptions options_;
    Baker baker_;
    std::size_t budget_,bytes_=0;
    std::uint64_t completed_=0,clock_=0,completedBakes_=0,staleBakes_=0;
    double now_=0;
};
} // namespace engine::render
