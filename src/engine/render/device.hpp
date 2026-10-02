#pragma once
// The graphics device, and the few things only it can do.
//
// Everything that talks to SDL_GPU directly goes through here: opening the
// device and claiming the window, turning HLSL into a shader, putting bytes on
// the card, and handing out samplers. A pass never calls SDL_CreateGPUBuffer;
// it asks the device and gets back something that releases itself.
//
// What this replaces is one struct holding the device, three pipelines, two
// samplers, eight textures, two buffers and a patch cache, with a shutdown()
// that had to remember all of them in the right order. That arrangement works
// exactly until somebody adds a ninth thing and forgets the ninth line.

#include <SDL3/SDL.h>
#include <SDL3_shadercross/SDL_shadercross.h>

#include <filesystem>
#include <functional>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class Device;

// A thing the card owns, which knows how to give itself back.
//
// SDL hands out raw pointers and expects each to be released against the device
// that made it. These carry that device with them, so the release is not a line
// in a checklist somebody has to keep up to date. Movable, never copyable: two
// owners of one buffer is one double free.
template <class T, void (*Release)(SDL_GPUDevice*, T*)>
class Owned {
public:
    Owned() = default;
    Owned(SDL_GPUDevice* device, T* thing) : device_(device), thing_(thing) {}
    Owned(Owned&& other) noexcept : device_(other.device_), thing_(other.thing_) {
        other.device_ = nullptr;
        other.thing_ = nullptr;
    }
    Owned& operator=(Owned&& other) noexcept {
        if (this != &other) {
            reset();
            device_ = other.device_;
            thing_ = other.thing_;
            other.device_ = nullptr;
            other.thing_ = nullptr;
        }
        return *this;
    }
    Owned(const Owned&) = delete;
    Owned& operator=(const Owned&) = delete;
    ~Owned() { reset(); }

    void reset() {
        if (device_ && thing_) Release(device_, thing_);
        device_ = nullptr;
        thing_ = nullptr;
    }
    T* get() const { return thing_; }
    explicit operator bool() const { return thing_ != nullptr; }

private:
    SDL_GPUDevice* device_ = nullptr;
    T* thing_ = nullptr;
};

using Buffer = Owned<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>;
using Texture = Owned<SDL_GPUTexture, SDL_ReleaseGPUTexture>;
using Sampler = Owned<SDL_GPUSampler, SDL_ReleaseGPUSampler>;
// Named for what it is rather than "Pipeline", which in this layer means the
// whole ordered run of a frame.
using GraphicsPipeline = Owned<SDL_GPUGraphicsPipeline, SDL_ReleaseGPUGraphicsPipeline>;
using ComputePipeline = Owned<SDL_GPUComputePipeline, SDL_ReleaseGPUComputePipeline>;

// How a pipeline wants its vertices, its blending and its depth. Enough of
// SDL_GPUGraphicsPipelineCreateInfo that a pass can describe itself in a dozen
// lines instead of forty, and no more: anything a pass needs that is not here
// belongs here rather than in a second way of building pipelines.
struct PipelineWanted {
    const char* shaderFile = nullptr;     // relative to assets/shaders
    const char* vertexEntry = nullptr;
    const char* fragmentEntry = nullptr;
    std::vector<SDL_GPUVertexBufferDescription> buffers;
    std::vector<SDL_GPUVertexAttribute> attributes;
    bool blend = false;
    bool depthTest = true;
    bool depthWrite = true;
    // Which faces to skip. NONE is the default because it is what every
    // pipeline here was hard-wired to before this existed at all - the engine
    // drew both sides of every triangle in the world, and for the ground that
    // is half the rasterisation thrown away.
    //
    // It is a choice per pipeline and not a global, because some of these
    // genuinely need both sides: an alpha card is a single quad and has to be
    // visible from behind, or a tree loses half its leaves depending on where
    // you stand. Front faces are counter-clockwise, and the world is Z-up.
    SDL_GPUCullMode cull = SDL_GPU_CULLMODE_NONE;
    SDL_GPUPrimitiveType primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // Draw the edges rather than the faces. An inspection mode: it is the only
    // way to see which level of which representation a thing is actually being
    // drawn at, as against what the counters claim.
    bool wireframe = false;
    // Terrain skirts retain depth clamping; ordinary scene views need clipping
    // so triangles wholly in front of near/behind far do not cover the viewport.
    bool depthClip = false;
    // Polygon offset, in the rasteriser. Negative pulls towards the eye (the
    // depth compare is LESS_OR_EQUAL). For surfaces laid over others that
    // meet them at a shallow angle - water over its own bed - where the two
    // depths are equal to within the depth buffer's precision and which one
    // wins changes from frame to frame.
    float depthBiasConstant = 0.0f;
    float depthBiasSlope = 0.0f;
    float depthBiasClamp = 0.0f;
};

