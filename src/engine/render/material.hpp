#pragma once
#include <array>
#include <cmath>
#include <memory>
#include "engine/render/mesh.hpp"
#include "engine/render/render_pipeline.hpp"

namespace engine {
struct Material {
    std::string name, shader, vertexEntry, fragmentEntry;
    bool blend = false, depthTest = true, depthWrite = true;
    // Both sides by default, which is what everything got before this existed.
    // A closed solid wants SDL_GPU_CULLMODE_BACK; an alpha card needs NONE.
    SDL_GPUCullMode cull = SDL_GPU_CULLMODE_NONE;
    SDL_GPUPrimitiveType primitive = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    // Edges rather than faces, for looking at what is actually being drawn.
    bool wireframe = false;
    // Optional shader contract: material data at b2; b0 is scene, b1 is object.
    std::array<float, 16> parameters{};
    bool parametersToVertex = false, parametersToFragment = false;
    // Optional ownership of textures/samplers supplied by an asset manager.
    // External streaming atlases instead live under the world's frame leases.
    std::vector<std::shared_ptr<const Texture>> textures;
    std::vector<std::shared_ptr<const Sampler>> samplers;
    bool valid() const {
        if (shader.empty() || vertexEntry.empty() || fragmentEntry.empty()) return false;
        for (float value : parameters) if (!std::isfinite(value)) return false;
        return true;
    }
    PipelineWanted pipeline(const VertexLayout& layout) const {
        PipelineWanted wanted;
        wanted.shaderFile = shader.c_str(); wanted.vertexEntry = vertexEntry.c_str();
        wanted.fragmentEntry = fragmentEntry.c_str();
        wanted.buffers = layout.buffers; wanted.attributes = layout.attributes;
        wanted.blend = blend; wanted.depthTest = depthTest; wanted.depthWrite = depthWrite;
        wanted.cull = cull;
        wanted.primitive = primitive;
        wanted.wireframe = wireframe;
        return wanted;
    }
};

// Device/pipeline-local instance of a reusable immutable material definition.
// The definition (and its owned resources) outlives queued commands.
class MaterialInstance {
public:
    bool setup(Device& device, RenderPipeline& into, std::shared_ptr<const Material> material,
               const VertexLayout& layout) {
        if (!material || !material->valid()) { device.fail("invalid mesh material"); return false; }
        auto pipeline = device.makePipeline(material->pipeline(layout));
        if (!pipeline) return false;
        definition_ = std::move(material);
        pipeline_ = into.take(std::move(pipeline));
        owner_ = &into;
        fragment_ = vertex_ = kNoBindings;
        return true;
    }
    void textures(std::vector<SDL_GPUTextureSamplerBinding> fragment,
                  std::vector<SDL_GPUTextureSamplerBinding> vertex = {}) {
        if (!owner_) throw std::logic_error("material instance is not initialized");
        if (fragment_ == kNoBindings) { if (!fragment.empty()) fragment_ = owner_->take(std::move(fragment)); }
        else owner_->replace(fragment_, std::move(fragment));
        if (vertex_ == kNoBindings) { if (!vertex.empty()) vertex_ = owner_->takeVertex(std::move(vertex)); }
        else owner_->replaceVertex(vertex_, std::move(vertex));
    }
    void apply(DrawItem& item) const {
        if (!definition_) throw std::logic_error("renderer requires a compiled material");
        item.pipeline = pipeline_; item.bindings = fragment_; item.vertexBindings = vertex_;
        item.materialData = definition_->parameters;
        item.materialToVertex = definition_->parametersToVertex;
        item.materialToFragment = definition_->parametersToFragment;
    }
    const std::shared_ptr<const Material>& definition() const { return definition_; }
private:
    std::shared_ptr<const Material> definition_;
    RenderPipeline* owner_ = nullptr; // instance cannot outlive this pipeline
    PipelineSlot pipeline_ = 0;
    BindingSet fragment_ = kNoBindings, vertex_ = kNoBindings;
};
} // namespace engine
