#include "engine/render/device.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>

namespace engine {
namespace {

std::string readText(const std::filesystem::path& path) {
    std::ifstream in(path);
    return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string{};
}

// One image into one level of one layer of a texture.
bool putImage(SDL_GPUDevice* device, SDL_GPUTexture* texture, Uint32 layer, Uint32 mip,
              const std::filesystem::path& path) {
    SDL_Surface* loaded = IMG_Load(path.string().c_str());
    if (!loaded) return false;
    SDL_Surface* surface = loaded->format == SDL_PIXELFORMAT_ABGR8888
                                   ? loaded
                                   : SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_ABGR8888);
    if (surface != loaded) SDL_DestroySurface(loaded);
    if (!surface) return false;

    const std::size_t bytes = static_cast<std::size_t>(surface->w) * surface->h * 4;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &info);
    if (!transfer) { SDL_DestroySurface(surface); return false; }
    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    std::memcpy(mapped, surface->pixels, bytes);
    SDL_UnmapGPUTransferBuffer(device, transfer);

    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
    SDL_GPUTextureTransferInfo source{transfer, 0, static_cast<Uint32>(surface->w),
                                      static_cast<Uint32>(surface->h)};
    SDL_GPUTextureRegion region{texture, mip, layer, 0, 0, 0,
                                static_cast<Uint32>(surface->w),
                                static_cast<Uint32>(surface->h), 1};
    SDL_UploadToGPUTexture(copy, &source, &region, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(commands);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    SDL_DestroySurface(surface);
    return true;
}

// How big the first file is, which is how big the texture has to be.
bool sizeOf(const std::filesystem::path& path, int& width, int& height) {
    SDL_Surface* surface = IMG_Load(path.string().c_str());
    if (!surface) return false;
    width = surface->w;
    height = surface->h;
    SDL_DestroySurface(surface);
    return true;
}

} // namespace

Device::~Device() { close(); }

bool Device::openHeadless(std::uint32_t width, std::uint32_t height, const std::filesystem::path& assets) {
    if (!open(nullptr, assets)) return false;
    headlessWidth_ = std::max<std::uint32_t>(1, width);
    headlessHeight_ = std::max<std::uint32_t>(1, height);
    return true;
}

void Device::waitInFlight(std::size_t frames) {
    while (submissions_.size() > frames) {
        SDL_GPUFence* fence = submissions_.front().fence;
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        completedSubmission();
    }
}

bool Device::open(SDL_Window* window, const std::filesystem::path& assets) {
    close();
    window_ = window;
    assets_ = assets;

    if (!SDL_ShaderCross_Init()) {
        error_ = std::string("shader compiler would not start: ") + SDL_GetError();
        return false;
    }
    shaderCross_ = true;

    // Every format the compiler can produce; SDL picks the one the machine
    // wants. Naming one would tie the game to a platform.
    const SDL_GPUShaderFormat formats =
            SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    device_ = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!device_) {
        error_ = std::string("no graphics device: ") + SDL_GetError();
        close();
        return false;
    }
    if (!window) {
        SDL_Log("GPU driver=%s headless", SDL_GetGPUDeviceDriver(device_));
        return true;
    }
    if (!SDL_ClaimWindowForGPUDevice(device_, window)) {
        error_ = std::string("the window would not take the device: ") + SDL_GetError();
        close();
        return false;
    }
    // Metal does not support mailbox. Do not silently ignore a failed request
    // and then describe the default (vsync) swapchain as mailbox in diagnostics.
    const auto present = SDL_WindowSupportsGPUPresentMode(device_, window, SDL_GPU_PRESENTMODE_MAILBOX)
        ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
    if (!SDL_SetGPUSwapchainParameters(device_, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, present)) {
        error_ = std::string("could not configure swapchain: ") + SDL_GetError();
        close();
        return false;
    }
    SDL_Log("GPU driver=%s present=%s", SDL_GetGPUDeviceDriver(device_),
            present == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "vsync");
    return true;
}

void Device::close() {
    if (device_) {
        SDL_WaitForGPUIdle(device_);
        textures_.clear();
        for (const auto& submission : submissions_)
            SDL_ReleaseGPUFence(device_, submission.fence);
        submissions_.clear();
        submitted_ = completed_ = 0;
        if (window_) SDL_ReleaseWindowFromGPUDevice(device_, window_);
        SDL_DestroyGPUDevice(device_);
        device_ = nullptr;
    }
    if (shaderCross_) {
        SDL_ShaderCross_Quit();
        shaderCross_ = false;
    }
    window_ = nullptr;
}

