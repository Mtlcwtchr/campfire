#include "engine/render/render_pipeline.hpp"

#include <algorithm>

#include <chrono>

#include "engine/render/frame.hpp"
#include "engine/render/geometry/instances.hpp"

namespace engine {

PipelineSlot RenderPipeline::take(GraphicsPipeline&& pipeline) {
    graphics_.push_back(std::move(pipeline));
    return static_cast<PipelineSlot>(graphics_.size() - 1);
}

ComputeSlot RenderPipeline::take(ComputePipeline&& pipeline) {
    computes_.push_back(std::move(pipeline));
    return static_cast<ComputeSlot>(computes_.size() - 1);
}

BindingSet RenderPipeline::take(std::vector<SDL_GPUTextureSamplerBinding> bindings) {
    bindings_.push_back(std::move(bindings));
    return static_cast<BindingSet>(bindings_.size() - 1);
}

BindingSet RenderPipeline::takeVertex(std::vector<SDL_GPUTextureSamplerBinding> bindings) {
    vertexBindings_.push_back(std::move(bindings));
    return static_cast<BindingSet>(vertexBindings_.size() - 1);
}

bool RenderPipeline::build(Device& device) {
    if (stages_.empty()) stages_.push_back(StageInfo{});
    for (std::unique_ptr<DrawPass>& pass : passes_) {
        pass->place = pass->setup(device, *this);
        if (pass->place.id == kNoPass) return false;
        if (pass->place.stage >= stages_.size()) {
            device.fail("a pass asked for a stage that was never declared");
            return false;
        }
    }
    return true;
}

void RenderPipeline::runDispatchList(Frame& frame, std::vector<ComputeDispatch>& jobs) {
    // After the upload and before any render pass. Both halves of that are
    // forced: a compute pass cannot be opened inside a render pass, and the
    // work exists to read what was just uploaded and write what is about to be
    // drawn - the instance lists a cull compacts, the arguments an indirect
    // draw reads. One pass per dispatch, so each sees the one before it.
    for (const ComputeDispatch& job : jobs) {
        if (job.pipeline >= computes_.size() || !computes_[job.pipeline]) continue;
        if (!job.groupsX || !job.groupsY || !job.groupsZ) continue;
        auto reads = job.reads;
        if (job.instanceReadSlot >= 0) {
            if (!frame.instances || !frame.instances->buffer() ||
                std::size_t(job.instanceReadSlot) >= reads.size()) {
                frame.device->fail("compute instance arena binding is unavailable");
                continue;
            }
            reads[std::size_t(job.instanceReadSlot)] = frame.instances->buffer();
        }
        std::vector<SDL_GPUStorageBufferReadWriteBinding> writes;
        writes.reserve(job.writes.size());
        for (SDL_GPUBuffer* buffer : job.writes) {
            SDL_GPUStorageBufferReadWriteBinding binding{};
            binding.buffer = buffer;
            // Never cycled: these buffers are the frame's working memory and a
            // fresh allocation behind the pass's back would hand the following
            // draw the old contents.
            binding.cycle = false;
            writes.push_back(binding);
        }
        std::vector<SDL_GPUStorageTextureReadWriteBinding> textureWrites;
        textureWrites.reserve(job.storageWrites.size());
        for (const ComputeDispatch::TextureWrite& texture : job.storageWrites) {
            SDL_GPUStorageTextureReadWriteBinding binding{};
            binding.texture = texture.texture;
            binding.mip_level = texture.mip;
            binding.layer = texture.layer;
            binding.cycle = false;
            textureWrites.push_back(binding);
        }
        SDL_GPUComputePass* pass = SDL_BeginGPUComputePass(
                frame.commands, textureWrites.data(), static_cast<Uint32>(textureWrites.size()),
                writes.data(), static_cast<Uint32>(writes.size()));
        if (!pass) continue;
        SDL_BindGPUComputePipeline(pass, computes_[job.pipeline].get());
        if (!job.samplers.empty())
            SDL_BindGPUComputeSamplers(pass, 0, job.samplers.data(),
                                       static_cast<Uint32>(job.samplers.size()));
        if (!job.storageReads.empty())
            SDL_BindGPUComputeStorageTextures(pass, 0, job.storageReads.data(),
                                              static_cast<Uint32>(job.storageReads.size()));
        if (!reads.empty())
            SDL_BindGPUComputeStorageBuffers(pass, 0, reads.data(),
                                             static_cast<Uint32>(reads.size()));
        SDL_PushGPUComputeUniformData(frame.commands, 0, job.own, sizeof(job.own));
        SDL_DispatchGPUCompute(pass, job.groupsX, job.groupsY, job.groupsZ);
        SDL_EndGPUComputePass(pass);
        ++lastDispatches_;
    }
}

void RenderPipeline::runDispatches(Frame& frame) {
    lastDispatches_ = 0;
    runDispatchList(frame, dispatches_);
    // Emptied here rather than only at the top of run(), because a frame that
    // records its dispatches without going through run() - a test of the
    // compute half alone - would otherwise re-run every dispatch of every
    // previous frame on top of this one's, and the counts would be the sum.
    dispatches_.clear();
}

void RenderPipeline::runPostDispatches(Frame& frame) {
    runDispatchList(frame, postDispatches_);
    postDispatches_.clear();
}

bool RenderPipeline::run(Frame& frame) {
    queue_.clear();
    dispatches_.clear();
    postDispatches_.clear();
#if ASR_ENABLE_PROFILING
    stats_ = DrawStats{};
    passMillis_.clear();
#endif

    // Every pass, into one queue. Nothing is drawn yet and nothing is bound.
    for (std::unique_ptr<DrawPass>& pass : passes_) {
        if (!pass->enabled || !pass->anything(frame)) continue;
#if ASR_ENABLE_PROFILING
        const auto at = std::chrono::steady_clock::now();
#endif
        queue_.openFor(pass->place);
        pass->collect(frame, queue_);
#if ASR_ENABLE_PROFILING
        passMillis_.emplace_back(
                pass->place.id,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at)
                        .count());
#endif
    }
    queue_.sort();
#if ASR_ENABLE_PROFILING
    stats_.items = static_cast<std::uint32_t>(queue_.size());
#endif

