#include "game/world/world_delta_codec.hpp"

#include <tuple>

#include <algorithm>
#include <bit>
#include <set>
#include <type_traits>

#include "engine/core/binary.hpp"

namespace world::delta {

using engine::world_store::floorDiv;

const char* originName(Origin origin) {
    switch (origin) {
        case Origin::Authoring: return "authoring";
        case Origin::Gameplay: return "gameplay";
        case Origin::Simulation: return "simulation";
    }
    return "?";
}

const char* opName(const Op& op) {
    static constexpr const char* names[] = {"terrain", "remove", "plant", "clear", "ecology", "restore"};
    return names[op.what.index()];
}

namespace codec {
namespace {

constexpr std::int64_t kSamplesPerFile = kFileMetres / EditLayer::kSampleMetres;           // 2048
constexpr std::int64_t kPatchSamples = kPatchMetres / EditLayer::kSampleMetres;            // 16
constexpr std::int64_t kPatchesPerFile = kFileMetres / kPatchMetres;                       // 128
constexpr std::int64_t kPatchesPerBlock = EditLayer::kBlockMetres / kPatchMetres;          // 8
constexpr std::int64_t kBlocksPerFile = kFileMetres / EditLayer::kBlockMetres;             // 16
// The journal's name for a terrain op at a coarse level (TerrainOp::level).
constexpr std::uint8_t kCoarseTerrainOp = 6;
constexpr std::int64_t kCellsPerFile = kFileMetres / ecology::kCellMetres;                 // 128
constexpr std::int64_t kPagesPerFile = kFileMetres / ecology::kPageMetres;                 // 64
constexpr std::size_t kPatchValues = std::size_t(kPatchSamples * kPatchSamples);           // 256
static_assert(kPatchesPerFile * kPatchesPerFile <= 65536 && kSamplesPerFile <= 65536);

void fail(std::string* why, const char* what) {
    if (why) *why = what;
}

void writeF64(core::BinaryWriter& out, double v) { out.u64(std::bit_cast<std::uint64_t>(v)); }
void writeF32(core::BinaryWriter& out, float v) { out.u32(std::bit_cast<std::uint32_t>(v)); }
double readF64(core::BinaryReader& in) { return std::bit_cast<double>(in.u64()); }
float readF32(core::BinaryReader& in) { return std::bit_cast<float>(in.u32()); }

void writeCell(core::BinaryWriter& out, const ecology::Cell& c) {
    for (const float v : {c.potentialForest, c.canopy, c.grass, c.shrubs, c.deadwood, c.fertility, c.moisture,
                          c.wetland, c.disturbance, c.youngGrowth, c.agriculture})
        writeF32(out, v);
    out.u16(c.masks);
    out.u8(static_cast<std::uint8_t>(c.biome));
    out.u8(c.succession);
}
ecology::Cell readCell(core::BinaryReader& in) {
    ecology::Cell c;
    for (float* v : {&c.potentialForest, &c.canopy, &c.grass, &c.shrubs, &c.deadwood, &c.fertility, &c.moisture,
                     &c.wetland, &c.disturbance, &c.youngGrowth, &c.agriculture})
        *v = readF32(in);
    c.masks = in.u16();
    c.biome = static_cast<ecology::Biome>(in.u8());
    c.succession = in.u8();
    return c;
}
constexpr std::size_t kCellBytes = 11 * 4 + 4;

void writeAdded(core::BinaryWriter& out, const ecology::Added& a) {
    writeF64(out, a.x);
    writeF64(out, a.y);
    writeF32(out, a.yaw);
    writeF32(out, a.scale);
    writeF32(out, a.tint);
    out.u32(a.model);
}
ecology::Added readAdded(core::BinaryReader& in) {
    ecology::Added a;
    a.x = readF64(in);
    a.y = readF64(in);
    a.yaw = readF32(in);
    a.scale = readF32(in);
    a.tint = readF32(in);
    a.model = in.u32();
    return a;
}

// Local indices within a file, checked on the way in: a record that names a
// place outside its own file is damage, not data.
std::uint16_t localIndex(std::int64_t x, std::int64_t y, std::int64_t side) {
    return std::uint16_t(y * side + x);
}
bool splitIndex(std::uint16_t index, std::int64_t side, std::int64_t& x, std::int64_t& y) {
    if (std::int64_t(index) >= side * side) return false;
    x = std::int64_t(index) % side;
    y = std::int64_t(index) / side;
    return true;
}

// A count read from a file, refused when the rest of the block could not hold
// that many records: a damaged count must not become a huge allocation.
bool plausible(const core::BinaryReader& in, std::uint32_t count, std::size_t minimumEach) {
    return in.ok() && std::uint64_t(count) * minimumEach <= in.remaining();
}

void touch(ecology::Delta& d, const ecology::Key& page) { d.regions[page] = ++d.revision; }

} // namespace

const char* blockName(std::uint16_t type) {
    switch (type) {
        case Heights: return "heights";
        case CoarseHeights: return "coarse heights";
        case Removed: return "removed";
        case Planted: return "planted";
        case Cleared: return "cleared";
        case Cells: return "ecology";
        case Journal: return "journal";
        default: return "unknown";
    }
}

ChunkKey chunkOfCell(const ecology::Key& c) {
    return {kFileLevel, floorDiv(c.first, kCellsPerFile), floorDiv(c.second, kCellsPerFile)};
}
ChunkKey chunkOfPage(const ecology::Key& p) {
    return {kFileLevel, floorDiv(p.first, kPagesPerFile), floorDiv(p.second, kPagesPerFile)};
}
ChunkKey chunkOfPoint(double x, double y) { return engine::world_store::chunkAt(kFileLevel, x, y); }
ChunkKey chunkOfSample(std::int64_t sx, std::int64_t sy) {
    return {kFileLevel, floorDiv(sx, kSamplesPerFile), floorDiv(sy, kSamplesPerFile)};
}
ChunkKey chunkOfEditBlock(std::int64_t bx, std::int64_t by) {
    return {kFileLevel, floorDiv(bx, kBlocksPerFile), floorDiv(by, kBlocksPerFile)};
}
ChunkKey chunkOfSample(int level, std::int64_t sx, std::int64_t sy) {
    const std::int64_t perFile = kFileMetres / EditLayer::stepOf(level);
    return {kFileLevel, floorDiv(sx, perFile), floorDiv(sy, perFile)};
}
ChunkKey chunkOfEditBlock(int level, std::int64_t bx, std::int64_t by) {
    const std::int64_t perFile = kFileMetres / EditLayer::blockMetresOf(level);
    return {kFileLevel, floorDiv(bx, perFile), floorDiv(by, perFile)};
}

std::map<ChunkKey, ChunkObjects> partition(const ecology::Delta& delta, const std::vector<ChunkKey>& wanted) {
    const std::set<ChunkKey> want(wanted.begin(), wanted.end());
    std::map<ChunkKey, ChunkObjects> out;
    for (const auto& key : want) out[key];
    for (const auto& [id, cell] : delta.removed)
        if (const auto c = chunkOfCell(cell); want.contains(c)) out[c].removed.emplace_back(id, cell);
    for (const auto& [cell, value] : delta.cells)
        if (const auto c = chunkOfCell(cell); want.contains(c)) out[c].cells.emplace_back(cell, value);
    for (const auto& [page, objects] : delta.added)
        if (const auto c = chunkOfPage(page); want.contains(c)) out[c].planted.emplace_back(page, &objects);
    for (const auto& [page, mask] : delta.cleared)
        if (const auto c = chunkOfPage(page); want.contains(c)) out[c].cleared.emplace_back(page, mask);
    return out;
}

std::vector<Block> encodeSnapshot(const ChunkKey& chunk, const ChunkObjects& objects,
                                  const std::vector<EditLayer::BlockCopy>& heights) {
    std::vector<Block> blocks;

    // Heights: the non-zero 64 m patches, in index order whatever order the
    // layer's hash map handed its blocks over in - the same state must make
    // the same bytes, or the payload hash would not name the state.
    struct Patch { std::uint16_t index; const core::Fixed* block; std::int64_t px, py; };
    std::vector<Patch> patches;
    for (const auto& b : heights) {
        if (b.level != 0 || chunkOfEditBlock(b.x, b.y) != chunk ||
            b.delta.size() != std::size_t(EditLayer::kBlockSamples) * EditLayer::kBlockSamples)
            continue;
        for (std::int64_t py = 0; py < kPatchesPerBlock; ++py)
            for (std::int64_t px = 0; px < kPatchesPerBlock; ++px) {
                bool any = false;
                for (std::int64_t sy = 0; sy < kPatchSamples && !any; ++sy)
                    for (std::int64_t sx = 0; sx < kPatchSamples && !any; ++sx)
                        any = b.delta[std::size_t((py * kPatchSamples + sy) * EditLayer::kBlockSamples +
                                                  px * kPatchSamples + sx)] != core::kZero;
                if (!any) continue;
                const auto lx = (b.x - chunk.x * kBlocksPerFile) * kPatchesPerBlock + px;
                const auto ly = (b.y - chunk.y * kBlocksPerFile) * kPatchesPerBlock + py;
                patches.push_back({localIndex(lx, ly, kPatchesPerFile), b.delta.data(), px, py});
            }
    }
    if (!patches.empty()) {
        std::sort(patches.begin(), patches.end(), [](const Patch& a, const Patch& b) { return a.index < b.index; });
        core::BinaryWriter out;
        out.u32(std::uint32_t(patches.size()));
        for (const auto& p : patches) {
            out.u16(p.index);
            for (std::int64_t sy = 0; sy < kPatchSamples; ++sy)
                for (std::int64_t sx = 0; sx < kPatchSamples; ++sx)
                    out.fixed(p.block[std::size_t((p.py * kPatchSamples + sy) * EditLayer::kBlockSamples +
                                                  p.px * kPatchSamples + sx)]);
        }
        blocks.push_back({Heights, 1, out.take()});
    }
    // The coarse levels: whole blocks, few of them - a file holds sixteen at
    // sixteen metres and one at sixty-four or two hundred and fifty-six -
    // each with its level and its place in the file, in that order.
    {
        std::vector<const EditLayer::BlockCopy*> coarse;
        for (const auto& b : heights) {
            if (b.level <= 0 || b.level >= EditLayer::kLevels || chunkOfEditBlock(b.level, b.x, b.y) != chunk) continue;
            const auto side = std::size_t(EditLayer::blockSamplesOf(b.level));
            if (b.delta.size() != side * side) continue;
            if (std::none_of(b.delta.begin(), b.delta.end(), [](core::Fixed v) { return v != core::kZero; })) continue;
            coarse.push_back(&b);
        }
        std::sort(coarse.begin(), coarse.end(), [](const auto* a, const auto* b) {
            return std::tie(a->level, a->y, a->x) < std::tie(b->level, b->y, b->x);
        });
        if (!coarse.empty()) {
            core::BinaryWriter out;
            out.u32(std::uint32_t(coarse.size()));
            for (const auto* b : coarse) {
                const std::int64_t perFile = kFileMetres / EditLayer::blockMetresOf(b->level);
                out.u8(std::uint8_t(b->level));
                out.u16(localIndex(b->x - chunk.x * perFile, b->y - chunk.y * perFile, perFile));
                for (const auto v : b->delta) out.fixed(v);
            }
            blocks.push_back({CoarseHeights, 1, out.take()});
        }
    }

    const auto cellIndex = [&](const ecology::Key& c) {
        return localIndex(c.first - chunk.x * kCellsPerFile, c.second - chunk.y * kCellsPerFile, kCellsPerFile);
    };
    const auto pageIndex = [&](const ecology::Key& p) {
        return localIndex(p.first - chunk.x * kPagesPerFile, p.second - chunk.y * kPagesPerFile, kPagesPerFile);
    };
    if (!objects.removed.empty()) {
        core::BinaryWriter out;
        out.u32(std::uint32_t(objects.removed.size()));
        for (const auto& [id, cell] : objects.removed) {
            out.u64(id);
            out.u16(cellIndex(cell));
        }
        blocks.push_back({Removed, 1, out.take()});
    }
    std::size_t planted = 0;
    for (const auto& [page, list] : objects.planted) planted += list->size();
    if (planted) {
        core::BinaryWriter out;
        out.u32(std::uint32_t(objects.planted.size()));
        for (const auto& [page, list] : objects.planted) {
            out.u16(pageIndex(page));
            out.u32(std::uint32_t(list->size()));
            for (const auto& [id, object] : *list) {
                out.u64(id);
                writeAdded(out, object);
            }
        }
        blocks.push_back({Planted, 1, out.take()});
    }
    if (!objects.cleared.empty()) {
        core::BinaryWriter out;
        out.u32(std::uint32_t(objects.cleared.size()));
        for (const auto& [page, mask] : objects.cleared) {
            out.u16(pageIndex(page));
            out.u32(mask);
        }
        blocks.push_back({Cleared, 1, out.take()});
    }
    if (!objects.cells.empty()) {
        core::BinaryWriter out;
        out.u32(std::uint32_t(objects.cells.size()));
        for (const auto& [cell, value] : objects.cells) {
            out.u16(cellIndex(cell));
            writeCell(out, value);
        }
        blocks.push_back({Cells, 1, out.take()});
    }
    return blocks;
}

Block encodeJournal(const ChunkKey& chunk, const std::vector<Op>& ops) {
    core::BinaryWriter out;
    out.u32(std::uint32_t(ops.size()));
    for (const auto& op : ops) {
        // The variant's index names the op, except a terrain op at a coarse
        // level, which is an op of its own: a reader from before levels takes
        // it as one it does not know rather than as four-metre samples.
        const auto* terrain = std::get_if<TerrainOp>(&op.what);
        out.u8(terrain && terrain->level > 0 ? std::uint8_t(kCoarseTerrainOp) : std::uint8_t(op.what.index()));
        out.u64(op.sequence);
        out.u8(static_cast<std::uint8_t>(op.origin));
        std::visit([&](const auto& what) {
            using T = std::decay_t<decltype(what)>;
            if constexpr (std::is_same_v<T, TerrainOp>) {
                out.u8(static_cast<std::uint8_t>(what.tool));
                for (const double v : {what.centreX, what.centreY, what.radius, what.strength, what.seconds})
                    writeF64(out, v);
                // A coarse op carries its level after the brush; a fine one is
                // written as it always was (kind 0, below).
                if (what.level > 0) out.u8(what.level);
                const std::int64_t perFile = kFileMetres / EditLayer::stepOf(what.level);
                out.u32(std::uint32_t(what.samples.size()));
                for (const auto& s : what.samples) {
                    out.u16(std::uint16_t(s.x - chunk.x * perFile));
                    out.u16(std::uint16_t(s.y - chunk.y * perFile));
                    out.fixed(s.add);
                }
            } else if constexpr (std::is_same_v<T, RemoveOp> || std::is_same_v<T, RestoreOp>) {
                out.u64(what.id);
                writeF64(out, what.x);
                writeF64(out, what.y);
            } else if constexpr (std::is_same_v<T, PlantOp>) {
                out.u64(what.id);
                writeAdded(out, what.object);
            } else if constexpr (std::is_same_v<T, ClearOp>) {
                writeF64(out, what.x);
                writeF64(out, what.y);
                out.u32(what.models);
            } else {
                writeF64(out, what.x);
                writeF64(out, what.y);
                writeCell(out, what.cell);
            }
        }, op.what);
    }
    return {Journal, 1, out.take()};
}

bool decodeSnapshot(const ChunkKey& chunk, const std::vector<Block>& blocks, ecology::Delta& objects,
                    EditLayer& heights, std::string* why) {
    for (const auto& block : blocks) {
        if (block.type == Journal) continue;
        core::BinaryReader in(block.bytes);
        if (block.version != 1 && (block.type <= Cells || block.type == CoarseHeights)) { fail(why, "delta block from a newer version"); return false; }
        switch (block.type) {
            case Heights: {
                const auto count = in.u32();
                if (!plausible(in, count, 2 + kPatchValues * 8)) { fail(why, "heights block damaged"); return false; }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::int64_t lx = 0, ly = 0;
                    if (!splitIndex(in.u16(), kPatchesPerFile, lx, ly)) { fail(why, "height patch outside its chunk"); return false; }
                    const auto baseX = chunk.x * kSamplesPerFile + lx * kPatchSamples;
                    const auto baseY = chunk.y * kSamplesPerFile + ly * kPatchSamples;
                    for (std::int64_t sy = 0; sy < kPatchSamples; ++sy)
                        for (std::int64_t sx = 0; sx < kPatchSamples; ++sx)
                            if (const auto v = in.fixed(); v != core::kZero) heights.add(baseX + sx, baseY + sy, v);
                }
                break;
            }
            case CoarseHeights: {
                const auto count = in.u32();
                if (!plausible(in, count, 3 + 32 * 32 * 8)) { fail(why, "coarse heights block damaged"); return false; }
                for (std::uint32_t i = 0; i < count; ++i) {
                    const int level = in.u8();
                    if (level <= 0 || level >= EditLayer::kLevels) { fail(why, "coarse heights at an unknown level"); return false; }
                    const std::int64_t perFile = kFileMetres / EditLayer::blockMetresOf(level);
                    std::int64_t lx = 0, ly = 0;
                    if (!splitIndex(in.u16(), perFile, lx, ly)) { fail(why, "coarse height block outside its chunk"); return false; }
                    const std::int64_t side = EditLayer::blockSamplesOf(level);
                    if (in.remaining() < std::size_t(side * side * 8)) { fail(why, "coarse heights block damaged"); return false; }
                    const auto baseX = (chunk.x * perFile + lx) * side, baseY = (chunk.y * perFile + ly) * side;
                    for (std::int64_t sy = 0; sy < side; ++sy)
                        for (std::int64_t sx = 0; sx < side; ++sx)
                            if (const auto v = in.fixed(); v != core::kZero) heights.add(level, baseX + sx, baseY + sy, v);
                }
                break;
            }
            case Removed: {
                const auto count = in.u32();
                if (!plausible(in, count, 10)) { fail(why, "removed block damaged"); return false; }
                for (std::uint32_t i = 0; i < count; ++i) {
                    const auto id = in.u64();
                    std::int64_t lx = 0, ly = 0;
                    if (!splitIndex(in.u16(), kCellsPerFile, lx, ly)) { fail(why, "removed object outside its chunk"); return false; }
                    const ecology::Key cell{chunk.x * kCellsPerFile + lx, chunk.y * kCellsPerFile + ly};
                    objects.removed[id] = cell;
                    touch(objects, {floorDiv(cell.first, 2), floorDiv(cell.second, 2)});
                }
                break;
            }
            case Planted: {
                const auto pages = in.u32();
                if (!plausible(in, pages, 6)) { fail(why, "planted block damaged"); return false; }
                for (std::uint32_t p = 0; p < pages; ++p) {
                    std::int64_t lx = 0, ly = 0;
                    if (!splitIndex(in.u16(), kPagesPerFile, lx, ly)) { fail(why, "planted page outside its chunk"); return false; }
                    const ecology::Key page{chunk.x * kPagesPerFile + lx, chunk.y * kPagesPerFile + ly};
                    const auto count = in.u32();
                    if (!plausible(in, count, 8 + 8 + 8 + 16)) { fail(why, "planted block damaged"); return false; }
                    for (std::uint32_t i = 0; i < count; ++i) {
                        const auto id = in.u64();
                        const auto object = readAdded(in);
                        if (ecology::page(object.x, object.y) != page) { fail(why, "planted object outside its page"); return false; }
                        objects.added[page][id] = object;
                    }
                    touch(objects, page);
                }
                break;
            }
            case Cleared: {
                const auto count = in.u32();
                if (!plausible(in, count, 6)) { fail(why, "cleared block damaged"); return false; }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::int64_t lx = 0, ly = 0;
                    if (!splitIndex(in.u16(), kPagesPerFile, lx, ly)) { fail(why, "cleared page outside its chunk"); return false; }
                    const ecology::Key page{chunk.x * kPagesPerFile + lx, chunk.y * kPagesPerFile + ly};
                    objects.cleared[page] |= in.u32();
                    touch(objects, page);
                }
                break;
            }
            case Cells: {
                const auto count = in.u32();
                if (!plausible(in, count, 2 + kCellBytes)) { fail(why, "ecology block damaged"); return false; }
                for (std::uint32_t i = 0; i < count; ++i) {
                    std::int64_t lx = 0, ly = 0;
                    if (!splitIndex(in.u16(), kCellsPerFile, lx, ly)) { fail(why, "ecology cell outside its chunk"); return false; }
                    const ecology::Key cell{chunk.x * kCellsPerFile + lx, chunk.y * kCellsPerFile + ly};
                    objects.cells[cell] = readCell(in);
                    touch(objects, {floorDiv(cell.first, 2), floorDiv(cell.second, 2)});
                }
                break;
            }
            default:
                continue;   // a layer from a newer writer: not ours to read
        }
        if (!in.ok() || in.remaining() != 0) { fail(why, "delta block has the wrong length"); return false; }
    }
    return true;
}

