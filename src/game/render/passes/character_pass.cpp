#include "game/render/passes/character_pass.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include "engine/render/draw_queue.hpp"
#include "engine/render/frame.hpp"
#include "engine/render/geometry/instances.hpp"
#include "engine/render/render_pipeline.hpp"
#include "game/render/pass_ids.hpp"

namespace game {
namespace {
struct CardVertex { float corner[2], uv[2]; };
// Projected height, in pixels, below which a level stops being worth its
// triangles; the last is the card's floor, below that nothing is drawn.
constexpr double kLevelPixels[] = {520, 230, 90, 26};
constexpr double kCardFloorPixels = 2.5;
} // namespace

engine::PassPlace CharacterPass::setup(engine::Device& device, engine::RenderPipeline& into) {
    const engine::PassPlace place{engine::passOf(Pass::Character), engine::stageOf(Stage::World),
                                  static_cast<engine::PassOrder>(Order::Opaque)};
    const auto root = device.assets().parent_path() / "generated/characters/knight";
    auto model = std::make_unique<CharacterModel>();
    std::string error;
    if (!model->load(root, error)) {
        std::cerr << "Character unavailable: " << error
                  << " (tools/character_import_blender.py, pack_character.py, bake_character_impostor.py)\n";
        return place;   // optional content: the pass draws nothing
    }

    engine::PipelineWanted wanted;
    wanted.shaderFile = "character.hlsl";
    wanted.vertexEntry = "CharacterVS";
    wanted.fragmentEntry = "CharacterPS";
    wanted.buffers = {
            {0, sizeof(float) * 2, SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
            {1, sizeof(CharacterSkinnedVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0},
    };
    wanted.attributes = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, 0},
            {1, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(CharacterSkinnedVertex, position)},
            {2, 1, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(CharacterSkinnedVertex, normal)},
    };
    // Both sides: the cape, the chainmail and the lashes are single sheets.
    wanted.cull = SDL_GPU_CULLMODE_NONE;
    wanted.depthClip = true;
    engine::GraphicsPipeline mesh = device.makePipeline(wanted);
    if (!mesh) return {};

    engine::PipelineWanted cardWanted = wanted;
    cardWanted.vertexEntry = "CharacterCardVS";
    cardWanted.fragmentEntry = "CharacterCardPS";
    cardWanted.buffers = {{0, sizeof(CardVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0}};
    cardWanted.attributes = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(CardVertex, corner)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(CardVertex, uv)},
    };
    engine::GraphicsPipeline card;
    if (model->impostor().present) card = device.makePipeline(cardWanted);

    SDL_GPUSamplerCreateInfo sampler{};
    sampler.min_filter = sampler.mag_filter = SDL_GPU_FILTER_LINEAR;
    sampler.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sampler.address_mode_u = sampler.address_mode_v = sampler.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    sampler.enable_anisotropy = true;
    sampler.max_anisotropy = 8.0f;
    sampler_ = device.makeSampler(sampler);
    if (!sampler_) return {};

    std::vector<std::vector<std::filesystem::path>> albedo, normal, surface;
    for (const auto& m : model->materials()) {
        albedo.push_back({root / m.albedo});
        normal.push_back({root / m.normal});
        surface.push_back({root / m.surface});
    }
    albedo_ = device.loadArrayMipped(albedo, true);
    normal_ = device.loadArrayMipped(normal, true);
    surface_ = device.loadArrayMipped(surface, true);
    if (!albedo_ || !normal_ || !surface_) return {};
    if (card) {
        std::vector<std::vector<std::filesystem::path>> colours, normals;
        for (const auto& c : model->impostor().colours) colours.push_back({root / c});
        for (const auto& n : model->impostor().normals) normals.push_back({root / n});
        cardColour_ = device.loadArrayMipped(colours, true);
        cardNormal_ = device.loadArrayMipped(normals, true);
        if (!cardColour_ || !cardNormal_) card = {};
    }

