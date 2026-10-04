#include "game/render/passes/foliage_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <iostream>
#if ASR_ENABLE_DIAGNOSTICS
#include <nlohmann/json.hpp>
#endif
#include "game/render/gpu_terrain.hpp"

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/instances.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"
#include "game/render/passes/terrain_pass.hpp"
#include "game/render/terrain_data.hpp"

namespace game {
namespace {
// The page fields and, after them, the terrain categories' plane and table
// (t12, t13 of PageGrassVS, foliage_pages.hlsl).
std::vector<SDL_GPUTextureSamplerBinding> withBiomes(const GpuTerrain& pages) {
    auto out = pages.bindings();
    if (out.empty()) return out;
    const auto biomes = pages.biomeBindings();
    out.insert(out.end(), biomes.begin(), biomes.end());
    return out;
}
} // namespace

FoliagePass::FoliagePass(const engine::MeshCache& cache, std::vector<std::string> cards,
                       SDL_GPUTextureSamplerBinding shadow, GpuTerrain* pages)
    : shadow_(shadow), pages_(pages), enabled_(std::getenv("ASR_GRASS_DISABLED")==nullptr),
      gpuCulling_(pages && std::getenv("ASR_GRASS_CULL_DISABLED")==nullptr),
      cache_(cache), cardNames_(std::move(cards)) {}

#if ASR_ENABLE_DIAGNOSTICS
std::string FoliagePass::report() const {
    std::size_t cached=0;
    for (const auto& [key,entry]:roots_) cached+=entry.values.size();
    return nlohmann::json{{"adaptive",bool(pages_)},{"enabled",enabled_},{"submitted_candidates",candidates_},
        {"candidate_budget",world::kGrassCandidateBudget*2},{"candidate_spacing_m",world::kGrassCell},
        {"near_candidates",nearCandidates_},{"far_candidates",farCandidates_},{"far_blocks",farBlocks_},
        {"far_candidate_step_m",farStep_},{"view_wide_ground_cover",true},
        {"draws",draws_},{"submitted_triangles",candidates_*2},{"cached_roots",cached},
        {"root_bytes",cached*sizeof(world::PageGrassRoot)},
        {"instance_upload_bytes",uploadBytes_},{"instance_stride_bytes",sizeof(world::PageGrassRoot)},
        {"gpu_frustum_culling",gpuCulling_},{"gpu_indirect_draw",gpuCulling_ && draws_>0},
        {"gpu_cull_working_bytes",culler_.workingBytes()},
        {"submitted_counts_are_pre_cull",gpuCulling_},
        {"visible_radius_m",192},{"focus_m",{x_,y_}}}.dump();
}

bool FoliagePass::readDrawnCandidates(engine::Device& device, std::uint32_t& count) const {
    count=0;
    if (!draws_) return true;
    if (!gpuCulling_) { count=std::uint32_t(candidates_);return true; }
    return culler_.readCount(device,count) && count<=candidates_;
}
#endif

engine::PassPlace FoliagePass::setup(engine::Device& device, engine::RenderPipeline& into) {
    renderer_=&into;
    if (pages_ && !pages_->ensure(device)) return {};
    engine::PipelineWanted wanted;
    wanted.shaderFile = pages_ ? "foliage_pages.hlsl" : "foliage.hlsl";
    wanted.vertexEntry = pages_ ? "PageGrassVS" : "FoliageVS";
    wanted.fragmentEntry = "FoliagePS";
    // Two streams: the quad, once, and one instance per blade.
    wanted.buffers = {
            {0, sizeof(FoliageVertexGpu), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
            {1, sizeof(FoliageInstanceGpu), SDL_GPU_VERTEXINPUTRATE_INSTANCE, 0},
    };
    wanted.attributes = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(FoliageVertexGpu, corner)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(FoliageVertexGpu, uv)},
            {2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(FoliageInstanceGpu, position)},
            {3, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(FoliageInstanceGpu, scale)},
            {4, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(FoliageInstanceGpu, tint)},
            {5, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(FoliageInstanceGpu, phase)},
            {6, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(FoliageInstanceGpu, variant)},
            {7, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(FoliageInstanceGpu, climate)},
    };
    if (pages_) {
        wanted.buffers[1].pitch=sizeof(world::PageGrassRoot);
        wanted.attributes.resize(2);
        wanted.attributes.push_back({2,1,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3,offsetof(world::PageGrassRoot,position)});
        wanted.attributes.push_back({3,1,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4,offsetof(world::PageGrassRoot,parentHeight)});
        wanted.attributes.push_back({4,1,SDL_GPU_VERTEXELEMENTFORMAT_FLOAT,offsetof(world::PageGrassRoot,run)});
    }
    // The shader dithers coverage. Unsorted alpha blending with depth writes
    // would let a barely visible distant clump hide the ones behind it.
    wanted.blend = false;
    wanted.depthTest = true;
    wanted.depthWrite = true;
    engine::GraphicsPipeline graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    engine::GraphicsPipeline pebbles, blades;
    if (pages_) {
        wanted.vertexEntry = "PebbleVS";
        wanted.fragmentEntry = "PebblePS";
        pebbles = device.makePipeline(wanted);
        if (!pebbles) std::cerr << "Pebbles unavailable: their pipeline did not build\n";
        // Blades are seen from both sides: no culling, as for the cards.
        wanted.vertexEntry = "BladeVS";
        wanted.fragmentEntry = "BladePS";
        blades = device.makePipeline(wanted);
        if (!blades) std::cerr << "Grass blades unavailable: their pipeline did not build\n";
    }

