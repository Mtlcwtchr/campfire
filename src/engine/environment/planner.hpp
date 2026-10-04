#pragma once
// Where features go (doc/plan_procedural_environment_2026-10-03.md, part C).
//
// Each recipe is planned over cells of its own scale - 128 m for micro
// features, 512 m for local, 8 km for macro - and each cell's instances are a
// pure function of (seed, recipe, cell). A cell owns the instances whose
// anchor lies inside it, and the whole of each: a gully that runs across
// three pages is one gully, planned once, and every page asks the cell that
// owns it.
//
// Candidates come from the recipe's source - jittered points, the historical
// drainage, crests, the feet of steep ground, the borders between zones - and
// are kept when the zone and the fields agree. Spacing is resolved without a
// chain: every candidate draws a priority, and a candidate gives way to any
// passing candidate of higher priority within min_spacing, whichever cell it
// is in. That needs only the neighbours' candidates, never their decisions,
// so no cell depends on a cell further away than its spacing.
//
// Recipes are planned in order: macro before local before micro, and within a
// scale in catalogue order. A recipe's `clearance` keeps it off the ground an
// earlier recipe has already taken.
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine/environment/catalogue.hpp"
#include "engine/environment/drainage.hpp"
#include "engine/environment/fields.hpp"
#include "engine/environment/painted_layer.hpp"
#include "engine/environment/terrain_ops.hpp"
#include "engine/environment/zones.hpp"

namespace engine::environment {

struct PlannerContext {
    std::shared_ptr<const Catalogue> catalogue;
    // The world BEFORE any feature: fields, zones and height read from ground
    // the features have not moved, or a feature would plan itself on itself.
    const FieldSource* fields = nullptr;
    const ZoneField* zones = nullptr;
    HeightAt height;
    DrainageClimate climate;
    DrainageSettings drainage;
    std::uint64_t seed = 0;
    double windX = -0.857, windY = 0.516;
    // Features a person removed by hand: an instance whose id this answers
    // true for (at its anchor) is not placed, and moves no ground. Null: none.
    std::function<bool(std::uint64_t id, double x, double y)> removed;
    // What a person painted (painted_layer.hpp), or null.
    std::shared_ptr<const PaintedLayer> painted;
};

using InstanceList = std::vector<FeatureInstance>;

class FeaturePlanner {
public:
    explicit FeaturePlanner(PlannerContext context);

    // The instances recipe `recipe` places in planning cell (cx, cy) of its
    // scale, secondaries included.
    [[nodiscard]] std::shared_ptr<const InstanceList> cell(std::uint32_t recipe, std::int64_t cx, std::int64_t cy) const;
    // Every instance of every recipe whose bounds overlap `area`.
    void instancesIn(const core::WorldRect& area, std::vector<FeatureInstance>& out) const;
    // The historical drainage over a cell of a scale, with its halo.
    [[nodiscard]] std::shared_ptr<const DrainageNetwork> drainage(FeatureScale scale, std::int64_t cx, std::int64_t cy) const;

    [[nodiscard]] const PlannerContext& context() const { return context_; }
    [[nodiscard]] const Catalogue& catalogue() const { return *context_.catalogue; }
    void clear();

    struct Candidate {
        double x = 0, y = 0;
        double priority = 0;
        double scale = 0;   // 0: drawn from the recipe's range
        double yaw = 0;
        std::vector<std::array<double, 2>> spline;
        ChannelClass channel = ChannelClass::Paleochannel;
    };
    // The candidates of a cell that pass the recipe's rules, before spacing.
    [[nodiscard]] std::shared_ptr<const std::vector<Candidate>> candidates(std::uint32_t recipe, std::int64_t cx, std::int64_t cy) const;

private:
    [[nodiscard]] bool passes(const FeatureRecipe& recipe, double x, double y) const;
    [[nodiscard]] double alignment(const FeatureRecipe& recipe, double x, double y, double fallback) const;
    [[nodiscard]] std::vector<std::array<double, 2>> march(const FeatureRecipe& recipe, double x, double y, double length,
                                                           double yaw) const;
    [[nodiscard]] FeatureInstance instance(std::uint32_t recipe, const Candidate& c, std::uint64_t id, bool secondary) const;
    [[nodiscard]] bool clearOfEarlier(std::uint32_t recipe, double x, double y) const;
    [[nodiscard]] std::int32_t order(std::uint32_t recipe) const;

    PlannerContext context_;
    std::vector<std::int32_t> order_;
    struct Key {
        std::uint32_t recipe;
        std::int64_t x, y;
        bool operator==(const Key&) const = default;
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const noexcept {
            return std::size_t(k.recipe * 0x9e3779b1u) ^ std::hash<std::int64_t>{}(k.x * 73856093 ^ k.y * 19349663);
        }
    };
    template <class T>
    struct Cache {
        std::mutex lock;
        std::unordered_map<Key, std::shared_ptr<const T>, KeyHash> map;
        std::vector<Key> order;
        std::size_t kept = 1024;
        std::shared_ptr<const T> find(const Key& k) {
            std::lock_guard g(lock);
            auto it = map.find(k);
            return it == map.end() ? nullptr : it->second;
        }
        std::shared_ptr<const T> put(const Key& k, std::shared_ptr<const T> v) {
            std::lock_guard g(lock);
            if (auto it = map.find(k); it != map.end()) return it->second;
            map.emplace(k, v);
            order.push_back(k);
            if (order.size() > kept) { map.erase(order.front()); order.erase(order.begin()); }
            return v;
        }
        void clear() { std::lock_guard g(lock); map.clear(); order.clear(); }
    };
    mutable Cache<std::vector<Candidate>> candidates_;
    mutable Cache<InstanceList> cells_;
    mutable Cache<DrainageNetwork> drainage_;
};

// Fixed point from a planner's double, on a 1/256 m grid: the planner's
// arithmetic is float, but what it hands the ground is exact from here on.
core::Fixed quantised(double metres);

} // namespace engine::environment
