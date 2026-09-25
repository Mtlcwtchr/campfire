#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace engine {

// Logical geometry page metadata. Hierarchy nodes remain valid even while a
// fine page is missing; fallback() supplies the coarser resident page.
struct GeometryPageDesc {
    static constexpr std::uint32_t kNoParent = 0xffffffffu;
    std::uint32_t id = 0;
    std::uint32_t parent = kNoParent;
    std::size_t bytes = 0;
    bool root = false;
};

class GeometryPageResidency {
public:
    // Replaces the catalogue. IDs are unique, parent chains are acyclic, and
    // every terminal page is explicitly marked root.
    bool configure(std::span<const GeometryPageDesc> pages, std::size_t budgetBytes);

    // Roots are the final fallback and cannot be evicted after pinning.
    bool pinRoot(std::uint32_t id);

    // Requests are idempotent and retain first-request order. commit() is
    // called only after the loader has uploaded the page bytes.
    bool request(std::uint32_t id);
    bool commit(std::uint32_t id);

    bool evict(std::uint32_t id);
    void beginFrame(std::uint64_t frame);
    std::size_t evictToBudget();

    [[nodiscard]] std::uint32_t fallback(std::uint32_t id) const;
    [[nodiscard]] bool resident(std::uint32_t id) const;
    [[nodiscard]] bool requested(std::uint32_t id) const;
    [[nodiscard]] std::span<const std::uint32_t> requests() const { return requests_; }
    void clearRequests() { requests_.clear(); }

    [[nodiscard]] std::size_t budgetBytes() const { return budgetBytes_; }
    [[nodiscard]] std::size_t residentBytes() const { return residentBytes_; }
    [[nodiscard]] std::size_t pageCount() const { return pages_.size(); }

private:
    enum class State : std::uint8_t { Missing, Requested, Resident };
    struct Entry {
        GeometryPageDesc desc;
        State state = State::Missing;
        bool pinned = false;
        std::uint64_t lastUsed = 0;
    };

    [[nodiscard]] std::size_t indexOf(std::uint32_t id) const;
    bool evictOldest();
    bool makeRoom(std::size_t bytes);

    std::vector<Entry> pages_;
    std::unordered_map<std::uint32_t, std::size_t> indices_;
    std::vector<std::uint32_t> requests_;
    std::size_t budgetBytes_ = 0;
    std::size_t residentBytes_ = 0;
    std::uint64_t frame_ = 0;
};

} // namespace engine
