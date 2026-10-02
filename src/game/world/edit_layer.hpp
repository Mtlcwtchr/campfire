#pragma once
// What a person changed about the ground, kept apart from what the generator
// made of it.
//
// The generator is a function of a seed: the same seed is the same world, and
// that is what makes a world shareable, replayable and small enough to hold. An
// editor cannot be allowed to break that. So nothing here edits the generator's
// output - it records a DIFFERENCE from it, sample by sample, and the height
// field adds the two together. Throw the layer away and the world is exactly
// the world the seed describes; keep it and it is that world with a valley dug
// into it.
//
// Sparse, because an edit is local and a world is not. Deltas live in blocks of
// five hundred and twelve metres, the same page the streaming already thinks
// in, and a block exists only once something in it has been touched. A world
// nobody has edited costs one empty hash map and one branch on the fast path,
// which matters: the height query below this is asked millions of times to
// build a page and was measured at a microsecond and a half before anything was
// added to it.
//
// And kept at the scale it was made at. A brush sixteen metres across shapes
// the ground at four metres to a sample; a brush two kilometres across lifts a
// country, and at four metres that is a quarter of a million samples a dab -
// hundreds of megabytes held, and a journal entry the size of a photograph for
// every frame the button is down, for ground whose shape has nothing in it
// finer than the brush. So the layer has levels, four metres to a sample and
// then sixteen, sixty-four and two hundred and fifty-six, each its own sparse
// blocks, and the ground is what they all add up to: a stroke writes the level
// its size asks for (levelFor), and a fine stroke over a coarse one is a
// valley cut into a raised plateau, both kept.
//
// Every edit carries a revision, and so does every patch it lands in.
// Whatever is derived from the ground - a baked height page, a region of
// placed trees - remembers the revision it was made at, and it is stale
// exactly when `revisionIn` over what it read is newer than that. That is the
// whole rule, for every consumer, whoever made the edit and by whatever road.
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"

namespace world {

class EditLayer {
public:
    // Four metres, which is the finest geometry the runtime draws. Editing
    // finer than the ground can be drawn is a promise that cannot be kept.
    static constexpr std::int32_t kSampleMetres = 4;
    static constexpr std::int32_t kBlockMetres = 512;
    static constexpr std::int32_t kBlockSamples = kBlockMetres / kSampleMetres;   // 128
    // How finely a block remembers WHERE it was last written: the ecology
    // cell, and the delta's snapshot patch. A stroke in one corner of a block
    // must not make a page on the far side of it rebake.
    static constexpr std::int32_t kPatchMetres = 64;
    static constexpr std::int32_t kPatchSamples = kPatchMetres / kSampleMetres;   // 16
    static constexpr std::int32_t kPatchesPerBlock = kBlockMetres / kPatchMetres; // 8

    // The levels. Level 0 is the four metres above; each one after it is four
    // times as coarse. A block is a hundred and twenty-eight samples a side,
    // except that no block is wider than a delta file (eight kilometres,
    // world_delta_codec.hpp), so the coarsest has thirty-two. Eight patches a
    // side, whatever the level.
    static constexpr int kLevels = 4;
    static constexpr std::int32_t stepOf(int level) { return kSampleMetres << (2 * level); }   // 4, 16, 64, 256
    static constexpr std::int32_t blockSamplesOf(int level) {
        return level < 3 ? kBlockSamples : kBlockSamples / 4;
    }
    static constexpr std::int32_t blockMetresOf(int level) { return stepOf(level) * blockSamplesOf(level); }
    static constexpr std::int32_t patchSamplesOf(int level) { return blockSamplesOf(level) / kPatchesPerBlock; }
    static constexpr std::int32_t patchMetresOf(int level) { return blockMetresOf(level) / kPatchesPerBlock; }
    // The level a brush of this radius shapes the ground at: the coarsest
    // whose step is no more than a sixth of it. A forty-metre brush works at
    // four metres, a hundred-metre one at sixteen, a two-kilometre one at two
    // hundred and fifty-six.
    static int levelFor(double radiusMetres) {
        int level = 0;
        while (level + 1 < kLevels && double(stepOf(level + 1)) * 6.0 <= radiusMetres) ++level;
        return level;
    }

    // How much this layer adds to the ground at a point, in metres. Nought
    // where nothing has been touched, which is almost everywhere.
    [[nodiscard]] core::Fixed at(core::Fixed x, core::Fixed y) const;

    // Add `metres` at a sample, spread over the brush's own falloff by the
    // caller. Returns what the sample now holds. Sample coordinates are the
    // level's own: stepOf(level) metres apart.
    core::Fixed add(std::int64_t sampleX, std::int64_t sampleY, core::Fixed metres) {
        return add(0, sampleX, sampleY, metres);
    }
    core::Fixed add(int level, std::int64_t sampleX, std::int64_t sampleY, core::Fixed metres);

    // Nothing has been edited at all. The one question the hot path asks.
    [[nodiscard]] bool empty() const { return !any_.load(std::memory_order_acquire); }
    // Changes whenever anything is written, so a cache can tell whether what it
    // holds is still the ground. Read it BEFORE reading the ground: everything
    // written at or below the number read is then in what was read.
    [[nodiscard]] std::uint64_t revision() const { return revision_.load(std::memory_order_acquire); }
    [[nodiscard]] std::size_t blocks() const;

