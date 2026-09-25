#include "game/client/explorer.hpp"
#include "game/client/world_inspection.hpp"

#include <cstdlib>
#include <cstdio>
#include "../../../assets/shaders/sand_motion.hlsli"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <utility>

#include "game/client/renderer.hpp"
#include "game/client/controls.hpp"
#include "engine/ui/ui.hpp"
#include "game/world/terrain_lod.hpp"

#include <SDL3_image/SDL_image.h>
#include <iostream>

namespace client {
namespace {

using core::Fixed;
using core::WorldPos;

world::streaming::PageStore::Config terrainPageConfig() {
    world::streaming::PageStore::Config config;
    config.residentBytes = 192u << 20;
    if (std::getenv("ASR_TERRAIN_NO_DISK_CACHE")) return config;
    if (const auto* root = std::getenv("ASR_TERRAIN_CACHE_DIR"); root && *root) {
        config.diskCacheRoot = root;
    } else {
        // The same project cache when launched from the root or a build folder.
        std::filesystem::path content = "content";
        std::error_code ec;
        for (int up = 0; up < 5 && !std::filesystem::exists(content, ec); ++up) content = ".." / content;
        config.diskCacheRoot = (std::filesystem::exists(content, ec) ? content.parent_path() :
            std::filesystem::path{}) / ".cache" / "terrain";
    }
    return config;
}

// Legacy CPU meshes interpolate materials at vertices, unlike page rendering
// which shades the selected data field per pixel. Keep their established LOD
// and morph scale: the page renderer's human-scale mesh policy is not a safe
// replacement for this path's data/shading density.
constexpr world::terrain::LodPolicy kTerrainLodPolicy{1.75, 2.5, 1.0};
constexpr double kWantedSamplePixels = kTerrainLodPolicy.targetTrianglePixels;

// How much ground one turn of a material's texture covers, at the two sizes it
// is laid at. Fixed in metres, because that is what gives the ground scale: at
// five and a half metres to the turn a plate of dried mud comes out about a
// hand's breadth, which is what it is, and a card that stretched with the zoom
// would be wallpaper. The closer size is there for when the camera is down
// among people, where one turn of the other would cover half the view.
//
// There is deliberately no third, broad card. A photograph laid at a hundred
// metres to the turn is not a material any more - it is a field of mud plates
// twenty metres wide printed over the country - and reduced until it is not, it
// is a blur with its own grid still in it. What the wide view needs from the
// ground is slow, structureless variation, and that is what the drift below is:
// noise rather than a card, so it has no grid to find.
constexpr double kFineMetres = 8.25;
constexpr double kCloseMetres = 2.35;

// How fast the ground falls, out of the normal that was already worked out for
// it: the flat part of the normal over its upright part.
Fixed slopeOf(const world::Normal& n) {
    if (n.z.raw <= 0) return Fixed::fromInt(8);
    return core::hypot(n.x, n.y) / n.z;
}

SDL_FColor rgb(int r, int g, int b, float a = 1.0f) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
}

// What each material looks like where nothing else is mixed with it. The colour
// carries the large reading - what kind of country this is - and the textures
// over it carry what the ground is made of.
SDL_FColor materialColour(world::Material m) {
    switch (m) {
        case world::Material::Grass: return rgb(104, 132, 62);
        case world::Material::Dirt:  return rgb(124, 100, 70);
        case world::Material::Sand:  return rgb(206, 188, 140);
        case world::Material::Rock:  return rgb(136, 132, 126);
        case world::Material::Marsh: return rgb(88, 106, 78);
        case world::Material::Snow:  return rgb(238, 240, 244);
        case world::Material::Count: break;
    }
    return rgb(128, 128, 128);
}

const char* materialTexture(world::Material m) {
    switch (m) {
        case world::Material::Grass: return "ground/grass";
        case world::Material::Dirt:  return "ground/dirt";
        case world::Material::Sand:  return "ground/sand";
        case world::Material::Rock:  return "ground/rock";
        case world::Material::Marsh: return "ground/marsh";
        case world::Material::Snow:  return "ground/snow";
        case world::Material::Count: break;
    }
    return nullptr;
}

const char* materialAccentTexture(world::Material m) {
    switch (m) {
        case world::Material::Grass: return "ground/grass_wild";
        case world::Material::Rock:  return "ground/rock_smooth";
        // Wet earth remains visible below it; wild growth breaks up broad bogs.
        case world::Material::Marsh: return "ground/grass_wild";
        default:                     return nullptr;
    }
}

// The light. One sun, from the north-west and fairly high, plus enough ambient
// that a north-facing slope is dark rather than black.
float lightOn(const world::Normal& n) {
    const double x = n.x.toDouble(), y = n.y.toDouble(), z = n.z.toDouble();
    // Mostly directional. An ambient-heavy light flatters a heightfield into a
    // wash of colour: the shape of the ground is carried by how much each slope
    // faces the sun, and if that is a fifth of the brightness there is nothing
    // to see but the material.
    const double facing = -0.55 * x - 0.55 * y + 0.63 * z;
    const double lit = 0.45 + 0.72 * std::max(0.0, facing);
    return static_cast<float>(std::clamp(lit, 0.38, 1.35));
}

// What the country looks like from far enough away, straight off the coarse
// map. The last thing between the view and a black screen: it needs no patch,
// no field query and no thread - the coarse map is four million cells that have
// been in memory since the world was raised.
SDL_FColor countryColour(const generation::WorldMapData& world, double worldX, double worldY) {
    const std::int32_t cx = static_cast<std::int32_t>(
            std::clamp<double>(worldX / generation::kMetresPerCell, 0, world.width - 1));
    const std::int32_t cy = static_cast<std::int32_t>(
            std::clamp<double>(worldY / generation::kMetresPerCell, 0, world.height - 1));
    const generation::WorldCell& cell = world.at({cx, cy});
    if (cell.sea) return {0.14f, 0.30f, 0.46f, 1.0f};
    const double metres = static_cast<double>(cell.elevation) *
                          generation::kMetresPerElevationStep;
    const double dry = std::clamp((160.0 - cell.moisture) / 160.0, 0.0, 1.0);
    const double high = std::clamp((metres - 700) / 900.0, 0.0, 1.0);
    // Green where it rains, sand where it does not, and washed towards snow at
    // height. The same reading the world map gives, which is the point: the
    // stand-in should look like the country it is standing in for.
    const double r = (0.38 + 0.34 * dry) * (1 - high) + 0.86 * high;
    const double g = (0.46 - 0.06 * dry) * (1 - high) + 0.88 * high;
    const double b = (0.24 + 0.20 * dry) * (1 - high) + 0.90 * high;
    return {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b), 1.0f};
}


std::string metres(double v) {
    char buffer[64];
    if (std::abs(v) >= 10000) std::snprintf(buffer, sizeof(buffer), "%.1f km", v / 1000);
    else std::snprintf(buffer, sizeof(buffer), "%.0f m", v);
    return buffer;
}

// Slow variation over the ground, so that a hillside of one material is not one
// colour. Four scales, from the size of a district down to the size of a room.
// The broadest is what stops a pulled-back view of grassland being a single flat
// green; the finest is most of what the ground has to show at the distances
// where the material cards have faded out. Floating point on purpose - this is
// the renderer, and nothing here decides anything.
//
// `sampleMetres` is how far apart the vertices this is being asked about are.
// A wave shorter than a few times that is not drawn, it is aliased: the ground
// is only coloured where there is a vertex, so a forty-seven metre figure read
// off vertices thirty-two metres apart comes out as a field of blotches the
// size of the samples, which is what the coarse levels used to look like. Each
// scale is faded out before it gets there and the rest are weighted up to make
// good the difference, so the ground keeps the same range at every level.
double macroVariation(double x, double y, double sampleMetres) {
    const auto wave = [](double x, double y, double scale, std::uint64_t seed) {
        const double u = x / scale, v = y / scale;
        const double i = std::floor(u), j = std::floor(v);
        const double fu = u - i, fv = v - j;
        const auto value = [seed](double a, double b) {
            std::uint64_t h = seed ^ (static_cast<std::uint64_t>(static_cast<std::int64_t>(a)) * 0x9e3779b97f4a7c15ULL);
            h ^= static_cast<std::uint64_t>(static_cast<std::int64_t>(b)) * 0xc2b2ae3d27d4eb4fULL;
            h ^= h >> 29;
            h *= 0xbf58476d1ce4e5b9ULL;
            h ^= h >> 32;
            return static_cast<double>(h & 0xffff) / 65535.0;
        };
        const auto ease = [](double t) { return t * t * (3 - 2 * t); };
        const double a = value(i, j), b = value(i + 1, j), c = value(i, j + 1), d = value(i + 1, j + 1);
        const double top = a + (b - a) * ease(fu);
        const double bottom = c + (d - c) * ease(fu);
        return top + (bottom - top) * ease(fv);
    };
    struct Scale { double metres; std::uint64_t seed; double weight; };
    static constexpr std::array<Scale, 4> scales{{
            {1500, 0x2c19, 0.36}, {260, 0x51ed, 0.30}, {47, 0x77a1, 0.20}, {13, 0x9b03, 0.14},
    }};
    double total = 0, weight = 0;
    for (const Scale& s : scales) {
        const double room = std::clamp(s.metres / (5.0 * sampleMetres) - 1.0, 0.0, 1.0);
        if (room <= 0) continue;
        total += room * s.weight * wave(x, y, s.metres, s.seed);
        weight += room * s.weight;
    }
    return weight > 0 ? total / weight : 0.5;
}

std::uint32_t foliageNoise(std::int64_t x, std::int64_t y, int sample) {
    std::uint64_t v = static_cast<std::uint64_t>(x) * 0x9e3779b97f4a7c15ULL;
    v ^= static_cast<std::uint64_t>(y) * 0xc2b2ae3d27d4eb4fULL;
    v ^= static_cast<std::uint64_t>(sample + 1) * 0x94d049bb133111ebULL;
    v ^= v >> 30;
    v *= 0xbf58476d1ce4e5b9ULL;
    v ^= v >> 27;
    return static_cast<std::uint32_t>(v ^ (v >> 31));
}

} // namespace

PatchWorkshop::PatchWorkshop(const generation::WorldMapData& world, std::uint64_t seed)
    : world_(world),
      seed_(seed),
      // Extracted once from the finished macro map, before a worker starts.
      // Every reach, every water body and the page index over them; a worker
      // that had to build its own would build the same thing ten times.
      hydrology_(world::streaming::buildHydrologyGraph(world)),
      quantisation_(world::streaming::hsimQuantisationFor(world)),
      // Resident pages, shared by every worker. A page near the camera is
      // asked for by several rings and again on the next epoch, and a coarse
      // page is a few hundred samples - so this is worth far more than it
      // costs, and it is bounded because a camera walks.
      pages_(world, hydrology_, quantisation_, terrainPageConfig()),
      pool_(pages_.workerPool()) {
    // Meshes, inspection, permanent preparation and GPU page packing share
    // half the hardware threads, rather than starting independent crews.
    const std::size_t count = pool_.size();
    fields_.reserve(count);
    for (std::size_t i = 0; i < count; ++i) fields_.emplace_back(&world_, seed_);
    samples_.resize(count);

    // The coarse world, baked once before anything is drawn.
    //
    // Sixteen and sixty-four metres to the sample over land, loaded from the
    // disk cache or baked on a miss, then kept for the life of the world.
    // H8 and H4 refine on demand; thirty-two is geometry over H16.
    // The 64 m land mask keeps pure ocean
    // out of this pinned set.
    // and it means a wide view never computes anything: it reads. Before this,
    // a view at a hundred and twenty-eight metres to the sample had to bake
    // pages nobody had ever asked for, and it took seconds every time.
    //
    // The near levels are not baked here. They are four hundred kilobytes a
    // page and the camera only ever stands on a handful of them.
    // Started here without blocking the frame. All shared workers first finish
    // H64/H16; runtime detail and inspection stay queued until publication.
    // The climate first, because every mesh reads it and it is a fifth of a
    // second for the whole world.
    climate_.raise(world_, fields_.front());
    pages_.prebakeInBackground({2, 4});

    using Task = world::streaming::TerrainWorkerPool::Task;
    source_ = pool_.add([this](std::size_t i) { return work(i, false); }, Task::Visible);
    try {
        inspectionSource_ = pool_.add([this](std::size_t i) { return work(i, true); }, Task::Inspection);
    } catch (...) {
        pool_.remove(source_);
        throw;
    }
}

PatchWorkshop::~PatchWorkshop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closing_ = true;
    }
    wake_.notify_all();
    pool_.remove(source_);
    pool_.remove(inspectionSource_);
}

