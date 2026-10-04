#include "game/environment/world_environment.hpp"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>
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
    const auto lattice = double(kSampleMetres);
    const bool aligned = std::fmod(step, lattice) == 0 && std::fmod(x0, lattice) == 0 && std::fmod(y0, lattice) == 0;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c) {
            const double x = x0 + c * step, y = y0 + r * step;
            out[std::size_t(r) * columns + c] = aligned
                    ? f.sampleHeight(std::int64_t(std::floor(x / lattice)), std::int64_t(std::floor(y / lattice))).toDouble()
                    : f.heightAt(pos(x, y)).toDouble();
        }
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
        out.set(gameFields().woodland, std::clamp(float(c.woodland.toDouble()), 0.0f, 1.0f));
        // The canopy as the trees are placed (scene_scatter.hpp): the climate's
        // forest cover times the metre-scale masses and clearings of the wood.
        const auto canopyAt = [&](double px, double py) {
            return std::clamp(climate_->forestCoverAt(px, py) * float(decor::forestDensity(map_->seed, px, py)), 0.0f, 1.0f);
        };
        out.set(field::Canopy, canopyAt(x, y));
        float ring = 0;
        for (int k = 0; k < 6; ++k)
            ring += canopyAt(x + 120.0 * std::cos(k * 1.0471975512), y + 120.0 * std::sin(k * 1.0471975512));
        out.set(gameFields().canopyBroad, ring / 6.0f);
    }
    // Height and slope are on the sample already (the shape came first):
    // the field's own materialsAt and underWater would work both out again.
    const bool shaped = out.has(field::Elevation) && out.has(field::Slope);
    const core::Fixed height = shaped ? core::Fixed::fromDoubleForContent(out[field::Elevation]) : f.heightAt(p);
    const core::Fixed slope = shaped ? core::Fixed::fromDoubleForContent(out[field::Slope]) : f.slopeAt(p);
    const auto m = f.materialsGiven(std::int64_t(std::floor(x / kSampleMetres)), std::int64_t(std::floor(y / kSampleMetres)),
                                    height, slope);
    out.set(field::Rockiness, float(m.of(Material::Rock).toDouble()));
    out.set(field::Sand, float(m.of(Material::Sand).toDouble()));
    out.set(field::Snow, float(m.of(Material::Snow).toDouble()));
    // How much of the country round the point is river, lake or sea: a
    // footprint two hundred metres across, so a bank knows its water. The
    // same answer says whether the point itself is under it.
    {
        const auto& g = gameFields();
        const auto here = f.waterOver(p, core::Fixed::fromInt(200), slope);
        const float share = std::clamp(float(here.cover.toDouble()), 0.0f, 1.0f);
        out.set(g.riverNear, here.kind == HeightField::WaterKind::River ? share : 0.0f);
        out.set(g.lakeNear, here.kind == HeightField::WaterKind::Lake ? share : 0.0f);
        out.set(g.seaNear, here.kind == HeightField::WaterKind::Ocean ? share : 0.0f);
        out.set(field::Water, share > 0.0f && here.level > height + core::Fixed::ratio(1, 20) ? 1.0f : 0.0f);
    }
    // Standing moisture of the ground: marsh where the ground is marsh, and
    // otherwise rain that the soil does not drain, gathered where the ground
    // converges.
    const float moisture = out.get(field::Moisture, 0.5f), drainage = out.get(field::Drainage, 0.7f);
    const float marsh = float(m.of(Material::Marsh).toDouble());
    const float gathered = out.get(field::Convergence, 0.0f);
    out.set(field::Wetness, std::clamp(std::max(marsh, moisture * (1 - drainage) * 0.8f + gathered * 0.3f), 0.0f, 1.0f));
    // Soil depth from what shows through it: rock and steepness. The soil
    // model itself costs as much again as everything above.
    out.set(field::SoilDepth, std::clamp(1.0f - out.get(field::Rockiness, 0) * 0.8f -
                                             std::max(0.0f, out.get(field::Slope, 0) - 0.5f) * 0.6f, 0.0f, 1.0f));
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
    // On the height field's own lattice the samples are read as they are,
    // not interpolated from the four around them.
    const auto lattice = double(kSampleMetres);
    const bool aligned = std::fmod(step, lattice) == 0 && std::fmod(x0, lattice) == 0 && std::fmod(y0, lattice) == 0;
    for (int r = 0; r < wr; ++r)
        for (int c = 0; c < wc; ++c) {
            const double x = x0 + (c - margin) * step, y = y0 + (r - margin) * step;
            heights[std::size_t(r) * wc + c] = aligned
                    ? f.sampleHeight(std::int64_t(std::floor(x / lattice)), std::int64_t(std::floor(y / lattice))).toDouble()
                    : f.heightAt(pos(x, y)).toDouble();
        }
    env::deriveShapeGrid(heights, columns, rows, margin, step, settings, out);
    // The climate, the ground's make and the nearness of water change over
    // tens of metres and cost a dozen field queries a point: at fine steps
    // they are asked every other point and shared with the neighbours.
    const int every = step < 24.0 ? 2 : 1;
    const auto fields = env::FieldId(env::fieldCount());
    for (int r = 0; r < rows; r += every)
        for (int c = 0; c < columns; c += every) {
            auto& here = out[std::size_t(r) * columns + c];
            climateInto(x0 + c * step, y0 + r * step, here);
            for (int dr = 0; dr < every && r + dr < rows; ++dr)
                for (int dc = 0; dc < every && c + dc < columns; ++dc) {
                    if (!dr && !dc) continue;
                    auto& other = out[std::size_t(r + dr) * columns + (c + dc)];
                    for (env::FieldId id = env::field::Wetness; id < fields; ++id)
                        if (here.has(id)) other.set(id, here[id]);
                }
        }
}

std::map<std::string, std::uint32_t> paintedLegend() {
    std::map<std::string, std::uint32_t> out;
    std::ifstream in(env::defaultContentDirectory() / "painted.json");
    const auto j = nlohmann::json::parse(in, nullptr, false, true);
    if (j.is_discarded() || !j.contains("ids") || !j["ids"].is_object()) return out;
    for (const auto& [key, value] : j["ids"].items())
        if (value.is_string()) out[value.get<std::string>()] = std::uint32_t(std::stoul(key));
    return out;
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
    setup.zoneMasks = naturalZoneMasks;
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
    setup.painted = map.features;
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
