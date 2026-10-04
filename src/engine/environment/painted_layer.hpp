#pragma once
// A painted layer of the world (doc/plan_procedural_environment_2026-10-03.md):
// where a person said a kind of place is. Categorical ids on a coarse grid
// (the world source's 256 m), sparse - only what was painted is held - with a
// legend of names. A recipe placed with source "painted" stands only where the
// layer holds one of its names; one with "away_from_painted" keeps off any.
//
// The engine knows nothing of what the names mean; the game's legend and
// recipes do (a megalith site, a ruin field, a sacred grove).
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine::environment {

class PaintedLayer {
public:
    static constexpr std::int64_t kChunk = 64;

    explicit PaintedLayer(double sampleMetres = 256.0) : sampleMetres_(sampleMetres) {}

    void set(std::int64_t sx, std::int64_t sy, std::uint8_t id);
    [[nodiscard]] std::uint8_t sample(std::int64_t sx, std::int64_t sy) const;
    // Nearest sample: ids are never blended.
    [[nodiscard]] std::uint8_t at(double x, double y) const {
        return sample(std::int64_t(std::floor(x / sampleMetres_)), std::int64_t(std::floor(y / sampleMetres_)));
    }
    [[nodiscard]] bool empty() const { return chunks_.empty(); }
    [[nodiscard]] double sampleMetres() const { return sampleMetres_; }

    // The legend: id to name, as the world source's manifest has it.
    std::map<std::uint8_t, std::string> legend;
    [[nodiscard]] const std::string* name(std::uint8_t id) const {
        auto it = legend.find(id);
        return it == legend.end() ? nullptr : &it->second;
    }
    // Every painted sample, for content that wants to count or list sites.
    [[nodiscard]] std::size_t painted() const;
    // A number that changes with what is painted: environments are filed under it.
    [[nodiscard]] std::uint64_t fingerprint() const;

private:
    static std::uint64_t key(std::int64_t cx, std::int64_t cy) {
        return (std::uint64_t(std::uint32_t(cx)) << 32) | std::uint32_t(cy);
    }
    double sampleMetres_;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> chunks_;
};

} // namespace engine::environment
