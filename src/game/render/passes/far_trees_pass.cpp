#include "game/render/passes/far_trees_pass.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/gpu_terrain.hpp"
#include "game/render/pass_ids.hpp"

namespace game {

engine::PassPlace FarTreesPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    renderer_ = &into;
    ready_ = false;
    if (!pages_ || !pages_->ensure(device)) return {};
    // The impostor views of the two tree species the scatter places, from the
    // same baked catalogue the placed trees draw: broadleaf then conifer, eight
    // views each.
    const auto directory = device.assets() / "../generated/scene_models";
    std::ifstream input(directory / "manifest.json");
    const auto manifest = input ? nlohmann::json::parse(input, nullptr, false) : nlohmann::json();
    if (!manifest.is_object() || !manifest.contains("models") || !manifest.contains("colours")) {
        std::cerr << "far trees: no scene model catalogue, off\n";
        return {};
    }
    std::vector<std::vector<std::filesystem::path>> layers;
    for (const char* name : {"CommonTree_1", "Pine_1"}) {
        const auto found = std::find_if(manifest["models"].begin(), manifest["models"].end(),
            [&](const auto& m) { return m.value("name", std::string()) == name; });
        if (found == manifest["models"].end()) { std::cerr << "far trees: no " << name << '\n'; return {}; }
        const int first = found->value("impostor", -1);
        for (int view = 0; view < 8; ++view) {
            const int index = first + view;
            if (first < 0 || index >= int(manifest["colours"].size())) return {};
            layers.push_back({directory / manifest["colours"][std::size_t(index)].get<std::string>()});
        }
    }
    atlas_ = device.loadArrayMipped(layers, true);
    if (!atlas_) return {};
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};

    engine::PipelineWanted wanted;
    wanted.shaderFile = "far_trees.hlsl";
    wanted.vertexEntry = "FarTreesVS";
    wanted.fragmentEntry = "FarTreesPS";
    wanted.blend = false;       // screen-door coverage writes depth; no sorting
    wanted.depthTest = true;
    wanted.depthWrite = true;
    auto pipeline = device.makePipeline(wanted);
    if (!pipeline) { std::cerr << "far trees: " << device.error() << '\n'; return {}; }
    pipeline_ = into.take(std::move(pipeline));
    bindings_ = into.take({{atlas_.get(), sampler_.get()}, shadow_});
    vertexBindings_ = into.takeVertex(pages_->bindings());
    ready_ = true;
    return {engine::passOf(Pass::FarTrees), engine::stageOf(Stage::World),
            static_cast<engine::PassOrder>(Order::Opaque)};
}

bool FarTreesPass::anything(const engine::Frame& frame) const {
    const auto* m = frame.scene.viewProjection;
    const bool perspective = m[12] != 0 || m[13] != 0 || m[14] != 0;
    return ready_ && enabled_ && perspective && frame.scene.extra[2] > 0.01f &&
           pages_ && pages_->finalStage() && !pages_->drawing().empty();
}

void FarTreesPass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    renderer_->replaceVertex(vertexBindings_, pages_->bindings());
    const double start = std::max(200.0, start_);
    // The cells are sized to the distance they start at, not to 40 m wherever
    // that is: a cell's size on screen is spacing / distance, and that - not
    // the metres - is what has to stay put. With the spacing pinned, pushing
    // the placed objects out to 6 km made every ring 600 x 600 cells and the
    // pass 1.8 million triangles, nearly all of them sea, sky and cull.
    const double spacing = kBaseSpacing * std::max(1.0, start / kReferenceStart);
    const auto side = std::uint32_t(std::ceil(4.0 * start / spacing));
    // Only the rings the draw distance reaches: each doubles the last, and a
    // ring that starts past the fog is all vertex work and no tree.
    const double reach = frame.scene.fog[0] > 0 ? double(frame.scene.fog[0]) : start * 32.0;
    std::uint32_t rings = 1;
    while (rings < kRings && start * std::exp2(double(rings)) < reach) ++rings;
    engine::DrawItem item;
    item.author = 7;
    item.pipeline = pipeline_;
    item.bindings = bindings_;
    item.vertexBindings = vertexBindings_;
    item.vertexCount = 3;
    item.instances = side * side * rings;
    item.hasOwnData = item.ownToVertex = true;
    // The page window and table size as every page draw carries them; the
    // vectors the page helpers do not read carry this pass's numbers.
    const auto parameters = pages_->parameters(pages_->drawing().front());
    std::copy(parameters.begin(), parameters.end(), item.own);
    item.own[0] = float(start); item.own[1] = float(spacing);
    item.own[2] = float(side); item.own[3] = float(rings);
    item.own[8] = frame.scene.camera[0]; item.own[9] = frame.scene.camera[1];
    item.own[10] = frame.scene.camera[2]; item.own[11] = float(kBand);
    item.own[14] = 3.0f; // ground data level to read: H64, resident far out
    queue.push(item);
}

} // namespace game

