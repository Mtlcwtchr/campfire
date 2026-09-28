#include "engine/pipeline/runner.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdio>

namespace engine {

bool Runner::build(Device& device) {
    // Before anything is built, because a pipeline has to be told how many
    // samples its targets take and cannot be changed afterwards.
    const bool many = samples_ > 1 && std::getenv("ASR_FORCE_SINGLE_SAMPLE") == nullptr &&
                      SDL_GPUTextureSupportsSampleCount(device.handle(), Device::kColourFormat,
                                                        SDL_GPU_SAMPLECOUNT_4) &&
                      SDL_GPUTextureSupportsSampleCount(device.handle(), Device::kDepthFormat,
                                                        SDL_GPU_SAMPLECOUNT_4);
    device.samples(many ? SDL_GPU_SAMPLECOUNT_4 : SDL_GPU_SAMPLECOUNT_1);
    for (std::unique_ptr<Pipeline>& pipeline : pipelines_)
        if (!pipeline->build(device)) return false;
    return true;
}

bool Runner::frame(Device& device, Scene scene) {
    // Lightweight clocks, always. Whether a frame is WAITING or WORKING is the
    // first question anybody asks about a frame rate and the last one this
    // could answer: the breakdown lived behind the profiling switch, which is
    // off in the build people actually run, so "what is it doing" had no
    // answer at all. Include the pipelines: they do most of the CPU work.
    const auto started = std::chrono::steady_clock::now();
    const auto ms = [](auto a, auto b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    };
    frameTiming_ = {};
#if ASR_ENABLE_PROFILING
    timings_.clear();
#endif
    const Uint64 now = SDL_GetTicksNS();
    if (startedAt_ == 0) startedAt_ = lastAt_ = now;
    ++frames_;
    const double sinceLast = static_cast<double>(now - lastAt_) / 1e9;
    lastAt_ = now;

    // --- the world, on its own clock -------------------------------------
    //
    // Before a command buffer exists, and before anything is asked of the
    // window. A tick can take as long as it takes; holding the swapchain across
    // it would make a slow world into a stuttering picture of a fast one, and on
    // some drivers into a stall.
    //
    // The leftover is kept rather than dropped. Frames do not arrive at
    // multiples of the step, and rounding the remainder away every frame is how
    // a simulation quietly runs slow.
    stepsLastFrame_ = clock_.advance(sinceLast);
    if (stepsLastFrame_ > 0 && !pipelines_.empty()) {
        Frame tick;
        tick.device = &device;
        tick.index = frames_;
        tick.seconds = static_cast<double>(now - startedAt_) / 1e9;
        tick.step = clock_.step();
        tick.scene = scene;
        for (std::uint32_t i = 0; i < stepsLastFrame_; ++i)
            for (std::unique_ptr<Pipeline>& pipeline : pipelines_) {
                if (pipeline->cadence() != Cadence::Fixed) continue;
                if (!pipeline->run(tick)) return false;
            }
    }

    const auto acquireAt = std::chrono::steady_clock::now();
    frameTiming_.fixed = ms(started, acquireAt);
    SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(device.handle());
    if (!commands) {
        device.fail(std::string("no command buffer: ") + SDL_GetError());
        return false;
    }
    SDL_GPUTexture* swapchain = nullptr;
    Uint32 width = 0, height = 0;
    if (device.headless()) {
        // Nothing to present to: the offscreen target is the picture. Two
        // frames on the card at most, which is what a swapchain would allow.
        device.waitInFlight(1);
        width = device.headlessWidth();
        height = device.headlessHeight();
    } else if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, device.window(), &swapchain, &width,
                                                      &height)) {
        device.fail(std::string("no image from the window: ") + SDL_GetError());
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }
    const auto acquiredAt = std::chrono::steady_clock::now();
    frameTiming_.acquire = ms(acquireAt, acquiredAt);
    if (!swapchain && !device.headless()) {
        // Minimised, resizing, or the drawable temporarily unavailable. This
        // is not a rendered frame. Without the delay the main loop spins at
        // thousands of FPS while the last presented image stays frozen; that
        // looks like a renderer crash and burns a CPU core for no reason.
        if (std::getenv("ASR_SCENE_DEBUG")) {
            static bool reported = false;
            if (!reported) {
                std::fprintf(stderr, "render skipped: swapchain drawable unavailable\n");
                reported = true;
            }
        }
        SDL_CancelGPUCommandBuffer(commands);
        SDL_Delay(8);
#if ASR_ENABLE_PROFILING
        frameMillis_ = ms(started, std::chrono::steady_clock::now());
#endif
        return true;
    }
    if (!targets_.resize(device, width, height)) {
        SDL_CancelGPUCommandBuffer(commands);
        return false;
    }

    Frame frame;
    frame.device = &device;
    frame.commands = commands;
    frame.colour = targets_.colour();
    frame.depth = targets_.depth();
    frame.resolve = targets_.multisampled() ? targets_.resolved() : nullptr;
    frame.grab = targets_.grab();
    frame.width = width;
    frame.height = height;
    frame.index = frames_;
    frame.seconds = static_cast<double>(now - startedAt_) / 1e9;
    frame.step = sinceLast;
    frame.alpha = clock_.alpha();
    // Emptied here, filled by whatever passes want instances, uploaded once by
    // the render pipeline before it issues anything.
    instances_.begin();
    arguments_.begin();
    work_ = {};
    frame.work = &work_;
    frame.instances = &instances_;
    frame.arguments = &arguments_;
    frame.scene = scene;
    // The two the runner is the authority on. A pass asking the window for these
    // itself could get a different answer on the frame it is being resized.
    frame.scene.viewport[0] = static_cast<float>(width);
    frame.scene.viewport[1] = static_cast<float>(height);
    frame.scene.viewport[2] = static_cast<float>(held_ >= 0 ? held_ : frame.seconds);

    // Once, for the whole frame: every pipeline and every pass reads the same
    // view. Pushing it per pass was how two of them ended up disagreeing about
    // where the camera was.
    // Both stages. They have separate sets of constant buffers and filling one
    // does not fill the other, which is a mistake that compiles, binds, draws,
    // and quietly hands every pixel shader a buffer of zeros.
    SDL_PushGPUVertexUniformData(commands, 0, &frame.scene, sizeof(frame.scene));
    SDL_PushGPUFragmentUniformData(commands, 0, &frame.scene, sizeof(frame.scene));

    const auto pipelinesAt = std::chrono::steady_clock::now();
    frameTiming_.setup = ms(acquiredAt, pipelinesAt);
    for (std::unique_ptr<Pipeline>& pipeline : pipelines_) {
        // The fixed ones already ran, above, on their own clock.
        if (pipeline->cadence() == Cadence::Fixed) continue;
#if ASR_ENABLE_PROFILING
        const auto at = std::chrono::steady_clock::now();
#endif
        if (!pipeline->run(frame)) {
            SDL_CancelGPUCommandBuffer(commands);
            return false;
        }
#if ASR_ENABLE_PROFILING
        pipeline->millis =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at)
                        .count();
        timings_.emplace_back(pipeline->id(), pipeline->millis);
