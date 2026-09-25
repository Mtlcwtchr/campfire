#include "game/render/calc/terrain_stream.hpp"

#include "engine/render/frame.hpp"
#include "game/client/explorer.hpp"
#include "game/render/pass_ids.hpp"

namespace game {

engine::PassId TerrainStreamPass::id() const { return engine::passOf(Pass::Stream); }

void TerrainStreamPass::run(engine::Frame& frame) { explorer_.update(camera_, frame.step); }

} // namespace game
