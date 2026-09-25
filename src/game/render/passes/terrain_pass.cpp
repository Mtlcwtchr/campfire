#include "game/render/passes/terrain_pass.hpp"

#include <cstddef>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/terrain_data.hpp"
#include "game/world/terrain_grid.hpp"
#include "game/render/gpu_terrain.hpp"
#include "game/render/world_materials.hpp"

namespace game {

const std::vector<SDL_GPUVertexBufferDescription>& terrainBuffers() {
    static const std::vector<SDL_GPUVertexBufferDescription> buffers{
            {0, sizeof(TerrainVertexGpu), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    return buffers;
}

const std::vector<SDL_GPUVertexAttribute>& terrainAttributes() {
    static const std::vector<SDL_GPUVertexAttribute> attributes{
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertexGpu, position)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertexGpu, normal)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(TerrainVertexGpu, weights0)},
            {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TerrainVertexGpu, weights1)},
            {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TerrainVertexGpu, uv)},
            {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(TerrainVertexGpu, waterHeight)},
            {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(TerrainVertexGpu, waterMotion)},
            {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(TerrainVertexGpu, waterCover)},
            {8, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(TerrainVertexGpu, morphHeight)},
            {9, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertexGpu, morphNormal)},
            {10, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TerrainVertexGpu, morphUv)},
            {11, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(TerrainVertexGpu, relief)},
    };
    return attributes;
}

