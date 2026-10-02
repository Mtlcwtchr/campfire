#pragma once
// The vector files of an authoring package (world_authoring_import_export_spec
// §2, §6), read and written.
//
// One file per kind, every feature with a stable id, coordinates in world
// metres:
//
//   { "version": 1, "kind": "rivers", "features": [
//       { "id": "river:amber", "geometry": "line",
//         "points": [[x, y], ...],
//         "values": { "width": [w0, w1, ...] },          // per vertex, optional
//         "properties": { "flow_class": 2, "strength": 200 } },
//       { "id": "lake:mere", "geometry": "polygon",
//         "rings": [ { "points": [[x, y], ...] } ],       // outer, then holes
//         "properties": { "water_level_m": 112.5 } } ] }
//
// Points of interest have their own file, as the spec has them:
//
//   { "version": 1, "poi": [
//       { "id": "poi:ford", "type": "settlement", "position": [x, y, z],
//         "yaw_deg": 90, "scale": 1, "metadata": { ... } } ] }
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "engine/world_source/world_source.hpp"

namespace engine::world_source {

std::optional<std::vector<Feature>> readVectorFile(const std::filesystem::path& file, const std::string& kind,
                                                   std::string* why = nullptr);
bool writeVectorFile(const std::filesystem::path& file, const std::string& kind, const std::vector<Feature>& features,
                     std::string* why = nullptr);
std::optional<std::vector<Feature>> readPoiFile(const std::filesystem::path& file, std::string* why = nullptr);
bool writePoiFile(const std::filesystem::path& file, const std::vector<Feature>& poi, std::string* why = nullptr);

} // namespace engine::world_source
