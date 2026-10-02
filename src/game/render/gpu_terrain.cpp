#include "game/render/gpu_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include "game/client/camera.hpp"
#include "game/world/terrain_grid.hpp"
#include "game/world/ring_mesh.hpp"

namespace game {
namespace {
std::filesystem::path terrainConfigFile() {
    std::filesystem::path content = "content";
    std::error_code ec;
    for (int up = 0; up < 5 && !std::filesystem::exists(content, ec); ++up) content = ".." / content;
    return content / "config" / "terrain.json";
}
}

GpuTerrain::GpuTerrain(std::shared_ptr<world::WorldPreparation> preparation)
    : preparation_(std::move(preparation)), world_(preparation_->world), configWatch_(terrainConfigFile()) {
    skirts_ = std::getenv("ASR_TERRAIN_NO_SKIRTS") == nullptr;
    // Pass setup calls ensure() before the first update(). Restart-only values
    // must already be loaded when atlases, topology and pipelines are created.
    std::string error;
    if (configWatch_.poll(config_, error))
        std::fprintf(stderr, "terrain config: %s loaded before GPU setup\n", configWatch_.file().string().c_str());
    else if (!error.empty())
        std::fprintf(stderr, "terrain config: %s: %s; using defaults\n", configWatch_.file().string().c_str(), error.c_str());
}

void GpuTerrain::reset() {
    stream_.reset(); // join before the borrowed PageStore is destroyed
    plan_.reset(); preparingStage_.reset(); residency_.reset();
    stageFrom_=stageTo_=generation::TerrainStage::Final;
    stageAmount_=1; stageStarted_=true;
    displayAmount_ = 1;
    drawing_.clear();
    gathered_ = {};
    resolved_.clear();
    meshes_.clear();
    leases_.clear(); persistent_.clear(); known_.clear();
    inflight_.clear(); // the atlases go below, with every pin they had
    stale_.clear(); groundSeen_ = 0; restream_ = false;
    staged_.clear();
    for (auto& atlas : atlases_) atlas.reset();
    table_.reset(); tableSampler_.reset();
    climate_ = ClimateTextures{};
    revision_ = 0; target_ = -1; haveCamera_ = false;
    progress_ = {};
    persistentTotal_ = gpuBytes_ = 0;
    fineResident_ = h8Resident_ = missing_ = coarse_ = 0;
    planTime_ = 0; lastDrawSerial_ = 0;
    settled_ = false;
    tableDirty_ = true; restartPlan_ = false; lastView_ = {};
}

bool GpuTerrain::ensure(engine::Device& device) {
    if (revision_ && table_) return true;
    reset();
    const auto& world = world_->worldMap();
    const int wide = (world.width * generation::kMetresPerCell + 511) / 512;
    const int high = (world.height * generation::kMetresPerCell + 511) / 512;
    tableWidth_ = tableHeight_ = kTableSide;
    // Kept from the world before, and only what it wrote cleared: allocating
    // it afresh was a quarter of a second of page faults on every rebuild.
    if (tableMirror_.size() != std::size_t(kTableSide) * kTableSide * kTableLayers)
        tableMirror_.assign(std::size_t(kTableSide) * kTableSide * kTableLayers, {0, 0, 0, 0});
    for (const auto& [texel, key] : tableOwner_) tableMirror_[texel] = {0, 0, 0, 0};
    tableOwner_.clear();
    tableUploaded_ = false;
    // How many H64 pages the land needs, for the progress read-out: by 8 km
    // squares first, so a world 2000 km a side of open sea is thousands of
    // questions and not sixteen million.
    std::size_t land = 0;
    const auto& mask = world_->pages().landMask();
    for (int sy = -1; sy * 16 < high + 2; ++sy)
        for (int sx = -1; sx * 16 < wide + 2; ++sx) {
            if (!mask.anyLandInWorldRect(sx * 8192 - 64, sy * 8192 - 64, sx * 8192 + 8192 + 64, sy * 8192 + 8192 + 64))
                continue;
            for (int y = std::max(-2, sy * 16); y < std::min(high + 2, sy * 16 + 16); ++y)
                for (int x = std::max(-2, sx * 16); x < std::min(wide + 2, sx * 16 + 16); ++x)
                    if (world_->pages().containsLand({x, y, 4})) ++land;
        }
    persistentTotal_ = land;
    // H64 is held for the window around the camera, not for the world: the
    // land of a world a thousand kilometres long is a million and a half of
    // these, gigabytes on the card, for a view that reaches forty. A hundred
    // and ninety-two a side covers any window at the widest draw distance
    // over land that is not all land; past that, the farthest are let go.
    constexpr std::uint32_t kCoarseSide = 192;
    const auto columns = std::min(kCoarseSide,
            static_cast<std::uint32_t>(std::ceil(std::sqrt(double(std::max<std::size_t>(1, land))))));
    constexpr std::array<int, 4> steps{4, 8, 16, 64};
    for (std::size_t i = 0; i < atlases_.size(); ++i) {
        const auto side = i == 0 ? 1u : i == 1 ? unsigned(config_.h8AtlasSide) :
            i == 2 ? std::min(columns,unsigned(config_.h8AtlasSide)*2) : columns;
        atlases_[i] = std::make_unique<HeightPageAtlas>(device,
            HeightPageAtlas::Layout(steps[i], side, side), true);
        if (!*atlases_[i]) return false;
        const auto& layout = atlases_[i]->layout();
        gpuBytes_ += std::size_t(layout.storedSamples()) * layout.storedSamples() *
                     layout.capacity() * 34;
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = tableWidth_; info.height = tableHeight_;
    info.layer_count_or_depth = kTableLayers; info.num_levels = 1;
    gpuBytes_ += std::size_t(tableWidth_) * tableHeight_ * kTableLayers * 16;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    table_ = device.makeTexture(info);
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    tableSampler_ = device.makeSampler(sampler);
    if (!table_ || !tableSampler_ || !climate_.ensure(device, world_->climate())) return false;
    if (!biomes_.ensure(device, world_->climate())) return false;
    if (!vertices_ || !indices_ || gridCells_ != config_.chunkCells) {
        const auto grid = world::terrain::makeGridTopology(config_.chunkCells);
        vertices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, grid.vertices.data(), grid.vertices.size() * sizeof(world::terrain::GridVertex));
        indices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, grid.indices.data(), grid.indices.size() * sizeof(std::uint16_t));
        indexCount_ = static_cast<std::uint32_t>(grid.indices.size());
        surfaceIndexCount_ = std::uint32_t(grid.cells) * grid.cells * 6;
        gridCells_ = grid.cells;
    }
    if (!vertices_ || !indices_) return false;
    const auto& quant = world_->pages().quantisation();
    low_ = static_cast<float>(quant.low.toDouble());
    range_ = static_cast<float>((quant.high - quant.low).toDouble());
    stream_ = std::make_unique<HeightPageStream>(world_->pages());
    preparation_->surface.configure(config_);
    auto residency = std::make_shared<world::terrain::TerrainResidency>();
    for (std::size_t i = 0; i < 3; ++i) residency->capacity[i] = atlases_[i]->layout().capacity();
    residency->capacity[0] = 0; // no full H4 pages in the regional renderer
    residency_ = std::move(residency);
    activeH8AtlasSide_ = config_.h8AtlasSide;
    activeChunkMetres_ = config_.chunkMetres;
    // Whatever was dug before now is in every page the stream will bake.
    if (const auto* edits = world_->edits()) groundSeen_ = edits->revision();
    revision_ = 1;
    tableDirty_ = !publishTable(device);
    return !tableDirty_;
}