const std::vector<SDL_GPUVertexBufferDescription>& pageGridBuffers() {
    static const std::vector<SDL_GPUVertexBufferDescription> buffers{
        {0, sizeof(world::terrain::AdaptiveVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    return buffers;
}
const std::vector<SDL_GPUVertexAttribute>& pageGridAttributes() {
    static const std::vector<SDL_GPUVertexAttribute> attributes{
        {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, 0},
        {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(world::terrain::AdaptiveVertex,parentBed)},
        {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(world::terrain::AdaptiveVertex,edgeBed)},
        {3, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(world::terrain::AdaptiveVertex,sourceBed)},
        {4, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(world::terrain::AdaptiveVertex,priorBed)},
        {5, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(world::terrain::AdaptiveVertex,diagnostic)},
        {6, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(world::terrain::AdaptiveVertex,drainage)},
        {7, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(world::terrain::AdaptiveVertex,displayFrom)}};
    return attributes;
}

TerrainPass::TerrainPass(const engine::MeshCache& cache, std::vector<std::string> materials,
                         std::vector<std::string> materialMaps,
                         ClimateTextures& climate, const world::ClimateField& field, GpuTerrain* pages)
    : pages_(pages), cache_(cache), materialNames_(std::move(materials)),
      materialMaps_(std::move(materialMaps)), climate_(climate),
      climateField_(field) {}

engine::PassPlace TerrainPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    if (pages_ && !pages_->ensure(device)) return {};
    if (!pages_) {
    const world::terrain::GridTopology grid = world::terrain::makeGridTopology();
    gridIndices_ = device.uploadBuffer(SDL_GPU_BUFFERUSAGE_INDEX, grid.indices.data(),
                                       grid.indices.size() * sizeof(std::uint16_t));
    gridIndexCount_ = static_cast<std::uint32_t>(grid.indices.size());
    if (!gridIndices_) return {};
    }

    engine::VertexLayout wanted;
    wanted.buffers = pages_ ? pageGridBuffers() : terrainBuffers();
    wanted.attributes = pages_ ? pageGridAttributes() : terrainAttributes();
    const bool cull = pages_ != nullptr && pages_->config().cullGround;
    terrainRenderer_.author = 1;
    backdropRenderer_.author = 2;
    if (!terrainRenderer_.material.setup(device, into, materials::terrain(bool(pages_), false, cull),
                                         wanted))
        return {};

    // The same shader for ground that is only standing in, writing depth like
    // anything else but pushed away from the eye. It used to leave the depth
    // buffer alone entirely, which meant nothing could be sorted against it -
    // so water and grass had to be kept off it, and a change of level, where
    // every patch on screen is a stand-in for a moment, drew the world with no
    // water and no grass at all. That is what "the water starts over from
    // scratch" was.
    if (!pages_) {
        if (!backdropRenderer_.material.setup(device, into, materials::terrain(false, true), wanted)) return {};
    }

    // Repeating, anisotropic, and with the whole mip chain: the material tiles
    // in world space, so a hillside seen at a glancing angle is exactly the case
    // anisotropy exists for.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.enable_anisotropy = true;
    sampler.max_anisotropy = 8.0f;
    sampler.max_lod = 16.0f;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};

    // The old five-level chain stopped at 128x128 for a 2048px scan: distant
    // ground still sampled hundreds of unresolved texels. Generate down to 1x1
    // for all channels, using the renderer's display-referred UNORM convention.
    // The shader renormalizes filtered normals, not individual mip texels.
    //
    // One array of layers rather than six textures. Six textures cannot be
    // indexed by a shader, so a pixel had to sample all six and throw four
    // away; as layers of one array the layer is a number, and a pixel samples
    // the two materials it is actually made of. One binding instead of six.
    auto mipLayers = [&](const std::string& suffix) {
        std::vector<std::vector<std::filesystem::path>> result;
        result.reserve(materialMaps_.size());
        for (const std::string& material : materialMaps_) {
            std::vector<std::filesystem::path> levels{device.assets() / (material + suffix + ".png")};
            result.push_back(std::move(levels));
        }
        return result;
    };
    auto layers = mipLayers("_albedo");
    if (materialMaps_.size() != materialNames_.size()) return {};
    for (std::size_t i = 0; i < materialMaps_.size(); ++i) {
        if (!std::filesystem::exists(layers[i].front())) return {};
    }
    materials_ = device.loadArrayMipped(layers, true);
    if (!materials_) return {};

    auto normalLayers = mipLayers("_normal");
    auto propertyLayers = mipLayers("_properties");
    for (std::size_t i = 0; i < materialMaps_.size(); ++i) {
        if (!std::filesystem::exists(normalLayers[i].front()) ||
            !std::filesystem::exists(propertyLayers[i].front())) return {};
    }
    materialNormals_ = device.loadArrayMipped(normalLayers, true);
    materialProperties_ = device.loadArrayMipped(propertyLayers, true);
    if (!materialNormals_ || !materialProperties_) return {};

    std::vector<SDL_GPUTextureSamplerBinding> bindings{{materials_.get(), sampler_.get()}};
    if (pages_) {
        const auto fields = pages_->bindings();
        bindings.insert(bindings.end(), fields.begin() + 3, fields.end());
    }
    bindings.push_back({materialNormals_.get(), sampler_.get()});
    bindings.push_back({materialProperties_.get(), sampler_.get()});

    // The weather of the whole world, uploaded once and shared with the water.
    if (!pages_ && !climate_.ensure(device, climateField_)) return {};
    const auto climateBindings = pages_ ? pages_->bindings() : climate_.bindings();
    if (climateBindings.size() < 3) return {};
    bindings.insert(bindings.end(),climateBindings.begin(),climateBindings.begin()+3);

    if (!pages_) backdropRenderer_.material.textures(bindings, climateBindings);
    terrainRenderer_.material.textures(std::move(bindings), climateBindings);
    return {engine::passOf(Pass::Terrain), engine::stageOf(Stage::World),
            static_cast<engine::PassOrder>(Order::Opaque)};
}

bool TerrainPass::anything(const engine::Frame& frame) const {
    (void)frame;
    return pages_ ? !pages_->drawing().empty() : !cache_.drawing().empty();
}

void TerrainPass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    (void)frame;
    if (pages_) {
        const auto fields = pages_->bindings();
        if (fields.empty()) return;
        std::vector<SDL_GPUTextureSamplerBinding> bindings{{materials_.get(), sampler_.get()}};
        bindings.insert(bindings.end(), fields.begin() + 3, fields.end());
        bindings.push_back({materialNormals_.get(), sampler_.get()});
        bindings.push_back({materialProperties_.get(), sampler_.get()});
        bindings.insert(bindings.end(),fields.begin(),fields.begin()+3);
        terrainRenderer_.material.textures(std::move(bindings), fields);
        // The cut, already walked once by the terrain gather: ordered, and with
        // each square's buffers and parameters resolved. This pass used to
        // re-derive all of it, and so did the water pass beside it.
        for (const auto& patch : pages_->gathered().patches) {
            const auto& drawn = pages_->drawn(patch.source);
            if (frame.work && patch.level < frame.work->groundByLevel.size()) {
                ++frame.work->groundByLevel[patch.level];
                if (drawn.mesh)
                    frame.work->groundTrianglesByLevel[patch.level] +=
                            drawn.mesh->surfaceIndices / 3;
            }
            terrainRenderer_.submitSurface(queue,
                {drawn.vertices, drawn.indices, SDL_GPU_INDEXELEMENTSIZE_32BIT, {}},
                *drawn.mesh, pages_->skirts(), {drawn.parameters, true, true});
        }
        return;
    }
    for (const engine::MeshCache::Drawn& drawn : cache_.drawing()) {
        const engine::Mesh* mesh = drawn.mesh;
        const bool sharedGrid = (mesh->tags & kMeshUsesSharedGrid) != 0;
        if (!mesh->vertices || (!sharedGrid && !mesh->indices)) continue;
        engine::DrawItem item;
        item.author = 2;
        // Real ground, or something standing in for it until the real thing is
        // cut. The queue is already ordered coarse-first, so the backdrops are
        // painted in the right order among themselves.
        ((drawn.now & kMeshIsGround) != 0 ? terrainRenderer_.material : backdropRenderer_.material).apply(item);
        item.vertex[0] = mesh->vertices.get();
        item.vertexStreams = 1;
        item.index = sharedGrid ? gridIndices_.get() : mesh->indices.get();
        item.indexSize = sharedGrid ? SDL_GPU_INDEXELEMENTSIZE_16BIT
                                    : SDL_GPU_INDEXELEMENTSIZE_32BIT;
        item.indexCount = sharedGrid ? gridIndexCount_ : mesh->indexCount;
        // How far along the way to the next level this patch stands, for the
        // vertex stage alone.
        item.ownToVertex = true;
        item.hasOwnData = true;
        for (std::size_t i = 0; i < drawn.data.size(); ++i) item.own[i] = drawn.data[i];
        item.own[0] = meshMorph(drawn.now);
        queue.push(item);
    }
}

} // namespace game
