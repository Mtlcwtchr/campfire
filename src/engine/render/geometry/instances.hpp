#pragma once
// One buffer for every instance drawn in a frame.
//
// A crowd is not a thousand things to draw, it is one thing to draw a thousand
// times, and the difference between those two sentences is the whole of why
// this exists. The card is told the shape once - a quad, a barrel, a pawn - and
// then handed a list: where each copy stands, how big, which way round, what
// colour, which picture off the atlas. One draw call, one bind, however many
// copies.
//
// What it is *not* is a buffer per batch. Instances change every frame, so a
// buffer per batch is a buffer created and destroyed every frame, and the driver
// charges for creating one whether or not the bytes in it changed - which was
// measured on the foliage, where a hundred patches arriving at once meant three
// hundred submits. So: one buffer, kept between frames, grown when a frame needs
// more than the last one did, and every batch a byte offset into it
// (DrawItem::vertexOffset).
//
// It knows nothing about what an instance is. Sixteen bytes or sixty-four, a
// sprite or a tree - the arena copies bytes and hands back where it put them.
// That is what lets the same arena serve a sprite pass, a wall pass and whatever
// the game grows next, and it is why this is engine code.

#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

#include "engine/render/device.hpp"

namespace engine {

class InstanceArena {
public:
    InstanceArena() = default;
    // The same arena with a different usage serves a different kind of
    // per-frame list. Draw arguments are the second one: a frame decides how
    // many draws it needs and what each covers, and that is a list that changes
    // every frame and must not be a buffer created every frame either.
    explicit InstanceArena(SDL_GPUBufferUsageFlags usage) : usage_(usage) {}

    // Forget the last frame's contents. The buffer on the card is kept.
    void begin();

    // Copies `count` instances of `stride` bytes and returns the byte offset to
    // hand to DrawItem::vertexOffset[1]. Alignment is the caller's stride, which
    // is what the vertex layout wants anyway.
    std::uint32_t add(const void* data, std::size_t count, std::size_t stride);

    // Prefer the typed range: byte counts cannot accidentally become instance counts.
    template<class T, std::size_t Extent>
    std::uint32_t add(std::span<T, Extent> data) {
        static_assert(std::is_trivially_copyable_v<T>);
        return add(data.data(), data.size(), sizeof(T));
    }

    // Onto the card, once, for the whole frame. Grows the buffer when this frame
    // asked for more room than the last one had - and grows it by half again
    // rather than to exactly what was asked for, so a crowd that walks in one at
    // a time does not recreate the buffer once per newcomer.
    bool upload(Device& device, Device::Uploader& uploader);

    SDL_GPUBuffer* buffer() const { return held_.get(); }
    std::size_t bytes() const { return staging_.size(); }
    std::size_t capacity() const { return capacity_; }
    // How many instances went in this frame, for the profiler: the number that
    // says whether a crowd is being batched or drawn one at a time.
    std::size_t instances() const { return instances_; }

private:
    SDL_GPUBufferUsageFlags usage_ =
            SDL_GPU_BUFFERUSAGE_VERTEX | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    std::vector<std::uint8_t> staging_;
    Buffer held_;
    std::size_t capacity_ = 0;
    std::size_t instances_ = 0;
};

} // namespace engine