    engine::Device::Uploader upload(device);
    uvs_ = upload.add(SDL_GPU_BUFFERUSAGE_VERTEX, model->uvs().data(), model->uvs().size() * sizeof(float));
    indices_ = upload.add(SDL_GPU_BUFFERUSAGE_INDEX, model->indices().data(),
                          model->indices().size() * sizeof(std::uint32_t));
    // The card: a unit width either side of the vertical, a unit tall.
    const std::array<CardVertex, 4> quad{{{{-1, 0}, {0, 1}}, {{1, 0}, {1, 1}}, {{-1, 1}, {0, 0}}, {{1, 1}, {1, 0}}}};
    const std::array<std::uint16_t, 6> order{{0, 2, 3, 0, 3, 1}};
    quad_ = upload.add(SDL_GPU_BUFFERUSAGE_VERTEX, quad.data(), sizeof(quad));
    quadIndices_ = upload.add(SDL_GPU_BUFFERUSAGE_INDEX, order.data(), sizeof(order));
    upload.finish();
    if (!uvs_ || !indices_ || !quad_ || !quadIndices_) return {};

    mesh_ = into.take(std::move(mesh));
    meshBindings_ = into.take({{albedo_.get(), sampler_.get()}, {normal_.get(), sampler_.get()},
                               {surface_.get(), sampler_.get()}, shadow_});
    if (card) {
        card_ = into.take(std::move(card));
        cardBindings_ = into.take({{cardColour_.get(), sampler_.get()}, {cardNormal_.get(), sampler_.get()},
                                   {surface_.get(), sampler_.get()}, shadow_});
    }
    std::cout << "Character: " << model->vertexCount() << " vertices, " << model->materials().size()
              << " materials, levels";
    for (std::size_t l = 0; l < model->levels(); ++l) std::cout << ' ' << model->trianglesOf(l);
    std::cout << (card_ ? ", impostor card\n" : "\n");
    animator_ = engine::animation::buildHumanoid(model->skeleton());
    model_ = std::move(model);
    return place;
}

bool CharacterPass::anything(const engine::Frame& frame) const {
    return model_ && state_.visible && state_.fade > 0.001 && frame.instances != nullptr &&
           frame.scene.extra[2] > 0.01f;
}

std::string CharacterPass::animationState() const {
    const auto* machine = animator_ ? animator_->baseMachine() : nullptr;
    return machine ? machine->currentName() : std::string();
}

