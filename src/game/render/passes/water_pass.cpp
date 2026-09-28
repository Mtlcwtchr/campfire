#include "game/render/passes/water_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/passes/terrain_pass.hpp"
#include "game/world/terrain_grid.hpp"
#include "game/render/gpu_terrain.hpp"

namespace game {

engine::PassPlace WaterPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    renderer_ = &into;
    if (pages_ && !pages_->ensure(device)) return {};
    if (!pages_) {
    const world::terrain::GridTopology grid = world::terrain::makeGridTopology();
    gridIndices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, grid.indices.data(),
                                       grid.indices.size() * sizeof(std::uint16_t));
    gridIndexCount_ = static_cast<std::uint32_t>(grid.indices.size());
    if (!gridIndices_) return {};
    }

    engine::PipelineWanted wanted;
    wanted.shaderFile = "water.hlsl";
    wanted.vertexEntry = "WaterVS";
    wanted.fragmentEntry = "WaterPS";
    wanted.buffers = terrainBuffers();
    wanted.attributes = terrainAttributes();
    wanted.blend = true;
    wanted.depthTest = true;
    // Water is a surface over the world rather than a part of it: it reads the
    // depth everything else wrote and does not write its own, or two rivers
    // crossing would each hide the other.
    wanted.depthWrite = false;
    engine::GraphicsPipeline graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    if (pages_) {
        // The open-sea sheet under the paged world: the same vertices as the
        // old path, the page water's shading, and only where the picture
        // behind it is sky (WaterSheetPS) - never a second sea over the first.
        wanted.shaderFile = "water_pages.hlsl";
        wanted.vertexEntry = "WaterSheetVS";
        wanted.fragmentEntry = "WaterSheetPS";
        auto sheetGraphics = device.makePipeline(wanted);
        if (!sheetGraphics) return {};
        sheetPipeline_ = into.take(std::move(sheetGraphics));
        wanted.shaderFile = "water_pages.hlsl";
        wanted.vertexEntry = "AdaptiveWaterVS";
        wanted.fragmentEntry = "WaterPagePS";
        wanted.buffers = pageGridBuffers();
        wanted.attributes = pageGridAttributes();
        auto pageGraphics = device.makePipeline(wanted);
        if (!pageGraphics) return {};
        pagePipeline_ = into.take(std::move(pageGraphics));
        pageBindings_ = into.takeVertex(pages_->bindings());
    }

    // The same sampler the ground gets, and for the same reason: the surface
    // tiles in world space, so water seen at a glancing angle across half a bay
    // is exactly the case anisotropy exists for.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.enable_anisotropy = true;
    sampler.max_anisotropy = 8.0f;
    sampler.max_lod = 4.0f;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};
    // The copy of the world behind the water (refraction) and around it
    // (reflection): read pixel for pixel, never tiled, never mipped.
    SDL_GPUSamplerCreateInfo scene{};
    scene.min_filter = scene.mag_filter = SDL_GPU_FILTER_LINEAR;
    scene.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    scene.address_mode_u = scene.address_mode_v = scene.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sceneSampler_ = device.makeSampler(scene);
    if (!sceneSampler_) return {};

    // Ripple, swell, moving foam, PHX residue - in shader layer order.
    // Residue is baked separately by tools/bake_foam_residue.py.
    // The mip chain is baked rather than generated: a normal map filtered
    // down by a driver is a normal map that is no longer unit length, and water
    // lit by one of those is lit wrong at every distance but the nearest.
    std::vector<std::vector<std::filesystem::path>> layers;
    for (const char* layer : {"ripple", "swell", "foam", "residue"}) {
        const std::filesystem::path base = device.assets() / "water";
        std::vector<std::filesystem::path> levels{base / (std::string(layer) + ".png")};
        for (int step : {2, 4, 8, 16})
            levels.push_back(base / (std::string(layer) + "@" + std::to_string(step) + ".png"));
        layers.push_back(std::move(levels));
    }
    surface_ = device.loadArrayMipped(layers);
    if (!surface_) return {};
    std::vector<SDL_GPUTextureSamplerBinding> bindings{{surface_.get(), sampler_.get()}};
    if (pages_) {
        auto detail = bindings;
        const auto fields = pages_->bindings();
        detail.insert(detail.end(), fields.begin() + 3, fields.end());
        // Slot 10, sceneGrab: replaced every frame with the frame's copy.
        detail.push_back({surface_.get(), sceneSampler_.get()});
        pageFragmentBindings_ = into.take(std::move(detail));
    }

    pipeline_ = into.take(std::move(graphics));
    if (pages_) {
        auto climateBindings = pages_->bindings();
        climateBindings.resize(3);
        vertexBindings_ = into.takeVertex(std::move(climateBindings));
    } else if (climate_.ensure(device, climateField_))
        vertexBindings_ = into.takeVertex(climate_.bindings());
    bindings_ = into.take(std::move(bindings));
    return {engine::passOf(Pass::Water), engine::stageOf(Stage::Surface),
            static_cast<engine::PassOrder>(Order::Blended)};
}

