#pragma once
#include <string>
#include <vector>
#include "game/client/camera.hpp"
#include "game/world/height_field.hpp"
#include "game/world/weather.hpp"

namespace client {
std::vector<std::string> inspectWorld(const generation::WorldMapData& map, const world::HeightField& field,
                                     const Camera& camera, int mouseX, int mouseY,
                                     const world::weather::Snapshot* weather = nullptr);
}