void PatchWorkshop::notifyWorkers() {
    wake_.notify_all(); // interrupt artificial delays / cancelled work
    pool_.notify();
}

void PatchWorkshop::wants(std::vector<Order> orders) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // What the view wants, by name. A tile is named by the ground it
        // covers, so this is a statement about the world rather than about a
        // snapshot: a worker is stopped when the ground it is cutting has left
        // the view, and it is not stopped merely because the camera moved.
        //
        // Epochs did that job before, and did it badly: every order carried
        // the snapshot it belonged to, moving made a new snapshot, and so
        // everything in flight was thrown away whether or not the ground was
        // still on screen.
        live_.clear();
        for (const Order& o : orders) live_.insert(world::tileKeyOf(o.tile));
        done_.erase(std::remove_if(done_.begin(), done_.end(), [&](const Finished& f) {
            return !live_.contains(world::tileKeyOf(f.tile));
        }), done_.end());
        std::set<std::int64_t> requested;
        orders.erase(std::remove_if(orders.begin(), orders.end(), [&](const Order& o) {
            const auto key = world::tileKeyOf(o.tile);
            return !requested.insert(key).second || inFlight_.count(key) ||
                std::any_of(done_.begin(), done_.end(), [&](const Finished& f) {
                    return world::tileKeyOf(f.tile) == key;
                });
        }), orders.end());
        queue_ = std::move(orders);
        // Nearest the middle of the view last, because that is the end a worker
        // takes from: what the eye is on is built first.
        std::sort(queue_.begin(), queue_.end(),
                  [](const Order& a, const Order& b) { return a.urgency > b.urgency; });
    }
    notifyWorkers();
}

std::vector<PatchWorkshop::Finished> PatchWorkshop::collect() {
    std::vector<Finished> out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.swap(done_);
    }
    // Room again: whoever was holding off can carry on.
    notifyWorkers();
    return out;
}

std::size_t PatchWorkshop::waiting() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + busy_;
}

PatchWorkshop::WorkerStats PatchWorkshop::workerStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {pool_.size(), busy_, peakBusy_, queue_.size(), done_.size()};
}

bool PatchWorkshop::containsLand(world::TileId tile) const {
    const auto side = world::tileMetresAt(tile.lod);
    const auto minX = static_cast<std::int64_t>(tile.x) * side;
    const auto minY = static_cast<std::int64_t>(tile.y) * side;
    return pages_.landMask().anyLandInWorldRect(static_cast<std::int32_t>(minX),
                                                static_cast<std::int32_t>(minY),
                                                static_cast<std::int32_t>(minX + side),
                                                static_cast<std::int32_t>(minY + side));
}

void PatchWorkshop::inspectionWants(std::shared_ptr<const world::InspectionSnapshot> state,
                                    std::vector<world::InspectionOrder> orders) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inspectionState_ = std::move(state);
        inspectionWanted_.clear();
        if (!inspectionState_) orders.clear();
        for (const auto& order : orders) inspectionWanted_.insert(order.key);
        std::erase_if(inspectionDone_, [&](const auto& result) {
            return !inspectionState_ || result.revision != inspectionState_->revision ||
                   !inspectionWanted_.contains(result.key);
        });
        std::set<std::uint64_t> unique;
        std::erase_if(orders, [&](const auto& order) {
            return !unique.insert(order.key).second ||
                inspectionInFlight_.contains({inspectionState_->revision, order.key}) ||
                std::any_of(inspectionDone_.begin(), inspectionDone_.end(), [&](const auto& result) {
                    return result.key == order.key;
                });
        });
        std::sort(orders.begin(), orders.end(), [](const auto& a, const auto& b) {
            return a.urgency > b.urgency;
        });
        inspectionQueue_ = std::move(orders);
    }
    notifyWorkers();
}

std::vector<world::InspectionResult> PatchWorkshop::collectInspection(std::size_t limit) {
    std::vector<world::InspectionResult> results;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!inspectionDone_.empty() && results.size() < limit) {
            results.push_back(std::move(inspectionDone_.back()));
            inspectionDone_.pop_back();
        }
    }
    notifyWorkers();
    return results;
}

PatchWorkshop::WorkerStats PatchWorkshop::inspectionStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {pool_.size(), inspectionBusy_, inspectionPeak_, inspectionQueue_.size(), inspectionDone_.size()};
}

bool PatchWorkshop::work(std::size_t index, bool inspectionOnly) {
    world::HeightField& field = fields_[index];
    world::TileSampleCache& samples = samples_[index];
    {
        Order order;
        world::InspectionOrder inspectionOrder;
        std::shared_ptr<const world::InspectionSnapshot> inspection;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            // Nothing to do, or nowhere to put it.
            //
            // The second is back-pressure, and it is not optional. A frame takes
            // a bounded number of finished patches into the cache, because
            // folding one in walks every vertex of it; the workers have no such
            // bound and, now that a coarse patch is a third of what it was, they
            // finish faster than any frame can swallow. Left to run they hold
            // thousands of finished meshes - tens of kilobytes each - and the
            // frame that finally collects them stalls for a second on the
            // allocation alone. Measured: 1007 ms on one frame of the flight.
            // A source gets one task per turn. Backpressure must release the
            // worker to page streaming/preparation, not park the shared thread.
            if (closing_ || frozen_ ||
                (inspectionOnly ? (inspectionQueue_.empty() || inspectionDone_.size() + inspectionBusy_ >= kMostWaiting)
                                : (queue_.empty() || done_.size() + busy_ >= kMostWaiting)))
                return false;
            if (inspectionOnly) {
                inspection = inspectionState_;
                inspectionOrder = std::move(inspectionQueue_.back());
                inspectionQueue_.pop_back();
                inspectionInFlight_.insert({inspection->revision, inspectionOrder.key});
                ++inspectionBusy_;
                inspectionPeak_ = std::max(inspectionPeak_, inspectionBusy_);
                wake_.wait_for(lock, std::chrono::milliseconds(artificialDelayMs_.load()), [&] {
                    return closing_ || !inspectionState_ || inspectionState_->revision != inspection->revision ||
                           !inspectionWanted_.contains(inspectionOrder.key);
                });
            } else {
                order = queue_.back();
                queue_.pop_back();
                ++busy_;
                peakBusy_ = std::max(peakBusy_, busy_);
                inFlight_.insert(world::tileKeyOf(order.tile));
                wake_.wait_for(lock, std::chrono::milliseconds(artificialDelayMs_.load()), [this, &order] {
                    return closing_ || !live_.contains(world::tileKeyOf(order.tile));
                });
                if (closing_ || !live_.contains(world::tileKeyOf(order.tile))) {
                    inFlight_.erase(world::tileKeyOf(order.tile));
                    --busy_;
                    return true;
                }
            }
        }

        if (inspection) {
            const auto cancelled = [&] {
                std::lock_guard<std::mutex> lock(mutex_);
                return closing_ || !inspectionState_ || inspectionState_->revision != inspection->revision ||
                       !inspectionWanted_.contains(inspectionOrder.key);
            };
            world::InspectionResult result;
            result.key = inspectionOrder.key;
            result.revision = inspection->revision;
            if (!cancelled()) {
                if (result.key == 0 && inspection->floodRequest) {
                    auto flood = std::make_shared<world::FloodPreview>();
                    const auto& request = *inspection->floodRequest;
                    if (flood->build(field, request.centre, request.span, cancelled)) result.flood = std::move(flood);
                } else if (inspectionOrder.positions) {
                    result.samples = world::sampleInspection(field, *inspectionOrder.positions, *inspection, cancelled);
                }
            }
            const bool complete = result.key == 0 ? result.flood != nullptr :
                inspectionOrder.positions && result.samples.size() == inspectionOrder.positions->size();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (complete && !closing_ && inspectionState_ && inspectionState_->revision == result.revision &&
                    inspectionWanted_.contains(result.key)) inspectionDone_.push_back(std::move(result));
                inspectionInFlight_.erase({inspection->revision, inspectionOrder.key});
                --inspectionBusy_;
            }
            return true;
        }

        Finished finished;
        finished.tile = order.tile;
        const auto cancelled = [this, &order] {
            std::lock_guard<std::mutex> lock(mutex_);
            return closing_ || !live_.contains(world::tileKeyOf(order.tile));
        };
        // The ground comes off the pages the navmesh and the simulation read,
        // at the level this ring's distance earns. The field is still asked
        // for what a page does not hold - climate, foliage, materials, the
        // wind that shapes a dune - none of which the water decides.
        finished.mesh =
                world::buildTileMesh(field, pages_, climate_, order.tile, cancelled, &samples);
        auto positions = std::make_shared<std::vector<core::WorldPos>>();
        positions->reserve(finished.mesh.vertices.size());
        finished.waterLevel.resize(finished.mesh.vertices.size());
        finished.waterCover.resize(finished.mesh.vertices.size());
        finished.waterMotion.resize(finished.mesh.vertices.size());
        // A render-only apron: terrain triangles already extend onto dry land,
        // but the water needs coverage there before a shader can fade the surf.
        // Same world-space query on both sides of every ring; no local dilation
        // that could miss a wet neighbour stored in a different patch.
        const Fixed footprint = Fixed::fromInt(
                std::max<std::int64_t>(12, world::sampleMetresAt(order.tile.lod) * 2));
        world::streaming::PageLattice pageWater(
                pages_, static_cast<std::uint8_t>(std::clamp(order.tile.lod, 0, 7)));
        finished.mostOf.fill(0.0f);
        for (std::size_t i = 0; i < finished.mesh.vertices.size(); ++i) {
            if (i % 128 == 0 && cancelled()) break;
            world::TerrainVertex& v = finished.mesh.vertices[i];
            positions->push_back(v.position);
            // From the same page as the ground under it. Asked of the field
            // beside a page-read ground these were two answers, and the ways
            // they disagree are a bed showing through its own river and a
            // surface hanging over land that moved beneath it.
            const world::streaming::PageLattice::Water water =
                    pageWater.waterAt(v.position, footprint);
            finished.waterLevel[i] = water.level;
            finished.waterCover[i] = water.cover;
            // A reach is not a body, which is what tells the three apart: the
            // ocean and a lake are named, and water that is wet and nameless
            // is a river.
            const bool ocean = water.body == world::streaming::kOceanWaterBodyId;
            const bool lake = water.body != world::streaming::kInvalidWaterBodyId && !ocean;
            finished.waterMotion[i] = {static_cast<float>(water.flowX.toDouble()),
                                       static_cast<float>(water.flowY.toDouble()),
                                       water.any && !ocean && !lake ? 1.0f : 0.0f,
                                       lake ? 1.0f : 0.0f};
            // An apron-only patch must draw too. The shader limits run-up by
            // signed depth, so this does not flood high ground near a channel.
            if (water.cover.raw > 0) finished.anyWater = true;
            for (std::size_t m = 0; m < world::kMaterialCount; ++m)
                finished.mostOf[m] = std::max(finished.mostOf[m],
                                              static_cast<float>(v.materials.weight[m].toDouble()));
        }

        finished.inspectionPositions = std::move(positions);
        if (!cancelled() && finished.mostOf[static_cast<std::size_t>(world::Material::Grass)] >= 0.12f)
            finished.foliage = world::buildRingFoliage(finished.mesh, finished.waterLevel, cancelled);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!closing_ && !finished.mesh.vertices.empty() &&
                live_.contains(world::tileKeyOf(order.tile)))
                done_.push_back(std::move(finished));
            inFlight_.erase(world::tileKeyOf(order.tile));
            --busy_;
        }
        return true;
    }
}

void PatchWorkshop::debugArtificialDelay(std::chrono::milliseconds delay) {
    artificialDelayMs_ = std::max<std::int64_t>(0, delay.count());
    notifyWorkers();
}
void PatchWorkshop::debugFreeze(bool frozen) { frozen_ = frozen; notifyWorkers(); }

// Declared since the debug controls went in and never defined, so nothing
// could use it. A test that wants a half-built view needs exactly this: stop
// the workers where they are, and what is on screen stays what it is.
void Explorer::setStreamingFrozen(bool frozen) { workshop_->debugFreeze(frozen); }
std::chrono::milliseconds PatchWorkshop::debugArtificialDelay() const {
    return std::chrono::milliseconds(artificialDelayMs_.load());
}
bool PatchWorkshop::debugFrozen() const { return frozen_.load(); }