    // Clamped, not repeating: a card is a picture with edges, and a blade that
    // wrapped would grow out of the other side of its own quad.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.enable_anisotropy = true;
    sampler.max_anisotropy = 8.0f;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};

    // With a mip chain: a card read at one level from two hundred metres is a
    // sparkle of single texels that swims with every step, and costs the
    // texture cache a full-size read per pixel. The shader keeps the coverage
    // of the smaller levels up (FoliagePS), so distant clumps do not thin out.
    std::vector<std::vector<std::filesystem::path>> layers;
    layers.reserve(cardNames_.size());
    for (const std::string& card : cardNames_) layers.push_back({device.assets() / card});
    cards_ = device.loadArrayMipped(layers, true);
    if (!cards_) return {};

    // The quad a blade is drawn on: standing on the ground, a unit wide and a
    // unit tall, with its origin at the foot so scaling makes it taller rather
    // than moving it.
    //
    // Four of them, one after another: a page root near the eye stands for a
    // patch of turf and draws all four (PageGrassVS reads which one it is from
    // the corner, offset by four a card); everything else draws the first six
    // indices, the one card. A lawn out of the same roots, not four times the
    // roots to build, cache and upload.
    constexpr int kTurfCards = world::kTurfCards;
    std::array<FoliageVertexGpu, 4 * kTurfCards> quad{};
    std::array<std::uint16_t, 6 * kTurfCards> indices{};
    for (int k = 0; k < kTurfCards; ++k) {
        const float shift = 4.0f * float(k);
        const std::array<FoliageVertexGpu, 4> card{{
                {{-1 + shift, 0}, {0, 1}}, {{1 + shift, 0}, {1, 1}},
                {{-1 + shift, 1}, {0, 0}}, {{1 + shift, 1}, {1, 0}},
        }};
        for (int v = 0; v < 4; ++v) quad[k * 4 + v] = card[v];
        const std::array<std::uint16_t, 6> order{{0, 2, 3, 0, 3, 1}};
        for (int i = 0; i < 6; ++i) indices[k * 6 + i] = std::uint16_t(k * 4 + order[i]);
    }
    engine::Device::Uploader uploader(device);
    quad_ = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, quad.data(), sizeof(quad));
    quadIndices_ = uploader.add(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), sizeof(indices));
    // The pebble: a unit octahedron, x and y in the corner, z in u. Eight
    // faces, 24 indices - exactly the grass draw's count (see the header).
    static_assert(6 * world::kTurfCards == 24, "pebbles share the grass draw's index count");
    if (pebbles) {
        const std::array<FoliageVertexGpu, 6> stone{{
                {{1, 0}, {0, 0}}, {{-1, 0}, {0, 0}}, {{0, 1}, {0, 0}},
                {{0, -1}, {0, 0}}, {{0, 0}, {1, 0}}, {{0, 0}, {-1, 0}},
        }};
        const std::array<std::uint16_t, 24> faces{{
                4, 2, 0, 4, 1, 2, 4, 3, 1, 4, 0, 3,
                5, 0, 2, 5, 2, 1, 5, 1, 3, 5, 3, 0,
        }};
        pebble_ = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, stone.data(), sizeof(stone));
        pebbleIndices_ = uploader.add(SDL_GPU_BUFFERUSAGE_INDEX, faces.data(), sizeof(faces));
    }
    // The blade clump: kBlades strips of kBladeSegments segments and a tip,
    // corner = (blade, height up it), uv.x = which edge (foliage_pages.hlsl BladeVS).
    if (blades) {
        constexpr int kBlades = 32, kSegments = 4;
        std::vector<FoliageVertexGpu> strip;
        std::vector<std::uint16_t> order;
        for (int b = 0; b < kBlades; ++b) {
            const auto first = std::uint16_t(strip.size());
            for (int s = 0; s < kSegments; ++s) {
                const float up = float(s) / float(kSegments);
                strip.push_back({{float(b), up}, {-1, 0}});
                strip.push_back({{float(b), up}, {1, 0}});
            }
            strip.push_back({{float(b), 1.0f}, {0, 0}});
            for (int s = 0; s + 1 < kSegments; ++s) {
                const auto a = std::uint16_t(first + 2 * s);
                order.insert(order.end(), {a, std::uint16_t(a + 1), std::uint16_t(a + 3), a, std::uint16_t(a + 3),
                                           std::uint16_t(a + 2)});
            }
            const auto last = std::uint16_t(first + 2 * (kSegments - 1));
            order.insert(order.end(), {last, std::uint16_t(last + 1), std::uint16_t(first + 2 * kSegments)});
        }
        blades_ = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, strip.data(), strip.size() * sizeof(FoliageVertexGpu));
        bladeIndices_ = uploader.add(SDL_GPU_BUFFERUSAGE_INDEX, order.data(), order.size() * sizeof(std::uint16_t));
        bladeIndexCount_ = std::uint32_t(order.size());
    }
    uploader.finish();
    if (!quad_ || !quadIndices_) return {};
    if (gpuCulling_ && !culler_.setup(device,into)) return {};
    if (gpuCulling_ && blades && !culler_.secondDraw(device, into, bladeIndexCount_)) {
        std::cerr << "Grass blades unavailable: their indirect draw did not build\n";
        blades = {};
    }

    pipeline_ = into.take(std::move(graphics));
    if (pebbles && pebble_ && pebbleIndices_) pebblePipeline_ = into.take(std::move(pebbles));
    if (blades && blades_ && bladeIndices_) bladePipeline_ = into.take(std::move(blades));
    bindings_ = into.take({{cards_.get(), sampler_.get()},shadow_});
    if (pages_) vertexBindings_=into.takeVertex(withBiomes(*pages_));
    return {engine::passOf(Pass::Foliage), engine::stageOf(Stage::World),
            static_cast<engine::PassOrder>(Order::Opaque)};
}