    // Whether any edit falls inside this rectangle of world metres. What a page
    // asks before it decides to rebuild.
    [[nodiscard]] bool touches(core::WorldRect area) const;

    // The newest revision that wrote inside this rectangle, to the patch; nought
    // for ground nobody touched. Something made from the ground in `area` at
    // revision R is stale exactly when this is above R.
    [[nodiscard]] std::uint64_t revisionIn(core::WorldRect area) const;
    // What the layer holds inside this rectangle, as one number: nought when it
    // holds nothing there but zeros, and the same number for the same samples
    // however they were written, in whatever order, in whichever session. The
    // name a derived product can be filed under on disk.
    [[nodiscard]] std::uint64_t fingerprint(core::WorldRect area) const;
    // Where the ground changed after `since`: rectangles around the patches
    // written since, a run of them along a row to a rectangle. Returns the
    // revision it answered at, which is what to pass next time.
    std::uint64_t changedSince(std::uint64_t since, std::vector<core::WorldRect>& out) const;

    void clear();

    // One sample as it is stored, without the bilinear read between samples.
    [[nodiscard]] core::Fixed sample(std::int64_t sampleX, std::int64_t sampleY) const {
        return sample(0, sampleX, sampleY);
    }
    [[nodiscard]] core::Fixed sample(int level, std::int64_t sampleX, std::int64_t sampleY) const;
    // What one level adds at a point, bilinear between its samples.
    [[nodiscard]] core::Fixed levelAt(int level, core::Fixed x, core::Fixed y) const;

    // Told of every write, under the layer's lock, with the level and block it
    // landed in and the revision it made. The persistent delta listens, so a
    // stroke is saved whichever tool made it. Must not call back into the layer.
    using Observer = std::function<void(int level, std::int64_t blockX, std::int64_t blockY, std::uint64_t revision)>;
    void observe(Observer observer);

    // What a saver copies out: every block of a level in an inclusive range of
    // its block coordinates, whole, and the revision the copy was taken at.
    struct BlockCopy {
        int level = 0;
        std::int64_t x = 0, y = 0;
        std::vector<core::Fixed> delta;   // blockSamplesOf(level) squared
    };
    std::uint64_t copyBlocks(std::int64_t minBlockX, std::int64_t minBlockY, std::int64_t maxBlockX,
                             std::int64_t maxBlockY, std::vector<BlockCopy>& out) const {
        return copyBlocks(0, minBlockX, minBlockY, maxBlockX, maxBlockY, out);
    }
    std::uint64_t copyBlocks(int level, std::int64_t minBlockX, std::int64_t minBlockY, std::int64_t maxBlockX,
                             std::int64_t maxBlockY, std::vector<BlockCopy>& out) const;
    // Every block of every level whose corner lies in [minX, maxX) x [minY, maxY)
    // metres: what one delta file holds, blocks never being wider than a file.
    std::uint64_t copyBlocksWithin(std::int64_t minX, std::int64_t minY, std::int64_t maxX, std::int64_t maxY,
                                   std::vector<BlockCopy>& out) const;
    // Every block of a level that exists, as block coordinates.
    [[nodiscard]] std::vector<std::pair<std::int64_t, std::int64_t>> blockKeys(int level = 0) const;

private:
    struct Block {
        explicit Block(int level)
            : side(blockSamplesOf(level)), delta(std::size_t(side) * std::size_t(side), core::kZero) {}
        // One delta per sample, in metres. Dense inside a block because a brush
        // stroke fills most of the block it lands in, and a hundred and
        // twenty-eight squared fixed-point numbers is a hundred and twenty-eight
        // kilobytes.
        std::int32_t side;
        std::vector<core::Fixed> delta;
        // The revision of the last write into each patch, and the newest of them.
        std::array<std::uint64_t, std::size_t(kPatchesPerBlock) * kPatchesPerBlock> patches{};
        std::uint64_t revision = 0;
    };
    using Blocks = std::unordered_map<std::uint64_t, std::unique_ptr<Block>>;
    [[nodiscard]] core::Fixed levelAtLocked(int level, core::Fixed x, core::Fixed y) const;
    // Calls `visit(blockX, blockY, block)` for every block that exists within
    // the inclusive block range, walking whichever is smaller. Lock held.
    template<class Visit>
    void eachBlockLocked(const Blocks& blocks, std::int64_t minX, std::int64_t minY, std::int64_t maxX,
                         std::int64_t maxY, Visit&& visit) const;

    // Read by the page builders on several workers while a brush writes from
    // the main thread, so it is shared rather than exclusive: many readers and
    // one writer is exactly what this is.
    mutable std::shared_mutex mutex_;
    std::array<Blocks, kLevels> levels_;
    // Anything held at any level: read without the lock by the hot path.
    std::atomic<bool> any_{false};
    std::atomic<std::uint64_t> revision_{0};
    // What `clear` threw away, and at which revision: those rectangles changed
    // too, and nothing is left in `blocks_` to say so.
    std::vector<std::pair<std::uint64_t, core::WorldRect>> cleared_;
    Observer observer_;
};

} // namespace world
