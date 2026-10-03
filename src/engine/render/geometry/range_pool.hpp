#pragma once
// Byte ranges handed out of a set of large pages, and handed back.
//
// What a GPU buffer pool needs from the CPU side, and nothing about the GPU:
// the caller owns whatever a page is (one big vertex or index buffer here) and
// creates it when `allocate` says a page was added. Ranges are first-fit within
// a page and coalesce with their free neighbours when released, so a pool that
// churns through meshes of every size settles instead of fragmenting.
//
// Why it exists: the terrain made two GPU buffers for every mesh it ever drew
// - and for every per-plan copy of one - and released them again when the
// mesh left the cache. A buffer create and release is a driver call and a
// kernel allocation each; a range out of a page is two map operations.
//
// Not thread safe; one owner (the render thread).
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <vector>

namespace engine {

class RangePool {
public:
    struct Range {
        std::uint32_t page = 0;
        std::uint64_t offset = 0, bytes = 0;
        explicit operator bool() const { return bytes != 0; }
        friend bool operator==(const Range&, const Range&) = default;
    };

    // `pageBytes`: the size of an ordinary page; a request larger than that
    // gets a page of its own, exactly its size. `alignment` applies to every
    // offset and length (a power of two).
    explicit RangePool(std::uint64_t pageBytes = 16u << 20, std::uint64_t alignment = 256)
        : pageBytes_(pageBytes), alignment_(alignment) {
        if (!pageBytes_ || !alignment_ || (alignment_ & (alignment_ - 1)) || pageBytes_ % alignment_)
            throw std::invalid_argument("RangePool: page size must be a positive multiple of a power-of-two alignment");
    }

    // A range of at least `bytes`. `newPage` is set to the page index when a
    // page had to be added (its size: `pageSize(index)`), so the caller can
    // create the buffer behind it before writing into the range.
    [[nodiscard]] Range allocate(std::uint64_t bytes, std::optional<std::uint32_t>& newPage) {
        newPage.reset();
        if (bytes == 0) return {};
        const std::uint64_t want = roundUp(bytes);
        for (std::uint32_t p = 0; p < pages_.size(); ++p) {
            auto& page = pages_[p];
            if (!page.live) continue;
            for (auto it = page.free.begin(); it != page.free.end(); ++it) {
                if (it->second < want) continue;
                const Range range{p, it->first, want};
                const auto rest = it->second - want;
                const auto at = it->first + want;
                page.free.erase(it);
                if (rest) page.free.emplace(at, rest);
                used_ += want;
                return range;
            }
        }
        // No page has room: reuse a released slot or add one.
        const std::uint64_t size = std::max(pageBytes_, want);
        std::uint32_t slot = 0;
        while (slot < pages_.size() && pages_[slot].live) ++slot;
        if (slot == pages_.size()) pages_.emplace_back();
        auto& page = pages_[slot];
        page = Page{size, {}, true};
        if (size > want) page.free.emplace(want, size - want);
        capacity_ += size;
        used_ += want;
        newPage = slot;
        return {slot, 0, want};
    }

    void release(const Range& range) {
        if (!range) return;
        if (range.page >= pages_.size() || !pages_[range.page].live)
            throw std::logic_error("RangePool: release of a range from no live page");
        auto& free = pages_[range.page].free;
        std::uint64_t offset = range.offset, length = range.bytes;
        auto next = free.lower_bound(offset);
        if (next != free.end() && next->first < offset + length)
            throw std::logic_error("RangePool: double release or overlapping range");
        if (next != free.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second > offset)
                throw std::logic_error("RangePool: double release or overlapping range");
            if (prev->first + prev->second == offset) {
                offset = prev->first;
                length += prev->second;
                free.erase(prev);
            }
        }
        next = free.lower_bound(offset + length);
        if (next != free.end() && next->first == offset + length) {
            length += next->second;
            free.erase(next);
        }
        free.emplace(offset, length);
        used_ -= range.bytes;
    }

    // Pages with nothing allocated in them, past the first `keep` such pages:
    // released here, returned so the caller drops their buffers. Their slots
    // are reused by later pages.
    std::vector<std::uint32_t> trim(std::size_t keep = 1) {
        std::vector<std::uint32_t> dropped;
        std::size_t empty = 0;
        for (std::uint32_t p = 0; p < pages_.size(); ++p) {
            auto& page = pages_[p];
            if (!page.live || page.free.size() != 1 || page.free.begin()->first != 0 ||
                page.free.begin()->second != page.bytes) continue;
            if (empty++ < keep) continue;
            capacity_ -= page.bytes;
            page = Page{};
            dropped.push_back(p);
        }
        return dropped;
    }

    void clear() { pages_.clear(); used_ = capacity_ = 0; }

    [[nodiscard]] std::uint64_t pageSize(std::uint32_t page) const {
        return page < pages_.size() && pages_[page].live ? pages_[page].bytes : 0;
    }
    [[nodiscard]] std::size_t livePages() const {
        return std::size_t(std::count_if(pages_.begin(), pages_.end(), [](const Page& p) { return p.live; }));
    }
    [[nodiscard]] std::uint64_t used() const { return used_; }
    [[nodiscard]] std::uint64_t capacity() const { return capacity_; }
    [[nodiscard]] std::uint64_t alignment() const { return alignment_; }
    // Free ranges across all pages: how fragmented the pool is.
    [[nodiscard]] std::size_t freeRanges() const {
        std::size_t n = 0;
        for (const auto& p : pages_) if (p.live) n += p.free.size();
        return n;
    }

private:
    struct Page {
        std::uint64_t bytes = 0;
        std::map<std::uint64_t, std::uint64_t> free;   // offset -> length, non-adjacent
        bool live = false;
    };
    std::uint64_t roundUp(std::uint64_t bytes) const { return (bytes + alignment_ - 1) & ~(alignment_ - 1); }

    std::uint64_t pageBytes_, alignment_;
    std::vector<Page> pages_;
    std::uint64_t used_ = 0, capacity_ = 0;
};

} // namespace engine