#endif
    }

    const auto submitAt = std::chrono::steady_clock::now();
    frameTiming_.pipelines = ms(pipelinesAt, submitAt);
    SDL_GPUBlitInfo blit{};
    blit.source.texture = targets_.resolved();
    blit.source.w = width;
    blit.source.h = height;
    blit.destination.texture = swapchain;
    blit.destination.w = width;
    blit.destination.h = height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_LINEAR;
    if (swapchain) SDL_BlitGPUTexture(commands, &blit);

    if (!device.submitFrame(commands)) return false;
    frameTiming_.submit = ms(submitAt, std::chrono::steady_clock::now());
    last_ = frame;
    last_.commands = nullptr;   // it is gone; keeping it would be a dangling invitation
#if ASR_ENABLE_PROFILING
    frameMillis_ =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                    .count();
    // The wall clock, not the work: a frame spent waiting for the display is
    // still a frame somebody waited through, and the number that matters is the
    // one that matches what they saw.
    if (sinceLast > 0.0) profile_.add(sinceLast * 1000.0);
#endif
#if ASR_ENABLE_FPS
    if (sinceLast > 0.0) {
        fpsElapsed_ += sinceLast;
        ++fpsFrames_;
        if (fpsElapsed_ >= 1.0) {
            frameRate_.fps = double(fpsFrames_) / fpsElapsed_;
            frameRate_.millis = fpsElapsed_ * 1000.0 / double(fpsFrames_);
            ++frameRate_.revision;
            fpsElapsed_ = 0; fpsFrames_ = 0;
        }
    }
#endif
    return true;
}

bool Runner::screenshot(Device& device, const std::string& path) const {
    return saveTargetAsPng(device, targets_, path);
}

} // namespace engine