std::uint64_t Device::completedSubmission() {
    while (!submissions_.empty() && SDL_QueryGPUFence(device_, submissions_.front().fence)) {
        completed_ = submissions_.front().serial;
        SDL_ReleaseGPUFence(device_, submissions_.front().fence);
        submissions_.pop_front();
    }
    return completed_;
}

bool Device::submitFrame(SDL_GPUCommandBuffer* commands) {
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (!fence) {
        fail(std::string("the frame would not submit: ") + SDL_GetError());
        return false;
    }
    submissions_.push_back({++submitted_, fence});
    completedSubmission();
    return true;
}

namespace {
constexpr std::uint64_t kFnvBasis = 0xcbf29ce484222325ull;
std::uint64_t fnv(std::uint64_t hash, const void* data, std::size_t bytes) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < bytes; ++i) hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}
std::uint64_t fnv(std::uint64_t hash, const std::string& text) { return fnv(hash, text.data(), text.size()); }
// A copy the caller frees with SDL_free, as it frees what the compiler gives.
void* sdlCopy(const std::vector<std::uint8_t>& bytes, std::size_t& size) {
    void* out = SDL_malloc(bytes.size());
    if (!out) return nullptr;
    std::memcpy(out, bytes.data(), bytes.size());
    size = bytes.size();
    return out;
}
} // namespace

std::uint64_t Device::shaderKey(const std::filesystem::path& file, const char* entry,
                                SDL_ShaderCross_ShaderStage stage) {
    namespace fs = std::filesystem;
    const fs::path folder = file.parent_path();
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(folder, ec))
        if (item.is_regular_file(ec)) files.push_back(item.path());
    std::sort(files.begin(), files.end());
    std::uint64_t stamp = kFnvBasis;
    for (const auto& f : files) {
        stamp = fnv(stamp, f.filename().string());
        const auto bytes = fs::file_size(f, ec);
        const auto when = fs::last_write_time(f, ec).time_since_epoch().count();
        stamp = fnv(stamp, &bytes, sizeof bytes);
        stamp = fnv(stamp, &when, sizeof when);
    }
    if (stamp != folderStamp_ || folderDigest_ == 0) {
        std::uint64_t digest = kFnvBasis;
        for (const auto& f : files) {
            digest = fnv(digest, f.filename().string());
            digest = fnv(digest, readText(f));
        }
        folderStamp_ = stamp;
        folderDigest_ = digest;
    }
    std::uint64_t key = fnv(folderDigest_, file.filename().string());
    key = fnv(key, std::string(entry));
    const int stageNumber = int(stage);
    return fnv(key, &stageNumber, sizeof stageNumber);
}

void* Device::compileSpirv(const std::filesystem::path& file, const char* entry,
                           SDL_ShaderCross_ShaderStage stage, std::size_t& size) {
    size = 0;
    const std::uint64_t key = shaderKey(file, entry, stage);
    if (const auto kept = spirv_.find(key); kept != spirv_.end()) return sdlCopy(kept->second, size);
    if (shaderCache_.empty() && !std::getenv("ASR_SHADER_NO_CACHE"))
        shaderCache_ = std::filesystem::absolute(assets_).lexically_normal().parent_path().parent_path() / ".cache" / "shaders";
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.spv", static_cast<unsigned long long>(key));
    const std::filesystem::path cached = shaderCache_.empty() ? std::filesystem::path{} : shaderCache_ / name;
    if (!cached.empty()) {
        std::ifstream in(cached, std::ios::binary);
        std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(in), {});
        // SPIR-V begins with its magic number; anything else is not ours.
        if (bytes.size() >= 4 && bytes[0] == 0x03 && bytes[1] == 0x02 && bytes[2] == 0x23 && bytes[3] == 0x07) {
            auto& kept = spirv_[key] = std::move(bytes);
            return sdlCopy(kept, size);
        }
    }
    void* spirv = compileFromSource(file, entry, stage, size);
    if (!spirv) return nullptr;
    std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(spirv), static_cast<const std::uint8_t*>(spirv) + size);
    if (!cached.empty()) {
        // Written beside and renamed, so a reader never finds half of one.
        std::error_code ec;
        std::filesystem::create_directories(shaderCache_, ec);
        const auto partial = cached.string() + ".partial";
        {
            std::ofstream out(partial, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        }
        std::filesystem::rename(partial, cached, ec);
        if (ec) std::filesystem::remove(partial, ec);
    }
    spirv_[key] = std::move(bytes);
    return spirv;
}