void GpuTerrain::protect(Key key, std::uint64_t serial) {
    auto& atlas = *atlases_[atlasOf(key.level)];
    if (leases_.empty() || leases_.back().serial != serial || leases_.back().plan)
        leases_.push_back({serial, {}, {}});
    if (!leases_.back().keys.contains(key) && atlas.pin(key, serial)) leases_.back().keys.insert(key);
}

void GpuTerrain::observeGround() {
    const auto* edits = world_->edits();
    if (!edits || edits->revision() == groundSeen_) return;
    std::vector<core::WorldRect> changed;
    groundSeen_ = edits->changedSince(groundSeen_, changed);
    if (changed.empty()) return;
    const auto& pages = world_->pages();
    for (const auto key : known_) {
        if (stale_.contains(key)) continue;
        const auto reach = pages.reach(key);
        if (std::any_of(changed.begin(), changed.end(), [&](const auto& area) { return area.overlaps(reach); }))
            stale_.insert(key);
    }
    restream_ = true;
}
void GpuTerrain::retire(std::uint64_t completed) {
    while (!leases_.empty() && leases_.front().serial <= completed) {
        for (const auto key : leases_.front().keys) atlases_[atlasOf(key.level)]->unpin(key);
        if (const auto& plan = leases_.front().plan)
            for (auto key : plan->pins) atlases_[atlasOf(key.level)]->unpin(key);
        leases_.pop_front();
    }
    std::unordered_set<const world::terrain::AdaptiveMesh*> held;
    if (plan_) for (const auto& block : plan_->coverage) held.insert(block.mesh.get());
    // Buffers no cut draws stay on the card, oldest out first, within the
    // config's budget: a mesh the CPU kept in its cache comes back drawn at
    // once instead of uploaded again. They used to go at the first frame the
    // cut stopped drawing them.
    struct Idle { std::uint64_t serial; const world::terrain::AdaptiveMesh* key; std::size_t bytes; };
    std::vector<Idle> idle;
    std::size_t idleBytes = 0;
    for (const auto& [key, buffers] : meshes_) {
        const std::size_t bytes = key->vertices.size() * sizeof(world::terrain::AdaptiveVertex) +
                                  key->indices.size() * sizeof(std::uint32_t);
        if (held.contains(key) || buffers.lastSerial > completed) continue;
        idle.push_back({buffers.lastSerial, key, bytes});
        idleBytes += bytes;
    }
    if (idleBytes > config_.gpuMeshCacheBytes) {
        std::sort(idle.begin(), idle.end(), [](const Idle& a, const Idle& b) { return a.serial < b.serial; });
        for (const auto& i : idle) {
            if (idleBytes <= config_.gpuMeshCacheBytes) break;
            meshes_.erase(i.key);
            idleBytes -= i.bytes;
        }
    }
}

bool GpuTerrain::requestPlan(const world::terrain::TerrainView& view) {
    auto& surface = preparation_->surface;
    if (!surface.request(view, residency_, planTime_, restartPlan_)) return false;
    restartPlan_ = false;
    // One plan in flight at a time (both callers wait for !busy()), so the
    // previous hold - if a result was never collected - is this one's now.
    releaseInflight();
    inflight_.reserve(residency_->pages.size());
    for (const auto key : residency_->pages)
        if (atlases_[atlasOf(key.level)]->hold(key)) inflight_.push_back(key);
    return true;
}

void GpuTerrain::releaseInflight() {
    for (const auto key : inflight_) atlases_[atlasOf(key.level)]->unpin(key);
    inflight_.clear();
}

