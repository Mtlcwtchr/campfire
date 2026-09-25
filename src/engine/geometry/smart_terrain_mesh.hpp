#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace engine {
struct SurfacePage {
    int side = 0, padding = 0, step = 0;
    std::vector<float> bed, head;
};
struct AdaptiveVertex {
    // Explicit source heights, exact parent triangles and stitched shared edges.
    std::uint16_t x = 0, y = 0, skirt = 0, corner = 0;
    float parentBed = 0, parentHead = 0;
    float edgeBed = 0, edgeHead = 0;
    float sourceBed = 0, sourceHead = 0;
    float priorBed = 0, priorParent = 0, priorEdge = 0;
    // Opaque application channels, not interpreted by geometry reconstruction.
    std::array<float, 4> diagnostic{};
    std::array<float, 2> drainage{};
    // Previous published surface: bed, water head, prior generation stage.
    // Bit 3 of skirt enables this endpoint; source/parent/edge stay immutable.
    std::array<float, 3> displayFrom{};
};
struct SmartTerrainMesh {
    int cells = 0;
    double step = 0, tolerance = 0;
    std::vector<float> bed, head, prior;
    std::vector<std::uint8_t> splits;
    std::vector<AdaptiveVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t surfaceIndices = 0;
    // A prefix of the surface: the triangles with water standing over at least
    // one corner. The water pass draws these and nothing else.
    std::uint32_t wetIndices = 0;
    double detailError = 0;
    std::shared_ptr<const SmartTerrainMesh> unstitched;
    // Queries the actual triangulation, never a bilinear substitute for morph.
    std::array<float, 2> sample(double x, double y) const;
    float samplePrior(double x, double y) const;
    std::size_t bytes() const;
};
using SurfaceSample = std::function<std::array<float, 2>(double, double)>;
using SurfaceTolerance = std::function<double(double, double)>;
using HeightSample = std::function<float(double, double)>;
struct AdaptiveCriteria {
    int boundaryStride = 1;
    double normalErrorDegrees = 0;
    double normalHeightFloor = 0.25;
};
// Diamond bisection with accumulated source error and parent-normal protection.
// Immutable result; height providers, regional policy and residency are external.
std::shared_ptr<const SmartTerrainMesh> makeAdaptiveMesh(int cells, double step, double tolerance,
    const SurfaceSample& surface, const SurfaceSample& parent = {}, double morphTolerance = 0.001,
    const SurfaceTolerance& localTolerance = {}, const HeightSample& prior = {},
    const HeightSample& priorParent = {}, AdaptiveCriteria criteria = {});
} // namespace engine