// The coarsest level worth having on a world this size: the one at which the
// world is about thirteen chunks across. See coarsest().
// The coarsest the ground is ever drawn.
//
// H64 is the coarsest data the world holds. Geometry may be wider: at the widest
// zoom a 64 m triangle is substantially smaller than a pixel, so 128/256 m grids
// filter the same H64 field instead of inventing H128/H256 datasets. This keeps
// raster work proportional to screen pixels without throwing persistent data
// away.
//
// It used to be worked out from the size of the map - "about thirteen chunks
// across at the coarsest level" - which put a large world at six or seven,
// that is two hundred and fifty-six or five hundred and twelve metres. That
// rule was about how many patches a widest view costs, and the ring planner
// answers that question now: it budgets by vertices, so a view is about the
// same number of them at every level and a sharper one is not a dearer one.
std::int32_t coarsestFor(const generation::WorldMapData& world) {
    (void)world;
    return 6;   // 4 m << 6 == 256 m geometry, still sampling H64
}

Explorer::Explorer(const generation::WorldMapData& world, std::uint64_t seed)
    : field_(&world, seed), world_(&world),
      workshop_(std::make_unique<PatchWorkshop>(world, seed)) {
    coarsest_ = coarsestFor(world);
    worldHeightBounds_ = world::ringHeightBounds(world, {},
            {Fixed::fromInt(std::int64_t(world.width) * generation::kMetresPerCell),
             Fixed::fromInt(std::int64_t(world.height) * generation::kMetresPerCell)}, 0);
    raiseFoundation(world);
    findLandmarks();
}

void Explorer::restart(const generation::WorldMapData& world, std::uint64_t seed) {
    ++worldRevision_;
    gpuPrepared_ = false;
    gpuPending_ = 0;
    // The threads first, and by destroying them: a worker holds its own view of
    // the country it is cutting, and waiting for it to put that down is exactly
    // what joining is.
    workshop_.reset();
    field_ = world::HeightField(&world, seed);
    world_ = &world;
    coarsest_ = coarsestFor(world);
    worldHeightBounds_ = world::ringHeightBounds(world, {},
            {Fixed::fromInt(std::int64_t(world.width) * generation::kMetresPerCell),
             Fixed::fromInt(std::int64_t(world.height) * generation::kMetresPerCell)}, 0);
    raiseFoundation(world);
    patches_.clear();
    arriving_.clear();
    visible_.clear();
    unfilled_.clear();
    wanted_ = {};
    previous_ = {};
    backdrop_ = {};
    preparedFrame_ = ~std::uint64_t(0);
    haveCameraSample_ = false;
    focusQuietSeconds_ = 0;
    haveFootprint_ = false;
    landmarks_.clear();
    frame_ = 0;
    builtThisFrame_ = 0;
    missing_ = 0;
    dropped_ = 0;
    workshop_ = std::make_unique<PatchWorkshop>(world, seed);
    workshop_->cutCliffs(cliffs_);
    findLandmarks();
}

void Explorer::warmUp(core::WorldPos around) {
    // A ring plan needs the actual zoom and viewport. Do not synchronously
    // build square proxies before the first frame; prepareVisible starts it.
    (void)around;
}

std::int32_t Explorer::levelFor(double pixelsPerTile) const {
    if (pixelsPerTile <= 0) return coarsest_;
    const std::int32_t wantedStep = world::terrain::geometryStepFor(
            1.0 / pixelsPerTile, 0.0, kTerrainLodPolicy);
    std::int32_t lod = 0;
    while (lod < coarsest_ && world::sampleMetresAt(lod) < wantedStep) ++lod;
    return lod;
}


double Explorer::levelExactFor(double pixelsPerTile) {
    if (pixelsPerTile <= 0) return 12;
    // sampleMetresAt(L) is kSampleMetres doubled L times, so the level at which
    // a sample is exactly the wanted size is a logarithm rather than a count.
    const double exact =
            std::log2(kWantedSamplePixels / (world::kSampleMetres * pixelsPerTile));
    return std::min(12.0, exact);
}

float Explorer::morphOf(std::int32_t lod) const {
    // A patch of level L is what the view asks for while the exact level is
    // between L-1 and L, so that is the span its morph runs across: nought where
    // the level was entered, one where the next one takes over. The same formula
    // answers the two other cases without a special case for either - a finer
    // patch standing in comes out fully morphed, which is exactly what makes it
    // match the level around it, and a coarser stand-in comes out at nought,
    // which is what it was built to look like.
    return static_cast<float>(
            std::clamp(lastLevelExact_ - static_cast<double>(lod - 1), 0.0, 1.0));
}

core::WorldPos Explorer::startingPoint() const {
    // The played site, unless it is in the water - a world opening on an empty
    // sea says nothing about the terrain, which is the one thing this mode is
    // for.
    for (const generation::WorldSite& site : world_->sites) {
        if (!site.played) continue;
        const WorldPos where{
                Fixed::fromInt(static_cast<std::int64_t>(site.cell.x) * generation::kMetresPerCell +
                               generation::kMetresPerCell / 2),
                Fixed::fromInt(static_cast<std::int64_t>(site.cell.y) * generation::kMetresPerCell +
                               generation::kMetresPerCell / 2)};
        if (!field_.underWater(where)) return where;
    }
    for (const Landmark& landmark : landmarks_)
        if (!field_.underWater(landmark.where)) return landmark.where;
    for (const generation::WorldSite& site : world_->sites)
        if (site.played)
            return {Fixed::fromInt(static_cast<std::int64_t>(site.cell.x) *
                                           generation::kMetresPerCell +
                                   generation::kMetresPerCell / 2),
                    Fixed::fromInt(static_cast<std::int64_t>(site.cell.y) *
                                           generation::kMetresPerCell +
                                   generation::kMetresPerCell / 2)};
    if (!landmarks_.empty()) return landmarks_.front().where;
    return {Fixed::fromInt(static_cast<std::int64_t>(world_->width) *
                           generation::kMetresPerCell / 2),
            Fixed::fromInt(static_cast<std::int64_t>(world_->height) *
                           generation::kMetresPerCell / 2)};
}

void Explorer::findLandmarks() {
    // Read off the coarse map: it knows where the country keeps its mountains,
    // its rivers and its coasts, and finding them by sampling the field would be
    // a million queries.
    core::TilePos steepest{0, 0}, biggestRiver{0, 0}, coast{0, 0}, flattest{0, 0};
    std::int32_t mostRelief = -1, mostWater = -1, leastRelief = 1 << 20;
    for (std::int32_t y = 2; y < world_->height - 2; ++y) {
        for (std::int32_t x = 2; x < world_->width - 2; ++x) {
            std::int32_t low = 999, high = -999;
            bool anySea = false, anyLand = false;
            for (std::int32_t dy = -1; dy <= 1; ++dy)
                for (std::int32_t dx = -1; dx <= 1; ++dx) {
                    const generation::WorldCell& c = world_->at({x + dx, y + dy});
                    if (c.sea) anySea = true; else anyLand = true;
                    low = std::min<std::int32_t>(low, c.elevation);
                    high = std::max<std::int32_t>(high, c.elevation);
                }
            const generation::WorldCell& here = world_->at({x, y});
            if (!anySea && high - low > mostRelief) { mostRelief = high - low; steepest = {x, y}; }
            if (anySea && anyLand) coast = {x, y};
            if (!here.sea && here.river && here.riverSize > mostWater) {
                mostWater = here.riverSize;
                biggestRiver = {x, y};
            }
            if (!anySea && here.elevation > 2 && high - low < leastRelief) {
                leastRelief = high - low;
                flattest = {x, y};
            }
        }
    }
    const auto centre = [](core::TilePos cell) {
        return WorldPos{Fixed::fromInt(static_cast<std::int64_t>(cell.x) *
                                               generation::kMetresPerCell +
                                       generation::kMetresPerCell / 2),
                        Fixed::fromInt(static_cast<std::int64_t>(cell.y) *
                                               generation::kMetresPerCell +
                                       generation::kMetresPerCell / 2)};
    };
    landmarks_.push_back({"mountains", centre(steepest)});
    landmarks_.push_back({"the big river", centre(biggestRiver)});
    landmarks_.push_back({"the coast", centre(coast)});
    landmarks_.push_back({"open country", centre(flattest)});
    // Where the map stops. Worth a place of its own: the world is finite now,
    // and what the ground does when it runs out of cells to be generated from is
    // a thing to go and look at rather than to find out from a player.
    core::TilePos edge{world_->width - 1, world_->height / 2};
    std::int32_t nearest = world_->width;
    for (std::int32_t y = 0; y < world_->height; ++y)
        for (std::int32_t x = 0; x < world_->width; ++x) {
            if (world_->at({x, y}).sea) continue;
            const std::int32_t out = std::min(std::min(x, world_->width - 1 - x),
                                              std::min(y, world_->height - 1 - y));
            if (out < nearest) { nearest = out; edge = {x, y}; }
        }
    landmarks_.push_back({"the border", centre(edge)});
    // Prefer interiors of a community over a single isolated climate cell.
    // Existing landmark indices stay unchanged; absent biomes add no fake stop.
    using generation::Climate;
    const std::pair<Climate, const char*> communities[]{
            {Climate::Steppe, "steppe"}, {Climate::Taiga, "taiga"},
            {Climate::TemperateForest, "temperate forest"}, {Climate::TropicalForest, "tropics"},
            {Climate::Desert, "desert"}};
    for (const auto& [climate, name] : communities) {
        int best = -100000;
        core::TilePos chosen{};
        for (int y = 2; y < world_->height - 2; ++y)
            for (int x = 2; x < world_->width - 2; ++x) {
                const auto& here = world_->at({x, y});
                if (here.sea || here.climate != climate) continue;
                int score = here.fertility;
                if (climate == Climate::Desert)
                    score = -int(here.moisture) * 12 - int(here.elevation) * 12 - (here.river ? 2000 : 0);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const auto& near = world_->at({x + dx, y + dy});
                        score += !near.sea && near.climate == climate ? 1000 : -1000;
                        score -= std::abs(int(near.elevation) - int(here.elevation)) * 10;
                    }
                if (score > best) { best = score; chosen = {x, y}; }
            }
        if (best <= -100000) continue;
        WorldPos where = centre(chosen);
        if (climate == Climate::Desert) {
            // A climate cell is not a material. Check the actual surface so 1
            // and --at desert don't land on a rock outcrop or in an oasis.
            float bestSupport = -1;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const WorldPos p{centre(chosen).x + Fixed::fromInt(dx * generation::kMetresPerCell / 8),
                                     centre(chosen).y + Fixed::fromInt(dy * generation::kMetresPerCell / 8)};
                    const auto ground = field_.groundAt(p);
                    const auto material = [&](world::Material m) {
                        return static_cast<float>(ground.materials.of(m).toDouble());
                    };
                    const float support = world::sand::sandDriftSupport(
                            static_cast<float>(field_.surfaceClimateAt(p).desert.toDouble()),
                            material(world::Material::Sand), material(world::Material::Rock),
                            material(world::Material::Grass), material(world::Material::Snow),
                            material(world::Material::Marsh),
                            static_cast<float>((field_.waterLevelAt(p) - ground.height).toDouble()),
                            static_cast<float>(ground.normal.z.toDouble()));
                    if (support > bestSupport) { bestSupport = support; where = p; }
                }
        }
        landmarks_.push_back({name, where});
    }
    int deepest=0;
    core::TilePos basin{};
    for (std::size_t i=0;i<std::min(world_->cells.size(),world_->lakeDepthField.size());++i) {
        if (!world_->cells[i].sea && world_->lakeDepthField[i]>deepest) {
            deepest=world_->lakeDepthField[i];
            basin={static_cast<int>(i%world_->width),static_cast<int>(i/world_->width)};
        }
    }
    Fixed bestWater;
    WorldPos lake;
    if (deepest>0) for (int y=-2;y<=2;++y) for (int x=-2;x<=2;++x) {
        const WorldPos p{centre(basin).x+Fixed::fromInt(x*generation::kMetresPerCell/8),
                         centre(basin).y+Fixed::fromInt(y*generation::kMetresPerCell/8)};
        const auto w=field_.waterOver(p,Fixed::fromInt(12),core::kZero);
        const auto depth=w.level-field_.heightAt(p);
        if (w.kind==world::HeightField::WaterKind::Lake && depth>bestWater) {
            bestWater=depth; lake=p;
        }
    }
    if (bestWater>core::kZero) landmarks_.push_back({"the lake",lake});
}