void* Device::compileFromSource(const std::filesystem::path& file, const char* entry,
                                SDL_ShaderCross_ShaderStage stage, std::size_t& size) {
    const std::string source = readText(file);
    size = 0;
    if (source.empty()) {
        error_ = "shader file is missing or empty: " + file.string();
        return nullptr;
    }
    // Shaders may include one another; the shared projection is one file that
    // every one of them pulls in, because a second copy of it is a second answer
    // to what is in front of what.
    const std::string includes = file.parent_path().string();
    SDL_ShaderCross_HLSL_Info hlsl{};
    hlsl.source = source.c_str();
    hlsl.entrypoint = entry;
    hlsl.include_dir = includes.c_str();
    hlsl.shader_stage = stage;
    void* spirv = SDL_ShaderCross_CompileSPIRVFromHLSL(&hlsl, &size);
    if (!spirv) error_ = std::string("shader ") + entry + " would not compile: " + SDL_GetError();
    return spirv;
}

SDL_GPUShader* Device::compile(const std::filesystem::path& file, const char* entry,
                               bool fragment) {
    const SDL_ShaderCross_ShaderStage stage = fragment ? SDL_SHADERCROSS_SHADERSTAGE_FRAGMENT
                                                       : SDL_SHADERCROSS_SHADERSTAGE_VERTEX;
    std::size_t spirvSize = 0;
    void* spirv = compileSpirv(file, entry, stage, spirvSize);
    if (!spirv) return nullptr;
    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = static_cast<const Uint8*>(spirv);
    info.bytecode_size = spirvSize;
    info.entrypoint = entry;
    info.shader_stage = stage;
    // What the shader binds is read out of the shader rather than declared
    // twice: a mismatch between the two is a black screen with no error.
    SDL_ShaderCross_GraphicsShaderMetadata* metadata =
            SDL_ShaderCross_ReflectGraphicsSPIRV(info.bytecode, info.bytecode_size, 0);
    SDL_GPUShader* shader =
            metadata ? SDL_ShaderCross_CompileGraphicsShaderFromSPIRV(device_, &info,
                                                                      &metadata->resource_info, 0)
                     : nullptr;
    if (!shader) error_ = std::string("shader ") + entry + " would not load: " + SDL_GetError();
    SDL_free(metadata);
    SDL_free(spirv);
    return shader;
}

ComputePipeline Device::makeCompute(const ComputeWanted& wanted) {
    const std::filesystem::path file = assets_.parent_path() / "shaders" / wanted.shaderFile;
    std::size_t size = 0;
    void* spirv = compileSpirv(file, wanted.entry, SDL_SHADERCROSS_SHADERSTAGE_COMPUTE, size);
    if (!spirv) return {};
    SDL_ShaderCross_SPIRV_Info info{};
    info.bytecode = static_cast<const Uint8*>(spirv);
    info.bytecode_size = size;
    info.entrypoint = wanted.entry;
    info.shader_stage = SDL_SHADERCROSS_SHADERSTAGE_COMPUTE;
    // Thread-group size and every binding come out of the shader, so the C++
    // side never repeats a number the shader already states. The two going out
    // of step is a dispatch that reads nothing and writes nothing, with no error.
    SDL_ShaderCross_ComputePipelineMetadata* metadata =
            SDL_ShaderCross_ReflectComputeSPIRV(info.bytecode, info.bytecode_size, 0);
    SDL_GPUComputePipeline* pipeline =
            metadata ? SDL_ShaderCross_CompileComputePipelineFromSPIRV(device_, &info, metadata, 0)
                     : nullptr;
    if (!pipeline)
        error_ = std::string("compute shader ") + wanted.entry + " would not load: " + SDL_GetError();
    SDL_free(metadata);
    SDL_free(spirv);
    return {device_, pipeline};
}

Buffer Device::makeBuffer(SDL_GPUBufferUsageFlags usage, std::size_t bytes) {
    if (bytes == 0) return {};
    SDL_GPUBufferCreateInfo info{};
    info.usage = usage;
    info.size = static_cast<Uint32>(bytes);
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &info);
    if (!buffer) fail(std::string("could not make a buffer: ") + SDL_GetError());
    return {device_, buffer};
}

