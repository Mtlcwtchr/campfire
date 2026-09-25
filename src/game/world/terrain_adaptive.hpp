#pragma once
// Compatibility vocabulary for the world's sampling/planning policies.
// Geometry storage and reconstruction belong to the reusable engine component.
#include "engine/geometry/smart_terrain_mesh.hpp"

namespace world::terrain {

using engine::SurfacePage;
using engine::AdaptiveVertex;
using AdaptiveMesh = engine::SmartTerrainMesh;
using engine::SurfaceSample;
using engine::SurfaceTolerance;
using engine::HeightSample;
using engine::AdaptiveCriteria;
using engine::makeAdaptiveMesh;

} // namespace world::terrain