// Which tiles the whole world is at the coarsest level.
//
// Not a view: it does not move, it does not shrink when the camera zooms in,
// and it is not asked again. It is the world, once.
void Explorer::raiseFoundation(const generation::WorldMapData& world) {
    const auto side = static_cast<double>(world::tileMetresAt(coarsest_));
    const double wide = static_cast<double>(std::int64_t(world.width) * generation::kMetresPerCell);
    const double high = static_cast<double>(std::int64_t(world.height) * generation::kMetresPerCell);
    foundation_ = {};
    foundation_.set = true;
    foundation_.lod = coarsest_;
    foundation_.firstX = foundation_.firstY = 0;
    foundation_.lastX = static_cast<std::int32_t>(std::floor((wide - 1) / side));
    foundation_.lastY = static_cast<std::int32_t>(std::floor((high - 1) / side));
}

void Explorer::forgetOldPatches() {
    dropped_ = 0;
    // Kept by when it was last drawn, not by which snapshot it belonged to.
    //
    // With epochs, everything not of the current or previous snapshot went -
    // which is to say, everything, every time the camera moved far enough.
    // A tile is named by its ground, so the question is whether the ground is
    // still near, and the answer for most of what was built a moment ago is
    // yes. A tail of a few seconds covers turning back to where you were.
    constexpr std::uint64_t kKeepFrames = 600;
    for (auto it = patches_.begin(); it != patches_.end();) {
        // The foundation is not a tenancy. Everything else here was built
        // because the eye came near and is forgotten when it leaves; the
        // coarsest level is the world itself, cut once, so that no place is
        // ever blank and every arrival refines something already drawn
        // rather than replacing nothing.
        if (it->second.mesh.lod >= coarsest_) { ++it; continue; }
        if (it->second.lastUsedFrame + kKeepFrames < frame_) {
            it = patches_.erase(it);
            ++dropped_;
        } else {
            ++it;
        }
    }
}

void Explorer::update(const Camera& camera, double seconds) {
    ++frame_;
    frameSeconds_ = std::clamp(seconds, 0.0, 0.1);
    const bool moved = !haveCameraSample_ || std::hypot(camera.centreX - lastCameraX_,
                                                       camera.centreY - lastCameraY_) > 1e-5;
    focusQuietSeconds_ = moved ? 0 : focusQuietSeconds_ + frameSeconds_;
    lastCameraX_ = camera.centreX;
    lastCameraY_ = camera.centreY;
    haveCameraSample_ = true;
    builtThisFrame_ = 0;
    buildMillis_ = 0;
    if (gpuPages_) return;

    // Whatever the workshop finished while the last frame was being drawn.
    const auto started = std::chrono::steady_clock::now();
    // Taken in at a limited rate. The workers can finish a hundred patches while
    // one frame is drawn, and folding a hundred into the cache - which walks
    // every vertex of each - is a stall of its own, from the other end.
    if (arriving_.empty()) arriving_ = workshop_->collect();
    // Folding is a handful of vector moves, so the budget here is generous:
    // what it guards against is a very large backlog arriving in one frame,
    // not the cost of any one patch.
    std::size_t taken = 0;
    while (!arriving_.empty() && taken < kUploadBytesPerFrame) {
        PatchWorkshop::Finished& finished = arriving_.back();
        taken += uploadBytesOf(finished.mesh);
        TerrainPatch& patch = patches_[world::tileKeyOf(finished.tile)];
        patch.mesh = std::move(finished.mesh);
        patch.inspectionPositions = std::move(finished.inspectionPositions);
        patch.cliffs = std::move(finished.cliffs);
        patch.waterLevel = std::move(finished.waterLevel);
        patch.waterCover = std::move(finished.waterCover);
        patch.waterMotion = std::move(finished.waterMotion);
        patch.foliage = std::move(finished.foliage);
        patch.anyWater = finished.anyWater;
        patch.mostOf = finished.mostOf;
        patch.lastUsedFrame = frame_;
        ++builtThisFrame_;
        arriving_.pop_back();
    }
    buildMillis_ =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                    .count();
    forgetOldPatches();
    (void)camera;
}

// Whether this ground is already had, whether or not it has been folded into
// the cache yet.
//
// The second half is the part that was missing. A finished patch waits in
// `arriving_` until a frame has room to take it in, and the order book is
// rebuilt every frame from what is *in the cache* - so everything waiting was
// asked for again, the workers built it again, and the book never emptied. It
// did not show while the intake could swallow everything the workers produced
// in a frame; the moment a patch got cheap enough that they produced more than
// that, the view stopped converging at all.
bool Explorer::held(world::TileId tile) const {
    const auto key = world::tileKeyOf(tile);
    if (patches_.count(key)) return true;
    // Including what is finished and waiting to be folded in. Without this the
    // order book was rebuilt every frame from what is *in* the cache, so
    // everything waiting was asked for again and the workers built it twice.
    for (const PatchWorkshop::Finished& waiting : arriving_)
        if (world::tileKeyOf(waiting.tile) == key) return true;
    return false;
}

double Explorer::viewRadius(const Camera& camera, std::int32_t lod) const {
    const double step = world::sampleMetresAt(lod);
    double radius = step;
    // Refine only from the coarse map. The old implementation ray-marched the
    // procedural HeightField up to 600 times on the frame thread whenever the
    // camera moved. Two cheap macro passes keep a plateau from inheriting the
    // world's sea-to-mountain range without doing generation work in a frame.
    auto heights = worldHeightBounds_;
    for (int refinement = 0; refinement < (camera.isometric ? 2 : 1); ++refinement) {
        double minX = std::numeric_limits<double>::max(), minY = minX;
        double maxX = -minX, maxY = -minX;
        radius = step;
        for (const auto [screenX, screenY] : std::array<std::pair<int, int>, 4>{{
                 {0, 0}, {camera.viewportWidth, 0},
                 {camera.viewportWidth, camera.viewportHeight}, {0, camera.viewportHeight}}})
            for (double height : {heights.first, heights.second}) {
                double wx, wy;
                camera.worldOfScreenAtHeight(screenX, screenY, height, wx, wy);
                radius = std::max(radius, std::hypot(wx - camera.centreX, wy - camera.centreY));
                minX = std::min(minX, wx); minY = std::min(minY, wy);
                maxX = std::max(maxX, wx); maxY = std::max(maxY, wy);
            }
        if (camera.isometric && refinement == 0)
            heights = world::ringHeightBounds(*world_,
                    {Fixed::fromDoubleForContent(minX), Fixed::fromDoubleForContent(minY)},
                    {Fixed::fromDoubleForContent(maxX), Fixed::fromDoubleForContent(maxY)}, lod);
    }
    return radius;
}

const std::vector<Explorer::Visible>& Explorer::prepareVisible(const Camera& camera) {
    if (gpuPages_) return visible_;
    if (preparedFrame_ == frame_) return visible_;
    preparedFrame_ = frame_;
    const double projectedSampleScale =
            camera.isometric ? camera.pixelsPerTile * 0.25 : camera.pixelsPerTile;
    const std::int32_t lod = levelFor(projectedSampleScale);
    if (!haveFootprint_ || camera.focusHeight != footprintCamera_.focusHeight ||
        camera.yaw != footprintCamera_.yaw || camera.pitch != footprintCamera_.pitch ||
        camera.pixelsPerTile != footprintCamera_.pixelsPerTile ||
        camera.isometric != footprintCamera_.isometric ||
        camera.viewportWidth != footprintCamera_.viewportWidth ||
        camera.viewportHeight != footprintCamera_.viewportHeight) {
        footprintRadius_ = viewRadius(camera, lod);
        footprintCamera_ = camera;
        haveFootprint_ = true;
    }
    const double radius = footprintRadius_;
    streamRadius_ = radius;

    // The tiles a disc of ground covers at a level. No centre and no epoch: a
    // tile is named by where it is, so moving the camera changes which tiles
    // are wanted without changing what any of them is.
    const auto cover = [](double centreX, double centreY, double reach, std::int32_t level) {
        const double side = static_cast<double>(world::tileMetresAt(level));
        TileView out;
        out.set = true;
        out.lod = level;
        out.firstX = static_cast<std::int32_t>(std::floor((centreX - reach) / side));
        out.lastX = static_cast<std::int32_t>(std::floor((centreX + reach) / side));
        out.firstY = static_cast<std::int32_t>(std::floor((centreY - reach) / side));
        out.lastY = static_cast<std::int32_t>(std::floor((centreY + reach) / side));
        return out;
    };

    // A margin ahead of the camera, so that walking into new ground finds it
    // already there. It costs the strip that entered, which is what a tiled
    // world is for: with an annulus there was no point keeping a margin,
    // because moving renamed everything anyway.
    const double margin = std::max(world::tileMetresAt(lod) * 1.0, radius * 0.25);
    if (wanted_.set && wanted_.lod != lod) previous_ = wanted_;
    wanted_ = cover(camera.centreX, camera.centreY, radius + margin, lod);
    if (previous_.set && previous_.lod == lod) previous_ = {};

    // A level is standable on the moment its own pages are pinned, which is
    // long before the bake as a whole is done - the coarse level is published
    // first and the finest one nothing here reads is published last. Waiting
    // for `running` to go false meant the backdrop did not appear at all in
    // the first several seconds, which is exactly the stretch it exists for.
    const auto baking = prebakeProgress();
    const std::int32_t backdropLod = std::clamp(lod + 2, kFinestHeldLevel, coarsest_);
    const bool wantBackdrop = baking.finestPinned <= backdropLod && lod < backdropLod;
    if (wantBackdrop)
        backdrop_ = cover(camera.centreX, camera.centreY, radius * 1.2, backdropLod);
    else
        backdrop_ = {};

    std::vector<PatchWorkshop::Order> orders;
    visible_.clear();
    unfilled_.clear();
    missing_ = 0;

    // What is asked for, nearest the eye first. The backdrop leads, because a
    // coarse picture of the whole view beats a sharp picture of a fifth of it,
    // and its pages are already in memory.
    const auto askFor = [&](const TileView& view, double bias, bool landOnly = false) {
        if (view.empty()) return;
        const double side = static_cast<double>(world::tileMetresAt(view.lod));
        for (std::int32_t y = view.firstY; y <= view.lastY; ++y)
            for (std::int32_t x = view.firstX; x <= view.lastX; ++x) {
                const world::TileId tile{x, y, view.lod};
                if (landOnly && !workshop_->containsLand(tile)) continue;
                if (held(tile)) continue;
                const double middleX = (x + 0.5) * side, middleY = (y + 0.5) * side;
                orders.push_back({tile, bias + std::hypot(middleX - camera.centreX,
                                                          middleY - camera.centreY)});
            }
    };
    askFor(backdrop_, -1e7);
    askFor(wanted_, 0);
    // And the world, once the pages it is cut from are all in memory. Last in
    // the book, so it never delays a tile the camera is waiting on. Asked
    // every frame rather than once: askFor already passes over what is held,
    // so when the last one lands this adds nothing, and until then a tile
    // whose build was cancelled under a moving camera is simply asked again.
    if (baking.finestPinned <= coarsest_) askFor(foundation_, 1e7, true);
    workshop_->wants(std::move(orders));

    // What is drawn: for every place the view covers, the finest ground there
    // is for it.
    //
    // A tile that has not arrived is not a hole. Because a tile is named by
    // its place, the same place at a coarser level is a tile too, and one of
    // those is nearly always already built - the level above was what the
    // camera was looking at a moment ago, or it is the backdrop, or it is the
    // coarse world that has been in memory since the map was raised. So the
    // question a missing tile asks is not "what do we hide this with" but
    // "what else do we have here", and the answer is walked for.
    std::set<std::int64_t> shown;
    std::vector<const TerrainPatch*> under;
    if (!wanted_.empty())
        for (std::int32_t y = wanted_.firstY; y <= wanted_.lastY; ++y)
            for (std::int32_t x = wanted_.firstX; x <= wanted_.lastX; ++x) {
                if (patches_.count(world::tileKeyOf({x, y, lod}))) continue;
                ++missing_;
                // Up through the levels until something covers this place. A
                // tile of level L+1 covers exactly four of level L, so the
                // place is found by halving the coordinates.
                std::int32_t cx = x, cy = y;
                for (std::int32_t level = lod + 1; level <= coarsest_; ++level) {
                    cx = cx >= 0 ? cx / 2 : (cx - 1) / 2;
                    cy = cy >= 0 ? cy / 2 : (cy - 1) / 2;
                    const auto key = world::tileKeyOf({cx, cy, level});
                    const auto found = patches_.find(key);
                    if (found == patches_.end()) continue;
                    if (shown.insert(key).second) under.push_back(&found->second);
                    break;
                }
            }
    // And the level the view was at before this one, wherever it is still
    // held: what covers the screen while a zoom's new level arrives.
    if (previous_.set && !previous_.empty())
        for (std::int32_t y = previous_.firstY; y <= previous_.lastY; ++y)
            for (std::int32_t x = previous_.firstX; x <= previous_.lastX; ++x) {
                const auto key = world::tileKeyOf({x, y, previous_.lod});
                const auto found = patches_.find(key);
                if (found == patches_.end() || !shown.insert(key).second) continue;
                under.push_back(&found->second);
            }
    // Coarse first, so the sharp ground lands on top of it.
    for (const TerrainPatch* patch : under) {
        Visible seen{patch, false, true};
        seen.draw[2] = 1;
        patch->lastUsedFrame = frame_;
        visible_.push_back(seen);
    }
    if (!wanted_.empty())
        for (std::int32_t y = wanted_.firstY; y <= wanted_.lastY; ++y)
            for (std::int32_t x = wanted_.firstX; x <= wanted_.lastX; ++x) {
                const auto found = patches_.find(world::tileKeyOf({x, y, lod}));
                if (found == patches_.end()) continue;
                // draw[1] stays nought, which is what tells the shaders that
                // this is a mesh with a morph target: a tile's vertices nest
                // with the level above's, so walking to them is a walk rather
                // than a dissolve. The annulus set it to one and every morph in
                // the ground shader was switched off behind that flag.
                Visible seen{&found->second, true, false};
                found->second.lastUsedFrame = frame_;
                visible_.push_back(seen);
            }

    lastLevel_ = lod;
    lastLevelExact_ = levelExactFor(projectedSampleScale);
    lastWanted_ = wanted_.count();
    blankShare_ = lastWanted_ ? double(missing_) / double(lastWanted_) : 0;
    coarseShare_ = 0;

    static const bool tracing = [] {
        const char* on = std::getenv("ASR_TRACE_STREAM");
        return on != nullptr && *on == '1';
    }();
    if (tracing) {
        const auto w = workshop_->workerStats();
        std::fprintf(stderr,
                     "f=%llu lod=%d wanted=%zu missing=%zu held=%zu backdrop=%zu "
                     "queued=%zu busy=%zu arrived=%zu\n",
                     (unsigned long long)frame_, lod, lastWanted_, missing_, patches_.size(),
                     backdrop_.count(), w.queued, w.busy, builtThisFrame_);
    }
    return visible_;
}

