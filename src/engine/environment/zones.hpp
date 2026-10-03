#pragma once
// Environment zones (doc/plan_procedural_environment_2026-10-03.md, part B).
//
// A biome is too coarse to place things by. Inside one forest there is dense
// old growth, a clearing, a wet hollow, a stream bank, the foot of a cliff;
// each wants different trees, different ground and different clutter. A zone
// is that local scene type, worked out from the fields, and everything placed
// afterwards asks the zone rather than the biome.
//
// The engine owns the mechanism: soft membership, a lattice of zones over a
// page, smoothing along the causes, a cache. The game owns the zones
// themselves - which types exist and how they follow from the fields - as a
// ZoneClassifier in C++.
//
// Membership is soft. A classifier answers up to four types with weights, so
// a transition is the weights changing, never a threshold: the edge of a wet
// hollow is where wetness falls, and it falls smoothly.
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/environment/fields.hpp"

namespace engine::environment {

using ZoneTypeId = std::uint8_t;
inline constexpr ZoneTypeId kNoZone = 0;      // type 0 is always "unclassified"
inline constexpr std::size_t kMaxZoneTypes = 64;
inline constexpr std::size_t kZoneSlots = 4;

struct ZoneType {
    std::string name;
    std::array<float, 3> debugColour{0.5f, 0.5f, 0.5f};
};

// The local character of a zone, beside its type. What the spec calls
// EnvironmentZone { density, wetness, exposure, age, disturbance, rockiness,
// canopy, soilDepth }.
struct ZoneScalars {
    float density = 0, wetness = 0, exposure = 0, age = 0;
    float disturbance = 0, rockiness = 0, canopy = 0, soilDepth = 0;
};
inline constexpr std::size_t kZoneScalars = 8;
inline float& scalarAt(ZoneScalars& s, std::size_t i) { return (&s.density)[i]; }
inline float scalarAt(const ZoneScalars& s, std::size_t i) { return (&s.density)[i]; }
inline constexpr const char* kZoneScalarNames[kZoneScalars] = {
        "density", "wetness", "exposure", "age", "disturbance", "rockiness", "canopy", "soil_depth"};

struct ZoneWeights {
    std::array<ZoneTypeId, kZoneSlots> type{};
    std::array<float, kZoneSlots> weight{};

    [[nodiscard]] ZoneTypeId dominant() const { return weight[0] > 0 ? type[0] : kNoZone; }
    [[nodiscard]] float of(ZoneTypeId t) const {
        for (std::size_t i = 0; i < kZoneSlots; ++i) if (type[i] == t && weight[i] > 0) return weight[i];
        return 0;
    }
    // Keeps the strongest kZoneSlots of a dense set, sorted, summing to one.
    void fromDense(std::span<const float> dense);
};

struct EnvironmentZone {
    ZoneWeights weights;
    ZoneScalars scalars;
    [[nodiscard]] ZoneTypeId type() const { return weights.dominant(); }
};

// The game's zones. `types()[0]` must be the unclassified type.
class ZoneClassifier {
public:
    virtual ~ZoneClassifier() = default;
    [[nodiscard]] virtual std::span<const ZoneType> types() const = 0;
    // Dense weights over types(), not yet normalised, and the scalars.
    virtual void classify(const FieldSample& fields, std::span<float> weights, ZoneScalars& scalars) const = 0;
};

struct ZoneSettings {
    double step = 16.0;              // metres between zone samples
    double pageMetres = 512.0;       // the page a grid is cached for
    // Smoothing: a kernel that is long along the contour and short across it,
    // so a zone runs along a slope or a stream and changes across it.
    double alongMetres = 40.0;
    double acrossMetres = 16.0;
    // Breakup: the sample point is pushed about by up to this many metres of
    // slow noise. Noise breaks a border up; it never decides one.
    double breakupMetres = 10.0;
    std::uint64_t seed = 0;
};

// One page of zones, with a halo so its border agrees with the neighbour's.
struct ZoneGrid {
    double x0 = 0, y0 = 0, step = 16;   // world position of cell (0, 0)
    int columns = 0, rows = 0;
    std::vector<EnvironmentZone> cells;

    // Bilinear between the four cells around the point.
    [[nodiscard]] EnvironmentZone at(double x, double y) const;
    [[nodiscard]] const EnvironmentZone& cell(int c, int r) const { return cells[std::size_t(r) * columns + c]; }
};

// Zones over the world, a page at a time, cached.
class ZoneField {
public:
    ZoneField(const FieldSource& fields, const ZoneClassifier& classifier, ZoneSettings settings = {});

    // The grid of the page containing (x, y). Pages are `pageMetres` squares
    // on a global lattice.
    [[nodiscard]] std::shared_ptr<const ZoneGrid> page(std::int64_t pageX, std::int64_t pageY) const;
    [[nodiscard]] EnvironmentZone at(double x, double y) const;

    // Builds a grid over any rectangle, not cached: what a tool or a test asks.
    [[nodiscard]] ZoneGrid build(double x0, double y0, int columns, int rows) const;

    [[nodiscard]] const ZoneSettings& settings() const { return settings_; }
    [[nodiscard]] const ZoneClassifier& classifier() const { return classifier_; }
    void clear();

private:
    const FieldSource& fields_;
    const ZoneClassifier& classifier_;
    ZoneSettings settings_;
    mutable std::mutex lock_;
    struct Key {
        std::int64_t x, y;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::hash<std::int64_t>{}(k.x * 73856093 ^ k.y * 19349663);
        }
    };
    mutable std::unordered_map<Key, std::shared_ptr<const ZoneGrid>, KeyHash> cache_;
    mutable std::vector<Key> order_;
    static constexpr std::size_t kKept = 256;
};

} // namespace engine::environment