bool Device::readBuffer(SDL_GPUBuffer* buffer, void* into, std::size_t bytes) {
    if (!buffer || !into || bytes == 0) return false;
    SDL_GPUTransferBufferCreateInfo create{};
    create.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    create.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &create);
    if (!transfer) {
        fail(std::string("could not make a download buffer: ") + SDL_GetError());
        return false;
    }
    bool ok = false;
    if (SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_)) {
        if (SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands)) {
            SDL_GPUBufferRegion region{buffer, 0, static_cast<Uint32>(bytes)};
            SDL_GPUTransferBufferLocation location{transfer, 0};
            SDL_DownloadFromGPUBuffer(copy, &region, &location);
            SDL_EndGPUCopyPass(copy);
        }
        // A fence rather than a wait on the whole device: the caller wants
        // these bytes, not everything the card has ever been asked to do.
        if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands)) {
            SDL_WaitForGPUFences(device_, true, &fence, 1);
            SDL_ReleaseGPUFence(device_, fence);
            if (void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false)) {
                std::memcpy(into, mapped, bytes);
                SDL_UnmapGPUTransferBuffer(device_, transfer);
                ok = true;
            }
        }
    }
    if (!ok) fail(std::string("could not read a buffer back: ") + SDL_GetError());
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

GraphicsPipeline Device::makePipeline(const PipelineWanted& wanted) {
    const std::filesystem::path file = assets_.parent_path() / "shaders" / wanted.shaderFile;
    SDL_GPUShader* vertex = compile(file, wanted.vertexEntry, false);
    SDL_GPUShader* fragment = vertex ? compile(file, wanted.fragmentEntry, true) : nullptr;
    if (!vertex || !fragment) {
        if (vertex) SDL_ReleaseGPUShader(device_, vertex);
        if (fragment) SDL_ReleaseGPUShader(device_, fragment);
        return {};
    }

    SDL_GPUColorTargetDescription colour{};
    colour.format = kColourFormat;
    if (wanted.blend) {
        colour.blend_state.enable_blend = true;
        colour.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colour.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colour.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colour.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colour.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colour.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex;
    info.fragment_shader = fragment;
    info.vertex_input_state.vertex_buffer_descriptions = wanted.buffers.data();
    info.vertex_input_state.num_vertex_buffers = static_cast<Uint32>(wanted.buffers.size());
    info.vertex_input_state.vertex_attributes = wanted.attributes.data();
    info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(wanted.attributes.size());
    info.primitive_type = wanted.primitive;
    info.rasterizer_state.fill_mode = wanted.wireframe ? SDL_GPU_FILLMODE_LINE
                                                       : SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = wanted.cull;
    // Clockwise, and it is worth saying why rather than leaving it as a magic
    // enum. The world is Z-up and its ground is wound counter-clockwise seen
    // from above, so its faces point at the sky - that much is checked by the
    // adaptive mesh's own topology test. Which side the rasteriser calls
    // "front" is decided AFTER the projection, and this projection's basis
    // reverses the handedness, so a face that is counter-clockwise in the world
    // arrives clockwise on the screen. Declared the other way round it culled
    // exactly the faces it should have kept.
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_CLOCKWISE;
    // Clamp interpolated fragment depth, not individual vertex depths in HLSL.
    // Long terrain skirts may leave the slab without becoming foreground walls.
    info.rasterizer_state.enable_depth_clip = wanted.depthClip;
    info.rasterizer_state.enable_depth_bias =
            wanted.depthBiasConstant != 0.0f || wanted.depthBiasSlope != 0.0f;
    info.rasterizer_state.depth_bias_constant_factor = wanted.depthBiasConstant;
    info.rasterizer_state.depth_bias_slope_factor = wanted.depthBiasSlope;
    info.rasterizer_state.depth_bias_clamp = wanted.depthBiasClamp;
    info.depth_stencil_state.enable_depth_test = wanted.depthTest;
    info.depth_stencil_state.enable_depth_write = wanted.depthWrite;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_GREATER_OR_EQUAL;   // reversed depth (Camera::viewProjection)
    info.multisample_state.sample_count = samples_;
    info.target_info.color_target_descriptions = &colour;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = true;
    info.target_info.depth_stencil_format = kDepthFormat;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_, &info);
    SDL_ReleaseGPUShader(device_, vertex);
    SDL_ReleaseGPUShader(device_, fragment);
    if (!pipeline) {
        error_ = std::string("pipeline ") + wanted.vertexEntry + " would not build: " +
                 SDL_GetError();
        return {};
    }
    return GraphicsPipeline(device_, pipeline);
}