void Explorer::draw(SDL_Renderer* sdl, Renderer& art, const Camera& camera) {
    const auto startedDrawing = std::chrono::steady_clock::now();
    drawnPatches_ = 0;
    drawnTriangles_ = 0;
    const std::vector<Visible>& seen = prepareVisible(camera);
    std::vector<const TerrainPatch*> ready;
    ready.reserve(seen.size());
    for (const Visible& one : seen) ready.push_back(one.patch);
    const std::int32_t lod = lastLevel_;
    const std::int32_t side = world::chunkMetresAt(lod);
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    // Nothing fades in any more. A tile is drawn when it exists and the
    // coarser ground behind it is drawn under it, so there is no frontier to
    // reveal across and nothing to hide while it travels.
    const auto revealAt = [](const TerrainPatch&, double, double) { return 1.0f; };

    // --- the ground -------------------------------------------------------
    // One pass of flat colour, then a pass per material with the material's own
    // texture and the vertex's weight as its alpha. The colour is what makes a
    // country readable at a glance; the textures are what make the ground look
    // like ground close up, and because they are blended by weight rather than
    // chosen per tile, grass gives way to sand over ten metres instead of at a
    // line.
    // `perTexture` is how many metres of ground one turn of the texture covers,
    // and `strength` how much of it is laid on. Each material is laid twice at
    // different sizes: one size repeats visibly however good the texture is,
    // and two multiplied together do not repeat until their least common
    // multiple, which is past the horizon.
    const auto emit = [&](const TerrainPatch& patch, SDL_Texture* texture,
                          world::Material material, bool byWeight, double perTexture = 24.0,
                          float strength = 1.0f, float accent = 0.0f) {
        vertices_.clear();
        indices_.clear();
        vertices_.reserve(patch.mesh.vertices.size());
        // Where this patch's corner falls within one turn of the texture.
        //
        // The obvious thing is to divide the world position by the size of the
        // texture and hand that over, and it draws the right picture - a
        // continuous surface with no seam at the patch edges. What it does not
        // do is draw it sharply: the world runs to a million metres, so a
        // texture coordinate out here is five thousand and something, and a
        // sampler carries only so many bits below the point. Past a few
        // hundred turns the fraction is quantised to a coarse grid and the
        // cracked earth arrives as soft blobs, which is what the ground has
        // looked like since the materials went in.
        //
        // So the coordinate is measured from the patch's own corner, which
        // keeps it under two, and the corner's position within the turn is
        // added back - the same number the whole patch over, and continuous
        // across the boundary because it is the world position modulo the turn.
        const double patchSide = world::chunkMetresAt(patch.mesh.lod);
        const double originX = patch.mesh.ringKey ? patch.mesh.ringCentre.x.toDouble()
                                                 : static_cast<double>(patch.mesh.chunk.x) * patchSide;
        const double originY = patch.mesh.ringKey ? patch.mesh.ringCentre.y.toDouble()
                                                 : static_cast<double>(patch.mesh.chunk.y) * patchSide;
        const auto turnOf = [perTexture](double v) {
            const double t = std::fmod(v / perTexture, 1.0);
            return static_cast<float>(t < 0 ? t + 1.0 : t);
        };
        const float baseU = turnOf(originX);
        const float baseV = turnOf(originY);
        for (const world::TerrainVertex& v : patch.mesh.vertices) {
            float sx, sy;
            camera.worldToScreen3D(v.position.x.toDouble(), v.position.y.toDouble(),
                                   v.height.toDouble(), sx, sy);
            // The sun on the slope, and then the shape of the ground on top of
            // it: a crest is brighter than an even slope at the same angle and
            // a hollow is darker, which is what makes a range read as a range.
            // Measured against the size of the sample, so it says the same
            // thing at every level of detail.
            // As a slope rather than as a height: how far the ground stands
            // above what is around it, over the distance that was measured
            // across. Scaled by the height alone, every wrinkle at every level
            // came out saturated and the country was drawn as a contour map of
            // its own noise.
            const double across = 4.0 * world::sampleMetresAt(patch.mesh.lod);
            const double stands = v.openness.toDouble() / across;
            const float light = std::clamp(
                    lightOn(v.normal) *
                            static_cast<float>(std::clamp(1.0 + 1.4 * stands, 0.86, 1.16)),
                    0.18f, 1.35f);
            SDL_FColor colour{};
            if (byWeight) {
                // The texture, half-tinted towards the material's own colour
                // and lit like everything else. All the way to the texture's
                // colours and the country stops reading at a distance; all the
                // way to the tint and the texture stops being a texture and
                // becomes a wash.
                const SDL_FColor tint = materialColour(material);
                colour = {0.55f + 0.45f * tint.r, 0.55f + 0.45f * tint.g, 0.55f + 0.45f * tint.b,
                          static_cast<float>(v.materials.of(material).toDouble()) * strength};
            } else {
                for (std::size_t i = 0; i < world::kMaterialCount; ++i) {
                    const SDL_FColor c = materialColour(static_cast<world::Material>(i));
                    const float w = static_cast<float>(v.materials.weight[i].toDouble());
                    colour.r += c.r * w;
                    colour.g += c.g * w;
                    colour.b += c.b * w;
                }
                colour.a = 1.0f;
            }
            // And the slow drift of the country over the top of it, on the
            // texture passes as much as on the colour underneath. It used to be
            // on the colour alone, which meant it was painted and then covered
            // over: the material passes lay their own even tone across the
            // whole patch and grassland came out as one flat green from one
            // side of the view to the other. It is the same figure at every
            // scale, so what a hillside gains close up it keeps when the camera
            // pulls back to the next valley.
            const double drift = macroVariation(v.position.x.toDouble(),
                                                v.position.y.toDouble(), across / 4.0);
            if (byWeight && accent != 0.0f) {
                // Alternate photographs come and go in broad, soft patches. A
                // hard per-triangle choice would put the material grid straight
                // back into a mesh designed specifically not to have one.
                double mix = std::clamp((drift - 0.24) / 0.52, 0.0, 1.0);
                mix = mix * mix * (3.0 - 2.0 * mix);
                if (accent < 0.0f) mix = 1.0 - mix;
                colour.a *= static_cast<float>(mix) * std::abs(accent);
            }
            const float shade = light * static_cast<float>(0.88 + 0.24 * drift);
            colour.r *= shade;
            colour.g *= shade;
            colour.b *= shade;
            if (!byWeight) {
                // And the colour drifts as well as the brightness, on the pass
                // that carries the colour. A hillside is not one green with the
                // lights turned up and down across it: the dry side of it is
                // warmer and the shaded side colder, and at the distance where
                // the cards have faded out this is the only thing left telling
                // the eye that the ground is ground.
                const float warm = static_cast<float>(drift - 0.5);
                colour.r *= 1.0f + 0.10f * warm;
                colour.g *= 1.0f + 0.02f * warm;
                colour.b *= 1.0f - 0.12f * warm;
            }
            SDL_Vertex vertex{};
            vertex.position = {sx, sy};
            vertex.color = colour;
            vertex.color.a *= revealAt(patch, v.position.x.toDouble(), v.position.y.toDouble());
            vertex.tex_coord = {
                    baseU + static_cast<float>((v.position.x.toDouble() - originX) / perTexture),
                    baseV + static_cast<float>((v.position.y.toDouble() - originY) / perTexture)};
            vertices_.push_back(vertex);
        }
        indices_.assign(patch.mesh.indices.begin(), patch.mesh.indices.end());
        SDL_RenderGeometry(sdl, texture, vertices_.data(), static_cast<int>(vertices_.size()),
                           indices_.data(), static_cast<int>(indices_.size()));
        drawnTriangles_ += indices_.size() / 3;
    };

    // Ground nobody has cut yet, painted in the colour of the country it is.
    // Coarse to the point of rudeness - one quad, four corners read off the
    // world map - and still better than black: the eye reads it as distance
    // rather than as a hole, and it is gone by the next frame or the one after.
    if (!unfilled_.empty()) {
        vertices_.clear();
        indices_.clear();
        for (const Unfilled& blank : unfilled_) {
            const world::ChunkId chunk = blank.chunk;
            const double x0 = static_cast<double>(chunk.x) * side;
            const double y0 = static_cast<double>(chunk.y) * side;
            const std::uint32_t base = static_cast<std::uint32_t>(vertices_.size());
            for (int corner = 0; corner < 4; ++corner) {
                const double wx = x0 + (corner == 1 || corner == 2 ? side : 0);
                const double wy = y0 + (corner >= 2 ? side : 0);
                float sx, sy;
                const core::WorldPos at{Fixed::fromDoubleForContent(wx),
                                        Fixed::fromDoubleForContent(wy)};
                camera.worldToScreen3D(wx, wy, field_.groundAt(at).height.toDouble(), sx, sy);
                SDL_Vertex vertex{};
                vertex.position = {sx, sy};
                vertex.color = countryColour(*world_, wx, wy);
                vertices_.push_back(vertex);
            }
            for (int index : {0, 1, 2, 0, 2, 3})
                indices_.push_back(static_cast<int>(base) + index);
        }
        SDL_RenderGeometry(sdl, nullptr, vertices_.data(), static_cast<int>(vertices_.size()),
                           indices_.data(), static_cast<int>(indices_.size()));
    }

    for (const TerrainPatch* patch : ready) {
        emit(*patch, nullptr, world::Material::Count, false);
        ++drawnPatches_;
    }
    // The ground, in as many sizes of texture as the camera is close enough to
    // see.
    //
    // Each size is laid at a fixed number of metres to the turn, because that is
    // what gives the ground scale: a crack in dried mud is a hand's breadth
    // whatever the camera is doing, and a card that stretches with the zoom is
    // wallpaper. The cost of that is repetition - pull back far enough and a
    // turn shrinks towards a pixel, and what a repeating card draws at that size
    // is not detail but a moire of its own grid. So each size is faded out over
    // the stretch before it gets there, and the wide view is left to the colour
    // of the country and its drift, which have no grid in them.
    // How much of a card is laid on, from how big one turn of it comes out on
    // screen. Measured in pixels rather than in zoom because that is what
    // decides whether it can be seen at all: below about a thumbnail to the turn
    // there is no material left in it, only its grid, and a grid drawn over the
    // whole country is worse than no card.
    const auto fade = [ppt = camera.pixelsPerTile](double perTexture) {
        return std::clamp((perTexture * ppt - 28.0) / 82.0, 0.0, 1.0);
    };
    const std::array<std::pair<double, double>, 2> laid{{
            {kFineMetres, fade(kFineMetres)},
            {kCloseMetres, 0.40 * fade(kCloseMetres)},
    }};
    if (laid[0].second > 0.02) {
        // Source-over compositing is order-dependent. Bare earth is the base;
        // vegetation, exposed rock and snow belong above it. Drawing enum order
        // put Dirt over Grass and spread its cracked photograph across the world.
        constexpr std::array<world::Material, world::kMaterialCount> materialOrder{{
                world::Material::Dirt, world::Material::Sand, world::Material::Grass,
                world::Material::Marsh, world::Material::Rock, world::Material::Snow}};
        for (world::Material material : materialOrder) {
            const std::size_t i = static_cast<std::size_t>(material);
            const char* name = materialTexture(material);
            if (name == nullptr) continue;
            SDL_Texture* texture = art.pictureOf(name);
            if (texture == nullptr) continue;
            // The copy whose texels are about the size of the screen's pixels.
            // SDL's renderer has no mipmaps, and a texture drawn at a fifteenth
            // of its size is not detail - it is a shimmer that averages out to
            // a wash, which is exactly what the ground looked like before this.
            const auto pick = [&](const char* textureName, SDL_Texture* full,
                                  double perTexture) -> SDL_Texture* {
                const double wanted = perTexture * camera.pixelsPerTile;
                int best = 1;
                double closest = std::abs(std::log2(512.0 / wanted));
                for (int step : {2, 4, 8, 16}) {
                    const double miss = std::abs(std::log2(512.0 / step / wanted));
                    if (miss < closest) { closest = miss; best = step; }
                }
                if (best == 1) return full;
                SDL_Texture* smaller = art.pictureOf(std::string(textureName) + "@" + std::to_string(best));
                return smaller ? smaller : full;
            };
            // The broadest size is drawn from the smallest copy on purpose,
            // however close the camera is. At a hundred and eighty metres to
            // the turn the full card is not a material any more, it is a field
            // of mud plates twenty metres wide laid over the country; what is
            // wanted from it at that size is only the slow light and dark the
            // photograph averages out to, which is exactly what is left of it
            // after it has been reduced sixteen times.
            for (const auto& [perTexture, strength] : laid) {
                if (strength <= 0.02) continue;
                SDL_Texture* card = pick(name, texture, perTexture);
                SDL_SetTextureBlendMode(card, SDL_BLENDMODE_BLEND);
                for (const TerrainPatch* patch : ready)
                    // A patch is skipped only when the material is all but
                    // absent from it. Anything higher shows as a rectangle of
                    // ground with a different surface to its neighbours - the
                    // patch grid, drawn.
                    if (patch->mostOf[i] > 0.01f)
                        emit(*patch, card, material, true, perTexture,
                             static_cast<float>(strength));
            }

            const char* accentName = materialAccentTexture(material);
            SDL_Texture* accentTexture = accentName ? art.pictureOf(accentName) : nullptr;
            const float accentMix = material == world::Material::Grass ? 0.62f
                                    : material == world::Material::Marsh ? 0.34f
                                                                        : -0.46f;
            if (accentTexture != nullptr) {
                for (const auto& [perTexture, strength] : laid) {
                    if (strength <= 0.02) continue;
                    SDL_Texture* card = pick(accentName, accentTexture, perTexture);
                    SDL_SetTextureBlendMode(card, SDL_BLENDMODE_BLEND);
                    for (const TerrainPatch* patch : ready)
                        if (patch->mostOf[i] > 0.01f)
                            emit(*patch, card, material, true, perTexture,
                                 static_cast<float>(strength), accentMix);
                }
            }
        }
    }

    // --- grass foliage ---------------------------------------------------
    // The material mesh says where grass belongs; upright cards supply the
    // close-range silhouette it cannot have on its own. They are deterministic
    // in world space, batched by texture, and fade away before becoming noise.
    const float foliageShow = static_cast<float>(
            std::clamp((camera.pixelsPerTile - 4.0) / 8.0, 0.0, 1.0));
    if (foliageShow > 0.01f) {
        constexpr std::array<const char*, 6> foliageNames{{
                "foliage/grass_0", "foliage/grass_1", "foliage/grass_2",
                "foliage/grass_3", "foliage/grass_4", "foliage/grass_5"}};
        const int samples = camera.pixelsPerTile >= 28.0 ? 10
                          : camera.pixelsPerTile >= 12.0 ? 6 : 2;
        // At distance fewer, larger cards cover the same ground. This is the
        // foliage equivalent of a mip level: density stays visually solid while
        // vertex count falls instead of rising to fill sub-pixel holes.
        const float distanceScale = static_cast<float>(
                std::clamp(18.0 / camera.pixelsPerTile, 1.0, 2.4));
        const float time = static_cast<float>(SDL_GetTicksNS()) * 0.000000001f;
        std::array<SDL_Texture*, kFoliageVariants> foliageTextures{};
        for (std::size_t variant = 0; variant < foliageNames.size(); ++variant) {
            foliageTextures[variant] = art.pictureOf(foliageNames[variant]);
            foliageVertices_[variant].clear();
            foliageIndices_[variant].clear();
        }
        for (const TerrainPatch* patch : ready) {
            if (patch->mostOf[static_cast<std::size_t>(world::Material::Grass)] < 0.12f)
                continue;
            const std::int64_t step = world::sampleMetresAt(patch->mesh.lod);
                for (std::size_t triangle = 0; triangle < patch->mesh.indices.size(); triangle += 3) {
                        const auto cell = patch->mesh.indices[triangle];
                        const auto next = patch->mesh.indices[triangle + 1];
                        const auto last = patch->mesh.indices[triangle + 2];
                        const world::TerrainVertex& nw = patch->mesh.vertices[cell];
                        const world::TerrainVertex& ne = patch->mesh.vertices[next];
                        const world::TerrainVertex& sw = patch->mesh.vertices[last];
                        const world::TerrainVertex& se = sw;
                        for (int sample = 0; sample < (samples + 1) / 2; ++sample) {
                            const std::int64_t cellX = world::floorDiv(nw.position.x.toInt(), step);
                            const std::int64_t cellY = world::floorDiv(nw.position.y.toInt(), step);
                            const std::uint32_t random = foliageNoise(cellX, cellY, sample);
                            const std::size_t variant = random % foliageNames.size();
                            if (foliageTextures[variant] == nullptr) continue;
                            const double jitterX = 0.08 + (random & 0xffu) / 255.0 * 0.84;
                            const double jitterY = 0.08 + ((random >> 8) & 0xffu) / 255.0 * 0.84;
                            const auto blend = [jitterX, jitterY](double a, double b,
                                                                  double c, double) {
                                const double r = std::sqrt(jitterX);
                                return (1 - r) * a + r * (1 - jitterY) * b + r * jitterY * c;
                            };
                            const double wx = blend(nw.position.x.toDouble(), ne.position.x.toDouble(), sw.position.x.toDouble(), 0);
                            const double wy = blend(nw.position.y.toDouble(), ne.position.y.toDouble(), sw.position.y.toDouble(), 0);
                            const double height = blend(nw.height.toDouble(), ne.height.toDouble(),
                                                        sw.height.toDouble(), se.height.toDouble());
                            const auto grassAt = [](const world::TerrainVertex& v) {
                                return v.materials.of(world::Material::Grass).toDouble();
                            };
                            const auto materialAt = [](const world::TerrainVertex& v,
                                                       world::Material material) {
                                return v.materials.of(material).toDouble();
                            };
                            const float grass = static_cast<float>(
                                    blend(grassAt(nw), grassAt(ne), grassAt(sw), grassAt(se)));
                            const float rock = static_cast<float>(blend(
                                    materialAt(nw, world::Material::Rock),
                                    materialAt(ne, world::Material::Rock),
                                    materialAt(sw, world::Material::Rock),
                                    materialAt(se, world::Material::Rock)));
                            const float shore = static_cast<float>(blend(
                                    materialAt(nw, world::Material::Sand) +
                                            materialAt(nw, world::Material::Marsh),
                                    materialAt(ne, world::Material::Sand) +
                                            materialAt(ne, world::Material::Marsh),
                                    materialAt(sw, world::Material::Sand) +
                                            materialAt(sw, world::Material::Marsh),
                                    materialAt(se, world::Material::Sand) +
                                            materialAt(se, world::Material::Marsh)));
                            const double normalZ = blend(nw.normal.z.toDouble(), ne.normal.z.toDouble(),
                                                         sw.normal.z.toDouble(), se.normal.z.toDouble());
                            const double water = blend(patch->waterLevel[cell].toDouble(),
                                                       patch->waterLevel[next].toDouble(),
                                                       patch->waterLevel[last].toDouble(), 0);
                            if (water > height + 0.02 || grass < 0.56f || rock > 0.16f ||
                                shore > 0.20f || normalZ < 0.90)
                                continue;
                            const float chance = static_cast<float>((random >> 16) & 0xffu) / 255.0f;
                            // A grass material already means a meadow. Its weight
                            // varies the lushness, not whether there are only a
                            // few lonely hairs scattered over sixteen square metres.
                            if (chance > 0.30f + grass * 0.70f) continue;

                            float sx, sy;
                            camera.worldToScreen3D(wx, wy, height, sx, sy);
                            const float scale = 0.82f + static_cast<float>((random >> 24) & 0xffu) /
                                                              255.0f * 0.38f;
                            const float spriteHeight = static_cast<float>(camera.pixelsPerTile) *
                                                       1.30f * distanceScale * scale;
                            const float halfWidth = spriteHeight * 0.43f;
                            const float phase = static_cast<float>(wx * 0.09 + wy * 0.07) - time * 1.2f;
                            const float sway = (std::sin(phase) * 0.7f +
                                                std::sin(phase * 0.47f - time) * 0.3f) *
                                               halfWidth * 0.24f;
                            const float variation = static_cast<float>((random >> 20) & 0x0fu) / 15.0f;
                            const float alpha = foliageShow * (0.58f + grass * 0.38f) * revealAt(*patch, wx, wy);
                            const SDL_FColor top{0.32f + variation * 0.08f,
                                                 0.57f + variation * 0.10f,
                                                 0.15f + variation * 0.05f, alpha};
                            const SDL_FColor root{0.16f + variation * 0.04f,
                                                  0.34f + variation * 0.07f,
                                                  0.07f + variation * 0.03f, alpha};
                            auto& vertices = foliageVertices_[variant];
                            auto& indices = foliageIndices_[variant];
                            const auto base = static_cast<int>(vertices.size());
                            vertices.push_back({{sx - halfWidth + sway, sy - spriteHeight}, top, {0, 0}});
                            vertices.push_back({{sx + halfWidth + sway, sy - spriteHeight}, top, {1, 0}});
                            vertices.push_back({{sx + halfWidth * 0.72f, sy}, root, {1, 1}});
                            vertices.push_back({{sx - halfWidth * 0.72f, sy}, root, {0, 1}});
                            for (int index : {0, 1, 2, 0, 2, 3}) indices.push_back(base + index);
                        }
                }
        }
        for (std::size_t variant = 0; variant < foliageNames.size(); ++variant) {
            if (foliageTextures[variant] == nullptr || foliageIndices_[variant].empty()) continue;
            SDL_RenderGeometry(sdl, foliageTextures[variant], foliageVertices_[variant].data(),
                               static_cast<int>(foliageVertices_[variant].size()),
                               foliageIndices_[variant].data(),
                               static_cast<int>(foliageIndices_[variant].size()));
        }
    }

    // --- the water --------------------------------------------------------
    // Drawn as its own surface over the ground, with the shore soft: a vertex
    // above the water line is clear, one below it is not, and the triangle
    // between them fades. Which is what a shore is.
    for (const TerrainPatch* patch : ready) {
        if (!patch->anyWater) continue;
        vertices_.clear();
        indices_.clear();
        for (std::size_t i = 0; i < patch->mesh.vertices.size(); ++i) {
            const world::TerrainVertex& v = patch->mesh.vertices[i];
            const double depth = patch->waterLevel[i].toDouble() - v.height.toDouble();
            float sx, sy;
            // SDL_Renderer has no depth buffer. Projecting a deep river at its
            // physical surface shifts the blue mesh many pixels over the near
            // bank, making it read as a raised ribbon. Keep it on the carved bed
            // (with a tiny anti-z-fighting lift); colour still carries the depth.
            const double drawnHeight = v.height.toDouble() + std::clamp(depth, 0.0, 0.18);
            camera.worldToScreen3D(v.position.x.toDouble(), v.position.y.toDouble(),
                                   drawnHeight, sx, sy);
            SDL_Vertex vertex{};
            vertex.position = {sx, sy};
            // Shallow water is the ground showing through; deep water is not.
            //
            // The shore is a ramp rather than a line, and a long one: the water
            // takes its full colour a couple of metres down, which is a
            // handful of steps of the lattice now that the bed comes up to the
            // water line at the bank rather than stopping dead at it - so the
            // shore is a line with shallows in front of it rather than the
            // staircase of four-metre treads it used to be.
            const float shallow = static_cast<float>(std::clamp(depth / 1.8, 0.0, 1.0));
            const float deep = static_cast<float>(std::clamp(depth / 14.0, 0.0, 1.0));
            vertex.color = {0.16f + 0.16f * (1 - deep), 0.34f + 0.24f * (1 - deep),
                            0.46f + 0.14f * (1 - deep),
                            depth > 0 ? 0.92f * shallow : 0.0f};
            vertex.color.a *= revealAt(*patch, v.position.x.toDouble(), v.position.y.toDouble());
            vertices_.push_back(vertex);
        }
        indices_.assign(patch->mesh.indices.begin(), patch->mesh.indices.end());
        SDL_RenderGeometry(sdl, nullptr, vertices_.data(), static_cast<int>(vertices_.size()),
                           indices_.data(), static_cast<int>(indices_.size()));
    }

    // --- the cliffs -------------------------------------------------------
    // Seen from above a face has no width, so what is drawn is what a face
    // leaves on the ground: a dark band of shadow and scree below the lip, as
    // deep as the drop, and a light line along the lip itself.
    //
    // And it fades out with distance rather than stopping at a threshold. A
    // shadow band is a few metres of ground, so far enough back it is a line of
    // a pixel or two - and a country's worth of them, drawn at full strength
    // over a snowfield, is a mess of black snakes rather than a range with
    // cliffs in it.
    const double cliffShow = std::clamp((camera.pixelsPerTile - 1.4) / 4.0, 0.0, 1.0);
    if (cliffShow > 0.02) {
        for (const TerrainPatch* patch : ready) {
            for (const world::CliffSegment& seg : patch->cliffs) {
                const double drop = seg.drop().toDouble();
                const double band = std::clamp(drop * 0.7, 1.5, 14.0);
                const double fx = seg.fallX.toDouble(), fy = seg.fallY.toDouble();
                const WorldPos middle{(seg.a.x + seg.b.x) / Fixed::fromInt(2),
                                      (seg.a.y + seg.b.y) / Fixed::fromInt(2)};
                const world::WaterNearby channel =
                        field_.macro().carve(middle, core::kZero, core::kZero);
                if (channel.found && channel.bankDistance < Fixed::fromInt(20)) continue;
                float ax, ay, bx, by, cx, cy, dx, dy;
                camera.worldToScreen3D(seg.a.x.toDouble(), seg.a.y.toDouble(), seg.top.toDouble(), ax, ay);
                camera.worldToScreen3D(seg.b.x.toDouble(), seg.b.y.toDouble(), seg.top.toDouble(), bx, by);
                camera.worldToScreen3D(seg.b.x.toDouble() + fx * band,
                                       seg.b.y.toDouble() + fy * band, seg.bottom.toDouble(), cx, cy);
                camera.worldToScreen3D(seg.a.x.toDouble() + fx * band,
                                       seg.a.y.toDouble() + fy * band, seg.bottom.toDouble(), dx, dy);
                const SDL_FColor dark =
                        rgb(46, 38, 34, 0.62f * static_cast<float>(cliffShow));
                const SDL_FColor fade = rgb(74, 62, 54, 0.0f);
                SDL_Vertex quad[4];
                quad[0] = {{ax, ay}, dark, {0, 0}};
                quad[1] = {{bx, by}, dark, {0, 0}};
                quad[2] = {{cx, cy}, fade, {0, 0}};
                quad[3] = {{dx, dy}, fade, {0, 0}};
                const int order[6] = {0, 1, 2, 0, 2, 3};
                SDL_RenderGeometry(sdl, nullptr, quad, 4, order, 6);
                SDL_SetRenderDrawColorFloat(sdl, 0.86f, 0.82f, 0.74f,
                                            0.55f * static_cast<float>(cliffShow));
                SDL_RenderLine(sdl, ax, ay, bx, by);
            }
        }
    }
    drawMillis_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                            startedDrawing)
                          .count();
}

