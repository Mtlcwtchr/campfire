#pragma once
// A small synthetic authoring package: an island skeleton heightmap (R16), a
// control map, region ids, flags, a river, a lake, a ridge, a region outline
// and a point of interest. Something to run the importer and the round trip
// on before real maps exist, and what the tests import.
#include <filesystem>
#include <optional>
#include <string>

namespace engine::world_source {

// A world `km` a side (rounded to whole 32 km chunks). The island stays
// inside the middle; everything within 35 % of the edge is sea. Returns a
// one-line summary.
std::optional<std::string> writeExamplePackage(const std::filesystem::path& package, double km,
                                               std::string* why = nullptr);

} // namespace engine::world_source