bool GpuTerrain::accept(engine::Device& device, std::shared_ptr<const Plan> plan) {
    lostPages_ = false;
    if (plan && (plan->view.stageFrom!=lastView_.stageFrom || plan->view.stageTo!=lastView_.stageTo)) return false;
    if (plan && plan_ && (plan_->view.stageFrom!=plan->view.stageFrom || plan_->view.stageTo!=plan->view.stageTo) &&
        (plan->blank || plan->deferredRegions || std::any_of(plan->coverage.begin(),plan->coverage.end(),
            [](const auto& block){return !block.mesh;}))) {
        preparingStage_=std::move(plan);
        return false; // keep old complete coverage while workers build the new endpoints
    }
    // A perspective snapshot may lag the moving eye. Its complete cut is safe
    // to re-cull now only when ALL dependencies are still resident. An older
    // residency revision with additive arrivals is not lost page data.
    if (!plan || tableDirty_ || (plan_ && plan->sequence <= plan_->sequence)) return false;
    if (!plan->compatible(*residency_)) {
        lostPages_ = true;
        if (std::getenv("ASR_TERRAIN_UNDERFOOT"))
            std::fprintf(stderr, "underfoot plan refused: pins no longer resident -> restart from the roots\n");
        return false;
    }
    // One copy submission for all new immutable topology buffers. Never replace
    // a cut with partially uploaded geometry; the old cut and its fences survive.
    std::vector<MeshBuffers> uploaded;
    engine::Device::Uploader upload(device);
    for (const auto& block : plan->coverage) {
        if (!block.mesh) return false;
        if (meshes_.contains(block.mesh.get())) continue;
        MeshBuffers buffers;
        buffers.source = block.mesh;
        buffers.highBed = -1e9f;
        const auto& bed = block.mesh->bed;
        const auto& head = block.mesh->head;
        for (std::size_t v = 0; v < bed.size(); ++v) {
            buffers.highBed = std::max(buffers.highBed, bed[v]);
            if (v < head.size() && head[v] > bed[v] + 0.05f && head[v] > 0.25f) buffers.inland = true;
        }
        if (bed.empty()) { buffers.highBed = 1e9f; buffers.inland = true; }
        buffers.vertices = upload.add(SDL_GPU_BUFFERUSAGE_VERTEX,block.mesh->vertices.data(),
            block.mesh->vertices.size()*sizeof(world::terrain::AdaptiveVertex));
        buffers.indices = upload.add(SDL_GPU_BUFFERUSAGE_INDEX,block.mesh->indices.data(),
            block.mesh->indices.size()*sizeof(std::uint32_t));
        if (!buffers.vertices || !buffers.indices) return false;
        uploaded.push_back(std::move(buffers));
    }
    if (!upload.finish()) return false;
    for (auto& buffers : uploaded) {
        const auto* key = buffers.source.get();
        meshes_.emplace(key,std::move(buffers));
    }
    std::size_t pinned = 0;
    for (auto key : plan->pins) {
        auto& atlas = *atlases_[atlasOf(key.level)];
        if (!atlas.find(key) || !atlas.pin(key, serial_)) {
            for (std::size_t i = 0; i < pinned; ++i)
                atlases_[atlasOf(plan->pins[i].level)]->unpin(plan->pins[i]);
            lostPages_ = true;
            return false;
        }
        ++pinned;
    }
    // ASR_TERRAIN_UNDERFOOT=1: one line per accepted plan that changes the
    // square under the eye - its tile, its built mesh, its stitched copy, its
    // morph - and whether the cut was restarted or lost pages. Diagnostic only.
    static const bool underfoot = std::getenv("ASR_TERRAIN_UNDERFOOT") != nullptr;
    if (underfoot) {
        const auto under = [&](const Plan* p) -> const Block* {
            if (!p) return nullptr;
            for (const auto& b : p->coverage)
                if (lastX_ >= b.bounds.minX && lastX_ < b.bounds.maxX && lastY_ >= b.bounds.minY && lastY_ < b.bounds.maxY)
                    return &b;
            return nullptr;
        };
        const auto base = [](const Block* b) -> const void* {
            return !b || !b->mesh ? nullptr : b->mesh->unstitched ? b->mesh->unstitched.get() : b->mesh.get();
        };
        const Block* was = under(plan_.get());
        const Block* now = under(plan.get());
        const bool tile = !was || !now || !(was->tile == now->tile);
        const bool built = !tile && base(was) != base(now);
        const bool stitched = !tile && !built && was->mesh != now->mesh;
        const bool morph = !tile && was->parentMorph != now->parentMorph;
        // How far the ground visibly travels while this plan's display
        // transition plays, near the eye: |displayFrom - what is now shown|,
        // and how far the plan's own morph phases move it.
        double jump30 = 0, jump128 = 0;
        std::size_t uploads = 0, nearUploads = 0;
        for (const auto& b : plan->coverage) {
            if (!b.mesh) continue;
            const bool fresh = !meshes_.contains(b.mesh.get());
            uploads += fresh;
            const double cx = std::clamp(lastX_, b.bounds.minX, b.bounds.maxX) - lastX_;
            const double cy = std::clamp(lastY_, b.bounds.minY, b.bounds.maxY) - lastY_;
            if (cx * cx + cy * cy > 128.0 * 128.0) continue;
            nearUploads += fresh;
            const auto& m = *b.mesh;
            for (const auto& v : m.vertices) {
                if (!(v.skirt & 8)) continue;
                const double dx = b.tile.x * double(b.metres()) + v.x * m.step - lastX_;
                const double dy = b.tile.y * double(b.metres()) + v.y * m.step - lastY_;
                const double d2 = dx * dx + dy * dy;
                if (d2 > 128.0 * 128.0) continue;
                const auto i = std::size_t(v.y) * (m.cells + 1) + v.x;
                const double shown = (v.skirt & 2) ? v.edgeBed : std::lerp(double(m.bed[i]), double(v.parentBed), double(b.parentMorph));
                const double moved = std::abs(double(v.displayFrom[0]) - shown);
                jump128 = std::max(jump128, moved);
                if (d2 <= 30.0 * 30.0) jump30 = std::max(jump30, moved);
            }
        }
        if (tile || built || stitched || morph || jump30 > 0.01)
            std::fprintf(stderr, "underfoot x=%.1f y=%.1f %s%s%s%s lod %d->%d tile %d,%d->%d,%d morph %.2f->%.2f "
                         "step %.0f->%.0f interpolated=%d coverage=%zu missing=%zu built=%zu restart=%d "
                         "moves30=%.3f moves128=%.3f uploads=%zu near-uploads=%zu\n",
                         lastX_, lastY_, tile ? "TILE " : "", built ? "REBUILT " : "", stitched ? "RESTITCHED " : "",
                         morph ? "MORPH " : "", was ? was->tile.lod : -1, now ? now->tile.lod : -1,
                         was ? was->tile.x : 0, was ? was->tile.y : 0, now ? now->tile.x : 0, now ? now->tile.y : 0,
                         was ? was->parentMorph : 0.0f, now ? now->parentMorph : 0.0f,
                         was && was->mesh ? was->mesh->step : 0.0, now && now->mesh ? now->mesh->step : 0.0,
                         int(plan->interpolated), plan->coverage.size(), plan->missing.size(), plan->meshesBuilt,
                         int(restartPlan_), jump30, jump128, uploads, nearUploads);
    }
    // Keep the old snapshot AND its pins alive through its last GPU reader.
    if (plan_) leases_.push_back({lastDrawSerial_, {}, std::move(plan_)});
    plan_ = std::move(plan);
    displayAmount_ = plan_->interpolated ? 0.0f : 1.0f;
    preparingStage_.reset();
    if (plan_->view.stageFrom==stageFrom_ && plan_->view.stageTo==stageTo_) stageStarted_=true;
    return true;
}