    // Every instance the passes appended, onto the card in one copy, before a
    // single draw is issued. Here rather than in the passes because a copy pass
    // cannot be opened inside a render pass, and because one upload for the
    // frame is the whole reason there is one arena for the frame.
    const bool anyInstances = frame.instances != nullptr && frame.instances->bytes() > 0;
    const bool anyArguments = frame.arguments != nullptr && frame.arguments->bytes() > 0;
    if (anyInstances || anyArguments) {
        Device::Uploader uploader(*frame.device);
        if (anyInstances && !frame.instances->upload(*frame.device, uploader)) return false;
        // The arguments go up with the instances, and before the dispatches: a
        // compute job may be about to overwrite an instance count in them.
        if (anyArguments && !frame.arguments->upload(*frame.device, uploader)) return false;
        uploader.finish();
    }

    runDispatches(frame);

    // One render pass per stage that has anything in it. The queue is already in
    // stage order, so this is a walk over runs rather than a search.
    const std::vector<DrawQueue::Sorted>& order = queue_.order();
    std::size_t start = 0;
    while (start < order.size()) {
        const std::uint8_t stage = static_cast<std::uint8_t>(order[start].key >> 56);
        std::size_t end = start;
        while (end < order.size() && static_cast<std::uint8_t>(order[end].key >> 56) == stage) ++end;

        const StageInfo& info = stages_[stage];
        // The picture so far, for a stage that has to see behind itself. Only
        // between render passes can it be copied, which is why it is a stage
        // boundary rather than something a pass does.
        if (info.grabColour && frame.grab != nullptr) {
            SDL_GPUTexture* source = frame.resolve != nullptr ? frame.resolve : frame.colour;
            if (SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(frame.commands)) {
                SDL_GPUTextureLocation from{};
                from.texture = source;
                SDL_GPUTextureLocation to{};
                to.texture = frame.grab;
                SDL_CopyGPUTextureToTexture(copy, &from, &to, frame.width, frame.height, 1, false);
                SDL_EndGPUCopyPass(copy);
            }
            if (info.grabMipmaps) SDL_GenerateMipmapsForGPUTexture(frame.commands, frame.grab);
        }
        SDL_GPUColorTargetInfo colour{};
        colour.texture = frame.colour;
        colour.clear_color = info.clear;
        colour.load_op = info.clearColour ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        // Folded down at the end of the stage, and kept as well, so a later
        // stage can draw over what this one left.
        if (frame.resolve != nullptr) {
            colour.resolve_texture = frame.resolve;
            colour.store_op = SDL_GPU_STOREOP_RESOLVE_AND_STORE;
        } else {
            colour.store_op = SDL_GPU_STOREOP_STORE;
        }

        SDL_GPUDepthStencilTargetInfo depth{};
        depth.texture = frame.depth;
        depth.clear_depth = 1.0f;
        depth.load_op = info.clearDepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        depth.store_op = SDL_GPU_STOREOP_STORE;
        depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
        depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

        frame.pass = SDL_BeginGPURenderPass(frame.commands, &colour, 1,
                                            info.useDepth ? &depth : nullptr);
#if ASR_ENABLE_PROFILING
        ++stats_.stages;
#endif
        issue(frame, start, end);
        SDL_EndGPURenderPass(frame.pass);
        frame.pass = nullptr;
        start = end;
    }
    runPostDispatches(frame);
    return true;
}

