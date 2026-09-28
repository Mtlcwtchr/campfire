#include "game/world/world_builder.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
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

WorldSnapshot::WorldSnapshot(generation::WorldMapData map, streaming::PageStore::Config config, std::uint64_t version)
    : version_(version), map_(validated(std::move(map))), hydrology_(streaming::buildHydrologyGraph(map_)),
      pages_(map_, hydrology_, streaming::hsimQuantisationFor(map_), std::move(config)) {
    auto query = field();
    climate_.raise(map_, query);
    findLandmarks();
    pages_.prebakeInBackground({2, 4});
}

void WorldBuilder::build(const generation::WorldMapParams& params) {
    const auto ticket = request();
    complete(ticket, generation::generateWorldMap(params));
}

void WorldBuilder::publish(generation::WorldMapData map) {
    complete(request(), std::move(map));
}

bool WorldBuilder::complete(Ticket ticket, generation::WorldMapData map) {
    if (!published_.current(ticket)) return false;
    // Failed construction leaves the current publication intact.
    auto next = std::make_shared<WorldSnapshot>(std::move(map), config_, ticket.version());
    return published_.publish(ticket, std::move(next));
}

decor::Scatter WorldSnapshot::scatter(int x, int y, int radiusMetres) const {
    if (radiusMetres<=0 || radiusMetres%decor::kCell || radiusMetres>decor::kRadius)
        throw std::invalid_argument("invalid scene scatter domain");
    const std::int64_t cx=std::int64_t(x)*decor::kRegion,cy=std::int64_t(y)*decor::kRegion;
    return scatter({cx-radiusMetres,cy-radiusMetres,cx+radiusMetres,cy+radiusMetres});
}

decor::Scatter WorldSnapshot::scatter(decor::ScatterBounds bounds) const {
    return scatter(bounds, *ecology_.read());
}

ecology::Cell WorldSnapshot::ecologyAt(double x, double y, HeightField& query, const ecology::Delta& delta) const {
    const auto key = ecology::key(x, y);
    if (const auto found = delta.cells.find(key); found != delta.cells.end()) return found->second;
    const double cx = (double(key.first) + 0.5) * ecology::kCellMetres;
    const double cy = (double(key.second) + 0.5) * ecology::kCellMetres;
    const core::WorldPos p{core::Fixed::fromDoubleForContent(cx), core::Fixed::fromDoubleForContent(cy)};
    const auto climate = climate_.at(p);
    const auto scalar = [](core::Fixed f) { return float(f.toDouble()); };
    const auto physicalField = [&](const std::vector<std::int32_t>& values, float fallback) {
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
    std::map<ecology::Key, ecology::Cell> cells;
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
            return site;
        });
    result.revision = delta.region(double(bounds.minX), double(bounds.minY));
    std::erase_if(result.objects, [&](const auto& object) {
        if (!delta.removed.contains(object.id)) return false;
        --result.populations[object.model];
        return true;
    });
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
    auto query = field();
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
            if (score > best) { best = score; chosen = {x, y}; }
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
}

} // namespace world