bool Device::readTexture(SDL_GPUTexture* texture, void* into, std::uint32_t width,
                         std::uint32_t height) {
    if (!texture || !into || width == 0 || height == 0) return false;
    const auto bytes = std::size_t(width) * height * 4;
    SDL_GPUTransferBufferCreateInfo create{};
    create.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    create.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &create);
    if (!transfer) {
        fail(std::string("could not make a download buffer: ") + SDL_GetError());
        return false;
    }
    bool ok = false;
    if (SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device_)) {
        if (SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands)) {
            SDL_GPUTextureRegion region{};
            region.texture = texture;
            region.w = width;
            region.h = height;
            region.d = 1;
            SDL_GPUTextureTransferInfo location{};
            location.transfer_buffer = transfer;
            location.pixels_per_row = width;
            location.rows_per_layer = height;
            SDL_DownloadFromGPUTexture(copy, &region, &location);
            SDL_EndGPUCopyPass(copy);
        }
        if (SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands)) {
            SDL_WaitForGPUFences(device_, true, &fence, 1);
            SDL_ReleaseGPUFence(device_, fence);
            if (void* mapped = SDL_MapGPUTransferBuffer(device_, transfer, false)) {
                std::memcpy(into, mapped, bytes);
                SDL_UnmapGPUTransferBuffer(device_, transfer);
                ok = true;
            }
        }
    }
    if (!ok) fail(std::string("could not read a texture back: ") + SDL_GetError());
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}

Buffer Device::uploadBuffer(SDL_GPUBufferUsageFlags usage, const void* data, std::size_t bytes) {
    Uploader one(*this);
    Buffer buffer = one.add(usage, data, bytes);
    one.finish();
    return buffer;
}

Device::Uploader::Uploader(Device& device) : device_(device) {}

Device::Uploader::~Uploader() { finish(); }

Buffer Device::Uploader::add(SDL_GPUBufferUsageFlags usage, const void* data, std::size_t bytes) {
    if (bytes == 0 || !device_.handle()) return {};
    SDL_GPUDevice* card = device_.handle();

    SDL_GPUBufferCreateInfo bufferInfo{};
    bufferInfo.usage = usage;
    bufferInfo.size = static_cast<Uint32>(bytes);
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(card, &bufferInfo);
    if (!buffer) return {};

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(card, &transferInfo);
    if (!transfer) {
        SDL_ReleaseGPUBuffer(card, buffer);
        return {};
    }
    void* mapped = SDL_MapGPUTransferBuffer(card, transfer, false);
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(card, transfer);

    if (!commands_) {
        commands_ = SDL_AcquireGPUCommandBuffer(card);
        copy_ = SDL_BeginGPUCopyPass(commands_);
    }
    SDL_GPUTransferBufferLocation source{transfer, 0};
    SDL_GPUBufferRegion destination{buffer, 0, static_cast<Uint32>(bytes)};
    SDL_UploadToGPUBuffer(copy_, &source, &destination, false);
    // Held until the copy has been submitted: releasing a transfer buffer the
    // copy pass still refers to is a use after free the driver does not check.
    transfers_.push_back(transfer);
    return Buffer(card, buffer);
}

void Device::Uploader::rewrite(SDL_GPUBuffer* buffer, const void* data, std::size_t bytes) {
    if (buffer == nullptr || data == nullptr || bytes == 0 || !device_.handle()) return;
    SDL_GPUDevice* card = device_.handle();
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(card, &info);
    if (transfer == nullptr) return;
    void* mapped = SDL_MapGPUTransferBuffer(card, transfer, false);
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(card, transfer);

    if (!commands_) {
        commands_ = SDL_AcquireGPUCommandBuffer(card);
        copy_ = SDL_BeginGPUCopyPass(commands_);
    }
    SDL_GPUTransferBufferLocation source{transfer, 0};
    SDL_GPUBufferRegion destination{buffer, 0, static_cast<Uint32>(bytes)};
    SDL_UploadToGPUBuffer(copy_, &source, &destination, true);
    transfers_.push_back(transfer);
}