bool GpuTerrain::publishTable(engine::Device& device) {
    const std::size_t side = kTableSide, plane = side * side;
    const auto wrap = [](std::int64_t v) { return std::size_t(((v % std::int64_t(kTableSide)) + kTableSide) % kTableSide); };
    const auto texelOf = [&](Key key, int layer) { return std::uint64_t(layer) * plane + wrap(key.y) * side + wrap(key.x); };
    // What should be where now. Two pages on one texel (a wrap the side is too
    // small for) keep the one nearer the camera; the other reads as absent.
    std::unordered_map<std::uint64_t, std::pair<Key, std::array<float, 4>>> wanted;
    wanted.reserve(known_.size());
    const auto distance = [&](Key key) {
        const double metres = world::streaming::pageMetresAtLevel(key.level);
        const double dx = (key.x + 0.5) * metres - lastX_, dy = (key.y + 0.5) * metres - lastY_;
        return dx * dx + dy * dy;
    };
    for (auto it = known_.begin(); it != known_.end();) {
        const auto key = *it;
        const auto address = atlases_[atlasOf(key.level)]->find(key);
        if (!address) { stale_.erase(key); it = known_.erase(it); continue; }
        const int layer = dataset(key.level);
        // uv of the page's first sample, uv per metre (the atlases are
        // square), and which turn of the wrap the page is on - a quarter past
        // the integer, so an empty texel is never turn nought's
        // (page_levels.hlsli, pageEntryFrom).
        const auto turn = [](std::int64_t v) {
            return std::int64_t(std::floor(double(v) / double(kTableSide)));
        };
        const std::array<float, 4> entry{address->uvOrigin[0], address->uvOrigin[1], address->uvPerMetre[0],
                                         float(turn(key.x) * 1024 + turn(key.y)) + 0.25f};
        const auto texel = texelOf(key, layer);
        const auto [slot, fresh] = wanted.try_emplace(texel, key, entry);
        if (!fresh && distance(key) < distance(slot->second.first)) slot->second = {key, entry};
        ++it;
    }
    // The texels that changed, and the rows they are in.
    std::array<std::pair<std::size_t, std::size_t>, kTableLayers> rows;
    rows.fill({side, 0});
    const auto touch = [&](std::uint64_t texel, const std::array<float, 4>& value) {
        tableMirror_[texel] = value;
        const auto layer = std::size_t(texel / plane), row = std::size_t((texel % plane) / side);
        rows[layer] = {std::min(rows[layer].first, row), std::max(rows[layer].second, row + 1)};
    };
    for (auto it = tableOwner_.begin(); it != tableOwner_.end();) {
        if (wanted.contains(it->first)) { ++it; continue; }
        touch(it->first, {0, 0, 0, 0});
        it = tableOwner_.erase(it);
    }
    for (const auto& [texel, value] : wanted) {
        if (tableMirror_[texel] == value.second) continue;
        touch(texel, value.second);
        tableOwner_[texel] = value.first;
    }
    engine::Device::Uploader upload(device);
    bool ok = true;
    for (std::uint32_t layer = 0; layer < kTableLayers; ++layer) {
        // The first time the whole of it (and cycled: nothing has read it yet);
        // after that only the rows that changed, in submission order behind
        // the draws that read the old ones.
        const auto [from, to] = tableUploaded_ ? rows[layer] : std::pair<std::size_t, std::size_t>{0, side};
        if (from >= to) continue;
        ok = upload.refillRegion(table_.get(), tableMirror_.data() + layer * plane + from * side, 0,
                                 std::uint32_t(from), std::uint32_t(side), std::uint32_t(to - from), 16, layer, 0,
                                 !tableUploaded_ && layer == 0) && ok;
    }
    ok = upload.finish() && ok;
    // A failed upload leaves the card behind the mirror: the whole of it next time.
    tableUploaded_ = ok;
    return ok;
}