// What a compute shader is, which is much less than a graphics pipeline: no
// targets, no blending, no vertex layout. What it reads and writes is read out
// of the shader by reflection, exactly as it is for the graphics path - a
// binding declared twice is a mismatch nobody sees until the screen is black.
struct ComputeWanted {
    const char* shaderFile = nullptr;   // relative to assets/shaders
    const char* entry = nullptr;
};

class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    // `assets` is the sprite directory; shaders are found beside it. Returns
    // false and leaves a reason in error() rather than throwing, because the
    // caller's answer to "no GPU" is to say so and exit, not to unwind.
    bool open(SDL_Window* window, const std::filesystem::path& assets);
    // No window at all: frames are drawn into the offscreen target and never
    // presented. For measuring and for pictures taken without taking anybody's
    // screen - a window, even a hidden one, is an application in the Dock.
    bool openHeadless(std::uint32_t width, std::uint32_t height, const std::filesystem::path& assets);
    bool headless() const { return device_ != nullptr && window_ == nullptr; }
    std::uint32_t headlessWidth() const { return headlessWidth_; }
    std::uint32_t headlessHeight() const { return headlessHeight_; }
    // Waits until at most `frames` submissions are still on the card. A
    // headless frame has no swapchain to pace it, so without this the CPU runs
    // ahead and every timing is of the queue instead of the frame.
    void waitInFlight(std::size_t frames);
    void close();

    // Serial of the frame currently being recorded. Only fence completion,
    // never CPU frame age, makes resources referenced by it reusable.
    std::uint64_t nextSubmission() const { return submitted_ + 1; }
    std::uint64_t completedSubmission();
    bool submitFrame(SDL_GPUCommandBuffer* commands);

    SDL_GPUDevice* handle() const { return device_; }
    SDL_Window* window() const { return window_; }
    const std::filesystem::path& assets() const { return assets_; }
    const std::string& error() const { return error_; }
    void fail(std::string why) { error_ = std::move(why); }

    // The format the offscreen target is drawn in. Pipelines have to agree with
    // it, so it is asked for rather than repeated.
    static constexpr SDL_GPUTextureFormat kColourFormat = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    static constexpr SDL_GPUTextureFormat kDepthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    // How many samples a pixel the offscreen targets take. A pipeline has to
    // agree with its targets, so it is asked for here rather than repeated.
    SDL_GPUSampleCount samples() const { return samples_; }
    void samples(SDL_GPUSampleCount count) { samples_ = count; }

    GraphicsPipeline makePipeline(const PipelineWanted& wanted);
    ComputePipeline makeCompute(const ComputeWanted& wanted);

    // A buffer with no initial contents, for something the card fills itself:
    // a list a compute pass appends to, the arguments of an indirect draw, a
    // counter. Uploading zeroes to it first would be a copy pass for bytes that
    // are about to be overwritten.
    Buffer makeBuffer(SDL_GPUBufferUsageFlags usage, std::size_t bytes);

    // Reads a buffer back to the CPU, waiting for the card to be idle first.
    //
    // Deliberately blunt: this stalls, and nothing in a frame may call it. It
    // exists so a test can check what a compute pass actually wrote, which is
    // the only way a GPU-driven path can be tested at all rather than taken on
    // trust.
    bool readBuffer(SDL_GPUBuffer* buffer, void* into, std::size_t bytes);

    // The pixels of a colour target, after everything queued has finished.
    //
    // Here so that a draw path can be TESTED rather than looked at. A capture
    // run with a window and a screenshot answers the same question days later
    // and only for the frame somebody happened to capture; this answers it in
    // a second, for the case that is actually in doubt. `into` receives
    // width * height * 4 bytes in the colour format's own order.
    bool readTexture(SDL_GPUTexture* texture, void* into, std::uint32_t width,
                     std::uint32_t height);

    // Bytes onto the card, once, for something that will not change again.
    //
    // Each call is its own command buffer, which is the honest cost of an
    // immediate upload; a pass that uploads many small things in one frame
    // should ask for an Uploader instead.
    Buffer uploadBuffer(SDL_GPUBufferUsageFlags usage, const void* data, std::size_t bytes);

    // Several uploads under one command buffer and one copy pass. Building a
    // patch of ground meant three separate submits per patch, and a hundred
    // patches arriving in a frame meant three hundred - which the driver
    // charges for whether or not there are bytes worth moving.
    class Uploader {
    public:
        explicit Uploader(Device& device);
        ~Uploader();
        Uploader(const Uploader&) = delete;
        Uploader& operator=(const Uploader&) = delete;
        Buffer add(SDL_GPUBufferUsageFlags usage, const void* data, std::size_t bytes);
        // New contents for a buffer that already exists. For the arena of
        // instances a frame fills and refills: a buffer per frame would be a
        // buffer created and destroyed sixty times a second, and the driver
        // charges for that whether or not the bytes changed.
        void rewrite(SDL_GPUBuffer* buffer, const void* data, std::size_t bytes);
        // One region of a buffer that already exists, leaving the rest of it
        // alone. `rewrite` above cycles the whole buffer, which is right for an
        // arena refilled end to end and wrong for a pool of slots: cycling
        // while writing one slot discards every other slot's contents.
        void rewriteAt(SDL_GPUBuffer* buffer, std::size_t offsetBytes, const void* data,
                       std::size_t bytes);
        // New contents for a texture that already exists, under the same copy
        // pass. For the small pictures a pass redraws every frame - a coverage
        // mask, a minimap - as against the ones loaded once from a file.
        void refill(SDL_GPUTexture* texture, const void* pixels, std::uint32_t width,
                    std::uint32_t height, std::uint32_t bytesPerPixel);
        // Uploads one tightly packed page into an existing atlas. The texture
        // remains stable while terrain residency changes around the camera.
        // True means queued, not submitted; check finish() before publishing
        // page addresses to a draw. Failure is also recorded in Device::error().
        bool refillRegion(SDL_GPUTexture* texture, const void* pixels,
                          std::uint32_t x, std::uint32_t y,
                          std::uint32_t width, std::uint32_t height,
                          std::uint32_t bytesPerPixel,
                          std::uint32_t layer = 0, std::uint32_t mip = 0,
                          bool cycle = false);
        // Submits what has been added. Called by the destructor as well, so
        // forgetting it is slow rather than wrong.
        bool finish();

    private:
        Device& device_;
        SDL_GPUCommandBuffer* commands_ = nullptr;
        SDL_GPUCopyPass* copy_ = nullptr;
        std::vector<SDL_GPUTransferBuffer*> transfers_;
        bool textureUploadsOk_ = true;
    };

    Texture makeTexture(const SDL_GPUTextureCreateInfo& info);
    Sampler makeSampler(const SDL_GPUSamplerCreateInfo& info);

    // A texture built from image files, one per mip level, in the order the
    // levels go: the material bakery writes name.png and name@2 through name@16
    // and the card wants exactly that chain.
    Texture loadMipped(const std::vector<std::filesystem::path>& levels);
    // Layers, each with its own chain of levels: `layers[i]` is the mip chain of
    // layer i, finest first.
    //
    // What the ground wants. Six materials as six textures is six sets of
    // bindings and, worse, six samples a pixel however few of them the pixel
    // actually needs - a shader cannot index into separate textures. As one
    // array it is one binding and the layer is a number, so a pixel samples the
    // two materials it is made of and nothing else.
    // generateMipmaps builds a complete chain from each base image, down to 1x1.
    // It averages linear UNORM channels; normal consumers must renormalize.
    Texture loadArrayMipped(const std::vector<std::vector<std::filesystem::path>>& layers,
                            bool generateMipmaps = false);

    // The same, but each file is a layer of an array rather than a level of one
    // image. What the foliage cards are: six pictures the shader picks between.
    Texture loadArray(const std::vector<std::filesystem::path>& layers);
    // Byte-exact PNG data, bypassing platform image import/premultiplication.
    // The caller owns the returned surface (SDL_DestroySurface).
    static SDL_Surface* loadDataPng(const std::filesystem::path& path);
    Texture loadDataArray(const std::vector<std::filesystem::path>& layers);

