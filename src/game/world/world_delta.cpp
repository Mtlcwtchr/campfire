#include "game/world/world_delta.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <type_traits>

#include "engine/core/hash.hpp"
#include "engine/core/rng.hpp"
#include "engine/world_store/atomic_file.hpp"
#include "engine/world_store/chunk_file.hpp"
#include "game/world/object_id.hpp"
#include "game/world/scene_scatter.hpp"

namespace world::delta {
namespace ws = engine::world_store;
namespace {

// Which delta is writing on this thread. The views report every change; the
// ones this delta makes itself are already in its journal and must not be
// counted twice as changes from outside.
thread_local const WorldDelta* tlsWriter = nullptr;
struct Writing {
    explicit Writing(const WorldDelta* delta) : previous(tlsWriter) { tlsWriter = delta; }
    ~Writing() { tlsWriter = previous; }
    Writing(const Writing&) = delete;
    Writing& operator=(const Writing&) = delete;
    const WorldDelta* previous;
};

// The lowest sun a shadow is drawn for, as a tangent: tan 5 degrees. A change
// of height h reaches h / tan(elevation) along the ground in shadow.
constexpr double kLowSun = 0.0875;
constexpr double kTallestObject = 40;    // metres: a tall tree, for how far its shadow falls
constexpr double kObjectReach = 8;       // metres: a crown, a footprint
constexpr double kMaxShadowReach = 2048;
constexpr std::size_t kMaxQueuedInvalidations = 4096;
constexpr std::uint32_t kPlantedDomain = 1;
// Which view reported a change: the objects, or a level of the heights.
constexpr int kObjectsView = -1;

constexpr std::uint16_t kGroundLayers = TerrainGeometry | Water | NavCollision;
constexpr std::uint16_t kLifeLayers = EcologyField | Vegetation | Impostors;

MetreRect pageRect(const ecology::Key& page) {
    const double s = ecology::kPageMetres;
    return {double(page.first) * s, double(page.second) * s, double(page.first + 1) * s, double(page.second + 1) * s};
}
MetreRect blockRect(int level, std::int64_t bx, std::int64_t by) {
    const double s = EditLayer::blockMetresOf(level);
    // A coarse sample shapes the ground a step past its block.
    const double reach = level == 0 ? 0.0 : double(EditLayer::stepOf(level));
    return {double(bx) * s - reach, double(by) * s - reach, double(bx + 1) * s + reach, double(by + 1) * s + reach};
}
MetreRect pointRect(double x, double y, double reach) { return {x - reach, y - reach, x + reach, y + reach}; }

void collapse(std::vector<Invalidation>& queue) {
    // Nobody is draining: keep what matters - that something went stale, and
    // where - in one record, not an unbounded list.
    if (queue.size() <= kMaxQueuedInvalidations) return;
    Invalidation all;
    for (const auto& i : queue) {
        all.layers |= i.layers;
        all.area = all.area.merged(i.area);
        all.sequence = std::max(all.sequence, i.sequence);
    }
    queue.assign(1, all);
}

// A copy of one level of an edit layer's non-zero samples, for building a
// terrain op.
std::vector<TerrainSample> samplesOf(const EditLayer& layer, int level) {
    std::vector<TerrainSample> out;
    const std::int64_t side = EditLayer::blockSamplesOf(level);
    for (const auto& [bx, by] : layer.blockKeys(level)) {
        std::vector<EditLayer::BlockCopy> copy;
        layer.copyBlocks(level, bx, by, bx, by, copy);
        for (const auto& block : copy)
            for (std::int64_t i = 0; i < std::int64_t(block.delta.size()); ++i)
                if (block.delta[std::size_t(i)] != core::kZero)
                    out.push_back({block.x * side + i % side, block.y * side + i / side, block.delta[std::size_t(i)]});
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    return out;
}

} // namespace

const char* derivedLayerName(std::uint16_t layer) {
    switch (layer) {
        case TerrainGeometry: return "terrain";
        case Water: return "water";
        case EcologyField: return "ecology";
        case Vegetation: return "vegetation";
        case NavCollision: return "nav";
        case Shadows: return "shadows";
        case Impostors: return "impostors";
        default: return "?";
    }
}

std::vector<Invalidation> invalidations(const Op& op) {
    std::vector<Invalidation> out;
    const auto add = [&](std::uint16_t layers, const MetreRect& area) {
        if (!area.empty()) out.push_back({layers, area, op.sequence});
    };
    std::visit([&](const auto& what) {
        using T = std::decay_t<decltype(what)>;
        if constexpr (std::is_same_v<T, TerrainOp>) {
            if (what.samples.empty()) return;
            std::int64_t lowX = what.samples.front().x, highX = lowX;
            std::int64_t lowY = what.samples.front().y, highY = lowY;
            double biggest = 0;
            for (const auto& s : what.samples) {
                lowX = std::min(lowX, s.x); highX = std::max(highX, s.x);
                lowY = std::min(lowY, s.y); highY = std::max(highY, s.y);
                biggest = std::max(biggest, std::abs(s.add.toDouble()));
            }
            // A sample shapes the ground to its neighbours on either side:
            // the height between samples is interpolated.
            const double step = EditLayer::stepOf(what.level);
            const MetreRect moved{double(lowX - 1) * step, double(lowY - 1) * step,
                                  double(highX + 1) * step, double(highY + 1) * step};
            // Meshes, water and paths read one sample further for normals and
            // gradients; life reads the 64 m ecology cell the slope is taken
            // over; a shadow runs as far as the height it moved casts one.
            add(kGroundLayers, moved.grown(step));
            add(kLifeLayers, moved.grown(ecology::kCellMetres));
            add(Shadows, moved.grown(std::min(kMaxShadowReach, biggest / kLowSun + step)));
        } else if constexpr (std::is_same_v<T, RemoveOp> || std::is_same_v<T, RestoreOp>) {
            add(Vegetation | Impostors, pointRect(what.x, what.y, kObjectReach));
            add(Shadows, pointRect(what.x, what.y, kTallestObject / kLowSun));
        } else if constexpr (std::is_same_v<T, PlantOp>) {
            add(Vegetation | Impostors, pointRect(what.object.x, what.object.y, kObjectReach));
            add(Shadows, pointRect(what.object.x, what.object.y, kTallestObject / kLowSun));
        } else if constexpr (std::is_same_v<T, ClearOp>) {
            const auto page = pageRect(ecology::page(what.x, what.y));
            add(Vegetation | Impostors, page.grown(kObjectReach));
            add(Shadows, page.grown(kTallestObject / kLowSun));
        } else {
            const auto cell = ecology::key(what.x, what.y);
            const double s = ecology::kCellMetres;
            const MetreRect area{double(cell.first) * s, double(cell.second) * s,
                                 double(cell.first + 1) * s, double(cell.second + 1) * s};
            add(kLifeLayers, area.grown(kObjectReach));
            add(Shadows, area.grown(kTallestObject / kLowSun));
        }
    }, op.what);
    return out;
}

WorldDelta::WorldDelta(std::uint64_t worldSeed, Options options) : worldSeed_(worldSeed), options_(options) {
    watch();
}

WorldDelta::~WorldDelta() {
    // The views may outlive this - a snapshot keeps the object store - and
    // must stop reporting to it first.
    objects_->observe(nullptr);
    heights_->observe(nullptr);
    {
        std::lock_guard lock(saverMutex_);
        stopping_ = true;
    }
    saverWake_.notify_all();
    if (saver_.joinable()) saver_.join();
}

void WorldDelta::watch() {
    objects_->observe([this](ecology::Key page, std::uint64_t revision) {
        if (tlsWriter == this) return;
        std::lock_guard lock(foreignMutex_);
        auto& latest = foreign_[{page, kObjectsView}];
        latest = std::max(latest, revision);
    });
    heights_->observe([this](int level, std::int64_t bx, std::int64_t by, std::uint64_t revision) {
        if (tlsWriter == this) return;
        std::lock_guard lock(foreignMutex_);
        auto& latest = foreign_[{{bx, by}, level}];
        latest = std::max(latest, revision);
    });
}

void WorldDelta::drainForeignLocked() const {
    std::map<std::pair<ecology::Key, int>, std::uint64_t> taken;
    {
        std::lock_guard lock(foreignMutex_);
        taken.swap(foreign_);
    }
    for (const auto& [what, revision] : taken) {
        const auto& [unit, view] = what;
        const bool isHeights = view != kObjectsView;
        const ChunkKey chunk = isHeights ? codec::chunkOfEditBlock(view, unit.first, unit.second) : codec::chunkOfPage(unit);
        auto& rec = records_[chunk];
        ++rec.revision;
        // Not in the journal, so the journal alone no longer rebuilds this
        // chunk: the next save takes a snapshot of it instead.
        rec.stale = true;
        if (isHeights) {
            rec.foreignHeights = std::max(rec.foreignHeights, revision);
            const auto area = blockRect(view, unit.first, unit.second);
            invalidations_.push_back({kGroundLayers | kLifeLayers, area.grown(ecology::kCellMetres), 0});
            invalidations_.push_back({Shadows, area.grown(kMaxShadowReach), 0});
        } else {
            rec.foreignObjects = std::max(rec.foreignObjects, revision);
            const auto area = pageRect(unit);
            invalidations_.push_back({kLifeLayers, area.grown(kObjectReach), 0});
            invalidations_.push_back({Shadows, area.grown(kTallestObject / kLowSun), 0});
        }
    }
    collapse(invalidations_);
}

void WorldDelta::source(std::string relativeLayout, std::uint64_t layoutHash) {
    std::lock_guard lock(mutex_);
    source_ = std::move(relativeLayout);
    sourceHash_ = layoutHash;
}

std::uint64_t WorldDelta::applyLocked(Op op) {
    drainForeignLocked();
    if (auto* terrain = std::get_if<TerrainOp>(&op.what))
        std::erase_if(terrain->samples, [](const TerrainSample& s) { return s.add == core::kZero; });
    op.sequence = nextSequence_;
    bool changed = false;
    {
        Writing writing(this);
        changed = std::visit([&](const auto& what) -> bool {
            using T = std::decay_t<decltype(what)>;
            if constexpr (std::is_same_v<T, TerrainOp>) {
                for (const auto& s : what.samples) heights_->add(what.level, s.x, s.y, s.add);
                return !what.samples.empty();
            } else if constexpr (std::is_same_v<T, RemoveOp>) {
                return objects_->remove(what.id, what.x, what.y);
            } else if constexpr (std::is_same_v<T, PlantOp>) {
                return objects_->add(what.id, what.object);
            } else if constexpr (std::is_same_v<T, ClearOp>) {
                return objects_->clear(what.x, what.y, what.models);
            } else if constexpr (std::is_same_v<T, RestoreOp>) {
                return objects_->unremove(what.id, what.x, what.y);
            } else {
                objects_->set(what.x, what.y, what.cell);
                return true;
            }
        }, op.what);
    }
    if (!changed) return 0;
    ++nextSequence_;

    // One record per file the op reached, each holding only its own part: a
    // stroke across a border is restored by each side identically, and the
    // manifest commits both sides together.
    std::map<ChunkKey, Op> parts;
    if (const auto* terrain = std::get_if<TerrainOp>(&op.what)) {
        for (const auto& s : terrain->samples) {
            auto [part, fresh] = parts.try_emplace(codec::chunkOfSample(terrain->level, s.x, s.y));
            if (fresh) {
                TerrainOp piece = *terrain;
                piece.samples.clear();
                part->second = Op{op.sequence, op.origin, std::move(piece)};
            }
            std::get<TerrainOp>(part->second.what).samples.push_back(s);
        }
    } else {
        const auto [x, y] = std::visit([](const auto& what) -> std::pair<double, double> {
            using T = std::decay_t<decltype(what)>;
            if constexpr (std::is_same_v<T, PlantOp>) return {what.object.x, what.object.y};
            else if constexpr (std::is_same_v<T, TerrainOp>) return {0, 0};
            else return {what.x, what.y};
        }, op.what);
        parts.emplace(codec::chunkOfPoint(x, y), op);
    }
    for (auto& [chunk, part] : parts) {
        auto& rec = records_[chunk];
        rec.journal.push_back(std::move(part));
        ++rec.revision;
    }
    for (auto& i : invalidations(op)) invalidations_.push_back(i);
    collapse(invalidations_);
    return op.sequence;
}

std::uint64_t WorldDelta::apply(Op op) {
    std::lock_guard lock(mutex_);
    return applyLocked(std::move(op));
}

std::size_t WorldDelta::brush(const Brush& brush, const GroundAt& ground, core::WorldPos at, double seconds,
                              Origin origin) {
    // Resolved on a scratch layer: the stroke reads the ground as it stands
    // and what it wrote is recorded sample by sample, so replaying it never
    // has to read the ground again.
    EditLayer scratch;
    if (!applyBrush(scratch, ground, brush, at, seconds, heights_.get())) return 0;
    TerrainOp op;
    op.tool = brush.kind;
    op.centreX = at.x.toDouble();
    op.centreY = at.y.toDouble();
    op.radius = brush.radiusMetres;
    op.strength = brush.strength;
    op.seconds = seconds;
    op.level = std::uint8_t(EditLayer::levelFor(brush.radiusMetres));
    op.samples = samplesOf(scratch, op.level);
    const auto count = op.samples.size();
    return apply(Op{0, origin, std::move(op)}) ? count : 0;
}

bool WorldDelta::removeObject(std::uint64_t id, double x, double y, Origin origin) {
    return apply(Op{0, origin, RemoveOp{id, x, y}}) != 0;
}

bool WorldDelta::restoreObject(std::uint64_t id, double x, double y, Origin origin) {
    return apply(Op{0, origin, RestoreOp{id, x, y}}) != 0;
}

std::uint64_t WorldDelta::plant(const ecology::Added& object, Origin origin) {
    if (object.model >= decor::kModels.size() || !std::isfinite(object.x) || !std::isfinite(object.y)) return 0;
    std::lock_guard lock(mutex_);
    // Named like a generated object, under a stage of its own, by the edit's
    // sequence within its page: unique, and the same on every replay.
    const auto page = ecology::page(object.x, object.y);
    const auto sequence = nextSequence_;
    auto id = objectId(worldSeed_, PlacementStage::Planted, kPlantedDomain, page.first, page.second,
                       std::uint32_t(sequence));
    if (sequence >> 32) id = core::splitmix64(id ^ (sequence >> 32)) | 1;
    return applyLocked(Op{0, origin, PlantOp{id, object}}) ? id : 0;
}

bool WorldDelta::clearPage(double x, double y, std::uint32_t models, Origin origin) {
    return apply(Op{0, origin, ClearOp{x, y, models}}) != 0;
}

void WorldDelta::setEcology(double x, double y, const ecology::Cell& cell, Origin origin) {
    apply(Op{0, origin, EcologyOp{x, y, cell}});
}

std::uint64_t WorldDelta::revision(const ChunkKey& chunk) const {
    std::lock_guard lock(mutex_);
    drainForeignLocked();
    const auto it = records_.find(chunk.within(kFileLevel));
    return it == records_.end() ? 0 : it->second.revision;
}

std::vector<Invalidation> WorldDelta::drainInvalidations() {
    std::lock_guard lock(mutex_);
    drainForeignLocked();
    return std::exchange(invalidations_, {});
}

bool WorldDelta::dirty() const {
    std::lock_guard lock(mutex_);
    drainForeignLocked();
    return std::any_of(records_.begin(), records_.end(),
                       [](const auto& r) { return r.second.revision != r.second.committed; });
}

void WorldDelta::compact() {
    std::lock_guard lock(mutex_);
    forceCompact_ = true;
    // Every chunk this delta holds, not only the ones changed since the last
    // save: "compact" is a request to rewrite the files, not to wait for a
    // reason to.
    for (auto& [key, rec] : records_)
        if (rec.committed && rec.revision == rec.committed) ++rec.revision;
}

std::vector<ChunkInfo> WorldDelta::chunks() const {
    std::lock_guard lock(mutex_);
    drainForeignLocked();
    std::vector<ChunkInfo> out;
    out.reserve(records_.size());
    for (const auto& [key, rec] : records_)
        out.push_back({key, rec.revision, rec.committed, rec.journal.size(), rec.stale, rec.damaged, rec.file,
                       rec.fileBytes});
    return out;
}

std::vector<Op> WorldDelta::journal(const ChunkKey& chunk) const {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(chunk.within(kFileLevel));
    return it == records_.end() ? std::vector<Op>{} : it->second.journal;
}

std::uint64_t WorldDelta::nextSequence() const {
    std::lock_guard lock(mutex_);
    return nextSequence_;
}

std::uint64_t WorldDelta::contentHash() const {
    core::Checksum sum;
    // Heights: every non-zero sample, in sample order; a block that holds
    // only noughts is the same ground as no block at all.
    // Level 0 as it always was, so a delta made before there were levels
    // hashes as it did; each coarse level after it under its own mark.
    for (int level = 0; level < EditLayer::kLevels; ++level) {
        auto keys = heights_->blockKeys(level);
        if (keys.empty()) continue;
        if (level > 0) sum.add(std::uint64_t(0x1e7e1000) + std::uint64_t(level));
        std::sort(keys.begin(), keys.end());
        const std::int64_t side = EditLayer::blockSamplesOf(level);
        for (const auto& [bx, by] : keys) {
            std::vector<EditLayer::BlockCopy> copy;
            heights_->copyBlocks(level, bx, by, bx, by, copy);
            for (const auto& block : copy)
                for (std::size_t i = 0; i < block.delta.size(); ++i)
                    if (block.delta[i] != core::kZero) {
                        sum.add(block.x * side + std::int64_t(i) % side);
                        sum.add(block.y * side + std::int64_t(i) / side);
                        sum.add(block.delta[i].raw);
                    }
        }
    }
    const auto objects = objects_->read();
    const auto bits = [](float f) { return std::uint64_t(std::bit_cast<std::uint32_t>(f)); };
    sum.add(std::uint64_t(0x7265));
    for (const auto& [id, cell] : objects->removed) { sum.add(id); sum.add(cell.first); sum.add(cell.second); }
    sum.add(std::uint64_t(0x6164));
    for (const auto& [page, list] : objects->added)
        for (const auto& [id, a] : list) {
            sum.add(id);
            sum.add(std::bit_cast<std::uint64_t>(a.x));
            sum.add(std::bit_cast<std::uint64_t>(a.y));
            sum.add(bits(a.yaw)); sum.add(bits(a.scale)); sum.add(bits(a.tint));
            sum.add(std::uint64_t(a.model));
        }
    sum.add(std::uint64_t(0x636c));
    for (const auto& [page, mask] : objects->cleared) { sum.add(page.first); sum.add(page.second); sum.add(std::uint64_t(mask)); }
    sum.add(std::uint64_t(0x6365));
    for (const auto& [where, c] : objects->cells) {
        sum.add(where.first); sum.add(where.second);
        for (const float v : {c.potentialForest, c.canopy, c.grass, c.shrubs, c.deadwood, c.fertility, c.moisture,
                              c.wetland, c.disturbance, c.youngGrowth, c.agriculture})
            sum.add(bits(v));
        sum.add(std::uint64_t(c.masks) << 16 | std::uint64_t(c.biome) << 8 | c.succession);
    }
    return sum.value();
}

std::unique_ptr<WorldDelta> WorldDelta::open(ws::WorldRoot root, std::uint64_t worldSeed, LoadReport* report,
                                             Options options) {
    LoadReport local;
    LoadReport& r = report ? *report : local;
    r = {};
    std::string why;
    const auto manifest = root.readManifest(&why);
    if (!manifest && !why.empty()) {
        r.fresh = false;
        r.damaged.push_back(std::string(ws::WorldRoot::kManifestName) + ": " + why);
        return nullptr;
    }
    auto delta = std::make_unique<WorldDelta>(worldSeed, options);
    delta->root_ = root;
    if (!manifest) return delta;

    r.fresh = false;
    if (manifest->worldSeed != worldSeed)
        r.warnings.push_back("delta was recorded for world seed " + std::to_string(manifest->worldSeed) +
                             ", this world is seed " + std::to_string(worldSeed) +
                             ": generated objects it removed are named for another world and will not match");
    if (const auto it = manifest->stableDomains.find("decor");
        it != manifest->stableDomains.end() && it->second != kDecorStableDomain)
        r.warnings.push_back("removed objects were named on decor lattice " + std::to_string(it->second) +
                             ", this build places on lattice " + std::to_string(kDecorStableDomain) +
                             ": they need migrating and match nothing until they are");
    r.swept = root.sweep(*manifest);

    ecology::Delta objects;
    for (const auto& entry : manifest->delta) {
        Record rec;
        rec.revision = rec.committed = entry.revision;
        rec.file = entry.file;
        rec.fileBytes = entry.bytes;
        rec.payloadHash = entry.payloadHash;
        std::string problem;
        std::optional<std::vector<Block>> blocks;
        ws::ChunkHeader header;
        if (const auto bytes = ws::readFileBytes(root.deltaFile(entry.file), &problem))
            blocks = ws::decodeChunk(*bytes, &header, &problem);
        bool ok = blocks.has_value();
        if (ok && (header.kind != ws::ChunkKind::Delta || header.key != entry.key ||
                   header.deltaRevision != entry.revision || header.payloadHash != entry.payloadHash ||
                   header.worldSeed != manifest->worldSeed)) {
            ok = false;
            problem = "the file is not the chunk the manifest names";
        }
        // Into scratch first: a chunk that fails half way leaves nothing of
        // itself behind in the world.
        ecology::Delta chunkObjects;
        EditLayer chunkHeights;
        std::vector<Op> ops;
        if (ok) ok = codec::decodeSnapshot(entry.key, *blocks, chunkObjects, chunkHeights, &problem);
        if (ok)
            for (const auto& b : *blocks)
                if (b.type == codec::Journal) {
                    auto decoded = codec::decodeJournal(entry.key, b, &problem);
                    if (!decoded) { ok = false; break; }
                    ops = std::move(*decoded);
                }
        if (!ok) {
            rec.damaged = true;
            r.damaged.push_back(entry.file + ": " + problem);
            delta->records_[entry.key] = std::move(rec);
            continue;
        }
        for (const auto& op : ops) codec::applyTo(op, chunkObjects, chunkHeights);

        // Chunks are disjoint in space, so merging them is putting each
        // record where it belongs, in any order: the world a set of chunks
        // makes does not depend on which was read first.
        objects.removed.insert(chunkObjects.removed.begin(), chunkObjects.removed.end());
        objects.cells.insert(chunkObjects.cells.begin(), chunkObjects.cells.end());
        objects.added.insert(chunkObjects.added.begin(), chunkObjects.added.end());
        objects.cleared.insert(chunkObjects.cleared.begin(), chunkObjects.cleared.end());
        for (const auto& [page, revision] : chunkObjects.regions) objects.regions[page] = 1;
        {
            Writing writing(delta.get());
            for (int level = 0; level < EditLayer::kLevels; ++level)
                for (const auto& s : samplesOf(chunkHeights, level)) delta->heights_->add(level, s.x, s.y, s.add);
        }
        for (auto& b : *blocks)
            if (b.type != codec::Journal) rec.snapshot.push_back(std::move(b));
        rec.journal = std::move(ops);
        r.ops += rec.journal.size();
        ++r.chunks;
        delta->records_[entry.key] = std::move(rec);
    }
    {
        Writing writing(delta.get());
        delta->objects_->restore(std::move(objects));
    }
    delta->nextSequence_ = std::max<std::uint64_t>(manifest->nextSequence, 1);
    delta->commit_ = manifest->commit;
    delta->source_ = manifest->source;
    delta->sourceHash_ = manifest->sourceHash;
    return delta;
}

SaveReport WorldDelta::saveNow() {
    std::lock_guard serial(saveMutex_);
    SaveReport report;
    if (!root_) {
        report.ok = false;
        report.error = "this delta has no world root to save to";
        return report;
    }
    struct Job {
        ChunkKey key;
        std::uint64_t revision = 0;
        bool compact = false, damaged = false, empty = false;
        std::vector<Block> snapshot;
        std::vector<Op> journal;
        std::size_t journalLength = 0;
        std::vector<EditLayer::BlockCopy> heights;
        std::uint64_t heightsRevision = 0;
        std::string previousFile, file;
        std::uint64_t payloadHash = 0, bytes = 0;
    };
    std::vector<Job> jobs;
    std::shared_ptr<const ecology::Delta> objects;
    ws::Manifest manifest;
    {
        std::lock_guard lock(mutex_);
        // The store first, then what the views reported: every change from
        // outside that this copy holds has been reported by now, and the ones
        // reported after it are newer than it and keep their chunk stale.
        objects = objects_->read();
        drainForeignLocked();
        for (auto& [key, rec] : records_) {
            if (rec.revision == rec.committed) continue;
            Job job;
            job.key = key;
            job.revision = rec.revision;
            job.damaged = rec.damaged;
            job.previousFile = rec.file;
            job.journalLength = rec.journal.size();
            job.compact = forceCompact_ || rec.stale || rec.damaged || rec.journal.size() > options_.journalLimit ||
                          rec.journal.empty();
            if (job.compact) {
                job.heightsRevision = heights_->copyBlocksWithin(key.x * kFileMetres, key.y * kFileMetres,
                                                                 (key.x + 1) * kFileMetres,
                                                                 (key.y + 1) * kFileMetres, job.heights);
            } else {
                job.snapshot = rec.snapshot;
                job.journal = rec.journal;
            }
            jobs.push_back(std::move(job));
        }
        if (jobs.empty()) {
            report.nothingToDo = true;
            report.commit = commit_;
            return report;
        }
        manifest.worldSeed = worldSeed_;
        manifest.source = source_;
        manifest.sourceHash = sourceHash_;
        manifest.stableDomains = {{"decor", kDecorStableDomain}, {"planted", kPlantedDomain}};
        manifest.commit = commit_ + 1;
        manifest.nextSequence = nextSequence_;
        for (const auto& [key, rec] : records_)
            if (rec.committed && !rec.file.empty())
                manifest.delta.push_back({key, rec.committed, rec.file, rec.payloadHash, rec.fileBytes});
    }

    // Encoding and writing happen without the lock: the world goes on.
    std::vector<ChunkKey> compacting;
    for (const auto& job : jobs)
        if (job.compact) compacting.push_back(job.key);
    const auto parts = codec::partition(*objects, compacting);
    std::string why;
    for (auto& job : jobs) {
        std::vector<Block> blocks;
        if (job.compact) {
            job.snapshot = codec::encodeSnapshot(job.key, parts.at(job.key), job.heights);
            blocks = job.snapshot;
        } else {
            blocks = job.snapshot;
            blocks.push_back(codec::encodeJournal(job.key, job.journal));
        }
        std::erase_if(manifest.delta, [&](const ws::DeltaEntry& e) { return e.key == job.key; });
        if (blocks.empty()) {
            // Everything that happened here was undone: the chunk has no file.
            job.empty = true;
            ++report.dropped;
            continue;
        }
        ws::ChunkHeader header;
        header.kind = ws::ChunkKind::Delta;
        header.generatorVersion = kDecorStableDomain;
        header.worldSeed = worldSeed_;
        header.key = job.key;
        header.sourceRevision = manifest.sourceHash;
        header.deltaRevision = job.revision;
        const auto bytes = ws::encodeChunk(header, blocks, options_.compression);
        job.file = ws::WorldRoot::deltaFileName(job.key, job.revision);
        job.payloadHash = ws::payloadHashOf(blocks);
        job.bytes = bytes.size();
        if (job.damaged && !job.previousFile.empty()) {
            // What could not be read is still somebody's world: it is kept
            // under a name the sweep leaves alone before it is superseded.
            std::error_code ec;
            std::filesystem::copy_file(root_->deltaFile(job.previousFile),
                                       root_->deltaFile(job.previousFile + ".damaged"),
                                       std::filesystem::copy_options::skip_existing, ec);
        }
        if (!ws::writeFileAtomic(root_->deltaFile(job.file), bytes, &why)) {
            report.ok = false;
            report.error = why;
            return report;
        }
        manifest.delta.push_back({job.key, job.revision, job.file, job.payloadHash, job.bytes});
        ++report.written;
        report.bytes += job.bytes;
        report.compacted += job.compact ? 1 : 0;
    }
    if (failBeforeCommit_) {
        report.ok = false;
        report.error = "stopped before the manifest (fault injection)";
        return report;
    }
    if (!root_->writeManifest(manifest, &why)) {
        report.ok = false;
        report.error = why;
        return report;
    }
    report.swept = root_->sweep(manifest);
    report.commit = manifest.commit;

    std::lock_guard lock(mutex_);
    commit_ = manifest.commit;
    forceCompact_ = false;
    for (auto& job : jobs) {
        auto& rec = records_[job.key];
        rec.committed = job.revision;
        rec.file = job.empty ? std::string{} : job.file;
        rec.fileBytes = job.bytes;
        rec.payloadHash = job.payloadHash;
        rec.damaged = false;
        if (job.compact) {
            rec.snapshot = std::move(job.snapshot);
            // Only the ops the snapshot was taken after; any made while this
            // save ran are still to come.
            rec.journal.erase(rec.journal.begin(),
                              rec.journal.begin() + std::ptrdiff_t(std::min(job.journalLength, rec.journal.size())));
            if (rec.foreignObjects <= objects->revision && rec.foreignHeights <= job.heightsRevision) rec.stale = false;
        }
    }
    std::erase_if(records_, [](const auto& r) {
        const auto& rec = r.second;
        return rec.revision == rec.committed && rec.file.empty() && rec.journal.empty() && rec.snapshot.empty() &&
               !rec.stale && !rec.damaged;
    });
    return report;
}

SaveReport WorldDelta::save() {
    auto report = saveNow();
    std::lock_guard lock(saverMutex_);
    lastSave_ = report;
    return report;
}

void WorldDelta::saveInBackground() {
    std::lock_guard lock(saverMutex_);
    if (stopping_) return;
    saveRequested_ = true;
    if (!saver_.joinable()) saver_ = std::thread([this] { saverLoop(); });
    saverWake_.notify_one();
}

void WorldDelta::saverLoop() {
    std::unique_lock lock(saverMutex_);
    for (;;) {
        saverWake_.wait(lock, [&] { return saveRequested_ || stopping_; });
        // A save that was asked for is finished even when stopping: it is
        // somebody's world.
        if (!saveRequested_) return;
        saveRequested_ = false;
        saving_ = true;
        lock.unlock();
        auto report = saveNow();
        lock.lock();
        lastSave_ = std::move(report);
        saving_ = false;
        saverIdle_.notify_all();
    }
}

SaveReport WorldDelta::waitForSave() {
    std::unique_lock lock(saverMutex_);
    saverIdle_.wait(lock, [&] { return !saving_ && !saveRequested_; });
    return lastSave_;
}

} // namespace world::delta