std::vector<std::string> Explorer::status(const Camera& camera, int mouseX, int mouseY,
                                           const world::weather::Snapshot* weather) const {
    return inspectWorld(*world_, field_, camera, mouseX, mouseY, weather);
}

std::vector<std::string> inspectWorld(const generation::WorldMapData& map, const world::HeightField& field_,
                                     const Camera& camera, int mouseX, int mouseY,
                                     const world::weather::Snapshot* weather) {
    const auto* world_ = &map;
    const auto worldHeightBounds_ = world::ringHeightBounds(map, {},
        {Fixed::fromInt(std::int64_t(map.width) * generation::kMetresPerCell),
         Fixed::fromInt(std::int64_t(map.height) * generation::kMetresPerCell)}, 6);
    std::vector<std::string> lines;
    double wx = 0, wy = 0;
    if (camera.perspective()) {
        const auto ray = camera.screenRay(mouseX, mouseY);
        double enter = camera.nearPlane, leave = camera.farPlane;
        const double lo[3]{0, 0, worldHeightBounds_.first - 1.0};
        const double hi[3]{double(world_->width) * generation::kMetresPerCell,
                           double(world_->height) * generation::kMetresPerCell,
                           worldHeightBounds_.second + 1.0};
        for (int axis = 0; axis < 3; ++axis) {
            if (std::abs(ray.direction[axis]) < 1e-9) {
                if (ray.origin[axis] < lo[axis] || ray.origin[axis] > hi[axis])
                    return {"No terrain/water hit under cursor"};
                continue;
            }
            const double a = (lo[axis] - ray.origin[axis]) / ray.direction[axis];
            const double b = (hi[axis] - ray.origin[axis]) / ray.direction[axis];
            enter = std::max(enter, std::min(a, b));
            leave = std::min(leave, std::max(a, b));
        }
        if (enter > leave) return {"No terrain/water hit under cursor"};
        const auto clearance = [&](double t) {
            wx = ray.origin[0] + ray.direction[0] * t;
            wy = ray.origin[1] + ray.direction[1] * t;
            const WorldPos at{Fixed::fromDoubleForContent(wx), Fixed::fromDoubleForContent(wy)};
            return ray.origin[2] + ray.direction[2] * t -
                   std::max(field_.heightAt(at).toDouble(), field_.waterLevelAt(at).toDouble());
        };
        double before = enter;
        double sign = clearance(before);
        bool hit = false;
        // Inspection only (at 4 Hz): bounded work, including horizontal/upward
        // rays. Search forward from the eye, never behind it.
        for (int i = 1; i <= 512; ++i) {
            const double after = std::lerp(enter, leave, i / 512.0);
            const double next = clearance(after);
            if ((sign > 0) != (next > 0) || next == 0) {
                double a = before, b = after;
                for (int j = 0; j < 20; ++j) {
                    const double middle = (a + b) * 0.5;
                    if ((clearance(middle) > 0) == (sign > 0)) a = middle;
                    else b = middle;
                }
                clearance((a + b) * 0.5);
                hit = true;
                break;
            }
            before = after; sign = next;
        }
        if (!hit) return {"No terrain/water hit under cursor"};
    } else {
        // March from above, then refine the first bracket. Fixed-point iteration
        // diverges on steep slopes after tilting; picking the bed also misses lakes.
        const auto clearance = [&](double height) {
            camera.worldOfScreenAtHeight(mouseX, mouseY, height, wx, wy);
            const WorldPos at{Fixed::fromDoubleForContent(wx), Fixed::fromDoubleForContent(wy)};
            return height - std::max(field_.heightAt(at).toDouble(), field_.waterLevelAt(at).toDouble());
        };
        double high = worldHeightBounds_.second + 1.0;
        double low = worldHeightBounds_.first - 1.0;
        const double top = high, bottom = low;
        bool hit = false;
        for (int i = 1; i <= 96; ++i) {
            const double height = top + (bottom - top) * i / 96.0;
            if (clearance(height) <= 0) { low = height; hit = true; break; }
            high = height;
        }
        if (!hit) return {"No terrain/water hit under cursor"};
        for (int i = 0; i < 20; ++i) {
            const double middle = (low + high) * 0.5;
            if (clearance(middle) > 0) high = middle; else low = middle;
        }
        clearance((low + high) * 0.5);
    }
    const WorldPos under{Fixed::fromDoubleForContent(wx), Fixed::fromDoubleForContent(wy)};

    char buffer[160];
    std::snprintf(buffer, sizeof(buffer), "at %s, %s east and south of the world's corner",
                  metres(wx).c_str(), metres(wy).c_str());
    lines.push_back(buffer);

    const world::GroundSample ground = field_.groundAt(under);
    const char* material = "";
    switch (ground.materials.strongest()) {
        case world::Material::Grass: material = "grass"; break;
        case world::Material::Dirt: material = "bare earth"; break;
        case world::Material::Sand: material = "sand"; break;
        case world::Material::Rock: material = "rock"; break;
        case world::Material::Marsh: material = "marsh"; break;
        case world::Material::Snow: material = "snow"; break;
        case world::Material::Count: break;
    }
    std::snprintf(buffer, sizeof(buffer), "ground: %.0f m up, %s, slope %.2f, %s",
                  ground.height.toDouble(), material, ground.slope.toDouble(),
                  ground.water ? "under water" : world::travelName(ground.travel));
    lines.push_back(buffer);

    const auto soilKind = ground.soil.weights.strongest();
    std::snprintf(buffer, sizeof(buffer), "soil: %s %.0f%% (substrate, not cover)",
                  world::soilName(soilKind), ground.soil.weights.of(soilKind).toDouble()*100);
    lines.push_back(buffer);
    std::snprintf(buffer, sizeof(buffer), "soil potential: fertility %.0f%%, permeability %.0f%%",
                  ground.soil.properties.fertility.toDouble()*100,
                  ground.soil.properties.permeability.toDouble()*100);
    lines.push_back(buffer);

    const auto percent = [&](world::Material kind) {
        return static_cast<int>(std::lround(ground.materials.of(kind).toDouble() * 100.0));
    };
    std::snprintf(buffer, sizeof(buffer),
                  "mix: grass %d%%  dirt %d%%  sand %d%%  rock %d%%  marsh %d%%  snow %d%%",
                  percent(world::Material::Grass), percent(world::Material::Dirt),
                  percent(world::Material::Sand), percent(world::Material::Rock),
                  percent(world::Material::Marsh), percent(world::Material::Snow));
    lines.push_back(buffer);

    const world::WaterNearby water = field_.macro().carve(under, ground.height, core::kZero);
    if (water.found) {
        std::snprintf(buffer, sizeof(buffer), "water: %s away%s", metres(water.distance.toDouble()).c_str(),
                      water.wet ? ", and this is in it" : "");
        lines.push_back(buffer);
    }

    const auto surface = field_.waterOver(under, Fixed::fromInt(world::kSampleMetres), ground.slope);
    const double depth = surface.level.toDouble() - ground.height.toDouble();
    const bool wet = surface.cover > core::kZero && depth > 0;
    const char* kind = !wet ? "dry" : surface.kind == world::HeightField::WaterKind::Lake ? "lake" :
                      surface.kind == world::HeightField::WaterKind::River ? "river" : "ocean";
    std::snprintf(buffer, sizeof(buffer), "%s: head %.1fm  depth %.1fm  cover %.0f%%", kind,
                  surface.level.toDouble(), std::max(0.0, depth), surface.cover.toDouble()*100);
    lines.push_back(buffer);
    if (wet) {
        std::snprintf(buffer, sizeof(buffer), "flow direction: (%+.2f, %+.2f), not m/s",
                      surface.flowX.toDouble(), surface.flowY.toDouble());
        lines.push_back(buffer);
    }
    if (water.nearestWet) {
        std::snprintf(buffer, sizeof(buffer), "nearest river width: %.1fm; bank: %+.1fm",
                      water.channelWidth.toDouble(),
                      water.bankDistance.toDouble());
        lines.push_back(buffer);
    }
    const core::TilePos cell{static_cast<int>(std::floor(wx/generation::kMetresPerCell)),
                             static_cast<int>(std::floor(wy/generation::kMetresPerCell))};
    if (world_->inBounds(cell)) {
        const auto index = static_cast<std::size_t>(cell.y)*world_->width+cell.x;
        if (index < world_->riverDischargeField.size()) {
            std::snprintf(buffer, sizeof(buffer), "cell (%d,%d): runoff %d, class %u/15", cell.x, cell.y,
                          world_->riverDischargeField[index], unsigned(world_->at(cell).drainSize));
            lines.push_back(buffer);
        }
    }
    if (weather && wet) {
        const auto climate = field_.surfaceClimateAt(under).environment;
        const auto scalar=[](Fixed value) { return static_cast<float>(value.toDouble()); };
        const auto local = weather->at(scalar(climate[0]), scalar(climate[2]), scalar(surface.level),
                                      static_cast<float>(wx), static_cast<float>(wy), scalar(climate[5]));
        // A river keeps an open channel where a pond of the same water freezes.
        const float flowing =
                surface.kind == world::HeightField::WaterKind::River ? 1.0f : 0.0f;
        const float ice = surface.kind == world::HeightField::WaterKind::Ocean ? 0.0f :
            world::weather::wxInlandIce(local.surface.ice, scalar(surface.level),
                                       scalar(surface.cover), flowing);
        std::snprintf(buffer, sizeof(buffer), "air %+.1f C; ice potential %.0f%% (rapids melt)",
                      local.air.temperature, ice*100);
        lines.push_back(buffer);
    }
    std::snprintf(buffer, sizeof(buffer), "camera: yaw %.0f tilt %.0f height %+.0fm",
                  camera.yaw*57.295779513, camera.pitch*57.295779513, camera.heightOffset);
    lines.push_back(buffer);

    const double across = camera.viewportWidth / std::max(0.0001, camera.pixelsPerTile);
    std::snprintf(buffer, sizeof(buffer), "the view is %s across", metres(across).c_str());
    lines.push_back(buffer);
    return lines;
}

