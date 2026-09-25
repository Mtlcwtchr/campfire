#include "engine/render/geometry/page_residency.hpp"

#include <algorithm>

namespace engine {

std::size_t GeometryPageResidency::indexOf(std::uint32_t id) const {
    const auto it = indices_.find(id);
    return it == indices_.end() ? pages_.size() : it->second;
}

bool GeometryPageResidency::configure(std::span<const GeometryPageDesc> pages,
                                      std::size_t budgetBytes) {
    if (!budgetBytes || pages.empty()) return false;
    std::unordered_map<std::uint32_t, std::size_t> indices;
    indices.reserve(pages.size());
    for (std::size_t i = 0; i < pages.size(); ++i) {
        const auto& page = pages[i];
        if (!page.bytes || !indices.emplace(page.id, i).second) return false;
        if (page.root != (page.parent == GeometryPageDesc::kNoParent)) return false;
    }
    for (const auto& page : pages)
        if (page.parent != GeometryPageDesc::kNoParent && !indices.contains(page.parent)) return false;

    // Validate that every chain terminates at a declared root before replacing
    // the live catalogue. The step limit makes malformed cycles deterministic.
    for (const auto& page : pages) {
        std::uint32_t current = page.id;
        bool terminated = false;
        for (std::size_t step = 0; step <= pages.size(); ++step) {
            const auto it = indices.find(current);
            if (it == indices.end()) return false;
            const auto& node = pages[it->second];
            if (node.parent == GeometryPageDesc::kNoParent) {
                terminated = node.root;
                break;
            }
            current = node.parent;
        }
        if (!terminated) return false;
    }

    pages_.clear();
    pages_.reserve(pages.size());
    for (const auto& page : pages) pages_.push_back({page});
    indices_ = std::move(indices);
    requests_.clear();
    budgetBytes_ = budgetBytes;
    residentBytes_ = 0;
    frame_ = 0;
    return true;
}

bool GeometryPageResidency::pinRoot(std::uint32_t id) {
    const auto index = indexOf(id);
    if (index == pages_.size() || !pages_[index].desc.root) return false;
    pages_[index].pinned = true;
    return commit(id);
}

bool GeometryPageResidency::request(std::uint32_t id) {
    const auto index = indexOf(id);
    if (index == pages_.size()) return false;
    auto& entry = pages_[index];
    entry.lastUsed = frame_;
    if (entry.state == State::Resident) return true;
    if (entry.state == State::Missing) {
        entry.state = State::Requested;
        requests_.push_back(id);
    }
    return true;
}

bool GeometryPageResidency::makeRoom(std::size_t bytes) {
    if (bytes > budgetBytes_) return false;
    while (residentBytes_ > budgetBytes_ - bytes)
        if (!evictOldest()) return false;
    return true;
}

bool GeometryPageResidency::evictOldest() {
    const auto it = std::min_element(pages_.begin(), pages_.end(), [](const Entry& a, const Entry& b) {
        const bool aEligible = !a.pinned && a.state == State::Resident;
        const bool bEligible = !b.pinned && b.state == State::Resident;
        if (aEligible != bEligible) return aEligible;
        if (!aEligible) return false;
        if (a.lastUsed != b.lastUsed) return a.lastUsed < b.lastUsed;
        return a.desc.id < b.desc.id;
    });
    if (it == pages_.end() || it->pinned || it->state != State::Resident) return false;
    residentBytes_ -= it->desc.bytes;
    it->state = State::Missing;
    return true;
}

bool GeometryPageResidency::commit(std::uint32_t id) {
    const auto index = indexOf(id);
    if (index == pages_.size()) return false;
    auto& entry = pages_[index];
    if (entry.state == State::Resident) {
        entry.lastUsed = frame_;
        return true;
    }
    if (!makeRoom(entry.desc.bytes)) return false;
    residentBytes_ += entry.desc.bytes;
    entry.state = State::Resident;
    entry.lastUsed = frame_;
    return true;
}

bool GeometryPageResidency::evict(std::uint32_t id) {
    const auto index = indexOf(id);
    if (index == pages_.size()) return false;
    auto& entry = pages_[index];
    if (entry.pinned || entry.state != State::Resident) return false;
    residentBytes_ -= entry.desc.bytes;
    entry.state = State::Missing;
    entry.lastUsed = frame_;
    return true;
}

void GeometryPageResidency::beginFrame(std::uint64_t frame) {
    frame_ = frame;
}

std::size_t GeometryPageResidency::evictToBudget() {
    std::size_t evicted = 0;
    while (residentBytes_ > budgetBytes_ && evictOldest()) ++evicted;
    return evicted;
}

std::uint32_t GeometryPageResidency::fallback(std::uint32_t id) const {
    auto index = indexOf(id);
    if (index == pages_.size()) return GeometryPageDesc::kNoParent;
    for (std::size_t step = 0; step <= pages_.size(); ++step) {
        const auto& entry = pages_[index];
        if (entry.state == State::Resident) return entry.desc.id;
        if (entry.desc.parent == GeometryPageDesc::kNoParent) break;
        index = indexOf(entry.desc.parent);
        if (index == pages_.size()) break;
    }
    return GeometryPageDesc::kNoParent;
}

bool GeometryPageResidency::resident(std::uint32_t id) const {
    const auto index = indexOf(id);
    return index != pages_.size() && pages_[index].state == State::Resident;
}

bool GeometryPageResidency::requested(std::uint32_t id) const {
    const auto index = indexOf(id);
    return index != pages_.size() && pages_[index].state == State::Requested;
}

} // namespace engine