private:
    // What the loaders above do the first time they are asked for a set of
    // files; after that the same files (by name, size and time) are the same
    // texture, lent out (see keptTexture).
    Texture loadMippedFromFiles(const std::vector<std::filesystem::path>& levels);
    Texture loadArrayMippedFromFiles(const std::vector<std::vector<std::filesystem::path>>& layers,
                                     bool generateMipmaps);
    Texture loadArrayFromFiles(const std::vector<std::filesystem::path>& layers);
    Texture loadDataArrayFromFiles(const std::vector<std::filesystem::path>& layers);
    // The texture under `key`, loaded once and owned here; what is returned is
    // borrowed (it releases nothing) and good until the device closes.
    Texture keptTexture(std::uint64_t key, const std::function<Texture()>& load);
    std::unordered_map<std::uint64_t, Texture> textures_;

    SDL_GPUShader* compile(const std::filesystem::path& file, const char* entry,
                           bool fragment);
    // The SPIR-V of one shader, whatever stage it is. Compiling HLSL is the
    // same for all three stages and only what is done with the result differs.
    void* compileSpirv(const std::filesystem::path& file, const char* entry,
                       SDL_ShaderCross_ShaderStage stage, std::size_t& size);
    // What names one compiled shader: the text of every shader file there is
    // (a shader is its includes too, and which ones it pulls in is the
    // compiler's business), the file, the entry and the stage.
    void* compileFromSource(const std::filesystem::path& file, const char* entry,
                            SDL_ShaderCross_ShaderStage stage, std::size_t& size);
    std::uint64_t shaderKey(const std::filesystem::path& file, const char* entry,
                            SDL_ShaderCross_ShaderStage stage);

    // Compiled SPIR-V, kept: compiling HLSL was ten seconds of a frozen
    // window every time a world was opened, because a new world rebuilds the
    // pipelines and every pipeline went back to the compiler. In memory for
    // the session, and on disk (.cache/shaders beside the project) so the
    // next session starts warm. A changed shader file is a different key.
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> spirv_;
    std::filesystem::path shaderCache_;
    // The digest of the shader folder, and what it was taken from: the
    // files' names, sizes and times. Asked again at every compile, read
    // again only when one of those moved.
    std::uint64_t folderStamp_ = 0, folderDigest_ = 0;

    SDL_Window* window_ = nullptr;

    std::uint32_t headlessWidth_ = 0, headlessHeight_ = 0;
    SDL_GPUDevice* device_ = nullptr;
    std::filesystem::path assets_;
    std::string error_;
    bool shaderCross_ = false;
    SDL_GPUSampleCount samples_ = SDL_GPU_SAMPLECOUNT_1;
    struct Submission { std::uint64_t serial; SDL_GPUFence* fence; };
    std::deque<Submission> submissions_;
    std::uint64_t submitted_ = 0, completed_ = 0;
};

} // namespace engine
