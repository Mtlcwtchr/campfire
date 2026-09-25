#include "engine/render/geometry/instanced.hpp"

#include <array>

namespace engine {

Geometry unitQuad(Device& device) {
    const auto corners = SmartSpriteMesh{}.vertices();
    const auto& order = SmartSpriteMesh::indices;
    Geometry shape;
    Device::Uploader uploader(device);
    shape.vertices = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, corners.data(), sizeof(corners));
    shape.indices = uploader.add(SDL_GPU_BUFFERUSAGE_INDEX, order.data(), sizeof(order));
    uploader.finish();
    shape.indexCount = static_cast<std::uint32_t>(order.size());
    shape.indexSize = SDL_GPU_INDEXELEMENTSIZE_16BIT;
    return shape;
}

DrawItem instanced(const Geometry& shape, const InstanceArena& arena, std::uint32_t at,
                   std::uint32_t count) {
    DrawItem item;
    (void)arena;   // the buffer is filled in at issue time; see instancesFromArena
    item.vertex[0] = shape.vertices.get();
    item.instancesFromArena = true;
    item.vertexOffset[1] = at;
    item.vertexStreams = 2;
    item.index = shape.indices.get();
    item.indexSize = shape.indexSize;
    item.indexCount = shape.indexCount;
    item.instances = count;
    return item;
}

DrawItem instanced(const Geometry& shape, const InstanceArena& arena, std::uint32_t at,
                   std::uint32_t count, IndexRange range) {
    DrawItem item = instanced(shape, arena, at, count);
    item.firstIndex = range.first;
    item.indexCount = range.count;
    return item;
}

} // namespace engine
