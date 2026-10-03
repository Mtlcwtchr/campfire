#pragma once
#include <algorithm>
#include "engine/render/material.hpp"
#include "engine/geometry/smart_terrain_mesh.hpp"

namespace engine {
struct MeshInstances {
    std::uint32_t count = 1, offset = 0;
    bool arena = false;
    SDL_GPUBuffer* buffer = nullptr;
    SDL_GPUBuffer* indirect = nullptr;
    std::uint32_t indirectOffset = 0, indirectDraws = 1;
};
struct ObjectParameters {
    std::array<float, 16> values{};
    bool vertex = false, fragment = false;
};
// A component, not a pass or another pipeline. Passes schedule components;
// geometry, material and instances can be shared/changed independently.
class MeshRenderer {
public:
    // Who this renderer belongs to, copied onto every draw it makes so the
    // frame's triangle tally can be broken down by pass.
    std::uint8_t author = 0;
    std::shared_ptr<const Mesh> mesh;
    MaterialInstance material;
    DrawItem drawItem(MeshView geometry, MeshInstances instances = {}, ObjectParameters object = {}) const {
        if (!geometry.vertices || !geometry.indices || !geometry.range.count)
            throw std::invalid_argument("mesh renderer requires resident indexed geometry");
        DrawItem item;
        item.author = author;
        material.apply(item);
        item.vertex[0] = geometry.vertices;
        item.vertexOffset[0] = geometry.vertexOffset;
        item.index = geometry.indices; item.indexSize = geometry.indexSize;
        item.firstIndex = geometry.range.first; item.indexCount = geometry.range.count;
        item.vertexStreams = instances.arena || instances.buffer ? 2 : 1;
        item.instancesFromArena = instances.arena;
        item.vertex[1] = instances.buffer; item.vertexOffset[1] = instances.offset;
        item.instances = instances.count;
        item.indirect = instances.indirect; item.indirectOffset = instances.indirectOffset;
        item.indirectDraws = instances.indirectDraws;
        std::copy(object.values.begin(), object.values.end(), item.own);
        item.ownToVertex = object.vertex; item.hasOwnData = object.fragment;
        return item;
    }
    void submit(DrawQueue& queue, MeshView geometry, MeshInstances instances = {}, ObjectParameters object = {}) const {
        if (instances.count || instances.indirect) queue.push(drawItem(geometry, instances, object));
    }
    void submit(DrawQueue& queue, MeshInstances instances = {}, ObjectParameters object = {}) const {
        if (!mesh) throw std::logic_error("mesh renderer has no mesh");
        submit(queue, MeshView::of(*mesh), instances, object);
    }
};
class SmartMeshRenderer : public MeshRenderer {
public:
    double pixelError = 1;
    std::shared_ptr<const SmartMesh> hierarchy;
    SmartMesh::Cut select(double pixelsPerMetre, std::span<const std::uint8_t> resident = {}) const {
        if (!hierarchy) throw std::logic_error("smart renderer has no hierarchy");
        return hierarchy->select(pixelsPerMetre, pixelError, resident);
    }
    bool submitCut(DrawQueue& queue, double pixelsPerMetre, MeshInstances instances = {},
                   ObjectParameters object = {}, std::span<const std::uint8_t> resident = {}) const {
        if (!mesh) throw std::logic_error("smart renderer has no mesh");
        const auto cut = select(pixelsPerMetre, resident);
        if (!cut.complete) return false;
        for (auto node : cut.nodes) {
            auto view = MeshView::of(*mesh); view.range = hierarchy->node(node).indices;
            submit(queue, view, instances, object);
        }
        return true;
    }
};
class SmartTerrainRenderer : public MeshRenderer {
public:
    // The reconstruction component already produced the error-bounded cut and
    // stitched its morph endpoints. Submission must not simplify it a second time.
    void submitSurface(DrawQueue& queue, MeshView geometry, const SmartTerrainMesh& surface,
                       bool skirts, ObjectParameters object) const {
        geometry.range = {geometry.range.first,
                          skirts ? static_cast<std::uint32_t>(surface.indices.size()) : surface.surfaceIndices};
        submit(queue, geometry, {}, object);
    }
};
} // namespace engine
