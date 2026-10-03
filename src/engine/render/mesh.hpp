#pragma once
#include "engine/render/device.hpp"
#include "engine/geometry/smart_mesh.hpp"

namespace engine {
struct VertexLayout {
    std::vector<SDL_GPUVertexBufferDescription> buffers;
    std::vector<SDL_GPUVertexAttribute> attributes;
};
// Geometry resource only. Materials and per-instance state are not baked in.
// Share it between renderer components, passes and different materials.
struct Mesh {
    Buffer vertices, indices, instances;
    std::uint32_t indexCount = 0;
    SDL_GPUIndexElementSize indexSize = SDL_GPU_INDEXELEMENTSIZE_16BIT;
    std::uint32_t instanceCount = 0;
    std::size_t instanceBytes = 0;
    std::uint32_t tags = 0;
    std::uint64_t lastUsed = 0; // legacy MeshCache bookkeeping
    explicit operator bool() const { return bool(vertices) && indexCount > 0; }
};
struct MeshView {
    SDL_GPUBuffer* vertices = nullptr;
    SDL_GPUBuffer* indices = nullptr;
    SDL_GPUIndexElementSize indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
    IndexRange range;
    // Where this mesh's vertices start in `vertices`, in bytes: a mesh that
    // lives in a pooled page rather than in a buffer of its own.
    std::uint32_t vertexOffset = 0;
    static MeshView of(const Mesh& mesh) {
        return {mesh.vertices.get(), mesh.indices.get(), mesh.indexSize, {0, mesh.indexCount}};
    }
};
} // namespace engine