bool FoliagePass::anything(const engine::Frame& frame) const {
    return enabled_ && frame.scene.extra[2] > 0.01f && (pages_ ? !pages_->drawing().empty() : !cache_.drawing().empty());
}

void FoliagePass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    candidates_=nearCandidates_=farCandidates_=0;
    ASR_DIAGNOSTIC(draws_=uploadBytes_=runs_=farBlocks_=0);
    if (pages_) {
        if (!frame.instances || !pages_->finalStage()) return;
        const int wx=int(std::floor(x_/world::kGrassRegion)),wy=int(std::floor(y_/world::kGrassRegion));
        if (wx!=windowX_ || wy!=windowY_) {
            std::erase_if(roots_,[](const auto& item){return std::get<3>(item.first)==world::kGrassCell;});
            windowX_=wx;windowY_=wy;
        }
        for (auto& [key,entry]:roots_) entry.used=false;
        renderer_->replaceVertex(vertexBindings_,withBiomes(*pages_));
        const double left=double(wx)*world::kGrassRegion-world::kGrassWindow;
        const double bottom=double(wy)*world::kGrassRegion-world::kGrassWindow;
        const auto cellFor=[](const auto& b,int base){return std::max(base,2<<std::clamp(b.tile.lod,0,6));};
        // A block counts for the far tier when any of it is within reach.
        const auto within=[&](const auto& b){
            const double m=double(b.metres()),ox=double(b.tile.x)*m,oy=double(b.tile.y)*m;
            const double dx=std::max({ox-x_,0.0,x_-(ox+m)}),dy=std::max({oy-y_,0.0,y_-(oy+m)});
            return dx*dx+dy*dy<=reach_*reach_;};
        const auto cost=[&](int step){double n=0;for (const auto& b:pages_->drawing()) if (within(b)) n+=std::pow(std::ceil(double(b.metres())/cellFor(b,step)),2);return n;};
        while (cost(farStep_)>world::kGrassCandidateBudget && farStep_<16384) farStep_*=2;
        while (farStep_>8 && cost(farStep_/2)<world::kGrassCandidateBudget/2) farStep_/=2;
        const double t=std::clamp((pages_->vegetationPixelsPerMetre(frame,x_,y_)-1.0)/2.0,0.0,1.0);
        const float nearWeight=float(t*t*(3-2*t));
        // One draw for the whole field, not one per block and tier. What forced
        // three hundred and forty-one of them was the draw's own uniform: each
        // block pushed its morph, its page level, its parent's and its
        // candidate spacing, and a uniform belongs to a draw. Those four
        // numbers are a table now, indexed per instance, so every block's
        // candidates go into one arena range and leave as one command.
        std::vector<Roots*> order;
        std::vector<std::size_t> counts;
        for (int tier=0;tier<2;++tier) for (const auto& block:pages_->drawing()) {
            if (!block.mesh) continue;
            const bool coarse=tier==1;
            if (!coarse && nearWeight<=0) continue;
            if (coarse && !within(block)) continue;
            const int cell=coarse?cellFor(block,farStep_):world::kGrassCell;
            const auto ox=block.tile.x*block.metres(),oy=block.tile.y*block.metres();
            if (!coarse && (ox>=left+2*world::kGrassWindow || oy>=bottom+2*world::kGrassWindow ||
                ox+block.metres()<=left || oy+block.metres()<=bottom)) continue;
            const auto key=std::tuple{block.mesh.get(),ox,oy,cell};
            auto [entry,fresh]=roots_.try_emplace(key);
            if (fresh) {
                entry->second.mesh=block.mesh;
                entry->second.values=world::buildPageGrass(*block.mesh,double(ox),double(oy),wx,wy,
                    cell,coarse?0:world::kGrassWindow);
            }
            entry->second.used=true;
            auto& tierCount=coarse?farCandidates_:nearCandidates_;
            const auto count=std::min(entry->second.values.size(),world::kGrassCandidateBudget-tierCount);
            if (!count) continue;
            const auto parameters=pages_->parameters(block);
            // Five bits of level and parent level, four of log2 spacing, eight
            // of morph. See decodeGrassRun in foliage_pages.hlsl.
            int logCell=0;
            while ((1<<(logCell+1))<=cell && logCell<14) ++logCell;
            const int run=(std::clamp(int(parameters[2]),0,7)) |
                          (std::clamp(int(parameters[8]),0,7)<<3) | (logCell<<6) |
                          (std::clamp(int(parameters[0]*255.0f+0.5f),0,255)<<10);
            ASR_DIAGNOSTIC(++runs_);
            // Re-stamped only when the code changes, which for a settled camera
            // is never: a block that has not moved or morphed rewrites nothing.
            if (entry->second.run!=run) {
                for (auto& root:entry->second.values) root.run=float(run);
                entry->second.run=run;
            }
            order.push_back(&entry->second);
            counts.push_back(count);
            candidates_+=count;tierCount+=count;ASR_DIAGNOSTIC(farBlocks_+=coarse);
        }
        if (order.empty()) { std::erase_if(roots_,[](const auto& item){return !item.second.used;});return; }

        ASR_DIAGNOSTIC(const auto before=frame.instances->bytes());
        std::uint32_t at=0;
        for (std::size_t i=0;i<order.size();++i) {
            const auto offset=frame.instances->add(std::span(order[i]->values).first(counts[i]));
            if (i==0) at=offset;
        }
        ASR_DIAGNOSTIC(uploadBytes_+=frame.instances->bytes()-before);

        engine::DrawItem item;
        item.author = 4;
        item.pipeline=pipeline_;item.bindings=bindings_;item.vertexBindings=vertexBindings_;
        item.vertex[0]=quad_.get();item.instancesFromArena=true;
        item.vertexOffset[1]=at;
        item.vertexStreams=2;item.index=quadIndices_.get();item.indexSize=SDL_GPU_INDEXELEMENTSIZE_16BIT;
        item.indexCount=6*world::kTurfCards;item.instances=std::uint32_t(candidates_);
        if (gpuCulling_) {
            if (!culler_.dispatch(frame,*renderer_,at,candidates_)) return;
            item.instancesFromArena=false;
            item.vertex[1]=culler_.instances();item.vertexOffset[1]=0;
            item.indirect=culler_.arguments();item.indirectDraws=1;
        }
        item.hasOwnData=item.ownToVertex=true;
        // Everything a block does NOT decide for itself: the stage, the height
        // window, the page table size, the near/far weight and the focus.
        const auto parameters=pages_->parameters(pages_->drawing().front());
        std::copy(parameters.begin(),parameters.end(),item.own);
        item.own[9]=nearWeight;
        item.own[10]=float(x_);item.own[11]=float(y_);
        queue.push(item);ASR_DIAGNOSTIC(++draws_);
        if (bladePipeline_ && nearCandidates_) {
            engine::DrawItem grass=item;
            grass.pipeline=bladePipeline_;
            grass.vertex[0]=blades_.get();grass.index=bladeIndices_.get();
            grass.indexCount=bladeIndexCount_;
            if (gpuCulling_) grass.indirect=culler_.secondArguments();
            queue.push(grass);ASR_DIAGNOSTIC(++draws_);
        }
        if (pebblePipeline_ && nearCandidates_) {
            engine::DrawItem stones=item;
            stones.pipeline=pebblePipeline_;
            stones.vertex[0]=pebble_.get();stones.index=pebbleIndices_.get();
            stones.indexCount=24;
            queue.push(stones);ASR_DIAGNOSTIC(++draws_);
        }
        std::erase_if(roots_,[](const auto& item){return !item.second.used;});
        return;
    }
    for (const engine::MeshCache::Drawn& drawn : cache_.drawing()) {
        const engine::Mesh* mesh = drawn.mesh;
        if (!mesh->instances || mesh->instanceCount == 0) continue;
        engine::DrawItem item;
        item.author = 4;
        item.pipeline = pipeline_;
        item.bindings = bindings_;
        item.vertex[0] = quad_.get();
        item.vertex[1] = mesh->instances.get();
        item.vertexStreams = 2;
        item.index = quadIndices_.get();
        item.indexSize = SDL_GPU_INDEXELEMENTSIZE_16BIT;
        item.indexCount = 6;
        item.instances = mesh->instanceCount;
        item.hasOwnData = true;
        for (std::size_t i = 0; i < drawn.data.size(); ++i) item.own[i] = drawn.data[i];
        item.own[0] = 1;
        queue.push(item);
    }
}

} // namespace game
