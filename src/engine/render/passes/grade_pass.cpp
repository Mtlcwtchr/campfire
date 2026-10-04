#include "engine/render/passes/grade_pass.hpp"

#include <algorithm>
#include <cmath>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {

namespace {
// A lookup laid out as a strip: `size` squares side by side, one per blue
// step, red across each square and green down it.
void strip(int size, std::span<const std::array<float, 3>> lut, std::uint8_t* out) {
    const int width = size * size;
    for (int b = 0; b < size; ++b)
        for (int g = 0; g < size; ++g)
            for (int r = 0; r < size; ++r) {
                const auto& c = lut[(std::size_t(b) * size + g) * size + r];
                std::uint8_t* px = out + (std::size_t(g) * width + std::size_t(b) * size + r) * 4;
                for (int k = 0; k < 3; ++k) px[k] = std::uint8_t(std::lround(std::clamp(c[k], 0.0f, 1.0f) * 255.0f));
                px[3] = 255;
            }
}
Texture makeLut(Device& device, int size) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D_ARRAY;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = Uint32(size * size);
    info.height = Uint32(size);
    info.layer_count_or_depth = 2;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return device.makeTexture(info);
}
} // namespace

GradePass::GradePass(PassPlace where, GradeLook look) : where_(where), look_(look) {}

PassPlace GradePass::setup(Device& device, RenderPipeline& into) {
    renderer_ = &into;
    PipelineWanted wanted;
    wanted.shaderFile = "grade.hlsl";
    wanted.vertexEntry = "GradeVS";
    wanted.fragmentEntry = "GradePS";
    // It replaces every pixel with its graded self, so nothing is blended and
    // nothing is tested: the depth of the world is still there for whatever
    // comes after, and the grade does not touch it.
    wanted.blend = false;
    wanted.depthTest = false;
    wanted.depthWrite = false;
    auto graphics = device.makePipeline(wanted);
    if (!graphics) return {};
    pipeline_ = into.take(std::move(graphics));

    // Linear within and between levels, clamped at the edge of the screen,
    // and every level reachable: the glow is read from the coarse end of the
    // copy's chain.
    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sampler.min_lod = 0.0f;
    sampler.max_lod = 16.0f;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampler.max_lod = 0.0f;
    lutSampler_ = device.makeSampler(sampler);
    if (!lutSampler_) return {};
    // A lookup always bound, so the shader's binding is never empty; the
    // draw says whether to use it.
    lutSize_ = 2;
    lut_ = makeLut(device, lutSize_);
    if (!lut_) return {};
    // The picture is the frame's own copy and changes every frame; the slot
    // is filled in collect().
    bindings_ = into.take(std::vector<SDL_GPUTextureSamplerBinding>{{nullptr, sampler_.get()},
                                                                    {lut_.get(), lutSampler_.get()}});
    return where_;
}

void GradePass::luts(int size, std::span<const std::array<float, 3>> a, std::span<const std::array<float, 3>> b, float t) {
    lutBlend_ = std::clamp(t, 0.0f, 1.0f);
    const std::size_t count = std::size_t(size) * size * size;
    if (size < 2 || a.size() != count) {
        if (wantedSize_ != 0) { wantedSize_ = 0; wanted_.clear(); dirty_ = true; }
        return;
    }
    std::vector<std::uint8_t> next(count * 4 * 2);
    strip(size, a, next.data());
    strip(size, b.size() == count ? b : a, next.data() + count * 4);
    if (size == wantedSize_ && next == wanted_) return;
    wantedSize_ = size;
    wanted_ = std::move(next);
    dirty_ = true;
}

bool GradePass::anything(const Frame& frame) const {
    return frame.grab != nullptr && (frame.scene.quality[2] > 0.0f || frame.scene.quality[3] > 0.0f);
}

void GradePass::collect(const Frame& frame, DrawQueue& queue) {
    if (dirty_ && frame.device && wantedSize_ >= 2) {
        if (wantedSize_ != lutSize_) {
            auto made = makeLut(*frame.device, wantedSize_);
            if (made) { lut_ = std::move(made); lutSize_ = wantedSize_; }
        }
        if (lutSize_ == wantedSize_) {
            Device::Uploader uploader(*frame.device);
            const auto layer = std::size_t(lutSize_) * lutSize_ * lutSize_ * 4;
            bool ok = true;
            for (Uint32 l = 0; l < 2; ++l)
                ok = ok && uploader.refillRegion(lut_.get(), wanted_.data() + layer * l, 0, 0,
                                                 Uint32(lutSize_ * lutSize_), Uint32(lutSize_), 4, l);
            if (uploader.finish() && ok) dirty_ = false;
        }
    } else if (dirty_ && wantedSize_ == 0) {
        dirty_ = false;
    }
    renderer_->replace(bindings_, {{frame.grab, sampler_.get()}, {lut_.get(), lutSampler_.get()}});
    DrawItem item;
    item.author = 6;
    item.pipeline = pipeline_;
    item.bindings = bindings_;
    item.vertexCount = 3;
    item.hasOwnData = true;
    const float strength = std::clamp(frame.scene.quality[2], 0.0f, 1.0f);
    // The look itself: strength, bloom, vignette, grain. Then how many levels
    // the copy has, so the shader never asks for one beyond its end.
    item.own[0] = strength;
    item.own[1] = look_.bloom;
    item.own[2] = look_.vignette;
    item.own[3] = look_.grain;
    float levels = 1.0f;
    for (Uint32 side = std::max(frame.width, frame.height); side > 1; side /= 2) levels += 1.0f;
    item.own[4] = levels;
    item.own[5] = frame.scene.quality[3] > 0.0f ? 1.0f : 0.0f;   // FXAA
    // The lookup: present, its size, the blend from the first to the second.
    const bool lut = wantedSize_ >= 2 && !dirty_ && lutSize_ == wantedSize_;
    item.own[6] = lut ? 1.0f : 0.0f;
    item.own[7] = float(lutSize_);
    // brightness, contrast, saturation: the settings' own, or the built-in look
    const bool dials = frame.scene.grading[3] > 0.5f;
    item.own[8] = dials ? frame.scene.grading[0] : 1.0f;
    item.own[9] = dials ? frame.scene.grading[1] : 1.0f;
    item.own[10] = dials ? frame.scene.grading[2] : 1.0f;
    item.own[11] = lutBlend_;
    queue.push(item);
}

} // namespace engine