void Device::Uploader::rewriteAt(SDL_GPUBuffer* buffer, std::size_t offsetBytes,
                                 const void* data, std::size_t bytes) {
    if (buffer == nullptr || data == nullptr || bytes == 0 || !device_.handle()) return;
    SDL_GPUDevice* card = device_.handle();
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(bytes);
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(card, &info);
    if (transfer == nullptr) return;
    void* mapped = SDL_MapGPUTransferBuffer(card, transfer, false);
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(card, transfer);

    if (!commands_) {
        commands_ = SDL_AcquireGPUCommandBuffer(card);
        copy_ = SDL_BeginGPUCopyPass(commands_);
    }
    SDL_GPUTransferBufferLocation source{transfer, 0};
    SDL_GPUBufferRegion destination{buffer, static_cast<Uint32>(offsetBytes),
                                    static_cast<Uint32>(bytes)};
    // Never cycle: the rest of this buffer belongs to other slots that are
    // still being drawn from.
    SDL_UploadToGPUBuffer(copy_, &source, &destination, false);
    transfers_.push_back(transfer);
}

void Device::Uploader::refill(SDL_GPUTexture* texture, const void* pixels, Uint32 width,
                              Uint32 height, Uint32 bytesPerPixel) {
    refillRegion(texture, pixels, 0, 0, width, height, bytesPerPixel);
}

bool Device::Uploader::refillRegion(SDL_GPUTexture* texture, const void* pixels,
                                    Uint32 x, Uint32 y, Uint32 width, Uint32 height,
                                    Uint32 bytesPerPixel, Uint32 layer, Uint32 mip,
                                    bool cycle) {
    const auto fail = [&](const std::string& why) {
        device_.fail(why);
        textureUploadsOk_ = false;
        return false;
    };
    if (!texture || !pixels || !width || !height || !device_.handle() || !bytesPerPixel)
        return fail("invalid texture region upload");
    if (std::uint64_t(width) * height > std::numeric_limits<Uint32>::max() / bytesPerPixel)
        return fail("texture region upload exceeds transfer buffer size");
    const Uint32 bytes = width * height * bytesPerPixel;
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_.handle(), &info);
    if (!transfer) return fail(std::string("texture transfer would not be made: ") + SDL_GetError());
    void* mapped = SDL_MapGPUTransferBuffer(device_.handle(), transfer, false);
    if (!mapped) {
        SDL_ReleaseGPUTransferBuffer(device_.handle(), transfer);
        return fail(std::string("texture transfer would not map: ") + SDL_GetError());
    }
    std::memcpy(mapped, pixels, bytes);
    SDL_UnmapGPUTransferBuffer(device_.handle(), transfer);

    if (!commands_) {
        commands_ = SDL_AcquireGPUCommandBuffer(device_.handle());
        if (!commands_) {
            SDL_ReleaseGPUTransferBuffer(device_.handle(), transfer);
            return fail(std::string("texture command buffer unavailable: ") + SDL_GetError());
        }
        copy_ = SDL_BeginGPUCopyPass(commands_);
        if (!copy_) {
            SDL_CancelGPUCommandBuffer(commands_);
            commands_ = nullptr;
            SDL_ReleaseGPUTransferBuffer(device_.handle(), transfer);
            return fail(std::string("texture copy pass unavailable: ") + SDL_GetError());
        }
    }
    SDL_GPUTextureTransferInfo source{transfer, 0, width, height};
    SDL_GPUTextureRegion region{texture, mip, layer, x, y, 0, width, height, 1};
    SDL_UploadToGPUTexture(copy_, &source, &region, cycle);
    transfers_.push_back(transfer);
    return true;
}

bool Device::Uploader::finish() {
    if (!commands_) return textureUploadsOk_;
    SDL_EndGPUCopyPass(copy_);
    if (!SDL_SubmitGPUCommandBuffer(commands_)) {
        device_.fail(std::string("upload submission failed: ") + SDL_GetError());
        textureUploadsOk_ = false;
    }
    for (SDL_GPUTransferBuffer* transfer : transfers_)
        SDL_ReleaseGPUTransferBuffer(device_.handle(), transfer);
    transfers_.clear();
    commands_ = nullptr;
    copy_ = nullptr;
    return textureUploadsOk_;
}

