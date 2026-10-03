#pragma once
// One world's procedural environment, put together
// (doc/plan_procedural_environment_2026-10-03.md).
//
//   fields -> zones -> planner -> feature layer -> masks -> cover / dressing / meshes
//                                                 style tables -> shaders
//
// The game builds one from its own pieces (classifier, field source, height
// before features, models) and the content directories, and sets it active.
// Everything made from an environment remembers its generation; a reload
// (content edited, world changed) makes a new environment with a new
// generation, and whatever was made from the old one is stale - the same rule
// as the biome registry's.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/environment/catalogue.hpp"
#include "engine/environment/feature_layer.hpp"
#include "engine/environment/masks.hpp"
#include "engine/environment/planner.hpp"
#include "engine/environment/style.hpp"
#include "engine/environment/zones.hpp"

namespace engine::environment {

struct EnvironmentSetup {
    std::filesystem::path content;   // content/config/environment
    std::filesystem::path style;     // content/config/style
    std::shared_ptr<const ZoneClassifier> classifier;
    std::shared_ptr<const FieldSource> fields;   // the world before features
    HeightAt height;                              // the same
    ModelResolver models;
    ZoneMasks zoneMasks;
    ZoneSettings zones;
    DrainageClimate climate;
    DrainageSettings drainage;
    std::uint64_t seed = 0;
};

struct EnvironmentProblem {
    std::string file;
    std::string what;
};

class Environment {
public:
    static std::shared_ptr<Environment> build(EnvironmentSetup setup, std::vector<EnvironmentProblem>* problems);
    // The same from a catalogue already made: tests and tools.
    static std::shared_ptr<Environment> build(EnvironmentSetup setup, std::shared_ptr<const Catalogue> catalogue);

    [[nodiscard]] const Catalogue& catalogue() const { return *catalogue_; }
    [[nodiscard]] std::shared_ptr<const Catalogue> catalogueShared() const { return catalogue_; }
    [[nodiscard]] const ZoneField* zones() const { return zones_.get(); }
    [[nodiscard]] const FeaturePlanner& planner() const { return *planner_; }
    [[nodiscard]] const FeatureLayer& features() const { return *features_; }
    [[nodiscard]] std::shared_ptr<const FeatureLayer> featuresShared() const { return features_; }
    [[nodiscard]] const StyleTable* palettes() const { return palettes_.get(); }
    [[nodiscard]] const StyleTable* grades() const { return grades_.get(); }
    [[nodiscard]] std::shared_ptr<const StyleTable> gradesShared() const { return grades_; }
    [[nodiscard]] const EnvironmentSetup& setup() const { return setup_; }
    [[nodiscard]] std::uint64_t generation() const { return generation_; }

    // The masks of a page, the way the page baker asks for them.
    [[nodiscard]] PageMasks masks(double x0, double y0, double step, int side) const;
    // The GPU cover table (cover.hpp) for this catalogue's zones.
    [[nodiscard]] std::vector<std::array<float, 4>> coverTable() const;

private:
    EnvironmentSetup setup_;
    std::shared_ptr<const Catalogue> catalogue_;
    std::unique_ptr<ZoneField> zones_;
    std::shared_ptr<const FeaturePlanner> planner_;
    std::shared_ptr<const FeatureLayer> features_;
    std::shared_ptr<const StyleTable> palettes_, grades_;
    std::uint64_t generation_ = 0;
};

// The environment the world is drawn and placed with. Null until a game sets one.
std::shared_ptr<const Environment> active();
void setActive(std::shared_ptr<const Environment> environment);
std::uint64_t activeGeneration();

// content/config/environment and content/config/style, found the way the
// biome registry finds content/config/terrain.
std::filesystem::path defaultContentDirectory();
std::filesystem::path defaultStyleDirectory();

} // namespace engine::environment
