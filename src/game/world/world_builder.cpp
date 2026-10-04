#include "game/world/world_builder.hpp"

#include "engine/biomes/category_field.hpp"
#include "engine/biomes/detail_edits.hpp"
#include "engine/biomes/registry.hpp"
#include "engine/core/progress.hpp"
#include "engine/environment/scatter.hpp"
#include "game/environment/world_environment.hpp"

#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include "engine/core/rng.hpp"
#include "../../../assets/shaders/sand_motion.hlsli"

namespace world {
namespace {
generation::WorldMapData validated(generation::WorldMapData map) {
    if (map.width <= 0 || map.height <= 0 ||
        map.cells.size() / std::size_t(map.width) != std::size_t(map.height) ||
        map.cells.size() % std::size_t(map.width) != 0)
        throw std::invalid_argument("world publication requires a complete non-empty map");
    return map;
}
core::WorldPos centre(core::TilePos cell) {
    return {core::Fixed::fromInt(std::int64_t(cell.x) * generation::kMetresPerCell + generation::kMetresPerCell / 2),
            core::Fixed::fromInt(std::int64_t(cell.y) * generation::kMetresPerCell + generation::kMetresPerCell / 2)};
}
}

streaming::PageStore::Config WorldBuilder::defaultPageConfig() {
    streaming::PageStore::Config config;
    config.residentBytes = 192u << 20;
    if (std::getenv("ASR_TERRAIN_NO_DISK_CACHE")) return config;
    if (const auto* root = std::getenv("ASR_TERRAIN_CACHE_DIR"); root && *root) config.diskCacheRoot = root;
    else {
        std::filesystem::path content = "content";
        std::error_code ec;
        for (int up = 0; up < 5 && !std::filesystem::exists(content, ec); ++up) content = ".." / content;
        config.diskCacheRoot = (std::filesystem::exists(content, ec) ? content.parent_path() :
            std::filesystem::path{}) / ".cache" / "terrain";
    }
    return config;
}

namespace {
streaming::PageStore::Config over(streaming::PageStore::Config config, std::shared_ptr<const EditLayer> heights) {
    config.edits = std::move(heights);
    return config;
}
}

WorldSnapshot::WorldSnapshot(generation::WorldMapData map, streaming::PageStore::Config config, std::uint64_t version,
                             std::shared_ptr<ecology::Store> edits, std::shared_ptr<const EditLayer> heights)
    : version_(version), map_(validated(std::move(map))),
      hydrology_(streaming::sharedHydrologyGraph(map_, config.diskCacheRoot)),
      ecology_(edits ? std::move(edits) : std::make_shared<ecology::Store>()), heights_(std::move(heights)),
      pages_(map_, *hydrology_, streaming::hsimQuantisationFor(map_), over(std::move(config), heights_)) {
    // The generator's ground, not the dug one: climate is a property of the
    // country, raised once, and a pit dug later in the session could not move
    // it anyway - so a pit dug in an earlier one must not either.
    const auto graphBuilt = std::chrono::steady_clock::now();
    core::progress("climate textures");
    HeightField query(&map_, map_.seed);
    climate_.raise(map_, query);
    const auto climateRaised = std::chrono::steady_clock::now();
    // The procedural environment, over the generator's ground and the climate
    // just raised. Features plan lazily, a cell at a time, as pages ask.
    core::progress("environment");
    std::call_once(macroOnce_, [&] { macroResolved_ = HeightField(&map_, map_.seed).macro().resolved(); });
    std::vector<engine::environment::EnvironmentProblem> problems;
    environment_ = environment::buildWorldEnvironment(map_, macroResolved_, climate_, &problems);
    for (const auto& p : problems)
        std::fprintf(stderr, "environment: %s: %s\n", p.file.c_str(), p.what.c_str());
    pages_.environment(environment_);
    core::progress("landmarks");
    findLandmarks();
    const auto landmarksFound = std::chrono::steady_clock::now();
    const auto millis = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    timings_.graph = millis(began_, graphBuilt);
    timings_.climate = millis(graphBuilt, climateRaised);
    timings_.landmarks = millis(climateRaised, landmarksFound);
    // No prebake. It baked and pinned every H64 page of the world's land
    // before anything was streamed: a hundred megabytes for one region, and
    // nine gigabytes - and as many again on the card - for the eight hundred
    // by two thousand kilometres the game is built for. H64 is streamed like
    // every other level, around the camera, and the disk keeps what was baked.
    if (std::getenv("ASR_TERRAIN_PREBAKE")) pages_.prebakeInBackground({2, 4});
}

std::shared_ptr<const engine::environment::Environment> WorldSnapshot::environment() const {
    const std::lock_guard<std::mutex> held(environmentGuard_);
    return environment_;
}

void WorldSnapshot::reloadEnvironment() const {
    std::vector<engine::environment::EnvironmentProblem> problems;
    auto next = environment::buildWorldEnvironment(map_, macroResolved_, climate_, &problems);
    for (const auto& p : problems)
        std::fprintf(stderr, "environment: %s: %s\n", p.file.c_str(), p.what.c_str());
    {
        const std::lock_guard<std::mutex> held(environmentGuard_);
        // The old one stays alive with the snapshot: height fields made from
        // it point into its feature layer and may still be answering.
        retiredEnvironments_.push_back(std::move(environment_));
        environment_ = next;
    }
    pages_.environment(std::move(next));
}

void WorldBuilder::build(const generation::WorldMapParams& params) {
    const auto ticket = request();
    complete(ticket, generation::generateWorldMap(params));
}

void WorldBuilder::publish(generation::WorldMapData map) {
    complete(request(), std::move(map));
}

std::shared_ptr<const WorldSnapshot> WorldBuilder::raise(Ticket ticket, generation::WorldMapData map) {
    if (!published_.current(ticket)) return nullptr;
    return std::make_shared<WorldSnapshot>(std::move(map), config_, ticket.version(), edits_, heights_);
}

bool WorldBuilder::publish(Ticket ticket, std::shared_ptr<const WorldSnapshot> raised) {
    return raised && published_.publish(ticket, std::move(raised));
}

bool WorldBuilder::complete(Ticket ticket, generation::WorldMapData map) {
    if (!published_.current(ticket)) return false;
    // Failed construction leaves the current publication intact.
    auto next = std::make_shared<WorldSnapshot>(std::move(map), config_, ticket.version(), edits_, heights_);
    return published_.publish(ticket, std::move(next));
}

decor::Scatter WorldSnapshot::scatter(int x, int y, int radiusMetres) const {
    if (radiusMetres<=0 || radiusMetres%decor::kCell || radiusMetres>decor::kRadius)
        throw std::invalid_argument("invalid scene scatter domain");
    const std::int64_t cx=std::int64_t(x)*decor::kRegion,cy=std::int64_t(y)*decor::kRegion;
    return scatter({cx-radiusMetres,cy-radiusMetres,cx+radiusMetres,cy+radiusMetres});
}

decor::Scatter WorldSnapshot::scatter(decor::ScatterBounds bounds) const {
    return scatter(bounds, *ecology_->read());
}

ecology::Cell WorldSnapshot::ecologyAt(double x, double y, HeightField& query, const ecology::Delta& delta) const {
    const auto key = ecology::key(x, y);
    if (const auto found = delta.cells.find(key); found != delta.cells.end()) return found->second;
    const double cx = (double(key.first) + 0.5) * ecology::kCellMetres;
    const double cy = (double(key.second) + 0.5) * ecology::kCellMetres;
    const core::WorldPos p{core::Fixed::fromDoubleForContent(cx), core::Fixed::fromDoubleForContent(cy)};
    const auto climate = climate_.at(p);
    const auto scalar = [](core::Fixed f) { return float(f.toDouble()); };
    // Read where it lies: a field of the world is sparse (cell_field.hpp), and
    // taking it as a vector here copied every cell of the map four times a sample.
    const auto physicalField = [&](const auto& values, float fallback) {
        if (values.size()!=map_.cells.size()) return fallback;
        const double gx=cx/generation::kMetresPerCell,gy=cy/generation::kMetresPerCell;
        const int ix=int(std::floor(gx)),iy=int(std::floor(gy));
        const auto at = [&](int x, int y) {
            return double(values[std::size_t(std::clamp(y,0,map_.height-1))*map_.width+
                std::size_t(std::clamp(x,0,map_.width-1))])/255.0;
        };
        return ecology::unit(float(std::lerp(std::lerp(at(ix,iy),at(ix+1,iy),gx-ix),
            std::lerp(at(ix,iy+1),at(ix+1,iy+1),gx-ix),gy-iy)));
    };
    ecology::Physical physical;
    physical.elevation = scalar(query.heightAt(p));
    physical.slope = scalar(query.slopeAt(p));
    physical.temperature = scalar(climate.environment[0]) * 80 - 30;
    physical.moisture = physicalField(map_.humidityField,scalar(climate.environment[2]));
    physical.naturalFertility = physicalField(map_.soilFertilityField,scalar(climate.environment[1]));
    physical.drainage = physicalField(map_.soilDrainageField,scalar(climate.environment[5]));
    physical.seasonality = physicalField(map_.seasonalityField,physical.seasonality);
    physical.conifer = scalar(climate.foliage[1]);
    // Soil depth and groundwater do not yet have independent simulation fields;
    // these explicit terrain-derived proxies are not read back from biome IDs.
    physical.soilDepth = ecology::unit(1 - physical.slope);
    // Sand and rock as the ground actually is here (what the renderer draws),
    // not only as climate: a dune field or a scree slope carries no forest.
    const auto ground = query.materialsAt(p);
    physical.rock = std::max(ecology::unit(physical.slope), float(ground.of(Material::Rock).toDouble()));
    physical.sand = std::max(scalar(climate.desert), float(ground.of(Material::Sand).toDouble()));
    const float aboveWater = physical.elevation - scalar(query.waterLevelAt(p));
    physical.groundwater = ecology::unit(1 - std::max(aboveWater, 0.0f) / 8);
    physical.flood = std::max(physicalField(map_.floodplainPotentialField,0),physical.groundwater) *
        (1 - ecology::unit(physical.slope * 4));
    physical.coast = physical.elevation < 4;
    physical.salinity = physical.coast ? 0.5f : 0;
    auto cell = ecology::initial(physical, float(decor::forestDensity(map_.seed, cx, cy)));
    if (aboveWater < 0.4f) { cell.canopy = cell.grass = cell.shrubs = 0; ecology::classify(cell, physical); }
    return cell;
}

decor::Scatter WorldSnapshot::scatter(decor::ScatterBounds bounds, const ecology::Delta& delta) const {
    auto query = field();
    return scatter(bounds, delta, query);
}

decor::Scatter WorldSnapshot::scatter(decor::ScatterBounds bounds, const ecology::Delta& delta, HeightField& query) const {
    // Before anything is read: every edit at or below this is under the objects.
    const std::uint64_t ground = heights_ ? heights_->revision() : 0;
    HeightField::QueryCache queries(query);
    std::map<ecology::Key, ecology::Cell> cells;
    // The terrain categories (engine/biomes): the registry held for the whole
    // scatter, the ids from the import's 256 m field or the climate's derived
    // ones, read a little off the point so a border between two forests is
    // ragged rather than a 256 m staircase.
    const auto registry = engine::biomes::active();
    const auto* painted = map_.categories.get();
    const bool biomes = registry && (painted || climate_.anyCategory());
    const auto* details = map_.details && !map_.details->empty() ? map_.details.get() : nullptr;
    const auto biomesAt = [&](double wx, double wy, decor::Site& site) {
        const auto h = core::splitmix64(core::splitmix64(std::uint64_t(std::int64_t(std::floor(wx / 40.0)))) ^
                                        std::uint64_t(std::int64_t(std::floor(wy / 40.0))) * 0x9e3779b97f4a7c15ull);
        const double jx = (double(h & 0xffff) / 65535.0 - 0.5) * 180.0, jy = (double((h >> 16) & 0xffff) / 65535.0 - 0.5) * 180.0;
        engine::biomes::CategoryField::Ids ids{};
        if (painted) ids = painted->at(wx + jx, wy + jy);
        if (ids[0] == 0 && ids[1] == 0 && ids[3] == 0) {
            const auto derived = climate_.categoriesAt(wx + jx, wy + jy);
            for (std::size_t k = 0; k < derived.size(); ++k) ids[k] = derived[k];
        }
        site.registry = registry.get();
        site.forestBias = engine::biomes::CategoryField::forestBias(ids);
        site.forestBiome = registry->forestBiome(registry->resolve(engine::biomes::Layer::Forest, ids[0], ids[1]));
        site.decorBiome = registry->decorBiome(registry->resolve(engine::biomes::Layer::Decor, ids[0], ids[3]));
    };
    auto result = decor::scatter(map_.seed, bounds,
        double(map_.width) * generation::kMetresPerCell, double(map_.height) * generation::kMetresPerCell,
        [&](int px, int py) { return pages_.containsLand({px, py, 4}); },
        [&](double wx, double wy) {
            const core::WorldPos p{core::Fixed::fromDoubleForContent(wx), core::Fixed::fromDoubleForContent(wy)};
            const auto climate = climate_.at(p);
            auto [cell, fresh] = cells.try_emplace(ecology::key(wx, wy));
            if (fresh) cell->second = ecologyAt(wx, wy, query, delta);
            decor::Site site{query.heightAt(p).toDouble(), query.waterLevelAt(p).toDouble(),
                query.slopeAt(p).toDouble(), climate.woodland.toDouble(), climate.foliage[1].toDouble(),
                cell->second, true};
            const auto ground = query.materialsAt(p);
            site.sand = ground.of(Material::Sand).toDouble();
            site.rock = ground.of(Material::Rock).toDouble();
            site.marsh = ground.of(Material::Marsh).toDouble();
            site.snow = ground.of(Material::Snow).toDouble();
            site.hasMaterials = true;
            if (biomes) biomesAt(wx, wy, site);
            if (details) site.detailDensity = details->density(wx, wy);
            return site;
        },
        [&](double wx, double wy) {
            return query.underWater({core::Fixed::fromDoubleForContent(wx),
                                     core::Fixed::fromDoubleForContent(wy)});
        },true);
    // The procedural environment (engine/environment): the supporting pieces
    // of every feature, and the secondary cover by zone. Each object is kept
    // only by the region its position falls in, so neighbouring regions that
    // both see a feature do not both set its stones down. In the same list,
    // and under the same removals, as everything the generator placed.
    if (const auto env = environment();
        env && (!env->catalogue().recipes().empty() || !env->catalogue().cover().rules.empty())) {
        namespace e = engine::environment;
        const auto fx = [](double v) { return core::Fixed::fromDoubleForContent(v); };
        const auto groundAt = [&](double x, double y) { return query.heightAt({fx(x), fx(y)}).toDouble(); };
        std::vector<e::PlacedObject> placed;
        std::vector<e::FeatureInstance> instances;
        env->planner().instancesIn({{fx(double(bounds.minX)), fx(double(bounds.minY))},
                                    {fx(double(bounds.maxX)), fx(double(bounds.maxY))}}, instances);
        for (const auto& in : instances) e::dress(env->catalogue(), in, groundAt, placed);
        e::CoverContext under;
        under.catalogue = &env->catalogue();
        under.fields = env->setup().fields.get();
        under.zones = env->zones();
        under.features = &env->features();
        under.zoneMasks = env->setup().zoneMasks;
        under.height = env->setup().height;
        under.ground = groundAt;
        under.seed = map_.seed;
        e::cover(under, double(bounds.minX), double(bounds.minY), double(bounds.maxX), double(bounds.maxY), placed);
        for (const auto& o : placed) {
            if (o.x < double(bounds.minX) || o.x >= double(bounds.maxX) || o.y < double(bounds.minY) ||
                o.y >= double(bounds.maxY) || o.model >= decor::kModels.size())
                continue;
            if (query.underWater({fx(o.x), fx(o.y)})) continue;
            result.objects.push_back({o.id | (1ull << 62), o.x, o.y, o.z - decor::groundSink(o.model, o.scale), o.scale,
                                      o.yaw, float(double(o.id & 0xffff) / 65535.0 * 6.2831853), o.tint, o.model});
            ++result.populations[o.model];
        }
    }
    result.revision = delta.region(double(bounds.minX), double(bounds.minY));
    result.ground = ground;
    std::erase_if(result.objects, [&](const auto& object) {
        if (!delta.removed.contains(object.id) && !delta.clears(object.x, object.y, object.model) &&
            !(details && details->removed(object.id, object.x, object.y)))
            return false;
        --result.populations[object.model];
        return true;
    });
    // Details pinned by hand (source/details): whatever the ids say now.
    if (details && registry)
        for (const auto& pin : details->pinnedIn(double(bounds.minX), double(bounds.minY), double(bounds.maxX),
                                                 double(bounds.maxY))) {
            std::string modelName;
            double scale = pin.scale;
            if (pin.kind == "plant") {
                if (const auto i = registry->plantIndex(pin.name)) modelName = registry->plants()[*i].model;
            } else if (pin.kind == "decal") {
                if (const auto i = registry->decalIndex(pin.name)) modelName = registry->decals()[*i].model;
            } else if (const auto i = registry->propIndex(pin.name)) {
                modelName = registry->props()[*i].model;
            }
            std::uint32_t model = std::uint32_t(decor::kModels.size());
            for (std::uint32_t m = 0; m < decor::kModels.size(); ++m)
                if (modelName == decor::kModels[m]) model = m;
            if (model >= decor::kModels.size()) continue;
            std::uint64_t id = 0xcbf29ce484222325ull;
            for (const unsigned char c : pin.id) id = (id ^ c) * 0x100000001b3ull;
            id |= 1ull << 63;
            const core::WorldPos p{core::Fixed::fromDoubleForContent(pin.x), core::Fixed::fromDoubleForContent(pin.y)};
            result.objects.push_back({id, pin.x, pin.y, query.heightAt(p).toDouble() - decor::groundSink(model, scale),
                                      float(scale), float(pin.yaw), float(double(id & 0xffff) / 65535.0 * 6.2831853),
                                      1.0f, model});
            ++result.populations[model];
        }
    // What people planted stands on the ground as it is, like anything the
    // generator put there, and in the same list: a consumer that draws, shades
    // or bakes a page does not need to know which hand placed what.
    if (!delta.added.empty()) {
        const auto page = [](std::int64_t v) {
            return v / ecology::kPageMetres - (v % ecology::kPageMetres < 0 ? 1 : 0);
        };
        const auto lowX = page(bounds.minX), lowY = page(bounds.minY);
        const auto highX = page(bounds.maxX - 1), highY = page(bounds.maxY - 1);
        const auto first = delta.added.lower_bound({lowX, lowY});
        for (auto at = first; at != delta.added.end() && at->first.first <= highX; ++at) {
            if (at->first.second < lowY || at->first.second > highY) continue;
            for (const auto& [id, a] : at->second) {
                if (a.x < double(bounds.minX) || a.x >= double(bounds.maxX) ||
                    a.y < double(bounds.minY) || a.y >= double(bounds.maxY) || a.model >= decor::kModels.size())
                    continue;
                const core::WorldPos p{core::Fixed::fromDoubleForContent(a.x), core::Fixed::fromDoubleForContent(a.y)};
                const double sink = decor::groundSink(a.model, a.scale);
                const auto phase = float(double(core::splitmix64(id) >> 11) * 0x1p-53 * 2 * std::acos(-1.0));
                result.objects.push_back({id, a.x, a.y, query.heightAt(p).toDouble() - sink,
                                          a.scale, a.yaw, phase, a.tint, a.model});
                ++result.populations[a.model];
            }
        }
    }
    return result;
}

core::WorldPos WorldSnapshot::startingPoint() const {
    auto query = field();
    for (const auto& site : map_.sites)
        if (site.played && !query.underWater(centre(site.cell))) return centre(site.cell);
    for (const auto& landmark : landmarks_)
        if (!query.underWater(landmark.where)) return landmark.where;
    for (const auto& site : map_.sites) if (site.played) return centre(site.cell);
    if (!landmarks_.empty()) return landmarks_.front().where;
    return {core::Fixed::fromInt(std::int64_t(map_.width) * generation::kMetresPerCell / 2),
            core::Fixed::fromInt(std::int64_t(map_.height) * generation::kMetresPerCell / 2)};
}

void WorldSnapshot::findLandmarks() {
    core::TilePos steepest{}, river{}, coast{}, flat{}, edge{map_.width - 1, map_.height / 2};
    int relief = -1, water = -1, least = 1 << 20, nearest = map_.width;
    for (int y = 2; y < map_.height - 2; ++y) for (int x = 2; x < map_.width - 2; ++x) {
        int low = 999, high = -999;
        bool sea = false, land = false;
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const auto& c = map_.at({x + dx, y + dy});
            sea |= c.sea; land |= !c.sea;
            low = std::min(low, int(c.elevation)); high = std::max(high, int(c.elevation));
        }
        const auto& c = map_.at({x, y});
        if (!sea && high - low > relief) { relief = high - low; steepest = {x, y}; }
        if (sea && land) coast = {x, y};
        if (!c.sea && c.river && c.riverSize > water) { water = c.riverSize; river = {x, y}; }
        if (!sea && c.elevation > 2 && high - low < least) { least = high - low; flat = {x, y}; }
    }
    landmarks_ = {{"mountains", centre(steepest)}, {"the big river", centre(river)},
                  {"the coast", centre(coast)}, {"open country", centre(flat)}};
    for (int y = 0; y < map_.height; ++y) for (int x = 0; x < map_.width; ++x) {
        if (map_.at({x, y}).sea) continue;
        const int out = std::min({x, y, map_.width - 1 - x, map_.height - 1 - y});
        if (out < nearest) { nearest = out; edge = {x, y}; }
    }
    landmarks_.push_back({"the border", centre(edge)});
    using generation::Climate;
    const std::pair<Climate, const char*> communities[] = {
        {Climate::Steppe, "steppe"}, {Climate::Taiga, "taiga"}, {Climate::TemperateForest, "temperate forest"},
        {Climate::TropicalForest, "tropics"}, {Climate::Desert, "desert"}};
    HeightField query(&map_, map_.seed);   // landmarks are the country's, like the climate
    for (const auto& [climate, name] : communities) {
        int best = -100000;
        core::TilePos chosen{};
        for (int y = 2; y < map_.height - 2; ++y) for (int x = 2; x < map_.width - 2; ++x) {
            const auto& here = map_.at({x, y});
            if (here.sea || here.climate != climate) continue;
            int score = here.fertility;
            if (climate == Climate::Desert)
                score = -int(here.moisture) * 12 - int(here.elevation) * 12 - (here.river ? 2000 : 0);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const auto& near = map_.at({x + dx, y + dy});
                score += !near.sea && near.climate == climate ? 1000 : -1000;
                score -= std::abs(int(near.elevation) - int(here.elevation)) * 10;
            }
            // Only dry ground: a carved valley under a lake's level is a lake
            // bed, and a "forest" there is a picture of the bottom of a lake.
            if (score > best && !query.underWater(centre({x, y}))) { best = score; chosen = {x, y}; }
        }
        if (best <= -100000) continue;
        auto where = centre(chosen);
        if (climate == Climate::Desert) {
            float bestSupport = -1;
            for (int dy = -2; dy <= 2; ++dy) for (int dx = -2; dx <= 2; ++dx) {
                const core::WorldPos p{centre(chosen).x + core::Fixed::fromInt(dx * generation::kMetresPerCell / 8),
                                       centre(chosen).y + core::Fixed::fromInt(dy * generation::kMetresPerCell / 8)};
                const auto ground = query.groundAt(p);
                const auto material = [&](Material m) { return float(ground.materials.of(m).toDouble()); };
                const float support = sand::sandDriftSupport(float(query.surfaceClimateAt(p).desert.toDouble()),
                    material(Material::Sand), material(Material::Rock), material(Material::Grass),
                    material(Material::Snow), material(Material::Marsh),
                    float((query.waterLevelAt(p) - ground.height).toDouble()), float(ground.normal.z.toDouble()));
                if (!query.underWater(p) && support > bestSupport) { bestSupport = support; where = p; }
            }
        }
        landmarks_.push_back({name, where});
    }
    // A meadow: open, gently rolling grass low down, clear of the woods - the
    // country a painted landscape is mostly made of, and where ground cover
    // is judged. Cells by their climate first, then the grassiest open spot
    // in the best one.
    {
        int best = -100000;
        core::TilePos chosen{};
        for (int y = 2; y < map_.height - 2; ++y) for (int x = 2; x < map_.width - 2; ++x) {
            const auto& here = map_.at({x, y});
            if (here.sea || here.river) continue;
            if (here.climate != Climate::TemperateForest && here.climate != Climate::Steppe) continue;
            const int metres = int(here.elevation) * generation::kMetresPerElevationStep;
            if (metres < 20 || metres > 700) continue;
            int score = int(here.fertility) - std::abs(int(here.moisture) - 150) * 2 -
                        (here.climate == Climate::Steppe ? 120 : 0);
            for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
                const auto& near = map_.at({x + dx, y + dy});
                score += near.sea ? -1000 : 0;
                score -= std::abs(int(near.elevation) - int(here.elevation)) * 20;
            }
            if (score > best && !query.underWater(centre({x, y}))) { best = score; chosen = {x, y}; }
        }
        if (best > -100000) {
            auto where = centre(chosen);
            float bestOpen = -1;
            for (int dy = -3; dy <= 3; ++dy) for (int dx = -3; dx <= 3; ++dx) {
                const core::WorldPos p{centre(chosen).x + core::Fixed::fromInt(dx * generation::kMetresPerCell / 8),
                                       centre(chosen).y + core::Fixed::fromInt(dy * generation::kMetresPerCell / 8)};
                if (query.underWater(p)) continue;
                // And dry round about: a meadow seen across, not a shore.
                bool wet = false;
                for (const auto [ox, oy] : {std::pair{60, 0}, {-60, 0}, {0, 60}, {0, -60}})
                    wet = wet || query.underWater({p.x + core::Fixed::fromInt(ox), p.y + core::Fixed::fromInt(oy)});
                if (wet) continue;
                const auto ground = query.groundAt(p);
                const float grass = float(ground.materials.of(Material::Grass).toDouble());
                const float wood = float(query.surfaceClimateAt(p).woodland.toDouble());
                const float open = grass * (1.0f - std::clamp(wood, 0.0f, 1.0f)) * float(ground.normal.z.toDouble());
                if (open > bestOpen) { bestOpen = open; where = p; }
            }
            landmarks_.push_back({"meadow", where});
        }
    }
}

} // namespace world