void RenderPipeline::issue(Frame& frame, std::size_t from, std::size_t to) {
    // What is currently bound. Nothing is, at the start of a render pass.
    PipelineSlot boundPipeline = 0xffff;
    BindingSet boundBindings = kNoBindings;
    BindingSet boundVertexBindings = kNoBindings;
    SDL_GPUBuffer* boundVertex[2]{nullptr, nullptr};
    std::uint32_t boundOffset[2]{0, 0};
    SDL_GPUBuffer* boundIndex = nullptr;
    SDL_GPUIndexElementSize boundIndexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;

    const std::vector<DrawQueue::Sorted>& order = queue_.order();
    for (std::size_t i = from; i < to; ++i) {
        const DrawItem& item = queue_.at(order[i].item);

        if (item.pipeline != boundPipeline) {
            SDL_BindGPUGraphicsPipeline(frame.pass, graphics_[item.pipeline].get());
            boundPipeline = item.pipeline;
#if ASR_ENABLE_PROFILING
            ++stats_.pipelineBinds;
#endif
            // A new pipeline may want different resources bound to the same
            // slots, so what was bound is no longer known to be right.
            boundBindings = kNoBindings;
            boundVertexBindings = kNoBindings;
            boundVertex[0] = boundVertex[1] = nullptr;
            boundOffset[0] = boundOffset[1] = 0;
            boundIndex = nullptr;
        }
        if (item.bindings != kNoBindings && item.bindings != boundBindings) {
            const std::vector<SDL_GPUTextureSamplerBinding>& set = bindings_[item.bindings];
            SDL_BindGPUFragmentSamplers(frame.pass, 0, set.data(), static_cast<Uint32>(set.size()));
            boundBindings = item.bindings;
#if ASR_ENABLE_PROFILING
            ++stats_.bindingBinds;
#endif
        }
        if (item.vertexBindings != kNoBindings && item.vertexBindings != boundVertexBindings) {
            const std::vector<SDL_GPUTextureSamplerBinding>& set =
                    vertexBindings_[item.vertexBindings];
            SDL_BindGPUVertexSamplers(frame.pass, 0, set.data(), static_cast<Uint32>(set.size()));
            boundVertexBindings = item.vertexBindings;
#if ASR_ENABLE_PROFILING
            ++stats_.bindingBinds;
#endif
        }
        // The offset counts as part of what is bound: two batches out of one
        // arena share a buffer and differ only in where they start, and a
        // comparison that looked at the pointer alone drew every one of them
        // from the first batch's data.
        SDL_GPUBuffer* second =
                item.instancesFromArena && frame.instances != nullptr
                        ? frame.instances->buffer()
                        : item.vertex[1];
        if (item.vertexStreams > 0 &&
            (item.vertex[0] != boundVertex[0] || item.vertexOffset[0] != boundOffset[0] ||
             (item.vertexStreams > 1 &&
              (second != boundVertex[1] || item.vertexOffset[1] != boundOffset[1])))) {
            SDL_GPUBufferBinding streams[2]{{item.vertex[0], item.vertexOffset[0]},
                                            {second, item.vertexOffset[1]}};
            SDL_BindGPUVertexBuffers(frame.pass, 0, streams, item.vertexStreams);
            boundVertex[0] = item.vertex[0];
            boundOffset[0] = item.vertexOffset[0];
            boundVertex[1] = item.vertexStreams > 1 ? second : nullptr;
            boundOffset[1] = item.vertexStreams > 1 ? item.vertexOffset[1] : 0;
#if ASR_ENABLE_PROFILING
            ++stats_.bufferBinds;
#endif
        }
        if (item.index != nullptr && (item.index != boundIndex || item.indexSize != boundIndexSize)) {
            SDL_GPUBufferBinding index{item.index, 0};
            SDL_BindGPUIndexBuffer(frame.pass, &index, item.indexSize);
            boundIndex = item.index;
            boundIndexSize = item.indexSize;
#if ASR_ENABLE_PROFILING
            ++stats_.bufferBinds;
#endif
        }
        // Anything this one draw needs to know, at slot 1. Slot 0 is the scene
        // and belongs to the whole frame.
        if (item.hasOwnData)
            SDL_PushGPUFragmentUniformData(frame.commands, 1, item.own, sizeof(item.own));
        if (item.ownToVertex)
            SDL_PushGPUVertexUniformData(frame.commands, 1, item.own, sizeof(item.own));
        if (item.materialToVertex)
            SDL_PushGPUVertexUniformData(frame.commands, 2, item.materialData.data(), sizeof(item.materialData));
        if (item.materialToFragment)
            SDL_PushGPUFragmentUniformData(frame.commands, 2, item.materialData.data(), sizeof(item.materialData));
        SDL_GPUBuffer* const arguments =
                item.indirectFromArena && frame.arguments != nullptr ? frame.arguments->buffer()
                                                                     : item.indirect;
        if (arguments != nullptr && item.index != nullptr) {
            SDL_DrawGPUIndexedPrimitivesIndirect(frame.pass, arguments, item.indirectOffset,
                                                 item.indirectDraws);
            if (frame.work) frame.work->indirectDraws += item.indirectDraws;
        } else if (arguments != nullptr) {
            SDL_DrawGPUPrimitivesIndirect(frame.pass, arguments, item.indirectOffset,
                                          item.indirectDraws);
            if (frame.work) frame.work->indirectDraws += item.indirectDraws;
        } else if (item.index != nullptr) {
            SDL_DrawGPUIndexedPrimitives(frame.pass, item.indexCount, item.instances,
                                         item.firstIndex, 0, 0);
        } else {
            SDL_DrawGPUPrimitives(frame.pass, item.vertexCount, item.instances, 0, 0);
        }
        if (frame.work) {
            if (const auto count = item.triangleCount()) {
                frame.work->triangles += *count;
                if (item.author < frame.work->trianglesByAuthor.size())
                    frame.work->trianglesByAuthor[item.author] += *count;
            } else {
                frame.work->unknownIndirectDraws += item.indirectDraws;
            }
            ++frame.work->draws;
            if (item.author < frame.work->drawsByAuthor.size()) ++frame.work->drawsByAuthor[item.author];
        }
    }
}

} // namespace engine
