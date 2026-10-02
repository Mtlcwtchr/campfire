#include "engine/render/dag_viewport.hpp"
#include <algorithm>
#include <cstring>
#include <limits>

namespace engine {
bool DagViewport::open(SDL_Window* window, const std::filesystem::path& assets) {
    if (device_.handle()) { device_.fail("Viewport is already open"); return false; }
    if (!device_.open(window, assets)) return false;
    PipelineWanted wanted;
    wanted.shaderFile = "dag_viewport.hlsl";
    wanted.vertexEntry = "meshVs"; wanted.fragmentEntry = "meshPs";
    wanted.depthClip = true;
    wanted.buffers = {{0, 3 * sizeof(float), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    wanted.attributes = {{0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, 0}};
    solid_ = device_.makePipeline(wanted);
    wanted.wireframe = true;
    wanted.fragmentEntry = "wirePs";
    wire_ = device_.makePipeline(wanted);
    wanted = {};
    wanted.shaderFile = "dag_viewport.hlsl";
    wanted.vertexEntry = "overlayVs"; wanted.fragmentEntry = "overlayPs";
    wanted.depthTest = wanted.depthWrite = false;
    wanted.blend = true;
    composite_ = device_.makePipeline(wanted);
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler_ = device_.makeSampler(sampling);
    return solid_ && wire_ && composite_ && sampler_;
}

bool DagViewport::resize(Uint32 width, Uint32 height) {
    if (!device_.handle() || !width || !height || width > 16384 || height > 16384) {
        device_.fail("Invalid viewport dimensions"); return false;
    }
    if (targets_.width() == width && targets_.height() == height) return true;
    if (!targets_.resize(device_, width, height)) return false;
    overlay_.reset(); overlayPixels_.clear();
    return true;
}

bool DagViewport::mesh(const geometry::ClusterDag& dag, std::uint64_t revision) {
    if (!device_.handle()) { device_.fail("Viewport is not open"); return false; }
    if (hasMesh_ && revision == revision_) return true;
    const auto vertexBytes = dag.positions.size() * sizeof(float);
    const auto indexBytes = dag.indices.size() * sizeof(std::uint32_t);
    const auto maxBytes = std::numeric_limits<Uint32>::max();
    if (dag.positions.size() % 3 || vertexBytes > maxBytes || indexBytes > maxBytes ||
        std::any_of(dag.positions.begin(), dag.positions.end(), [](float p) { return !std::isfinite(p); }) ||
        std::any_of(dag.indices.begin(), dag.indices.end(), [&](auto i) { return i >= dag.positions.size()/3; })) {
        device_.fail("Invalid DAG vertex/index data"); return false;
    }
    std::vector<IndexRange> ranges;
    for (const auto& cluster : dag.clusters) {
        const auto range = cluster.indices;
        if (range.count % 3 || std::uint64_t(range.first) + range.count > dag.indices.size()) {
            device_.fail("Invalid DAG cluster range"); return false;
        }
        ranges.push_back(range);
    }
    // Publish only a complete replacement. The old preview survives failed uploads.
    Buffer vertices, indices;
    if (vertexBytes && indexBytes) {
        Device::Uploader upload(device_);
        vertices = upload.add(SDL_GPU_BUFFERUSAGE_VERTEX, dag.positions.data(), vertexBytes);
        indices = upload.add(SDL_GPU_BUFFERUSAGE_INDEX, dag.indices.data(), indexBytes);
        if (!vertices || !indices || !upload.finish()) return false;
    }
    vertices_ = std::move(vertices); indices_ = std::move(indices);
    ranges_ = std::move(ranges); revision_ = revision; hasMesh_ = true;
    residentBytes_ = vertexBytes + indexBytes; ++meshUploads_;
    return true;
}

bool DagViewport::uploadOverlay(const SDL_Surface& surface) {
    if (surface.format != SDL_PIXELFORMAT_RGBA32 || surface.w != int(targets_.width()) ||
        surface.h != int(targets_.height()) || !surface.pixels || surface.pitch < surface.w * 4) {
        device_.fail("Overlay must be a viewport-sized RGBA32 surface"); return false;
    }
    const std::size_t rowBytes = std::size_t(surface.w) * 4;
    const std::size_t bytes = rowBytes * surface.h;
    const auto* pixels = static_cast<const std::uint8_t*>(surface.pixels);
    bool changed = overlayPixels_.size() != bytes;
    for (int y = 0; !changed && y < surface.h; ++y)
        changed = std::memcmp(overlayPixels_.data() + y * rowBytes, pixels + y * surface.pitch, rowBytes) != 0;
    if (!changed && overlay_) return true;
    if (!overlay_) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D; info.format = Device::kColourFormat;
        info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        info.width = surface.w; info.height = surface.h;
        info.layer_count_or_depth = 1; info.num_levels = 1;
        info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        overlay_ = device_.makeTexture(info);
        if (!overlay_) return false;
    }
    overlayPixels_.resize(bytes);
    for (int y = 0; y < surface.h; ++y)
        std::memcpy(overlayPixels_.data() + y * rowBytes, pixels + y * surface.pitch, rowBytes);
    Device::Uploader upload(device_);
    if (!upload.refillRegion(overlay_.get(), overlayPixels_.data(), 0, 0, surface.w, surface.h, 4, 0, 0, true) ||
        !upload.finish()) { overlayPixels_.clear(); return false; }
    ++overlayUploads_;
    return true;
}

bool DagViewport::draw(const camera::Camera& camera, SDL_Rect area,
                       std::span<const std::uint32_t> cut, bool wireframe, const SDL_Surface* overlay) {
    if (!solid_ || !targets_.colour() || !camera.viewState().valid() ||
        !std::isfinite(camera.farPlane) || camera.farPlane <= camera.nearPlane ||
        area.x < 0 || area.y < 0 || area.w <= 0 || area.h <= 0 ||
        std::int64_t(area.x) + area.w > targets_.width() ||
        std::int64_t(area.y) + area.h > targets_.height() ||
        std::any_of(cut.begin(), cut.end(), [&](auto id) { return id >= ranges_.size(); }) ||
        (!cut.empty() && (!vertices_ || !indices_))) {
        device_.fail("Invalid viewport frame or cluster cut"); return false;
    }
    device_.waitInFlight(1);
    if (overlay && !uploadOverlay(*overlay)) return false;
    auto* commands = SDL_AcquireGPUCommandBuffer(device_.handle());
    if (!commands) { device_.fail(SDL_GetError()); return false; }
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 windowWidth = 0, windowHeight = 0;
    if (device_.window() && !SDL_WaitAndAcquireGPUSwapchainTexture(commands, device_.window(),
            &swapchain, &windowWidth, &windowHeight)) {
        device_.fail(SDL_GetError()); SDL_CancelGPUCommandBuffer(commands); return false;
    }
    SDL_GPUColorTargetInfo colour{};
    colour.texture = targets_.colour();
    colour.load_op = SDL_GPU_LOADOP_CLEAR; colour.store_op = SDL_GPU_STOREOP_STORE;
    colour.clear_color = {22/255.0f, 30/255.0f, 40/255.0f, 1};
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = targets_.depth(); depth.clear_depth = 0;
    depth.load_op = SDL_GPU_LOADOP_CLEAR; depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE; depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    auto* pass = SDL_BeginGPURenderPass(commands, &colour, 1, &depth);
    if (!pass) {
        device_.fail(SDL_GetError()); SDL_CancelGPUCommandBuffer(commands); return false;
    }
    SDL_GPUViewport viewport{float(area.x), float(area.y), float(area.w), float(area.h), 0, 1};
    SDL_SetGPUViewport(pass, &viewport); SDL_SetGPUScissor(pass, &area);
    if (!cut.empty()) {
        float matrix[16];
        camera.viewProjection(matrix, 0, 1.0 / (4.0 * camera.farPlane));
        SDL_PushGPUVertexUniformData(commands, 0, matrix, sizeof(matrix));
        SDL_BindGPUGraphicsPipeline(pass, wireframe ? wire_.get() : solid_.get());
        SDL_GPUBufferBinding vertex{vertices_.get(), 0}, index{indices_.get(), 0};
        SDL_BindGPUVertexBuffers(pass, 0, &vertex, 1);
        SDL_BindGPUIndexBuffer(pass, &index, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        for (const auto id : cut) {
            const auto range = ranges_[id];
            SDL_DrawGPUIndexedPrimitives(pass, range.count, 1, range.first, 0, 0);
        }
    }
    if (overlay) {
        viewport = {0, 0, float(targets_.width()), float(targets_.height()), 0, 1};
        const SDL_Rect full{0, 0, int(targets_.width()), int(targets_.height())};
        SDL_SetGPUViewport(pass, &viewport); SDL_SetGPUScissor(pass, &full);
        SDL_BindGPUGraphicsPipeline(pass, composite_.get());
        const SDL_GPUTextureSamplerBinding binding{overlay_.get(), sampler_.get()};
        SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }
    SDL_EndGPURenderPass(pass);
    if (swapchain) {
        SDL_GPUBlitInfo blit{};
        blit.source.texture = targets_.resolved();
        blit.source.w = targets_.width(); blit.source.h = targets_.height();
        blit.destination.texture = swapchain;
        blit.destination.w = windowWidth; blit.destination.h = windowHeight;
        blit.load_op = SDL_GPU_LOADOP_DONT_CARE; blit.filter = SDL_GPU_FILTER_LINEAR;
        SDL_BlitGPUTexture(commands, &blit);
    }
    return device_.submitFrame(commands);
}

bool DagViewport::capture(const std::string& path) { return saveTargetAsPng(device_, targets_, path); }
bool DagViewport::readPixels(std::span<std::uint8_t> pixels) {
    if (!targets_.colour() || pixels.size() != std::size_t(targets_.width()) * targets_.height() * 4) return false;
    return device_.readTexture(targets_.resolved(), pixels.data(), targets_.width(), targets_.height());
}
} // namespace engine