// --- the mode itself ------------------------------------------------------

int runExplorer(SDL_Window* window, SDL_Renderer* sdl, Renderer& art, Camera& camera,
                std::uint64_t seed, std::int32_t worldCells, const std::string& shotPath,
                double startZoom, const std::string& startAt, bool measuring, bool closeUp) {
    generation::WorldMapParams params;
    params.seed = seed;
    params.width = params.height = worldCells;
    std::cout << "raising a " << params.width << "x" << params.height << " world ..." << std::flush;
    const auto started = std::chrono::steady_clock::now();
    const generation::WorldMapData world = generation::generateWorldMap(params);
    std::cout << " " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count()
              << " s, " << static_cast<double>(params.width) * generation::kMetresPerCell / 1000
              << " km across, " << world.lakesKept << " lakes ("
              << world.basinsDried << " basins dried)\n";

    Explorer explorer(world, seed);
    core::WorldPos start = explorer.startingPoint();

    // Somewhere named, for a picture asked for from a script.
    for (const Explorer::Landmark& landmark : explorer.landmarks())
        if (!startAt.empty() && landmark.name.find(startAt) != std::string::npos)
            start = landmark.where;

    // The coarse ground under wherever the view is actually opening, built here
    // and now. Warming the wrong place - which is what happens if this runs
    // before the landmark is chosen - leaves the first frames as empty as
    // having no warm-up at all.
    const auto warming = std::chrono::steady_clock::now();
    explorer.warmUp(start);
    std::cout << "coarse ground for the first frame: "
              << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                           warming)
                         .count()
              << " ms\n";
    camera.centreX = start.x.toDouble();
    camera.centreY = start.y.toDouble();
    camera.isometric = true;
    camera.focusHeight = explorer.field().groundAt(start).height.toDouble();
    // Far enough back to see country rather than ground: a couple of kilometres
    // across, which is what a judgement about terrain is made at.
    camera.pixelsPerTile = startZoom > 0 ? startZoom : 0.6;

    const double worldMetres = static_cast<double>(params.width) * generation::kMetresPerCell;
    camera.setBounds(0, 0, worldMetres, worldMetres);

    ui::Ui gui;
    if (!gui.init(sdl)) {
        std::cerr << "the interface could not build its font\n";
        return 1;
    }
    ui::Input pointer;
    CameraControl controls;

    struct Sample { double frameMillis, blank, coarse; std::size_t arrived; std::int32_t level; std::size_t wanted; };
    std::vector<Sample> measured;

    bool running = true;
    Uint64 last = SDL_GetTicks();
    while (running) {
        const auto frameStarted = std::chrono::steady_clock::now();
        SDL_GetRenderOutputSize(sdl, &camera.viewportWidth, &camera.viewportHeight);
        // Far enough back to hold the whole world, whatever the window is.
        camera.minZoom = std::min(camera.viewportWidth / worldMetres,
                                  camera.viewportHeight * 2.0 / worldMetres) * 0.9;

        pointer.wheel = 0;
        pointer.pressed = pointer.released = false;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) running = false;
            if (event.type == SDL_EVENT_MOUSE_WHEEL) pointer.wheel += event.wheel.y;
            if (event.type == SDL_EVENT_KEY_DOWN) {
                const SDL_Keycode key = event.key.key;
                if (key == SDLK_ESCAPE) running = false;
                // The landmarks, by number: somewhere worth looking at without
                // having to find it by dragging across a continent.
                const std::size_t which = key >= SDLK_1 && key <= SDLK_9
                                                  ? static_cast<std::size_t>(key - SDLK_1)
                                                  : explorer.landmarks().size();
                if (which < explorer.landmarks().size()) {
                    camera.centreX = explorer.landmarks()[which].where.x.toDouble();
                    camera.centreY = explorer.landmarks()[which].where.y.toDouble();
                }
                if (key == SDLK_HOME) {
                    camera.centreX = start.x.toDouble();
                    camera.centreY = start.y.toDouble();
                    camera.pixelsPerTile = 8.0;
                }
            }
        }

        float mouseX = 0, mouseY = 0;
        const SDL_MouseButtonFlags buttons = SDL_GetMouseState(&mouseX, &mouseY);
        pointer.mouseX = mouseX;
        pointer.mouseY = mouseY;
        pointer.down = (buttons & SDL_BUTTON_LMASK) != 0;
        pointer.middleDown = (buttons & SDL_BUTTON_MMASK) != 0;
        pointer.rightDown = (buttons & SDL_BUTTON_RMASK) != 0;

        const Uint64 now = SDL_GetTicks();
        const double seconds = std::clamp((now - last) / 1000.0, 0.0, 0.1);
        last = now;

        const bool* keys = SDL_GetKeyboardState(nullptr);
        controls.update(camera, pointer, keys, seconds, false);
        camera.clampTo(static_cast<int>(worldMetres), static_cast<int>(worldMetres));
        const core::WorldPos focus{
                Fixed::fromDoubleForContent(camera.centreX),
                Fixed::fromDoubleForContent(camera.centreY)};
        camera.focusHeight = explorer.field().groundAt(focus).height.toDouble() + camera.heightOffset;

        explorer.update(camera, seconds);
        SDL_SetRenderDrawColorFloat(sdl, 0.04f, 0.05f, 0.07f, 1.0f);
        SDL_RenderClear(sdl);
        explorer.draw(sdl, art, camera);

        // What is on screen, said in words. This is a tool for judging terrain,
        // and half of judging it is knowing what you are looking at.
        gui.begin(pointer, camera.viewportWidth, camera.viewportHeight);
        const std::vector<std::string> lines =
                explorer.status(camera, static_cast<int>(mouseX), static_cast<int>(mouseY));
        float widest = 0;
        for (const std::string& line : lines) widest = std::max(widest, gui.textWidth(line, 0.95f));
        const ui::Rect panel{10, 10, widest + 24, 26.0f + lines.size() * 15};
        gui.panel(panel);
        float y = panel.y + 10;
        for (const std::string& line : lines) {
            gui.text(panel.x + 12, y, line, gui.theme().label, 0.95f);
            y += 15;
        }
        std::string keysLine = "1-4 jump to";
        for (std::size_t i = 0; i < explorer.landmarks().size(); ++i)
            keysLine += (i ? ", " : " ") + explorer.landmarks()[i].name;
        keysLine += "   Home reset; wheel zoom; MMB pan; RMB orbit; Q/E yaw; Z/X tilt; PgUp/Dn height";
        gui.text(panel.x + 12, y, keysLine, gui.theme().labelSoft, 0.9f);
        gui.end();

        if (measuring) {
            // A flight over the world with the numbers taken: pan, then zoom
            // out to the whole continent and back in. What is being asked is
            // whether a frame ever waits and whether the screen is ever empty.
            const int frame = static_cast<int>(measured.size());
            if (frame >= 300) {
                double worst = 0, total = 0, worstBlank = 0, worstCoarse = 0;
                std::size_t built = 0;
                for (const Sample& sample : measured) {
                    worst = std::max(worst, sample.frameMillis);
                    total += sample.frameMillis;
                    worstBlank = std::max(worstBlank, sample.blank);
                    worstCoarse = std::max(worstCoarse, sample.coarse);
                    built += sample.arrived;
                }
                std::printf("flight over %zu frames: %.1f ms average, %.1f ms worst\n",
                            measured.size(), total / measured.size(), worst);
                std::printf("  drawn as flat country colour: worst %.1f%% of the view\n",
                            worstBlank * 100);
                std::printf("  drawn coarser than asked: worst %.1f%% of the view\n",
                            worstCoarse * 100);
                std::printf("  %zu patches arrived, %zu still on order at the end\n", built,
                            explorer.onOrder());
                std::vector<std::size_t> order(measured.size());
                for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return measured[a].frameMillis > measured[b].frameMillis;
                });
                for (std::size_t i = 0; i < 5 && i < order.size(); ++i) {
                    const Sample& sample = measured[order[i]];
                    std::printf("  slowest: frame %zu, %.1f ms, %zu arrived, %.0f%% blank\n",
                                order[i], sample.frameMillis, sample.arrived, sample.blank * 100);
                }
                std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
                    return measured[a].blank > measured[b].blank;
                });
                for (std::size_t i = 0; i < 5 && i < order.size(); ++i) {
                    const Sample& sample = measured[order[i]];
                    if (sample.blank <= 0) break;
                    std::printf("  flattest: frame %zu, %.0f%% flat, level %d, %zu wanted, %.1f ms\n",
                                order[i], sample.blank * 100, sample.level, sample.wanted,
                                sample.frameMillis);
                }
                gui.shutdown();
                return 0;
            }
            // Pan for a hundred frames, pull back for a hundred, come in again.
            // A third of the view every frame is a camera being thrown about
            // far harder than a hand can throw it, and the zoom crosses eight
            // levels of detail in a second.
            const double across = camera.viewportWidth / std::max(0.001, camera.pixelsPerTile);
            if (frame == 0 && closeUp) camera.pixelsPerTile = kMaxPixelsPerTile;
            if (frame < 100) camera.centreX += across / 3;
            else if (frame < 200 && !closeUp) camera.pixelsPerTile *= 0.90;
            else if (!closeUp) camera.pixelsPerTile /= 0.90;
            else camera.centreY += across / 3;
            // Held between the same stops the wheel is held between, or the
            // flight sails out past the world and measures nothing.
            camera.pixelsPerTile = std::clamp(camera.pixelsPerTile, camera.minZoom,
                                              static_cast<double>(kMaxPixelsPerTile));
        }

        // A picture waits for the ground it asked for, not merely for something
        // to be on screen: the coarse stand-in arrives in a frame or two, and a
        // shot taken then is a photograph of the loading.
        if (!shotPath.empty() && !explorer.stillFillingIn() && explorer.coarseShare() < 0.02 &&
            explorer.onOrder() == 0) {
            SDL_Surface* shot = SDL_RenderReadPixels(sdl, nullptr);
            if (!shot || !IMG_SavePNG(shot, shotPath.c_str())) {
                std::cerr << "could not write " << shotPath << ": " << SDL_GetError() << "\n";
                return 1;
            }
            SDL_DestroySurface(shot);
            std::cout << "wrote " << shotPath << "\n";
            gui.shutdown();
            return 0;
        }
        SDL_RenderPresent(sdl);
        if (measuring)
            measured.push_back({std::chrono::duration<double, std::milli>(
                                        std::chrono::steady_clock::now() - frameStarted)
                                        .count(),
                                explorer.blankShare(), explorer.coarseShare(),
                                explorer.arrived(), explorer.lastLevel(), explorer.lastWanted()});
    }
    gui.shutdown();
    (void)window;
    return 0;
}

} // namespace client
