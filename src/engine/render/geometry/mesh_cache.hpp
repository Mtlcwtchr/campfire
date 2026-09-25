#pragma once
// Immutable geometry on the card, kept by key.
//
// Something builds a mesh once and then never changes it - a patch of ground, a
// chunk of a building, a batch of sprites for a settlement that is not moving.
// This uploads it the first time it is asked for, hands back the buffers every
// frame after that, and releases it when nobody has asked for a while.
//
// It knows nothing about what the bytes mean. A mesh here is three blocks of
// memory and two counts; whether the vertices are ground with material weights
// or a wall with a normal map is the caller's business entirely. That is what
// makes this engine code rather than terrain code, and it is what lets the same
// cache hold the world and the interface.

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/render/mesh.hpp"

namespace engine {

// What the owner hands over the first time a key is asked for. The pointers are
// borrowed for the call only: the cache copies onto the card and keeps nothing.
struct MeshUpload {
    const void* vertices = nullptr;
    std::size_t vertexBytes = 0;
    const void* indices = nullptr;
    std::size_t indexBytes = 0;
    const void* instances = nullptr;
    std::size_t instanceBytes = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t instanceCount = 0;
    // Four bytes the owner may use for anything it wants to know later without
    // going back to the world - whether this mesh has water in it, which
    // material dominates, how far away it was built for. The cache stores them
    // and never reads them.
    std::uint32_t tags = 0;
    SDL_GPUIndexElementSize indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
};

class MeshCache {
public:
    // Starts a frame's list: everything wanted between here and forget() is what
    // the passes will be given to draw.
    void begin(std::uint64_t frame);

    // `make` is only called when the key is not on the card yet, because turning
    // a patch of world into vertices is a walk over every vertex in it and doing
    // that for geometry that is already uploaded was most of what the old draw
    // loop spent its time on.
    // `now` is four bytes the owner attaches to this mesh *for this frame*,
    // as against the tags baked in when it was built. What a mesh is is settled
    // once; what it is being used for can change every frame - the same ground
    // is the real thing when its level is the one the view asked for and a
    // stand-in when it is not.
    template <class Make>
    void want(Device::Uploader& uploader, std::int64_t key, std::uint32_t now, Make&& make,
              const std::array<float, 16>& drawData = {}) {
        auto [it, fresh] = meshes_.try_emplace(key);
        Mesh& mesh = it->second;
        if (fresh) {
            const MeshUpload source = make();
            mesh.vertices = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, source.vertices,
                                         source.vertexBytes);
            mesh.indices =
                    uploader.add(SDL_GPU_BUFFERUSAGE_INDEX, source.indices, source.indexBytes);
            if (source.instanceBytes > 0)
                mesh.instances = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, source.instances,
                                              source.instanceBytes);
            mesh.indexCount = source.indexCount;
            mesh.indexSize = source.indexSize;
            mesh.instanceCount = source.instanceCount;
            mesh.instanceBytes = source.instanceBytes;
            mesh.tags = source.tags;
            ++uploaded_;
        }
        mesh.lastUsed = frame_;
        drawing_.push_back({&mesh, now, drawData});
    }

    // Releases what nobody has asked for in `keepFrames`. A short tail rather
    // than none: a camera turning back to where it just was should not pay for
    // the same geometry twice.
    void forget(std::uint64_t keepFrames);

    struct Drawn {
        const Mesh* mesh;
        std::uint32_t now;
        std::array<float, 16> data{};
    };
    const std::vector<Drawn>& drawing() const { return drawing_; }
    std::size_t held() const { return meshes_.size(); }
    std::size_t uploadedThisFrame() const { return uploaded_; }
    void clear();

private:
    std::unordered_map<std::int64_t, Mesh> meshes_;
    std::vector<Drawn> drawing_;
    std::uint64_t frame_ = 0;
    std::size_t uploaded_ = 0;

    // Instance buffers a forgotten mesh leaves behind, held rather than
    // released outright: a camera turning back and forth across the same
    // ground would otherwise pay for the same buffer's creation and
    // destruction on every crossing. Nothing reads from this pool yet - the
    // budget just caps how much it is allowed to hold.
    struct PooledInstances { Buffer buffer; std::size_t bytes = 0; };
    static constexpr std::size_t kInstancePoolBudget = 8 * 1024 * 1024;
    std::vector<PooledInstances> instancePool_;
    std::size_t instancePoolBytes_ = 0;
};

} // namespace engine
