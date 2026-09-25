#include "game/render/passes/foliage_pass.hpp"

#include <array>
#include <cstddef>
#include <cmath>
#include <cstdlib>
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

FoliagePass::FoliagePass(const engine::MeshCache& cache, std::vector<std::string> cards, GpuTerrain* pages)
    : pages_(pages), enabled_(std::getenv("ASR_GRASS_DISABLED")==nullptr),
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

    std::vector<std::filesystem::path> layers;
    layers.reserve(cardNames_.size());
    for (const std::string& card : cardNames_) layers.push_back(device.assets() / card);
    cards_ = device.loadArray(layers);
    if (!cards_) return {};

    // The quad a blade is drawn on: standing on the ground, a unit wide and a
    // unit tall, with its origin at the foot so scaling makes it taller rather
    // than moving it.
    const std::array<FoliageVertexGpu, 4> quad{{
            {{-1, 0}, {0, 1}}, {{1, 0}, {1, 1}}, {{-1, 1}, {0, 0}}, {{1, 1}, {1, 0}},
    }};
    const std::array<std::uint16_t, 6> indices{{0, 2, 3, 0, 3, 1}};
    engine::Device::Uploader uploader(device);
    quad_ = uploader.add(SDL_GPU_BUFFERUSAGE_VERTEX, quad.data(), sizeof(quad));
    quadIndices_ = uploader.add(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), sizeof(indices));
    uploader.finish();
    if (!quad_ || !quadIndices_) return {};
    if (gpuCulling_ && !culler_.setup(device,into)) return {};

    pipeline_ = into.take(std::move(graphics));
    bindings_ = into.take({{cards_.get(), sampler_.get()}});
    if (pages_) vertexBindings_=into.takeVertex(pages_->bindings());
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
        renderer_->replaceVertex(vertexBindings_,pages_->bindings());
        const double left=double(wx)*world::kGrassRegion-world::kGrassWindow;
        const double bottom=double(wy)*world::kGrassRegion-world::kGrassWindow;
        const auto cellFor=[](const auto& b,int base){return std::max(base,2<<std::clamp(b.tile.lod,0,6));};
        const auto cost=[&](int step){double n=0;for (const auto& b:pages_->drawing()) n+=std::pow(std::ceil(double(b.metres())/cellFor(b,step)),2);return n;};
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
        item.indexCount=6;item.instances=std::uint32_t(candidates_);
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
