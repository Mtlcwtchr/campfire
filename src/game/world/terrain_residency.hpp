#pragma once
// Fixed-budget residency for runtime terrain detail pages. Page contents live in
// a GPU atlas; this class owns only stable slot assignments and morph-safe pins.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace world::terrain {

class PageResidency {
public:
    struct Slot {
        std::int64_t key = 0;
        std::uint64_t lastUsed = 0;
        std::uint32_t pins = 0;
        bool occupied = false;
    };

    explicit PageResidency(std::size_t capacity) : slots_(capacity) {}

    [[nodiscard]] std::optional<std::size_t> find(std::int64_t key) const {
        const auto found = byKey_.find(key);
        return found == byKey_.end() ? std::nullopt : std::optional<std::size_t>(found->second);
    }

    // Returns an existing slot, an empty slot, or the oldest unpinned slot.
    // The caller uploads page bytes only when replacedKey has a value or the
    // returned slot was previously empty.
    struct Allocation {
        std::size_t slot = 0;
        std::optional<std::int64_t> replacedKey;
        bool fresh = false;
    };
    [[nodiscard]] std::optional<Allocation> acquire(std::int64_t key, std::uint64_t frame) {
        if (const auto existing = find(key)) {
            slots_[*existing].lastUsed = frame;
            return Allocation{*existing, std::nullopt, false};
        }
        std::size_t chosen = slots_.size();
        std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (!slots_[i].occupied) { chosen = i; break; }
            if (slots_[i].pins == 0 && slots_[i].lastUsed <= oldest) {
                chosen = i;
                oldest = slots_[i].lastUsed;
            }
        }
        if (chosen == slots_.size()) return std::nullopt;
        std::optional<std::int64_t> replaced;
        if (slots_[chosen].occupied) {
            replaced = slots_[chosen].key;
            byKey_.erase(slots_[chosen].key);
        }
        const bool fresh = !slots_[chosen].occupied;
        slots_[chosen] = Slot{key, frame, 0, true};
        byKey_[key] = chosen;
        return Allocation{chosen, replaced, fresh};
    }

    bool pin(std::int64_t key, std::uint64_t frame) {
        const auto at = find(key);
        if (!at) return false;
        ++slots_[*at].pins;
        slots_[*at].lastUsed = frame;
        return true;
    }
    // A pin that does not count as a use. For pages held only so that a plan
    // being built against them finds them there when it is published: the
    // whole resident set is held for that, and refreshing every page's age
    // with it would make the recency order meaningless - the next page let go
    // would be one beside the eye instead of the oldest.
    bool hold(std::int64_t key) {
        const auto at = find(key);
        if (!at) return false;
        ++slots_[*at].pins;
        return true;
    }
    bool unpin(std::int64_t key) {
        const auto at = find(key);
        if (!at || slots_[*at].pins == 0) return false;
        --slots_[*at].pins;
        return true;
    }
    void touch(std::int64_t key, std::uint64_t frame) {
        if (const auto at = find(key)) slots_[*at].lastUsed = frame;
    }

    [[nodiscard]] std::size_t capacity() const { return slots_.size(); }
    [[nodiscard]] std::size_t resident() const { return byKey_.size(); }
    [[nodiscard]] const Slot& slot(std::size_t at) const { return slots_.at(at); }

private:
    std::vector<Slot> slots_;
    std::unordered_map<std::int64_t, std::size_t> byKey_;
};

} // namespace world::terrain

