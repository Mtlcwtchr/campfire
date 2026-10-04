#pragma once
// This game's procedural environment (doc/plan_procedural_environment_2026-10-03.md):
// what the engine's mechanism (engine/environment) is told about this world.
//
//   WorldFields     the engine's named fields, answered from the height field,
//                   the climate and the soil of a generated world
//   NaturalZones    the zone classifier: which local scene types this game
//                   has and how they follow from the fields
//   buildWorldEnvironment   the two, the content directories and the model
//                   catalogue, put together for one world
//
// The zones are the game's to decide and are not decided yet: until they are
// approved the classifier knows only "unclassified", which the engine treats
// as no zones at all - nothing is sampled, nothing changes on screen.
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "engine/environment/environment.hpp"
#include "game/world/macro.hpp"

namespace generation { struct WorldMapData; }
namespace world { class ClimateField; class HeightField; }

namespace world::environment {

namespace env = engine::environment;

// Bumped whenever WorldFields or NaturalZones change what they answer: baked
// pages are filed under it (Environment::fingerprint).
inline constexpr std::uint64_t kGameEnvironmentVersion = 1;

class WorldFields final : public env::FieldSource {
public:
    WorldFields(const generation::WorldMapData* map, std::shared_ptr<const MacroWorld::Resolved> macro,
                const ClimateField* climate);
    void sample(double x, double y, env::FieldSample& out) const override;
    void sampleGrid(double x0, double y0, double step, int columns, int rows,
                    std::span<env::FieldSample> out) const override;
    // The ground before any feature, metres: what the features plan on.
    [[nodiscard]] double height(double x, double y) const;
    // How easily the ground gives way, 0..1, from its soil.
    [[nodiscard]] double erodibility(double x, double y) const;
    [[nodiscard]] double moisture(double x, double y) const;
    // Heights before features over a grid, in one batch (the drainage asks).
    void heights(double x0, double y0, double step, int columns, int rows, std::vector<double>& out) const;

private:
    world::HeightField& field() const;
    void climateInto(double x, double y, env::FieldSample& out) const;

    const generation::WorldMapData* map_;
    std::shared_ptr<const MacroWorld::Resolved> macro_;
    const ClimateField* climate_;
    std::uint64_t id_;
};

class NaturalZones final : public env::ZoneClassifier {
public:
    NaturalZones();
    [[nodiscard]] std::span<const env::ZoneType> types() const override { return types_; }
    void classify(const env::FieldSample& fields, std::span<float> weights, env::ZoneScalars& scalars) const override;

private:
    std::vector<env::ZoneType> types_;
};

// The environment of one world. `macro` is the snapshot's resolved macro
// layer (shared so no field over the map works it out again).
std::shared_ptr<const env::Environment> buildWorldEnvironment(
        const generation::WorldMapData& map, std::shared_ptr<const MacroWorld::Resolved> macro,
        const ClimateField& climate, std::vector<env::EnvironmentProblem>* problems = nullptr);

} // namespace world::environment