void GpuTerrain::update(engine::Frame& frame, const client::Camera& camera) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    std::string configError;
    if (configWatch_.poll(config_, configError, started)) {
        std::fprintf(stderr, "terrain config: %s loaded; detail-distance=%.2f morph-seconds=%.3f mesh-builds-per-batch=%zu preload-pages=%zu\n",
            configWatch_.file().string().c_str(), config_.detailDistanceScale, config_.morphSeconds,
            config_.meshesPerPlan, config_.preloadPages);
        if (revision_ && (gridCells_ != config_.chunkCells || activeChunkMetres_ != config_.chunkMetres ||
            activeH8AtlasSide_ != config_.h8AtlasSide))
            std::fprintf(stderr, "terrain config: chunk_metres / chunk_metres_per_lod / chunk_cells / h8_atlas_side require world restart; cells/atlas active=%d/%d requested=%d/%d\n",
                gridCells_, activeH8AtlasSide_, config_.chunkCells, config_.h8AtlasSide);
    } else if (!configError.empty()) {
        std::fprintf(stderr, "terrain config: %s: %s; keeping previous settings\n",
            configWatch_.file().string().c_str(), configError.c_str());
    }
    if (!ensure(*frame.device)) { tableDirty_ = true; progress_.uploadFailed = true; return; }
    biomes_.refresh(*frame.device);
    serial_ = frame.device->nextSubmission();
    retire(frame.device->completedSubmission());
    stream_->frozen(frozen_);
    planTime_ += std::clamp(frame.step, 0.0, 0.1);
    displayAmount_ = std::min(1.0f, displayAmount_ + float(std::clamp(frame.step, 0.0, 0.1) /
        std::max(1.0/60.0, std::min(0.1, config_.morphSeconds))));
    if (stageStarted_) stageAmount_=std::min(1.0f,stageAmount_+float(std::clamp(frame.step,0.0,0.1)/std::max(0.15,config_.morphSeconds)));
    const auto wantedStage=world_->worldMap().terrainFoundation ? requestedStage_ : generation::TerrainStage::Final;
    if (wantedStage!=stageTo_ && displayAmount_>=1 && (!stageStarted_ || stageAmount_>=1)) {
        if (stageStarted_) stageFrom_=stageTo_;
        stageTo_=wantedStage; stageAmount_=0; stageStarted_=false;
        preparingStage_.reset(); restartPlan_=true;
    }
    world::terrain::TerrainView view;
    view.stageFrom=stageFrom_; view.stageTo=stageTo_;
    view.config = config_;
    view.config.chunkCells = gridCells_;
    view.config.chunkMetres = activeChunkMetres_;
    view.config.h8AtlasSide = activeH8AtlasSide_;
    camera.viewProjection(view.matrix.data(), 0, 1);
    view.width = camera.viewportWidth; view.height = camera.viewportHeight;
    // Orthographic zoom uses the actual projection, including tilted dimetric
    // views. In perspective this is only a focus diagnostic; each block chooses
    // its own mesh LOD from depth/FOV/viewport in TerrainView::refine().
    const double mpp = view.lodMetresPerPixel({camera.centreX, camera.centreY,
        camera.centreX, camera.centreY, camera.focusHeight, camera.focusHeight});
    target_ = world::terrain::geometryLevelFor(mpp, target_, config_.lod);
    view.x = camera.centreX; view.y = camera.centreY;
    if (camera.perspective()) {
        const auto eye = camera.eyePosition();
        view.x = eye[0]; view.y = eye[1];
        const int px=int(std::floor(eye[0]/512)),py=int(std::floor(eye[1]/512));
        if (const auto it=residency_->surfaces.find({px,py,2});it!=residency_->surfaces.end()) {
            const auto& p=*it->second;
            const int ix=std::clamp(int(std::lround((eye[0]-px*512)/p.step))+p.padding,0,p.side-1);
            const int iy=std::clamp(int(std::lround((eye[1]-py*512)/p.step))+p.padding,0,p.side-1);
            const double altitude=eye[2]-p.bed[std::size_t(iy)*p.side+ix];
            view.localDetail=altitude>=-8 && altitude<=config_.localDetailHeightMetres;
        }
    }
    view.target = target_;
    if (!haveCamera_ || view.matrix != lastView_.matrix || view.width != lastView_.width ||
        view.height != lastView_.height || view.target != lastView_.target) {
        radius_ = camera.farPlane;
        if (!camera.perspective()) {
            const auto& map = world_->worldMap();
            auto heights = world::ringHeightBounds(map, {},
                {core::Fixed::fromInt(std::int64_t(map.width) * generation::kMetresPerCell),
                 core::Fixed::fromInt(std::int64_t(map.height) * generation::kMetresPerCell)}, target_);
            for (int refinement = 0; refinement < (camera.isometric ? 2 : 1); ++refinement) {
                double minX = std::numeric_limits<double>::max(), minY = minX, maxX = -minX, maxY = -minX;
                radius_ = world::sampleMetresAt(target_);
                for (const auto [sx, sy] : std::array<std::pair<int, int>, 4>{{{0, 0}, {camera.viewportWidth, 0},
                         {camera.viewportWidth, camera.viewportHeight}, {0, camera.viewportHeight}}})
                    for (double height : {heights.first, heights.second}) {
                        double x, y;
                        camera.worldOfScreenAtHeight(sx, sy, height, x, y);
                        radius_ = std::max(radius_, std::hypot(x - camera.centreX, y - camera.centreY));
                        minX = std::min(minX, x); minY = std::min(minY, y);
                        maxX = std::max(maxX, x); maxY = std::max(maxY, y);
                    }
                if (camera.isometric && refinement == 0)
                    heights = world::ringHeightBounds(map,
                        {core::Fixed::fromDoubleForContent(minX), core::Fixed::fromDoubleForContent(minY)},
                        {core::Fixed::fromDoubleForContent(maxX), core::Fixed::fromDoubleForContent(maxY)}, target_);
            }
        }
    }
    view.radius = radius_;
    // An orthographic view has no fog to hide the edge: its window is what
    // it shows, with a margin, and never more than a hundred and twenty
    // kilometres of it.
    view.window = window_ <= 0 ? 0.0 : camera.perspective() ? window_ : std::min(radius_ * 1.1 + 8192.0, 120000.0);
    view.windowX = camera.centreX;
    view.windowY = camera.centreY;
    const double lookAhead = std::min(radius_,config_.maxLookAheadMetres);
    // Where the eye is going, from its velocity eased over a few frames: the
    // raw step over one frame's time jumped by up to the whole look-ahead
    // with every uneven frame, and every jump was a new view to plan.
    if (haveCamera_ && frame.step > 0) {
        const double ease = 0.15;
        velocityX_ += ((view.x - lastX_) / frame.step - velocityX_) * ease;
        velocityY_ += ((view.y - lastY_) / frame.step - velocityY_) * ease;
    }
    const auto snapped = [](double v) { return std::round(v / 16.0) * 16.0; };
    view.lookX = view.x + snapped(std::clamp(velocityX_ * config_.lookAheadSeconds, -lookAhead, lookAhead));
    view.lookY = view.y + snapped(std::clamp(velocityY_ * config_.lookAheadSeconds, -lookAhead, lookAhead));
    auto prediction = camera;
    prediction.centreX += view.lookX - view.x; prediction.centreY += view.lookY - view.y;
    prediction.viewProjection(view.prediction.data(), 0, 1);
    const bool viewChanged = view != lastView_;
    lastX_ = view.x; lastY_ = view.y; haveCamera_ = true;
    lastView_ = view;
    // Map requests retain latest-only cancellation. Continuous perspective
    // movement drains one complete snapshot before submitting the newest eye,
    // rather than invalidating every result just before collect().
    auto& surface = preparation_->surface;
    // Asking here holds every resident page until the plan is collected, and
    // the uploads below may need to let one go: with pages waiting for a slot
    // the request waits until after them (the second call, further down).
    if (displayAmount_ >= 1 && !surface.busy() && staged_.empty()) requestPlan(view);
    bool changed = false;
    if (auto completed = surface.collect()) {
        changed = accept(*frame.device,std::move(completed));
        // Accepted, its own pins now hold what it stands on; refused, nothing
        // it stood on is wanted. Either way the snapshot is let go.
        releaseInflight();
        // Started again from the roots only when pages the plan stood on are
        // gone. Every other refusal (an older plan, a mesh not built yet, a
        // stage being prepared) keeps the cut it has and refines on from it:
        // restarting on each of them dropped the whole view back to its
        // roots and built it up again, over and over.
        restartPlan_ = !changed && lostPages_;
    }
    if (plan_ && (changed || viewChanged)) {
        // Pins protect the complete accepted cut, not just its old viewport.
        // Re-cull cached bounds immediately; never wait for a new LOD job to
        // reveal the already resident H64 beside the previous camera position.
        plan_->selectDrawing(view, drawing_);
        coarse_ = plan_->coarse; // same base-source criterion, not refined mesh error
    }
#if ASR_ENABLE_PROFILING
    const auto planAt = Clock::now();
#endif
    const auto nearer = [&](Key a, Key b) {
        return HeightPageStream::nearer(a, b, view.x, view.y);
    };
    observeGround();
    auto arrived = stream_->collect();
    const bool arrivals = !arrived.empty();
    for (auto& page : arrived) staged_.push_back(std::move(page));
    if (changed || arrivals) std::stable_sort(staged_.begin(), staged_.end(), [&](const auto& a, const auto& b) {
        const auto ka = a.source->base.key, kb = b.source->base.key;
        // The coarsest first: what a wide view is drawn from arrives before
        // what refines it.
        if (ka.level >= 4 || kb.level >= 4) if (ka.level != kb.level) return ka.level > kb.level;
        const bool wa = plan_ && plan_->demanded.contains(ka), wb = plan_ && plan_->demanded.contains(kb);
        if (wa != wb) return wa;
        return nearer(ka, kb);
    });
    // Bound staging in bytes, not pages: H64 is about 5 KB, H4 about 600 KB.
    std::size_t stagedBytes = 0, keep = 0;
    for (const auto& page : staged_) {
        const auto cost = page.source->base.heightQuantized.size() * 34;
        if (stagedBytes + cost > std::max<std::size_t>(8u << 20, config_.uploadBytesPerFrame)) break;
        stagedBytes += cost; ++keep;
    }
    staged_.resize(keep);
    engine::Device::Uploader upload(*frame.device);
    std::vector<Key> uploaded;
    std::size_t bytes = 0;
    bool attempted = false;
    const auto& store = world_->pages();
    for (auto it = staged_.begin(); it != staged_.end();) {
        const auto& page = *it;
        const auto key = page.source->base.key;
        auto& atlas = *atlases_[atlasOf(key.level)];
        const auto& demand=preparingStage_?preparingStage_:plan_;
        const bool resident = atlas.find(key).has_value();
        const bool refresh = resident && stale_.contains(key);
        if (!demand || !demand->wanted.contains(key) || (resident && !refresh)) {
            it = staged_.erase(it); continue;
        }
        // Baked before the latest dig inside its reach: a picture of ground
        // that is gone. Asked for again; what is drawn meanwhile stays.
        if (!store.current(*page.source)) {
            restream_ = true;
            it = staged_.erase(it); continue;
        }
        const auto cost = page.source->base.heightQuantized.size() * 34;
        if (bytes && bytes + cost > config_.uploadBytesPerFrame) break;
        attempted = true;
        if (atlas.upload(upload, page, frame.index, refresh)) {
            protect(key, serial_); // includes provisional uploads: no same-batch eviction
            uploaded.push_back(key);
            bytes += cost;
            it = staged_.erase(it);
        } else {
            ++it; // pinned atlas: retain the packed page, keep drawing the parent
        }
    }
    const bool submitted = upload.finish();