bool WaterPass::anything(const engine::Frame& frame) const {
    (void)frame;
    return (pages_ && !pages_->gathered().water.empty()) ||
                       std::any_of(cache_.drawing().begin(), cache_.drawing().end(),
                       [](const engine::MeshCache::Drawn& drawn) {
                           return (drawn.mesh->tags & kMeshHasWater) != 0;
                       });
}

std::array<double, 2> WaterPass::sheetCentre(const engine::Frame& frame) {
    const float* m = frame.scene.viewProjection;
    const float* eye = frame.scene.camera;
    double x = eye[0], y = eye[1];
    // The w row of a perspective projection is the view axis: follow it down
    // to sea level (at most a few hundred metres out, where the detail that
    // needs the dense grid can still be seen).
    const double fx = m[12], fy = m[13], fz = m[14];
    const double length = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (length > 0) {
        const double dx = fx / length, dy = fy / length, dz = fz / length;
        double t = dz < -0.02 ? double(eye[2]) / -dz : 300.0;
        t = std::clamp(t, 0.0, 300.0);
        x += dx * t;
        y += dy * t;
    }
    constexpr double kSnap = 0.5;
    return {std::floor(x / kSnap) * kSnap, std::floor(y / kSnap) * kSnap};
}

void WaterPass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    (void)frame;
    if (pages_) {
        auto current = pages_->bindings();
        if (current.empty()) return;
        renderer_->replaceVertex(pageBindings_, current);
        // The world as it stood before the water: without it the water
        // cannot see its bed or its shores, and is not drawn at all.
        if (!frame.grab) return;
        std::vector<SDL_GPUTextureSamplerBinding> detail{{surface_.get(), sampler_.get()}};
        detail.insert(detail.end(), current.begin() + 3, current.end());
        detail.push_back({frame.grab, sceneSampler_.get()});
        renderer_->replace(pageFragmentBindings_, std::move(detail));
        current.resize(3);
        renderer_->replaceVertex(vertexBindings_, std::move(current));
    }
    if (pages_) {
        // The squares that carry water, listed by the terrain gather rather
        // than found by re-testing every square in the cut.
        const auto& terrain = pages_->gathered();
        if (std::getenv("ASR_WATER_DEBUG") && frame.index % 120 == 0) {
            std::size_t under = 0, levels[16]{};
            float lowest = 1e9f, highest = -1e9f;
            for (const auto index : terrain.water) {
                const auto& patch = terrain.patches[index];
                under += patch.highZ < -0.5f;
                lowest = std::min(lowest, patch.highZ); highest = std::max(highest, patch.highZ);
                ++levels[std::min<std::uint32_t>(patch.level, 15)];
            }
            std::fprintf(stderr, "water-debug patches=%zu water=%zu submerged=%zu highZ=[%.1f, %.1f] by-level",
                         terrain.patches.size(), terrain.water.size(), under, lowest, highest);
            for (int l = 0; l < 16; ++l) if (levels[l]) std::fprintf(stderr, " %d:%zu", l, levels[l]);
            std::fprintf(stderr, "\n");
        }
        for (const auto index : terrain.water) {
            // A square wholly under the sea has nothing left for page water
            // to draw: the sea over it is the sheet's, and page water keeps
            // only rivers, lakes and the swash above the waterline.
            const auto& drawn = pages_->drawn(terrain.patches[index].source);
            if (drawn.highBed < -0.5f && !drawn.inland) continue;
            // No triangle with water over it: an empty draw is still a draw.
            if (drawn.waterIndices == 0) continue;
            engine::DrawItem item;
        item.author = 3;
            item.pipeline = pagePipeline_;
            item.bindings = pageFragmentBindings_;
            item.vertexBindings = pageBindings_;
            item.vertex[0] = drawn.vertices;
            item.vertexStreams = 1;
            item.index = drawn.indices;
            item.indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
            item.indexCount = drawn.waterIndices; // surface only
            item.hasOwnData = item.ownToVertex = true;
            std::copy(drawn.parameters.begin(), drawn.parameters.end(), item.own);
            queue.push(item);
        }
    }
    for (const engine::MeshCache::Drawn& drawn : cache_.drawing()) {
        const engine::Mesh* mesh = drawn.mesh;
        // Over stand-in ground as well as real. A coarse patch's idea of where
        // the water is is a guess, but a river that vanishes for ten frames
        // every time the level changes is a worse one.
        const bool sharedGrid = (mesh->tags & kMeshUsesSharedGrid) != 0;
        if ((mesh->tags & kMeshHasWater) == 0 || !mesh->vertices ||
            (!sharedGrid && !mesh->indices)) continue;
        engine::DrawItem item;
        item.author = 3;
        if (pages_) {
            // The one sea (water_pages.hlsl: WaterSheetPS). Its vertex stage
            // reads nothing of its own; its pixels read the page atlas.
            item.pipeline = sheetPipeline_;
            item.bindings = pageFragmentBindings_;
            item.vertex[0] = mesh->vertices.get();
            item.vertexStreams = 1;
            item.index = sharedGrid ? gridIndices_.get() : mesh->indices.get();
            item.indexSize = sharedGrid ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                        : SDL_GPU_INDEXELEMENTSIZE_32BIT;
            item.indexCount = sharedGrid ? gridIndexCount_ : mesh->indexCount;
            item.vertexBindings = pageBindings_;
            item.hasOwnData = true;
            item.ownToVertex = true;
            const auto sheet = pages_->sheetParameters();
            std::copy(sheet.begin(), sheet.end(), item.own);
            // Where the grid is dense: the point the view looks at on the sea,
            // snapped to the finest vertex spacing so the waves do not swim
            // under a grid that slides by fractions of a vertex.
            const auto centre = sheetCentre(frame);
            item.own[4] = float(centre[0]);
            item.own[5] = float(centre[1]);
            queue.push(item);
            continue;
        }
        item.pipeline = pipeline_;
        item.bindings = bindings_;
        item.vertexBindings = vertexBindings_;
        item.vertex[0] = mesh->vertices.get();
        item.vertexStreams = 1;
        item.index = sharedGrid ? gridIndices_.get() : mesh->indices.get();
        item.indexSize = sharedGrid ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                    : SDL_GPU_INDEXELEMENTSIZE_32BIT;
        item.indexCount = sharedGrid ? gridIndexCount_ : mesh->indexCount;
        // The bed this water lies on morphs, so the water morphs with it.
        item.ownToVertex = true;
        item.hasOwnData = true;
        for (std::size_t i = 0; i < drawn.data.size(); ++i) item.own[i] = drawn.data[i];
        item.own[0] = meshMorph(drawn.now);
        queue.push(item);
    }
}

} // namespace game