std::optional<std::vector<Op>> decodeJournal(const ChunkKey& chunk, const Block& block, std::string* why) {
    if (block.type != Journal || block.version != 1) { fail(why, "not a journal this version reads"); return std::nullopt; }
    core::BinaryReader in(block.bytes);
    const auto count = in.u32();
    if (!plausible(in, count, 10)) { fail(why, "journal damaged"); return std::nullopt; }
    std::vector<Op> ops;
    ops.reserve(count);
    const auto inside = [&](double x, double y) { return chunkOfPoint(x, y) == chunk; };
    for (std::uint32_t i = 0; i < count; ++i) {
        Op op;
        const auto kind = in.u8();
        op.sequence = in.u64();
        const auto origin = in.u8();
        if (origin < 1 || origin > 3) { fail(why, "journal names an unknown origin"); return std::nullopt; }
        op.origin = static_cast<Origin>(origin);
        switch (kind) {
            case 0:
            case kCoarseTerrainOp: {
                TerrainOp t;
                const auto tool = in.u8();
                if (tool >= static_cast<std::uint8_t>(BrushKind::Count)) { fail(why, "journal names an unknown tool"); return std::nullopt; }
                t.tool = static_cast<BrushKind>(tool);
                for (double* v : {&t.centreX, &t.centreY, &t.radius, &t.strength, &t.seconds}) *v = readF64(in);
                if (kind == kCoarseTerrainOp) {
                    t.level = in.u8();
                    if (t.level == 0 || t.level >= EditLayer::kLevels) { fail(why, "journal names an unknown level"); return std::nullopt; }
                }
                const std::int64_t perFile = kFileMetres / EditLayer::stepOf(t.level);
                const auto samples = in.u32();
                if (!plausible(in, samples, 12)) { fail(why, "journal damaged"); return std::nullopt; }
                t.samples.reserve(samples);
                for (std::uint32_t s = 0; s < samples; ++s) {
                    const auto lx = in.u16(), ly = in.u16();
                    if (lx >= perFile || ly >= perFile) { fail(why, "journal sample outside its chunk"); return std::nullopt; }
                    t.samples.push_back({chunk.x * perFile + lx, chunk.y * perFile + ly, in.fixed()});
                }
                op.what = std::move(t);
                break;
            }
            case 1: {
                RemoveOp r;
                r.id = in.u64();
                r.x = readF64(in);
                r.y = readF64(in);
                if (!inside(r.x, r.y)) { fail(why, "journal removal outside its chunk"); return std::nullopt; }
                op.what = r;
                break;
            }
            case 2: {
                PlantOp p;
                p.id = in.u64();
                p.object = readAdded(in);
                if (!inside(p.object.x, p.object.y)) { fail(why, "journal planting outside its chunk"); return std::nullopt; }
                op.what = p;
                break;
            }
            case 3: {
                ClearOp c;
                c.x = readF64(in);
                c.y = readF64(in);
                c.models = in.u32();
                if (!inside(c.x, c.y)) { fail(why, "journal clearing outside its chunk"); return std::nullopt; }
                op.what = c;
                break;
            }
            case 4: {
                EcologyOp e;
                e.x = readF64(in);
                e.y = readF64(in);
                e.cell = readCell(in);
                if (!inside(e.x, e.y)) { fail(why, "journal ecology outside its chunk"); return std::nullopt; }
                op.what = e;
                break;
            }
            case 5: {
                RestoreOp r;
                r.id = in.u64();
                r.x = readF64(in);
                r.y = readF64(in);
                if (!inside(r.x, r.y)) { fail(why, "journal restoring outside its chunk"); return std::nullopt; }
                op.what = r;
                break;
            }
            default:
                fail(why, "journal names an unknown op");
                return std::nullopt;
        }
        if (!in.ok()) { fail(why, "journal truncated"); return std::nullopt; }
        ops.push_back(std::move(op));
    }
    if (in.remaining() != 0) { fail(why, "journal has the wrong length"); return std::nullopt; }
    return ops;
}

bool applyTo(const Op& op, ecology::Delta& objects, EditLayer& heights) {
    return std::visit([&](const auto& what) -> bool {
        using T = std::decay_t<decltype(what)>;
        if constexpr (std::is_same_v<T, TerrainOp>) {
            bool changed = false;
            for (const auto& s : what.samples) {
                if (s.add == core::kZero) continue;
                heights.add(what.level, s.x, s.y, s.add);
                changed = true;
            }
            return changed;
        } else if constexpr (std::is_same_v<T, RemoveOp>) {
            return ecology::removeObject(objects, what.id, what.x, what.y);
        } else if constexpr (std::is_same_v<T, PlantOp>) {
            return ecology::addObject(objects, what.id, what.object);
        } else if constexpr (std::is_same_v<T, ClearOp>) {
            return ecology::clearPage(objects, what.x, what.y, what.models);
        } else if constexpr (std::is_same_v<T, RestoreOp>) {
            return ecology::restoreObject(objects, what.id, what.x, what.y);
        } else {
            ecology::setCell(objects, what.x, what.y, what.cell);
            return true;
        }
    }, op.what);
}

} // namespace codec
} // namespace world::delta