#if ASR_ENABLE_PROFILING
    const auto uploadAt = Clock::now();
#endif
    if (attempted) for (auto& atlas : atlases_) atlas->publishUploads(submitted);
    if (submitted) for (const auto key : uploaded) {
        if (!atlases_[atlasOf(key.level)]->find(key)) continue;
        known_.insert(key);
        refreshed_ += stale_.erase(key);
        // H16/H8 are evictable after their last frame lease, just like meshes.
        // Counted, not pinned for good: an H64 page is held by the plans that
        // want it, like any other, and let go when the camera has left it.
        if (key.level >= 4) persistent_.insert(key);
    }
    // Even a failed partial upload may have evicted an old mapping.
    tableDirty_ = tableDirty_ || attempted || !submitted;
    const bool tableChanged = tableDirty_;
    if (tableDirty_) {
        tableDirty_ = !publishTable(*frame.device);
        if (tableDirty_) { progress_.uploadFailed = true; return; }
        // Snapshot only after atlas AND indirection table publication. Copying
        // residency is event-driven, not a per-frame walk of the whole world.
        auto next = std::make_shared<world::terrain::TerrainResidency>();
        next->revision = residency_->revision + 1;
        next->capacity = residency_->capacity;
        next->pages = known_;
        next->surfaces.reserve(known_.size());
        fineResident_ = h8Resident_ = 0;
        for (auto key : known_) {
            const auto& atlas = *atlases_[atlasOf(key.level)];
            if (!atlas.mayHaveWater(key)) next->dryPages.insert(key);
            if (auto surface = atlas.surface(key)) next->surfaces.emplace(key, std::move(surface));
            fineResident_ += key.level == 0;
            h8Resident_ += key.level == 1;
        }
        residency_ = std::move(next);
    }
#if ASR_ENABLE_PROFILING
    const auto tableAt = Clock::now();
#endif
    if (displayAmount_ >= 1 && !surface.busy() &&
        (restartPlan_ || !plan_ || plan_->dirty(view, residency_->revision)))
        requestPlan(view);

    // Queue replacement is also event-driven. GPU/staged pages must be removed
    // from an older worker snapshot before handing its requests to the stream.
    const bool retry = frame.index % 60 == 0 && plan_ && (!plan_->missing.empty() || !plan_->preload.empty() ||
                                                           !stale_.empty());
    if (plan_ && (changed || arrivals || tableChanged || retry || preparingStage_ || restream_)) {
        std::unordered_set<Key> stagedKeys;
        for (const auto& page : staged_) stagedKeys.insert(page.source->base.key);
        const auto queued = [&](Key key) {
            return (known_.contains(key) && !stale_.contains(key)) || stagedKeys.contains(key);
        };
        const auto& demand=preparingStage_?preparingStage_:plan_;
        auto requests = demand->missing, speculation = demand->preload;
        std::erase_if(requests, queued);
        std::erase_if(speculation, queued);
        // Pages drawn over ground that has moved since, after everything
        // missing outright: the old ground is still drawn, a hole is not.
        std::vector<Key> refresh;
        for (const auto key : stale_)
            if (demand->wanted.contains(key) && !stagedKeys.contains(key)) refresh.push_back(key);
        std::sort(refresh.begin(), refresh.end(), nearer);
        for (const auto key : refresh) (demand->demanded.contains(key) ? requests : speculation).push_back(key);
        stream_->wants(requests, speculation);
        restream_ = false;
    }
    missing_ = plan_ ? plan_->missing.size() : 0;
    const bool currentPlan = plan_ && plan_->view == view && plan_->residencyRevision == residency_->revision;
    settled_ = currentPlan && displayAmount_ >= 1 && !tableDirty_ && !missing_ && !coarse_ && !plan_->morphing && !plan_->blank &&
        !plan_->staleMeshes &&
        std::none_of(stale_.begin(), stale_.end(), [&](Key key) { return plan_->wanted.contains(key); });