Texture Device::makeTexture(const SDL_GPUTextureCreateInfo& info) {
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_, &info);
    if (!texture) {
        error_ = std::string("texture would not be made: ") + SDL_GetError();
        return {};
    }
    return Texture(device_, texture);
}

Sampler Device::makeSampler(const SDL_GPUSamplerCreateInfo& info) {
    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_, &info);
    if (!sampler) {
        error_ = std::string("sampler would not be made: ") + SDL_GetError();
        return {};
    }
    return Sampler(device_, sampler);
}

Texture Device::loadMippedFromFiles(const std::vector<std::filesystem::path>& levels) {
    if (levels.empty()) return {};
    int width = 0, height = 0;
    if (!sizeOf(levels.front(), width, height)) {
        error_ = "material image is missing: " + levels.front().string();
        return {};
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = 1;
    info.num_levels = static_cast<Uint32>(levels.size());
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    Texture texture = makeTexture(info);
    if (!texture) return {};
    for (std::size_t level = 0; level < levels.size(); ++level)
        if (!putImage(device_, texture.get(), 0, static_cast<Uint32>(level), levels[level])) {
            error_ = "material level would not upload: " + levels[level].string();
            return {};
        }
    return texture;
}

Texture Device::loadArrayMippedFromFiles(const std::vector<std::vector<std::filesystem::path>>& layers,
                                        bool generateMipmaps) {
    if (layers.empty() || layers.front().empty()) return {};
    int width = 0, height = 0;
    if (!sizeOf(layers.front().front(), width, height)) {
        error_ = "image is missing: " + layers.front().front().string();
        return {};
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = static_cast<Uint32>(layers.size());
    info.num_levels = static_cast<Uint32>(layers.front().size());
    if (generateMipmaps) {
        info.num_levels = 1;
        for (int side = std::max(width, height); side > 1; side /= 2) ++info.num_levels;
        info.usage |= SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    }
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    Texture texture = makeTexture(info);
    if (!texture) return {};
    for (std::size_t layer = 0; layer < layers.size(); ++layer) {
        if (layers[layer].size() != layers.front().size()) {
            error_ = "layers of an array must have the same number of levels";
            return {};
        }
        int layerWidth = 0, layerHeight = 0;
        if (!sizeOf(layers[layer].front(), layerWidth, layerHeight) ||
            layerWidth != width || layerHeight != height) {
            error_ = "layers of an array must have matching image dimensions";
            return {};
        }
        const std::size_t levels = generateMipmaps ? 1 : layers[layer].size();
        for (std::size_t level = 0; level < levels; ++level)
            if (!putImage(device_, texture.get(), static_cast<Uint32>(layer),
                          static_cast<Uint32>(level), layers[layer][level])) {
                error_ = "layer would not upload: " + layers[layer][level].string();
                return {};
            }
    }
    if (generateMipmaps && info.num_levels > 1) {
        auto* commands = SDL_AcquireGPUCommandBuffer(device_);
        if (!commands) {
            error_ = std::string("mip generation command failed: ") + SDL_GetError();
            return {};
        }
        SDL_GenerateMipmapsForGPUTexture(commands, texture.get());
        if (!SDL_SubmitGPUCommandBuffer(commands)) {
            error_ = std::string("mip generation failed: ") + SDL_GetError();
            return {};
        }
    }
    return texture;
}

SDL_Surface* Device::loadDataPng(const std::filesystem::path& path) {
    auto* io=SDL_IOFromFile(path.string().c_str(),"rb");if (!io) return nullptr;
    auto* image=IMG_LoadPNG_IO(io);SDL_CloseIO(io);if (!image) return nullptr;
    if (image->format==SDL_PIXELFORMAT_RGBA32) return image;
    auto* rgba=SDL_ConvertSurface(image,SDL_PIXELFORMAT_RGBA32);SDL_DestroySurface(image);return rgba;
}

Texture Device::loadDataArrayFromFiles(const std::vector<std::filesystem::path>& layers) {
    if (layers.empty()) return {};
    Texture texture;Uint32 width=0,height=0;
    Uploader upload(*this);
    for (std::size_t layer=0;layer<layers.size();++layer) {
        auto* image=loadDataPng(layers[layer]);
        if (!image) {fail("data PNG missing: "+layers[layer].string());return {};}
        if (!texture) {
            width=image->w;height=image->h;
            SDL_GPUTextureCreateInfo info{};info.type=SDL_GPU_TEXTURETYPE_2D_ARRAY;
            info.format=kColourFormat;info.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
            info.width=width;info.height=height;info.layer_count_or_depth=Uint32(layers.size());info.num_levels=1;
            texture=makeTexture(info);
        }
        const bool ok=texture && image->w==int(width) && image->h==int(height) && image->pitch==int(width*4) &&
            upload.refillRegion(texture.get(),image->pixels,0,0,width,height,4,Uint32(layer));
        SDL_DestroySurface(image);
        if (!ok) {fail("data PNG upload/size mismatch: "+layers[layer].string());return {};}
    }
    if (!upload.finish()) return {};
    return texture;
}

Texture Device::loadArrayFromFiles(const std::vector<std::filesystem::path>& layers) {
    if (layers.empty()) return {};
    int width = 0, height = 0;
    if (!sizeOf(layers.front(), width, height)) {
        error_ = "image is missing: " + layers.front().string();
        return {};
    }
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = static_cast<Uint32>(width);
    info.height = static_cast<Uint32>(height);
    info.layer_count_or_depth = static_cast<Uint32>(layers.size());
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    Texture texture = makeTexture(info);
    if (!texture) return {};
    for (std::size_t layer = 0; layer < layers.size(); ++layer)
        if (!putImage(device_, texture.get(), static_cast<Uint32>(layer), 0, layers[layer])) {
            error_ = "layer would not upload: " + layers[layer].string();
            return {};
        }
    return texture;
}

// --- images from files, kept -------------------------------------------------
//
// A world's renderer is made again whenever a different world is shown - its
// passes hold that world's climate, shadows and ground - and every one of
// them loaded its pictures from disk again: the ground's sixteen scans with
// their mip chains alone were four seconds of a frozen window. The pictures
// are not the world's. They are loaded once, kept here, and each pass is
// handed a borrowed handle, which gives nothing back when it goes; the device
// gives them all back when it closes.

namespace {
std::uint64_t stampOf(std::uint64_t key, const std::filesystem::path& file) {
    std::error_code ec;
    key = fnv(key, file.string());
    const auto bytes = std::filesystem::file_size(file, ec);
    const auto when = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    key = fnv(key, &bytes, sizeof bytes);
    return fnv(key, &when, sizeof when);
}
} // namespace

Texture Device::keptTexture(std::uint64_t key, const std::function<Texture()>& load) {
    // Loaded afresh each time, owned by the caller, as before there was a
    // kept set: what a check of the loaders themselves wants.
    static const bool fresh = std::getenv("ASR_TEXTURE_NO_CACHE") != nullptr;
    if (fresh) return load();
    if (const auto found = textures_.find(key); found != textures_.end()) return Texture(nullptr, found->second.get());
    Texture made = load();
    if (!made) return {};
    SDL_GPUTexture* raw = made.get();
    textures_.emplace(key, std::move(made));
    return Texture(nullptr, raw);
}

Texture Device::loadMipped(const std::vector<std::filesystem::path>& levels) {
    std::uint64_t key = fnv(kFnvBasis, std::string("mipped"));
    for (const auto& f : levels) key = stampOf(key, f);
    return keptTexture(key, [&] { return loadMippedFromFiles(levels); });
}

Texture Device::loadArrayMipped(const std::vector<std::vector<std::filesystem::path>>& layers,
                               bool generateMipmaps) {
    std::uint64_t key = fnv(kFnvBasis, std::string(generateMipmaps ? "array-mipped-generated" : "array-mipped"));
    for (const auto& layer : layers) {
        key = fnv(key, std::string("|"));
        for (const auto& f : layer) key = stampOf(key, f);
    }
    return keptTexture(key, [&] { return loadArrayMippedFromFiles(layers, generateMipmaps); });
}

Texture Device::loadDataArray(const std::vector<std::filesystem::path>& layers) {
    std::uint64_t key = fnv(kFnvBasis, std::string("data-array"));
    for (const auto& f : layers) key = stampOf(key, f);
    return keptTexture(key, [&] { return loadDataArrayFromFiles(layers); });
}

Texture Device::loadArray(const std::vector<std::filesystem::path>& layers) {
    std::uint64_t key = fnv(kFnvBasis, std::string("array"));
    for (const auto& f : layers) key = stampOf(key, f);
    return keptTexture(key, [&] { return loadArrayFromFiles(layers); });
}

} // namespace engine
