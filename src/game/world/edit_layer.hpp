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
// Every edit carries a revision. The streaming rebuilds what it holds when the
// number changes, which is what makes a brush stroke show up in the geometry
// rather than only in the data.
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
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

    // How much this layer adds to the ground at a point, in metres. Nought
    // where nothing has been touched, which is almost everywhere.
    [[nodiscard]] core::Fixed at(core::Fixed x, core::Fixed y) const;

    // Add `metres` at a point, spread over the brush's own falloff by the
    // caller. Returns what the sample now holds.
    core::Fixed add(std::int64_t sampleX, std::int64_t sampleY, core::Fixed metres);

    // Nothing has been edited at all. The one question the hot path asks.
    [[nodiscard]] bool empty() const { return blocks_.empty(); }
    // Changes whenever anything is written, so a cache can tell whether what it
    // holds is still the ground.
    [[nodiscard]] std::uint64_t revision() const { return revision_; }
    [[nodiscard]] std::size_t blocks() const { return blocks_.size(); }

    // Whether any edit falls inside this rectangle of world metres. What a page
    // asks before it decides to rebuild.
    [[nodiscard]] bool touches(core::WorldRect area) const;

    void clear();

private:
    struct Block {
        // One delta per sample, in metres. Dense inside a block because a brush
        // stroke fills most of the block it lands in, and a hundred and
        // twenty-eight squared floats is sixty-four kilobytes.
        std::vector<core::Fixed> delta = std::vector<core::Fixed>(
                std::size_t(kBlockSamples) * kBlockSamples, core::kZero);
    };
    static std::uint64_t keyOf(std::int64_t blockX, std::int64_t blockY) {
        return (std::uint64_t(std::uint32_t(blockX)) << 32) | std::uint32_t(blockY);
    }

    // Read by the page builders on several workers while a brush writes from
    // the main thread, so it is shared rather than exclusive: many readers and
    // one writer is exactly what this is.
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::uint64_t, std::unique_ptr<Block>> blocks_;
    std::uint64_t revision_ = 0;
};

} // namespace world