#if ASR_ENABLE_PROFILING
    if (changed || tableChanged || frame.index % 6 == 0 || !progress_.initialized) {
        world::terrain::StreamingProgress status;
        status.initialized = status.gridReady = true;
        status.targetLod = target_;
        for (const auto& block : drawing()) if (block.mesh) {
            const int meshStep = int(block.mesh->step), dataStep = 4 << block.dataLevel;
            status.meshStepMin = status.meshStepMin ? std::min(status.meshStepMin,meshStep) : meshStep;
            status.meshStepMax = std::max(status.meshStepMax,meshStep);
            status.dataStepMin = status.dataStepMin ? std::min(status.dataStepMin,dataStep) : dataStep;
            status.dataStepMax = std::max(status.dataStepMax,dataStep);
            const int chunkMetres = int(block.metres());
            status.chunkMetresMin = status.chunkMetresMin ? std::min(status.chunkMetresMin,chunkMetres) : chunkMetres;
            status.chunkMetresMax = std::max(status.chunkMetresMax,chunkMetres);
            status.chunkCells = block.chunkCells;
        }
        status.meshesBuilt = plan_ ? plan_->meshesBuilt : 0;
        status.deferredRegions = plan_ ? plan_->deferredRegions : 0;
        status.cameraMode = static_cast<int>(camera.mode);
        status.gridMode = grid_;
        status.flightSpeed = camera.flightSpeed;
        status.frozen = frozen_;
        status.capacityLimited = plan_ && plan_->capacityLimited;
        status.uploadFailed = !submitted;
        status.gpuBytes = gpuBytes_;
        status.fineCapacity = atlases_[0]->layout().capacity();
        status.h8Capacity = atlases_[1]->layout().capacity();
        status.fineResident = fineResident_; status.h8Resident = h8Resident_;
        status.persistentGpu = {double(persistent_.size()), persistentTotal_, true};
        const auto baking = world_->pages().prebakeProgress();
        status.backgroundRunning = baking.running;
        status.backgroundFailed = baking.failed;
        status.ramPrepared = {double(baking.done - std::min(baking.done, baking.failed)), baking.total, true};
        status.ramPublished = {double(baking.published), baking.total, true};
        status.ramBytes = world_->pages().pinnedBytes();
        status.cacheBytes = world_->pages().stats().residentBytes;
        const auto workers = stream_->stats();
        const auto plans = surface.stats();
        const auto pool = world_->pages().workerPool().stats();
        using Task = world::streaming::TerrainWorkerPool::Task;
        status.workers = pool.workers; status.busy = pool.busy;
        status.visibleBusy = pool.tasks[static_cast<std::size_t>(Task::Visible)];
        status.preloadBusy = pool.tasks[static_cast<std::size_t>(Task::Preload)];
        status.preparationBusy = pool.tasks[static_cast<std::size_t>(Task::Preparation)];
        status.inspectionBusy = pool.tasks[static_cast<std::size_t>(Task::Inspection)];
        status.queued = workers.queued; status.ready = workers.ready;
        status.failed = workers.failed + plans.failed;
        status.plansSubmitted = plans.submitted; status.plansCompleted = plans.completed;
        status.plansDiscarded = plans.discarded;
        status.planBuildMs = plans.lastBuildMs; status.planLatencyMs = plans.lastLatencyMs;
        status.staged = staged_.size(); status.speculative = plan_ ? plan_->preload.size() : 0;
        status.lastBuildMs = workers.lastBuildMs; status.longestBuildMs = workers.longestBuildMs;
        status.longestFetchMs = workers.longestFetchMs; status.longestPackMs = workers.longestPackMs;
        status.longestPageX = workers.longestPage.x; status.longestPageY = workers.longestPage.y;
        status.longestPageStep = 4 << workers.longestPage.level;
#if ASR_ENABLE_PROFILING
        status.uploadMs = std::chrono::duration<double, std::milli>(uploadAt - planAt).count();
#endif
        if (plan_ && plan_->view == view && plan_->residencyRevision == residency_->revision) {
            status.viewMesh = plan_->viewMesh;
            status.viewPages = plan_->viewPages;
        }
        progress_ = status;
    }
    // Never report the previous camera's 100% while its replacement is pending.
    if (!plan_ || plan_->view != view || plan_->residencyRevision != residency_->revision) {
        progress_.viewPages.known = false; progress_.viewMesh.known = false;
    }
#else
    progress_.uploadFailed = !submitted;
#endif
    lastDrawSerial_ = serial_;
    for (const auto& block : drawing()) if (auto it = meshes_.find(block.mesh.get()); it != meshes_.end())
        it->second.lastSerial = serial_;
    gather(frame);
#if ASR_ENABLE_PROFILING
    static const bool profile = std::getenv("ASR_TERRAIN_PROFILE") != nullptr;
    if (profile && (frame.index <= 3 || frame.index % 60 == 0)) {
        const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b-a).count(); };
        const auto jobs = surface.stats();
        std::fprintf(stderr, "terrain frame=%llu wall=%.2f cpu=%.2f accept=%.2f upload=%.2f table=%.2f tail=%.2f lod=%d radius=%.0f draw=%zu missing=%zu uploaded=%zu bytes=%zu resident=%zu plans=%zu/%zu failed=%zu discarded=%zu plan-build-ms=%.2f plan-latency-ms=%.2f dirty-roots=%zu cached-roots=%zu\n",
            static_cast<unsigned long long>(frame.index), frame.step*1000, ms(started,Clock::now()),
            ms(started,planAt), ms(planAt,uploadAt), ms(uploadAt,tableAt), ms(tableAt,Clock::now()),
            target_, radius_, drawing().size(), missing_, uploaded.size(), bytes, known_.size(), jobs.completed, jobs.submitted,
            jobs.failed, jobs.discarded, jobs.lastBuildMs, jobs.lastLatencyMs,
            plan_ ? plan_->rootsVisited : 0, plan_ ? plan_->rootsReused : 0);
        std::fprintf(stderr, "terrain cut coverage=%zu planned-visible=%zu selected=%zu table-dirty=%d blank=%zu pins=%zu\n",
            plan_ ? plan_->coverage.size() : 0, plan_ ? plan_->drawing.size() : 0,
            drawing_.size(), int(tableDirty_), plan_ ? plan_->blank : 0, plan_ ? plan_->pins.size() : 0);
        const auto& status = progress_;
        std::fprintf(stderr,"terrain batch meshes=%zu deferred-regions=%zu mesh-step=%d..%d data-step=%d..%d chunk-metres=%d..%d chunk-cells=%d\n",
            status.meshesBuilt,status.deferredRegions,status.meshStepMin,status.meshStepMax,
            status.dataStepMin,status.dataStepMax,status.chunkMetresMin,status.chunkMetresMax,status.chunkCells);
        std::fprintf(stderr, "terrain progress ram=%d%% published=%d%% retained-gpu=%d%% grid=%d%% view-data=%d%% view-detail=%d%% jobs=%zu/%zu queue=%zu staged=%zu build-ms=%.1f max-ms=%.1f limited=%d\n",
            status.ramPrepared.percent(), status.ramPublished.percent(), status.persistentGpu.percent(),
            status.gridReady ? 100 : 0, status.viewPages.percent(), status.viewMesh.percent(),
            status.busy, status.workers, status.queued, status.staged,
            status.lastBuildMs, status.longestBuildMs, int(status.capacityLimited));
        std::array<std::size_t, world::terrain::kGeometryLevels> levels{};
        for (const auto& block : drawing()) ++levels[block.tile.lod];
        const auto waterBlocks = std::size_t(std::count_if(drawing().begin(), drawing().end(),
            [](const auto& block) { return block.mayHaveWater; }));
        std::size_t groundTriangles = 0, waterTriangles = 0, surfaceTriangles = 0, sourceGridTriangles = 0;
        for (const auto& block : drawing()) {
            groundTriangles += indexCount(block)/3;
            if (block.mayHaveWater) waterTriangles += indexCount(block,true)/3;
            if (block.mesh) {
                surfaceTriangles += block.mesh->surfaceIndices/3;
                sourceGridTriangles += std::size_t(block.mesh->cells)*block.mesh->cells*2;
            }
        }
        std::fprintf(stderr, "terrain submitted ground-triangles=%zu water-blocks=%zu water-skipped=%zu water-triangles=%zu (adaptive pages only)\n",
            groundTriangles, waterBlocks, drawing().size() - waterBlocks, waterTriangles);
        std::size_t meshBytes=0;
        for (const auto& [key,buffers]:meshes_)
            meshBytes+=buffers.source->vertices.size()*sizeof(world::terrain::AdaptiveVertex)+
                buffers.source->indices.size()*sizeof(std::uint32_t);
        std::fprintf(stderr,"terrain memory gpu-mesh-payload=%zu gpu-page-payload=%zu mesh-buffers=%zu stable-features=%d\n",
            meshBytes,gpuBytes_,meshes_.size(),int(std::getenv("ASR_TERRAIN_STABLE_FEATURES")!=nullptr));
        std::fprintf(stderr, "terrain geometry surface-triangles=%zu skirt-triangles=%zu source-grid-triangles=%zu reduction=%.2f%% (same source lattice; not a frame-time comparison)\n",
            surfaceTriangles, groundTriangles-surfaceTriangles, sourceGridTriangles,
            sourceGridTriangles ? 100.0*(1.0-double(surfaceTriangles)/sourceGridTriangles) : 0.0);
        if (plan_) std::fprintf(stderr,"terrain detail local-h4=%zu local-evaluated=%zu local-morph=%.2f feature-samples=%zu feature-evaluated=%zu full-h4-pages=%zu\n",
            plan_->detail.localSamples,plan_->detail.localEvaluated,plan_->detail.localAmount,
            plan_->detail.featureSamples,plan_->detail.featureEvaluated,fineResident_);
        std::fprintf(stderr, "terrain camera=%s grid=%d lod-blocks=", camera.modeName(), grid_);
        for (std::size_t lod = 0; lod < levels.size(); ++lod)
            std::fprintf(stderr, "%s%zu", lod ? "," : "", levels[lod]);
        std::fprintf(stderr, " max-page=H%d[%d,%d] fetch-bake-ms=%.1f pack-ms=%.1f\n",
            status.longestPageStep, status.longestPageX, status.longestPageY,
            status.longestFetchMs, status.longestPackMs);
    }
