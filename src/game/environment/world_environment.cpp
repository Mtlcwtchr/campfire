#include "game/environment/world_environment.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

#include "engine/biomes/detail_edits.hpp"
#include "game/generation/world_map_gen.hpp"
#include "game/world/climate_field.hpp"
#include "game/world/height_field.hpp"
#include "game/world/scene_scatter.hpp"

namespace world::environment {

using env::FieldSample;
namespace field = env::field;

namespace {

std::atomic<std::uint64_t> nextFieldsId{1};

core::WorldPos pos(double x, double y) {
    return {core::Fixed::fromDoubleForContent(x), core::Fixed::fromDoubleForContent(y)};
}

} // namespace

WorldFields::WorldFields(const generation::WorldMapData* map, std::shared_ptr<const MacroWorld::Resolved> macro,
                         const ClimateField* climate)
    : map_(map), macro_(std::move(macro)), climate_(climate), id_(nextFieldsId.fetch_add(1)) {}

HeightField& WorldFields::field() const {
    // A height field is not thread-safe (its memos), so every thread asking
    // gets its own over the same map - the generator's ground, without the
    // features (they plan on it) and without anybody's edits.
    thread_local std::unordered_map<std::uint64_t, std::unique_ptr<HeightField>> fields;
    auto& slot = fields[id_];
    if (!slot) slot = macro_ ? std::make_unique<HeightField>(map_, map_->seed, macro_)
                             : std::make_unique<HeightField>(map_, map_->seed);
    return *slot;
}

double WorldFields::height(double x, double y) const { return field().heightAt(pos(x, y)).toDouble(); }

double WorldFields::erodibility(double x, double y) const {
    return std::clamp(field().soilAt(pos(x, y)).properties.erodibility.toDouble(), 0.0, 1.0);
}

double WorldFields::moisture(double x, double y) const {
    return climate_ && climate_->ready() ? climate_->at(pos(x, y)).environment[2].toDouble() : 0.5;
}

void WorldFields::heights(double x0, double y0, double step, int columns, int rows, std::vector<double>& out) const {
    auto& f = field();
    HeightField::QueryCache memo(f);
    out.resize(std::size_t(columns) * rows);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            out[std::size_t(r) * columns + c] = f.heightAt(pos(x0 + c * step, y0 + r * step)).toDouble();
}

void WorldFields::climateInto(double x, double y, FieldSample& out) const {
    auto& f = field();
    const auto p = pos(x, y);
    if (climate_ && climate_->ready()) {
        const auto c = climate_->at(p);
        out.set(field::Temperature, float(c.environment[0].toDouble() * 80.0 - 30.0));
        out.set(field::Fertility, float(c.environment[1].toDouble()));
        out.set(field::Moisture, float(c.environment[2].toDouble()));
        out.set(field::Drainage, float(c.environment[5].toDouble()));
        out.set(field::Canopy, std::clamp(climate_->forestCoverAt(x, y), 0.0f, 1.0f));
    }
    const auto m = f.materialsAt(p);
    out.set(field::Rockiness, float(m.of(Material::Rock).toDouble()));
    out.set(field::Sand, float(m.of(Material::Sand).toDouble()));
    out.set(field::Snow, float(m.of(Material::Snow).toDouble()));
    const bool wet = f.underWater(p);
    out.set(field::Water, wet ? 1.0f : 0.0f);
    // Standing moisture of the ground: marsh where the ground is marsh, and
    // otherwise rain that the soil does not drain, gathered where the ground
    // converges.
    const float moisture = out.get(field::Moisture, 0.5f), drainage = out.get(field::Drainage, 0.7f);
    const float marsh = float(m.of(Material::Marsh).toDouble());
    const float gathered = out.get(field::Convergence, 0.0f);
    out.set(field::Wetness, std::clamp(std::max(marsh, moisture * (1 - drainage) * 0.8f + gathered * 0.3f), 0.0f, 1.0f));
    const auto soil = f.soilAt(p);
    out.set(field::SoilDepth, std::clamp(1.0f - float(soil.weights.of(Soil::Rocky).toDouble()) -
                                             out.get(field::Rockiness, 0) * 0.5f, 0.0f, 1.0f));
}

void WorldFields::sample(double x, double y, FieldSample& out) const {
    env::deriveShape([this](double px, double py) { return height(px, py); }, x, y, env::ShapeSettings{}, out);
    climateInto(x, y, out);
}

void WorldFields::sampleGrid(double x0, double y0, double step, int columns, int rows, std::span<FieldSample> out) const {
    auto& f = field();
    HeightField::QueryCache memo(f);
    const env::ShapeSettings settings;
    const int margin = int(std::ceil(settings.largeRing / step)) + 1;
    const int wc = columns + 2 * margin, wr = rows + 2 * margin;
    std::vector<double> heights(std::size_t(wc) * wr);
    for (int r = 0; r < wr; ++r)
        for (int c = 0; c < wc; ++c)
            heights[std::size_t(r) * wc + c] = f.heightAt(pos(x0 + (c - margin) * step, y0 + (r - margin) * step)).toDouble();
    env::deriveShapeGrid(heights, columns, rows, margin, step, settings, out);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            climateInto(x0 + c * step, y0 + r * step, out[std::size_t(r) * columns + c]);
}

NaturalZones::NaturalZones() : types_{{"unclassified", {0.5f, 0.5f, 0.5f}}} {}

void NaturalZones::classify(const FieldSample& f, std::span<float> weights, env::ZoneScalars& s) const {
    std::fill(weights.begin(), weights.end(), 0.0f);
    weights[0] = 1;
    s.density = f.get(field::Canopy, 0);
    s.wetness = f.get(field::Wetness, 0);
    s.exposure = f.get(field::Exposure, 0.5f);
    s.age = f.get(field::Age, 0.5f);
    s.disturbance = f.get(field::Disturbance, 0);
    s.rockiness = f.get(field::Rockiness, 0);
    s.canopy = f.get(field::Canopy, 0);
    s.soilDepth = f.get(field::SoilDepth, 0.5f);
}

std::shared_ptr<const env::Environment> buildWorldEnvironment(
        const generation::WorldMapData& map, std::shared_ptr<const MacroWorld::Resolved> macro,
        const ClimateField& climate, std::vector<env::EnvironmentProblem>* problems) {
    auto fields = std::make_shared<WorldFields>(&map, std::move(macro), &climate);
    env::EnvironmentSetup setup;
    setup.content = env::defaultContentDirectory();
    setup.style = env::defaultStyleDirectory();
    setup.classifier = std::make_shared<NaturalZones>();
    setup.fields = fields;
    setup.height = [fields](double x, double y) { return fields->height(x, y); };
    setup.models = [](std::string_view name) -> std::optional<std::uint32_t> {
        for (std::size_t i = 0; i < decor::kModels.size(); ++i)
            if (name == decor::kModels[i]) return std::uint32_t(i);
        return std::nullopt;
    };
    // The past was wetter than today, and most so where today is dry: that
    // is what leaves broad dry valleys in country with little water now.
    setup.climate.rainToday = [fields](double x, double y) { return 0.2 + fields->moisture(x, y); };
    setup.climate.pastRain = [fields](double x, double y) { return 1.0 + 0.6 * (1.0 - fields->moisture(x, y)); };
    setup.climate.erodibility = [fields](double x, double y) { return fields->erodibility(x, y); };
    setup.climate.heights = [fields](double x0, double y0, double step, int columns, int rows, std::vector<double>& out) {
        fields->heights(x0, y0, step, columns, rows, out);
    };
    setup.seed = map.seed;
    setup.gameVersion = kGameEnvironmentVersion;
    // A feature removed by hand is filed with the other removed details
    // (engine/biomes/detail_edits.hpp), under its instance id.
    if (map.details && !map.details->empty()) {
        const auto details = map.details;
        setup.removed = [details](std::uint64_t id, double x, double y) { return details->removed(id, x, y); };
        setup.removedVersion = details->removedFingerprint();
    }
    return env::Environment::build(std::move(setup), problems);
}

} // namespace world::environment