void CharacterPass::collect(const engine::Frame& frame, engine::DrawQueue& queue) {
    drawnLevel_ = -1;
    // The animator's parameters, from what the controller reports. Time goes
    // on whether or not anything is drawn: a character that turns its back on
    // the camera does not freeze mid-stride.
    auto& params = animator_->parameters();
    params.set("speed", float(state_.gait.speed));
    params.set("turn", float(state_.gait.turn));
    params.set("grounded", state_.gait.grounded ? 1.0f : 0.0f);
    params.set("vz", float(state_.gait.vz));
    params.set("jump", state_.gait.jumped ? 1.0f : 0.0f);
    params.set("locomotion_phase", float(state_.gait.phase / 6.283185307179586));
    params.set("time", float(state_.gait.time));
    params.set("acceleration", float(state_.gait.acceleration));
    params.set("slope", float(state_.gait.slope));
    params.set("impact", float(state_.gait.impact));
    const float* m = frame.scene.viewProjection;
    // The origin projected in double: see character.hlsl.
    const double o[3] = {state_.x, state_.y, state_.z};
    double clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = double(m[r * 4]) * o[0] + double(m[r * 4 + 1]) * o[1] + double(m[r * 4 + 2]) * o[2] + double(m[r * 4 + 3]);
    // Behind the eye, or far past the frustum's sides: nothing to draw.
    const double height = model_->height();
    if (clip[3] < -height) { animator_->update(float(state_.seconds)); return; }
    const double w = std::max(0.05, clip[3]);
    const double pixelsPerMetre = std::hypot(m[4], m[5], m[6]) * frame.scene.viewport[1] * 0.5 / w;
    const double pixels = height * pixelsPerMetre;
    if (std::abs(clip[0]) > w + height * 2 * std::hypot(m[0], m[1], m[2]) ||
        std::abs(clip[1]) > w + height * 2 * std::hypot(m[4], m[5], m[6])) {
        animator_->update(float(state_.seconds));   // off the screen: advanced, not evaluated
        return;
    }

    if (std::getenv("ASR_CHARACTER_DEBUG")) {
        static int every = 0;
        if (++every % 60 == 0)
            std::cerr << "character: pixels " << pixels << " clip " << clip[0] << ' ' << clip[1] << ' ' << clip[2]
                      << ' ' << clip[3] << " state " << animationState() << '\n';
    }
    std::size_t level = model_->levels();
    for (std::size_t l = 0; l < model_->levels() && l < std::size(kLevelPixels); ++l)
        if (pixels >= kLevelPixels[l]) { level = l; break; }
    if (level == model_->levels() && pixels >= kLevelPixels[std::size(kLevelPixels) - 1] * 0.5)
        level = model_->levels() - 1;
    if (const char* force = std::getenv("ASR_CHARACTER_FORCE_MESH")) level = std::size_t(std::clamp(std::atoi(force), 0, int(model_->levels()) - 1));
    const bool useCard = level == model_->levels();
    if (useCard && (!card_ || pixels < kCardFloorPixels)) { animator_->update(float(state_.seconds)); return; }

    engine::DrawItem item;
    item.author = 9;
    item.hasOwnData = item.ownToVertex = true;
    for (int r = 0; r < 4; ++r) item.own[r] = float(clip[r]);
    item.own[4] = float(state_.x); item.own[5] = float(state_.y); item.own[6] = float(state_.z);
    item.own[7] = float(std::clamp(state_.fade, 0.0, 1.0));

    if (useCard) {
        animator_->update(float(state_.seconds));   // the card is its rest pose; time goes on
        skinnedValid_ = false;
        // The baked view nearest to where the eye stands, relative to the
        // way the character faces (view k was baked from k * tau / 8 about
        // the model's own frame, which CharacterModel turned to face +x).
        // The card faces the screen: its right is the screen's x axis in the
        // world (from the matrix, so a map view straight above, where the eye
        // is over the character, still has one), and the eye is square to it.
        double rx = m[0], ry = m[1];
        const double length = std::hypot(rx, ry);
        if (length < 1e-9) { rx = 1; ry = 0; } else { rx /= length; ry /= length; }
        const double ex = -ry, ey = rx;
        const double toEye = std::atan2(ey, ex) - state_.yaw;
        const double turns = toEye / (2 * std::acos(-1.0));
        const int view = int(std::lround((turns - std::floor(turns)) * 8)) % 8;
        item.pipeline = card_; item.bindings = cardBindings_;
        item.vertex[0] = quad_.get(); item.vertexStreams = 1;
        item.index = quadIndices_.get(); item.indexSize = SDL_GPU_INDEXELEMENTSIZE_16BIT; item.indexCount = 6;
        item.own[8] = float(view);
        item.own[10] = 1;
        item.own[12] = float(rx); item.own[13] = float(ry);   // the card's right
        item.own[14] = model_->impostor().width; item.own[15] = model_->impostor().height;
        queue.push(item);
        drawnLevel_ = int(level);
        return;
    }

    // Animation level of detail from the same projected size: the full graph
    // with foot IK and the look close up, then fewer evaluations a second.
    animator_->level(state_.firstPerson || pixels > 180 ? engine::animation::Animator::Level::Full
                     : pixels > 90 ? engine::animation::Animator::Level::Half
                                    : engine::animation::Animator::Level::Quarter);
    const bool due = animator_->update(float(state_.seconds)) || !skinnedValid_ || level != skinnedLevel_;
    if (!due) {
        // The last pose's vertices, as they were: the level and the pose are unchanged.
        const auto again = frame.instances->add(std::span<const CharacterSkinnedVertex>(skinned_));
        item.pipeline = mesh_; item.bindings = meshBindings_;
        item.vertex[0] = uvs_.get(); item.instancesFromArena = true; item.vertexOffset[1] = again;
        item.vertexStreams = 2;
        item.index = indices_.get(); item.indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
        const auto& ranges = state_.firstPerson ? model_->firstPersonRangesOf(level) : model_->rangesOf(level);
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            if (!ranges[i].count) continue;
            engine::DrawItem part = item;
            part.firstIndex = ranges[i].first; part.indexCount = ranges[i].count;
            part.own[8] = float(i); part.own[9] = model_->materials()[i].cutout ? 1.0f : 0.0f;
            queue.push(part);
        }
        drawnLevel_ = int(level);
        return;
    }
    // Feet on the ground: the ground in the character's own frame (turned by
    // its facing, its feet at the origin).
    if (state_.ground && state_.gait.grounded) {
        animator_->feet.frame = {state_.x, state_.y, state_.z, state_.yaw, true};
        const double cy = std::cos(state_.yaw), sy = std::sin(state_.yaw);
        const double baseX = state_.x, baseY = state_.y, baseZ = state_.z;
        const auto ground = state_.ground;
        animator_->feet.ground = [=](float x, float y) {
            return float(ground(baseX + cy * x - sy * y, baseY + sy * x + cy * y) - baseZ);
        };
        animator_->feetWeight = 1.0f;
    } else {
        animator_->feet.ground = nullptr;
        animator_->feet.reset();
    }
    // The head turns towards where the camera looks, in the character's frame.
    const double lookLength = std::hypot(state_.lookX, state_.lookY, state_.lookZ);
    if (lookLength > 1e-6 && !state_.firstPerson) {
        const double cy = std::cos(-state_.yaw), sy = std::sin(-state_.yaw);
        const double lx = state_.lookX / lookLength, ly = state_.lookY / lookLength;
        animator_->lookDirection = {float(cy * lx - sy * ly), float(sy * lx + cy * ly), float(state_.lookZ / lookLength)};
        // Only what the character can look at without turning: behind him, no.
        animator_->lookWeight = animator_->lookDirection.x > -0.2f ? 0.7f : 0.0f;
    } else {
        animator_->lookWeight = 0;
    }
    animator_->evaluate().model(animator_->skeleton(), modelPose_);
    model_->skinning(modelPose_, skinning_);
    // Turned to its facing here, in the matrices, rather than per vertex.
    const float c = float(std::cos(state_.yaw)), s = float(std::sin(state_.yaw));
    for (auto& k : skinning_)
        for (int col = 0; col < 4; ++col) {
            const float x = k[col], y = k[4 + col];
            k[col] = c * x - s * y;
            k[4 + col] = s * x + c * y;
        }
    model_->skin(skinning_, level, skinned_);
    skinnedValid_ = true;
    skinnedLevel_ = level;
    const auto at = frame.instances->add(std::span<const CharacterSkinnedVertex>(skinned_));

    item.pipeline = mesh_; item.bindings = meshBindings_;
    item.vertex[0] = uvs_.get(); item.instancesFromArena = true; item.vertexOffset[1] = at;
    item.vertexStreams = 2;
    item.index = indices_.get(); item.indexSize = SDL_GPU_INDEXELEMENTSIZE_32BIT;
    const auto& ranges = state_.firstPerson ? model_->firstPersonRangesOf(level) : model_->rangesOf(level);
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (!ranges[i].count) continue;
        engine::DrawItem part = item;
        part.firstIndex = ranges[i].first;
        part.indexCount = ranges[i].count;
        part.own[8] = float(i);
        part.own[9] = model_->materials()[i].cutout ? 1.0f : 0.0f;
        queue.push(part);
    }
    drawnLevel_ = int(level);
}

} // namespace game