#endif
}

std::vector<SDL_GPUTextureSamplerBinding> GpuTerrain::bindings() const {
    if (!table_ || !tableSampler_ || !climate_.ready()) return {};
    for (const auto& atlas : atlases_) if (!atlas || !*atlas) return {};
    auto out = climate_.bindings();
    for (const auto& atlas : atlases_) out.push_back(atlas->binding());
    out.push_back({table_.get(), tableSampler_.get()});
    for (const auto& atlas : atlases_) out.push_back(atlas->fieldBinding());
    return out;
}
double GpuTerrain::vegetationPixelsPerMetre(const engine::Frame& frame,double x,double y) const {
    const auto* m=engine::cullMatrix(frame.scene);
    if (m[12]==0 && m[13]==0 && m[14]==0) return frame.scene.camera[3];
    double nearest=std::numeric_limits<double>::infinity(),pixels=0;
    for (const auto& b:drawing()) {
        const auto ox=double(b.tile.x*b.metres()),oy=double(b.tile.y*b.metres());
        if (!b.mesh) continue;
        const double px=std::clamp(x,ox,ox+double(b.metres()));
        const double py=std::clamp(y,oy,oy+double(b.metres()));
        const double distance=std::hypot(px-x,py-y);
        if (distance>=nearest) continue;
        const double height=b.mesh->sample(px-ox,py-oy)[0];
        const double depth=m[12]*px+m[13]*py+m[14]*height+m[15];
        if (depth<=0) continue;
        nearest=distance;
        pixels=std::hypot(std::hypot(m[0],m[1]),m[2])*double(frame.width)*0.5/std::max(1.0,depth);
    }
    return pixels; // no resident surface: coarse fallback, never an eager HeightField query
}

void GpuTerrain::gather(const engine::Frame& frame) {
    const auto& blocks = drawing();
    resolved_.assign(blocks.size(), Drawn{});
    std::vector<engine::render::TerrainPatch> cut;
    cut.reserve(blocks.size());
    for (std::uint32_t i = 0; i < blocks.size(); ++i) {
        const auto& block = blocks[i];
        if (!block.mesh) continue;
        const auto it = meshes_.find(block.mesh.get());
        if (it == meshes_.end() || !it->second.vertices) continue;
        auto& into = resolved_[i];
        into.vertices = it->second.vertices.get();
        into.indices = it->second.indices.get();
        into.waterIndices = block.mesh->wetIndices;
        into.highBed = it->second.highBed;
        into.inland = it->second.inland;
        into.mesh = block.mesh.get();
        into.parameters = parameters(block);
        engine::render::TerrainPatch patch;
        patch.originX = block.bounds.minX;
        patch.originY = block.bounds.minY;
        patch.metres = double(block.metres());
        patch.lowZ = float(block.bounds.low);
        patch.highZ = float(block.bounds.high);
        patch.level = std::uint32_t(block.tile.lod);
        patch.morph = block.parentMorph;
        patch.water = block.mayHaveWater;
        patch.source = i;
        cut.push_back(patch);
    }
    const auto& scene = frame.scene;
    const auto* m = engine::cullMatrix(scene);
    engine::render::ScreenScale screen;
    for (int i = 0; i < 4; ++i) { screen.rowX[i] = m[i]; screen.rowY[i] = m[4 + i]; screen.rowW[i] = m[12 + i]; }
    const bool perspective = m[12] != 0 || m[13] != 0 || m[14] != 0;
    screen.focal = perspective ? std::hypot(std::hypot(m[0], m[1]), m[2]) * frame.width * 0.5 : 0;
    screen.scale = scene.camera[3];
    gathered_ = engine::render::gatherTerrain(cut, screen);
}

std::array<float, 16> GpuTerrain::parameters(const Block& block) const {
    const auto tile = block.tile;
    const float step = float(block.mesh ? block.mesh->step : world::sampleMetresAt(tile.lod));
    const int parent = std::min(int(world::terrain::kGeometryLevels) - 1, tile.lod + 1);
    const float parentStep = float(world::sampleMetresAt(parent));
    const float amount=plan_ && plan_->view.stageFrom==stageFrom_ && plan_->view.stageTo==stageTo_ ? stageAmount_ : 1.0f;
    const bool staged=plan_ && (plan_->view.stageTo!=generation::TerrainStage::Final || amount<1);
    return {block.parentMorph, staged?3.0f+amount:2.0f, float(dataset(block.dataLevel)), step,
            float(tile.x * block.metres()), float(tile.y * block.metres()), low_, range_,
            float(dataset(block.parentDataLevel)), block.mesh ? displayAmount_ : parentStep, std::lerp(step, parentStep, block.parentMorph) * 2, 14,
            float(tableWidth_), float(tableHeight_), block.skirtLow, float(1 + 2 * grid_)};
}

SDL_GPUBuffer* GpuTerrain::vertices(const Block& block) const { return meshes_.at(block.mesh.get()).vertices.get(); }
SDL_GPUBuffer* GpuTerrain::indices(const Block& block) const { return meshes_.at(block.mesh.get()).indices.get(); }
std::uint32_t GpuTerrain::indexCount(const Block& block, bool water) const {
    return !block.mesh ? 0 : water || !skirts_ ? block.mesh->surfaceIndices : std::uint32_t(block.mesh->indices.size());
}

} // namespace game
