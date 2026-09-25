#include "game/client/renderer.hpp"

#include "game/client/wall_tiling.hpp"

#include <SDL3_image/SDL_image.h>

#include <fstream>

#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>

#include "game/simulation/inventory.hpp"
#include "game/simulation/zones.hpp"

namespace client {

namespace {

SDL_FColor rgb(int r, int g, int b, float a = 1.0f) {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a};
}

SDL_FColor shade(SDL_FColor c, float factor) {
    return {c.r * factor, c.g * factor, c.b * factor, c.a};
}

// Ground tones are kept dark and separated, because everything that matters -
// people, animals, piles, standing resources - is drawn on top of them and has to
// be distinguishable at a glance.
SDL_FColor terrainColour(sim::Terrain t) {
    switch (t) {
        case sim::Terrain::Grass:  return rgb( 92, 122,  68);
        case sim::Terrain::Forest: return rgb( 52,  82,  48);
        case sim::Terrain::Dirt:   return rgb(110,  90,  64);
        case sim::Terrain::Rock:   return rgb( 92,  90,  88);
        case sim::Terrain::Water:  return rgb( 46,  84, 122);
        case sim::Terrain::Sand:   return rgb(170, 152, 106);
        case sim::Terrain::Marsh:  return rgb( 66,  84,  64);
        default:                   return rgb(110, 110, 110);
    }
}

SDL_FColor resourceColour(content::ResourceKind k) {
    switch (k) {
        case content::ResourceKind::Tree:        return rgb( 44, 104,  50);
        case content::ResourceKind::Rock:        return rgb(176, 174, 168);
        case content::ResourceKind::Bush:        return rgb(178,  62,  82);
        case content::ResourceKind::WildPlant:   return rgb(224, 198,  92);
        case content::ResourceKind::Game:        return rgb(196, 132,  70);
        default:                                  return rgb(210, 210, 210);
    }
}

// Each kind fills a different share of its tile and stands a different height, so
// a wood, a rock field and a stand of grain are told apart at a glance.
float resourceScale(content::ResourceKind k) {
    switch (k) {
        case content::ResourceKind::Tree:      return 0.80f;
        case content::ResourceKind::Rock:      return 0.66f;
        case content::ResourceKind::Bush:      return 0.50f;
        case content::ResourceKind::WildPlant: return 0.46f;
        case content::ResourceKind::Game:      return 0.62f;
        default:                                return 0.60f;
    }
}

SDL_FColor itemColour(content::ItemCategory c) {
    switch (c) {
        case content::ItemCategory::Food:             return rgb(214, 158,  70);
        case content::ItemCategory::Raw:              return rgb(170, 158, 130);
        case content::ItemCategory::Tool:             return rgb(206, 206, 214);
        case content::ItemCategory::Weapon:           return rgb(214, 168, 168);
        case content::ItemCategory::Clothing:         return rgb(180, 170, 210);
        case content::ItemCategory::BuildingMaterial: return rgb(150, 116,  76);
        case content::ItemCategory::Component:        return rgb(188, 178, 148);
        default:                                       return rgb(160, 160, 160);
    }
}

SDL_FColor buildingColour(content::BuildingKind k, bool complete) {
    SDL_FColor c;
    switch (k) {
        case content::BuildingKind::Housing:       c = rgb(150, 118,  84); break;
        case content::BuildingKind::Storage:       c = rgb(132, 116,  70); break;
        case content::BuildingKind::Workshop:      c = rgb(126, 126, 148); break;
        case content::BuildingKind::Hearth:        c = rgb(214, 120,  56); break;
        case content::BuildingKind::Fortification: c = rgb(110, 106, 100); break;
        default:                                    c = rgb(140, 140, 140); break;
    }
    if (!complete) { c = shade(c, 0.5f); c.a = 0.7f; }
    return c;
}

SDL_FColor zoneColour(sim::ZoneKind kind, sim::ZoneMode mode) {
    if (mode == sim::ZoneMode::Forbidden) return rgb(210, 70, 70, 0.30f);
    switch (kind) {
        case sim::ZoneKind::Settlement:    return rgb(200, 200, 220, 0.13f);
        case sim::ZoneKind::Storage:       return rgb(220, 186, 110, 0.26f);
        case sim::ZoneKind::Extraction:    return rgb(150, 170, 200, 0.22f);
        case sim::ZoneKind::Farm:          return rgb(180, 220, 120, 0.26f);
        case sim::ZoneKind::Pasture:       return rgb(130, 210, 170, 0.24f);
        case sim::ZoneKind::Hunting:       return rgb(220, 150,  90, 0.22f);
        case sim::ZoneKind::Fishing:       return rgb(110, 190, 220, 0.24f);
        case sim::ZoneKind::Fortification: return rgb(190, 120, 200, 0.26f);
        case sim::ZoneKind::Patrol:        return rgb(200, 200, 120, 0.20f);
        default:                            return rgb(200, 200, 200, 0.16f);
    }
}

// Night darkens the whole scene, which is also the clearest signal that the
// working day has ended.
float daylightFactor(const sim::World& w) { return w.now().isDaylight ? 1.0f : 0.58f; }

// How many drawings the art gives for a growing crop, and which one a field is
// showing. Ripe is the last, so a field ready to reap looks ready to reap.
constexpr int kCropStages = 4;
// How much of its tile's width each stage covers. The drawing's own proportions
// then decide how high it stands, so a ripe stand is taller than a seedling
// without anyone having to say so twice.
constexpr std::array<float, kCropStages> kCropTilesWide{0.5f, 0.72f, 0.9f, 1.0f};
// How much of a crop card is sampled, leaving its drawn border outside the tile.
constexpr float kCropUvInset = 0.88f;

int cropStage(core::Fixed growth) {
    const int stage = static_cast<int>((growth * (kCropStages - 1)).toInt());
    return std::clamp(stage, 0, kCropStages - 1);
}

// The pawn rig, mirroring assets/sprites/pawn/pawn.json. Cells are square, all
// layers share one pivot, and the whole cell is drawn - so the figure's height on
// screen is a fraction of the cell rather than something measured per part.
constexpr float kPawnCell = 512.0f;
constexpr float kPawnPivotY = 456.0f / 512.0f;
// A grown figure fills about 0.6 of its cell, which puts it near 1.3 m tall.
constexpr float kPawnCellInTiles = 2.2f;
// Where the hand sits in the pawn's cell, and how long a held tool is against
// the figure. Taken off the rig: the arms hang at about a third of the torso.
constexpr float kHeldToolHandX = 0.20f;
constexpr float kHeldToolHandY = 0.30f;
constexpr float kHeldToolOfFigure = 0.34f;

// A tile's corners, clockwise from the north-west, in half-tiles. Edge i runs
// from corner i to corner i + 1, so edge 0 is the top, 1 the right, 2 the
// bottom and 3 the left.
constexpr int kTileCorners = 4;
// How hard the hill shading bites, and how far it is allowed to take a tile.
// One step of elevation is about a third of a metre; a bank four steps high
// across one tile is the strongest thing on this map, and it should read as a
// bank rather than as a black line.
constexpr float kReliefStrength = 0.038f;
constexpr float kReliefFloor = 0.72f;
constexpr float kReliefCeiling = 1.24f;

// A stable integer hash keeps every clump nailed to the same patch of ground as
// the camera moves. It is deliberately independent of the simulation RNG: wind
// and decoration must never change the world they are presenting.
std::uint32_t foliageNoise(std::int32_t x, std::int32_t y, int sample, int salt) {
    std::uint32_t v = static_cast<std::uint32_t>(x) * 0x9e3779b1u;
    v ^= static_cast<std::uint32_t>(y) * 0x85ebca77u;
    v ^= static_cast<std::uint32_t>(sample) * 0xc2b2ae3du;
    v ^= static_cast<std::uint32_t>(salt) * 0x27d4eb2fu;
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    return v ^ (v >> 16);
}

constexpr std::array<float, 4> kCornerX{-1.0f, +1.0f, +1.0f, -1.0f};
constexpr std::array<float, 4> kCornerY{-1.0f, -1.0f, +1.0f, +1.0f};

// Which edge of a tile faces each of core::neighbourOffsets' eight directions.
// A diagonal neighbour shares only a corner, so it has no edge and gets -1: an
// area outline is drawn along shared edges only, or it comes out as a chain of
// squares instead of one line. Checked by a test rather than by eye.
constexpr std::array<int, 8> kDirectionToEdge{1, -1, 0, -1, 3, -1, 2, -1};

} // namespace

const char* overlayName(Overlay o) {
    switch (o) {
        case Overlay::None:  return "none";
        case Overlay::Zones: return "zones";
        case Overlay::Jobs:  return "jobs";
        default:             return "?";
    }
}

Renderer::~Renderer() { shutdown(); }

bool Renderer::loadSpriteLibrary(const std::filesystem::path& spriteDirectory) {
    const auto load = [this](const std::filesystem::path& path) -> SDL_Texture* {
        SDL_Texture* texture = IMG_LoadTexture(sdl_, path.string().c_str());
        if (!texture) {
            SDL_Log("sprite %s did not load: %s", path.string().c_str(), SDL_GetError());
            return nullptr;
        }
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        return texture;
    };

    const auto manifestPath = spriteDirectory / "sprites.json";
    std::ifstream in(manifestPath);
    if (!in) {
        SDL_Log("no sprite manifest at %s; the world will draw as flat tiles",
                manifestPath.string().c_str());
    } else {
        nlohmann::json manifest;
        try {
            in >> manifest;
        } catch (const std::exception& e) {
            SDL_Log("sprite manifest %s is not readable: %s", manifestPath.string().c_str(), e.what());
            return false;
        }
        for (auto it = manifest["sprites"].begin(); it != manifest["sprites"].end(); ++it) {
            const auto file = it.value().value("file", std::string{});
            if (file.empty()) continue;
            if (SDL_Texture* texture = load(spriteDirectory / file)) {
                sprites_[it.key()] = texture;
                if (it.value().value("directional", false)) directional_.insert(it.key());
                if (it.value().value("ground", false)) ground_.insert(it.key());
            }
        }
    }

    // The pawn rig. One atlas per layer, all sharing a pivot, so every layer is
    // drawn into the same rectangle - see assets/sprites/pawn/pawn.json. The
    // manifest says which layers there are and what a person wearing a
    // particular thing is drawn in, so adding a garment is asset work rather
    // than a change here.
    const auto pawn = spriteDirectory / "pawn";
    pawnLayers_[kBody] = load(pawn / "body.png");
    pawnLayers_[kHead] = load(pawn / "head.png");
    pawnLayers_[kArms] = load(pawn / "arms.png");

    std::ifstream rig(pawn / "pawn.json");
    if (rig) {
        try {
            nlohmann::json manifest;
            rig >> manifest;
            for (auto it = manifest["layers"].begin(); it != manifest["layers"].end(); ++it) {
                const std::string layer = it.key();
                if (layer.rfind("garment", 0) != 0) continue;
                if (SDL_Texture* texture = load(pawn / it.value().get<std::string>()))
                    garmentLayers_[layer] = texture;
            }
            for (auto it = manifest["content_mapping"].begin();
                 it != manifest["content_mapping"].end(); ++it)
                garmentOfItem_[it.key()] = it.value().get<std::string>();
        } catch (const std::exception& e) {
            SDL_Log("pawn rig manifest is not readable: %s; nobody will be dressed", e.what());
        }
    }

    SDL_Log("sprite library: %zu named sprites, pawn rig %s", sprites_.size(),
            pawnLayers_[kBody] && pawnLayers_[kHead] ? "loaded" : "MISSING");
    return pawnLayers_[kBody] != nullptr && pawnLayers_[kHead] != nullptr;
}

bool Renderer::init(SDL_Renderer* sdl, const std::filesystem::path& spriteDirectory) {
    sdl_ = sdl;
    if (!sdl_) return false;
    if (!loadSpriteLibrary(spriteDirectory)) {
        shutdown();
        return false;
    }
    return true;
}

SDL_Texture* Renderer::sprite(const std::string& name, const std::string& fallback) const {
    auto it = sprites_.find(name);
    if (it != sprites_.end()) return it->second;
    if (!fallback.empty()) {
        it = sprites_.find(fallback);
        if (it != sprites_.end()) return it->second;
    }
    return nullptr;
}

bool Renderer::queueSprite(const Camera& cam, float sx, float sy, const std::string& name,
                           const std::string& fallback, float tiles, SDL_FColor tint, Fit fit,
                           bool flip) {
    SDL_Texture* texture = sprite(name, fallback);
    if (!texture) return false;

    float tw = 0.0f, th = 0.0f;
    SDL_GetTextureSize(texture, &tw, &th);
    if (tw <= 0.0f || th <= 0.0f) return false;

    // The world says how wide the thing is; the drawing says how tall it is for
    // that width. It used to be the other way round - a height in metres with a
    // free aspect - and that made every wide card enormous: a wall segment two
    // metres tall came out six tiles across, wider than the house beside it.
    const float span = tiles * static_cast<float>(cam.pixelsPerTile);
    const float width = fit == Fit::ByWidth ? span : span * std::min(1.0f, tw / th);
    const float height = width * th / tw;
    DrawItem item;
    item.cx = sx;
    item.cy = sy;
    item.flipHorizontal = flip;
    // Sprites stand on the ground: the bottom edge sits on the tile centre.
    item.layers[0] = {texture, tint, {}, {sx - width * 0.5f, sy - height, width, height}};
    item.layerCount = 1;
    addItem(item);
    return true;
}

void Renderer::shutdown() {
    for (auto& [name, texture] : sprites_) SDL_DestroyTexture(texture);
    sprites_.clear();
    for (auto& layer : pawnLayers_) {
        SDL_DestroyTexture(layer);
        layer = nullptr;
    }
    peopleMotion_.clear();
    animalMotion_.clear();
    if (worldMapTexture_ != nullptr) {
        SDL_DestroyTexture(worldMapTexture_);
        worldMapTexture_ = nullptr;
        worldMapKey_ = 0;
    }
    sdl_ = nullptr;
}

void Renderer::updateMotion(MotionState& state, float targetX, float targetY,
                            double frameSeconds, double tickSeconds) {
    if (!state.initialized) {
        state.x = state.fromX = state.targetX = targetX;
        state.y = state.fromY = state.targetY = targetY;
        state.initialized = true;
        return;
    }

    const float targetDx = targetX - state.targetX;
    const float targetDy = targetY - state.targetY;
    if (std::abs(targetDx) > 0.000001 || std::abs(targetDy) > 0.000001) {
        // A healthy adult can spend the configured 5.5-metre budget in one tick.
        // Only larger jumps are catch-up/teleportation that should not sweep a
        // pawn across the map.
        if (std::hypot(targetX - state.x, targetY - state.y) > 8.0 || tickSeconds <= 0.0) {
            state.x = state.fromX = targetX;
            state.y = state.fromY = targetY;
        } else {
            state.fromX = state.x;
            state.fromY = state.y;
        }
        state.targetX = targetX;
        state.targetY = targetY;
        state.elapsed = 0.0;
        state.duration = tickSeconds;

        if (std::abs(targetDx) > std::abs(targetDy) * 0.5) {
            state.facingFrame = 1;
            state.flipHorizontal = targetDx < 0.0;
        } else {
            state.facingFrame = targetDy < 0.0 ? 2 : 0;
            state.flipHorizontal = false;
        }
    }

    if (state.duration <= 0.0) return;
    state.elapsed = std::min(state.duration, state.elapsed + std::max(0.0, frameSeconds));
    const float alpha = static_cast<float>(state.elapsed / state.duration);
    state.x = state.fromX + (state.targetX - state.fromX) * alpha;
    state.y = state.fromY + (state.targetY - state.fromY) * alpha;
}

void Renderer::updatePresentation(const sim::World& w, double frameSeconds, double requestedTicksPerSecond) {
    const double tickSeconds = requestedTicksPerSecond > 0.0 ? 1.0 / requestedTicksPerSecond : 0.0;
    peopleMotion_.resize(w.people().size());
    for (const auto& p : w.people()) {
        if (p.id.value >= peopleMotion_.size()) continue;
        if (!p.alive) { peopleMotion_[p.id.value].initialized = false; continue; }
        updateMotion(peopleMotion_[p.id.value], static_cast<float>(p.pos.x.toDouble()),
                     static_cast<float>(p.pos.y.toDouble()),
                     frameSeconds, tickSeconds);
    }
    animalMotion_.resize(w.animals().size());
    for (const auto& a : w.animals()) {
        if (a.id.value >= animalMotion_.size()) continue;
        if (!a.alive) { animalMotion_[a.id.value].initialized = false; continue; }
        updateMotion(animalMotion_[a.id.value], static_cast<float>(a.pos.x.toDouble()),
                     static_cast<float>(a.pos.y.toDouble()),
                     frameSeconds, tickSeconds);
    }
}

const Renderer::MotionState* Renderer::personMotion(core::PersonId id) const {
    return id.valid() && id.value < peopleMotion_.size() && peopleMotion_[id.value].initialized
                   ? &peopleMotion_[id.value] : nullptr;
}

const Renderer::MotionState* Renderer::animalMotion(core::AnimalId id) const {
    return id.valid() && id.value < animalMotion_.size() && animalMotion_[id.value].initialized
                   ? &animalMotion_[id.value] : nullptr;
}

// --- tile batching ----------------------------------------------------------

void Renderer::beginTiles() {
    vertices_.clear();
    indices_.clear();
}

void Renderer::addQuadAt(float cx, float cy, float rx, float ry, SDL_FColor c) {
    const auto base = static_cast<int>(vertices_.size());
    for (int i = 0; i < kTileCorners; ++i) {
        SDL_Vertex v{};
        v.position = {cx + kCornerX[i] * rx, cy + kCornerY[i] * ry};
        v.color = c;
        vertices_.push_back(v);
    }
    // Two triangles, no centre vertex: a square needs neither.
    for (int i : {0, 1, 2, 0, 2, 3}) indices_.push_back(base + i);
}

void Renderer::addTile(const Camera& cam, core::TilePos t, SDL_FColor c, float scale) {
    float sx, sy;
    cam.tileToScreen(t, sx, sy);
    addQuadAt(sx, sy, cam.tileRadiusX() * scale, cam.tileRadiusY() * scale, c);
}

void Renderer::addItem(const DrawItem& item) { items_.push_back(item); }

void Renderer::flushItems() {
    if (items_.empty()) return;
    // Back to front. Within one geometry call the triangles are painted in the
    // order they are given, so sorting here is the whole of the depth handling.
    std::sort(items_.begin(), items_.end(), [](const DrawItem& a, const DrawItem& b) {
        return a.cy != b.cy ? a.cy < b.cy : a.cx < b.cx;
    });

    beginTiles();
    for (const auto& p : items_) {
        if (p.layerCount > 0) {
            // Geometry queued before this pawn must remain behind it; start a new
            // batch afterwards so later world objects can still cover the pawn.
            flushTiles();
            for (std::size_t i = 0; i < p.layerCount; ++i) {
                const auto& layer = p.layers[i];
                if (!layer.texture) continue;
                SDL_SetTextureColorModFloat(layer.texture, layer.colour.r, layer.colour.g, layer.colour.b);
                SDL_SetTextureAlphaModFloat(layer.texture, layer.colour.a);
                const SDL_FRect* source = layer.source.w > 0.0f && layer.source.h > 0.0f ? &layer.source : nullptr;
                SDL_RenderTextureRotated(sdl_, layer.texture, source, &layer.destination, 0.0, nullptr,
                                         p.flipHorizontal ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
            }
            beginTiles();
            continue;
        }

        // Flat top-down placeholders until their hand-drawn sprites arrive. Their
        // footprint still communicates kind and quantity, but no wall extrusion
        // makes the map look pseudo-3D.
        addQuadAt(p.cx, p.cy, p.rx, p.ry, p.colour);
    }
    flushTiles();
    items_.clear();
}

std::array<float, 4> Renderer::cornerLight(const sim::World& w, core::TilePos t) const {
    // Hill shading, the oldest trick there is: the land is lit from the
    // north-west, so a slope that climbs away from the light is bright and one
    // that falls away is dark. Worked out at the tile's corners rather than at
    // its middle, so the shading runs smoothly across the ground instead of
    // stepping tile by tile - the map is flat, and it is this that says it isn't.
    const auto height = [&](std::int32_t x, std::int32_t y) {
        const core::TilePos p{std::clamp(x, 0, w.map().width() - 1),
                              std::clamp(y, 0, w.map().height() - 1)};
        return static_cast<float>(w.map().at(p).elevation);
    };
    // Corner order is north-west, north-east, south-east, south-west.
    constexpr int kdx[4] = {0, 1, 1, 0};
    constexpr int kdy[4] = {0, 0, 1, 1};
    std::array<float, 4> out{};
    for (int i = 0; i < 4; ++i) {
        const std::int32_t x = t.x + kdx[i];
        const std::int32_t y = t.y + kdy[i];
        // The four tiles meeting at this corner.
        const float nw = height(x - 1, y - 1), ne = height(x, y - 1);
        const float sw = height(x - 1, y), se = height(x, y);
        const float dzdx = (ne + se) - (nw + sw);
        const float dzdy = (sw + se) - (nw + ne);
        out[i] = std::clamp(1.0f + (dzdx + dzdy) * kReliefStrength, kReliefFloor, kReliefCeiling);
    }
    return out;
}

void Renderer::addTexturedTile(const Camera& cam, core::TilePos t, SDL_FColor c, bool mayTurn,
                               float uvInset, int turn, const std::array<float, 4>* cornerShade) {
    float cx, cy;
    cam.tileToScreen(t, cx, cy);
    const float rx = cam.tileRadiusX();
    const float ry = cam.tileRadiusY();

    // Which way round this tile's drawing is turned. Every tile samples the same
    // card, so without this the ground reads as a lattice of identical medallions
    // rather than as ground. A square is symmetric under a quarter turn, so
    // rotating which corner takes which texture coordinate costs nothing.
    // A given turn wins: the shore cards have to face the water, not whichever
    // way the tile's hash fell.
    if (turn < 0) turn = mayTurn ? static_cast<int>(std::hash<core::TilePos>{}(t) % 4) : 0;

    // The whole card, corner to corner. On the tile grid this window had to be a
    // regular squares inscribed inside the tile, and stop short of the edge, or
    // the drawn outline on each ground card showed up as a field of dark bars
    // halfway up every tile. A square tile is the shape the card was drawn as.
    const auto base = static_cast<int>(vertices_.size());
    for (int i = 0; i < kTileCorners; ++i) {
        SDL_Vertex v{};
        v.position = {cx + kCornerX[i] * rx, cy + kCornerY[i] * ry};
        v.color = c;
        if (cornerShade) {
            const float k = (*cornerShade)[static_cast<std::size_t>(i)];
            v.color.r *= k;
            v.color.g *= k;
            v.color.b *= k;
        }
        const int u = (i + turn) % kTileCorners;
        // A card whose drawing has its own border cannot be sampled corner to
        // corner: the border lands on the tile edge and the field reads as a
        // lattice of separate cards. Sampling just inside it costs a rim of the
        // drawing and hides the seam. Ripe ears overhang their card, so there is
        // no clean border to cut away at extraction time either.
        v.tex_coord = {0.5f + kCornerX[u] * 0.5f * uvInset, 0.5f + kCornerY[u] * 0.5f * uvInset};
        vertices_.push_back(v);
    }
    for (int i : {0, 1, 2, 0, 2, 3}) indices_.push_back(base + i);
}

void Renderer::flushTiles(SDL_Texture* texture) {
    if (indices_.empty()) return;
    SDL_RenderGeometry(sdl_, texture, vertices_.data(), static_cast<int>(vertices_.size()),
                       indices_.data(), static_cast<int>(indices_.size()));
    vertices_.clear();
    indices_.clear();
}

void Renderer::strokeTileEdge(const Camera& cam, core::TilePos t, int direction, SDL_FColor c,
                             float scale) {
    const int edge = kDirectionToEdge[static_cast<std::size_t>(direction) % 8];
    if (edge < 0) return;                     // a diagonal shares a corner, not an edge
    float sx, sy;
    cam.tileToScreen(t, sx, sy);
    const float rx = cam.tileRadiusX() * scale;
    const float ry = cam.tileRadiusY() * scale;
    const int i = edge, j = (edge + 1) % kTileCorners;
    SDL_SetRenderDrawColorFloat(sdl_, c.r, c.g, c.b, c.a);
    SDL_RenderLine(sdl_, sx + kCornerX[i] * rx, sy + kCornerY[i] * ry,
                   sx + kCornerX[j] * rx, sy + kCornerY[j] * ry);
}

void Renderer::strokeTile(const Camera& cam, core::TilePos t, SDL_FColor c, float scale) {
    float sx, sy;
    cam.tileToScreen(t, sx, sy);
    const float rx = cam.tileRadiusX() * scale;
    const float ry = cam.tileRadiusY() * scale;
    SDL_SetRenderDrawColorFloat(sdl_, c.r, c.g, c.b, c.a);
    for (int i = 0; i < kTileCorners; ++i) {
        const int j = (i + 1) % kTileCorners;
        SDL_RenderLine(sdl_, sx + kCornerX[i] * rx, sy + kCornerY[i] * ry,
                       sx + kCornerX[j] * rx, sy + kCornerY[j] * ry);
    }
}

// --- layers ----------------------------------------------------------------

// The file name of the ground tile for a terrain, matching the keys the sprite
// library was written with.
const char* terrainSpriteName(sim::Terrain t) {
    switch (t) {
        case sim::Terrain::Grass:  return "terrain/grass";
        case sim::Terrain::Forest: return "terrain/forest";
        case sim::Terrain::Dirt:   return "terrain/dirt";
        case sim::Terrain::Rock:   return "terrain/rock";
        case sim::Terrain::Water:  return "terrain/water";
        case sim::Terrain::Sand:   return "terrain/sand";
        case sim::Terrain::Marsh:  return "terrain/marsh";
        default:                   return "terrain/grass";
    }
}

void Renderer::drawTerrain(const sim::World& w, const Camera& cam) {
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());
    const float light = daylightFactor(w);

    // One batch per terrain: a geometry call carries a single texture, and there
    // are seven terrains against thousands of tiles, so this is seven draws.
    for (int kind = 0; kind < static_cast<int>(sim::Terrain::Count); ++kind) {
        const auto terrain = static_cast<sim::Terrain>(kind);
        SDL_Texture* texture = sprite(terrainSpriteName(terrain));
        const bool mayTurn = !directional_.count(terrainSpriteName(terrain));
        beginTiles();
        for (std::int32_t y = view.min.y; y < view.max.y; ++y) {
            for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
                const core::TilePos p{x, y};
                const auto& tile = w.map().at(p);
                if (tile.terrain != terrain) continue;
                SDL_FColor c = texture ? shade(rgb(255, 255, 255), light)
                                       : shade(terrainColour(terrain), light);
                // Fertile floodplain reads a shade greener; it is what farming
                // keys off, so it is worth seeing - but only just.
                if (!texture && (terrain == sim::Terrain::Grass || terrain == sim::Terrain::Dirt))
                    c.g = std::min(1.0f, c.g + static_cast<float>(tile.fertility.toDouble()) * 0.06f);
                const std::array<float, 4> lit = cornerLight(w, p);
                if (texture) addTexturedTile(cam, p, c, mayTurn, 1.0f, -1, &lit);
                else addTile(cam, p, shade(c, (lit[0] + lit[1] + lit[2] + lit[3]) * 0.25f));
            }
        }
        flushTiles(texture);
    }

    // Ways worn in by use, drawn over the ground they were worn into: bare
    // earth, more of it the more the way is used. Nobody laid these out - they
    // are simply where the community walks.
    // On green ground a path is bare earth; on ground that is bare already it is
    // the paler, packed stuff underfoot. Drawing earth over earth showed nothing
    // at all, and a river-valley village stands on silt, so the ways it wore in
    // were invisible exactly where they mattered.
    for (int surface = 0; surface < 2; ++surface) {
        SDL_Texture* trodden = sprite(surface == 0 ? "terrain/dirt" : "terrain/sand");
        beginTiles();
        bool any = false;
        for (std::int32_t y = view.min.y; y < view.max.y; ++y)
            for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
                const core::TilePos p{x, y};
                const auto& tile = w.map().at(p);
                if (tile.traffic < sim::kPathVisible) continue;
                if (tile.terrain == sim::Terrain::Water) continue;
                const bool bare = tile.terrain == sim::Terrain::Dirt ||
                                  tile.terrain == sim::Terrain::Sand ||
                                  tile.terrain == sim::Terrain::Rock;
                if ((surface == 1) != bare) continue;
                const float wear =
                        std::min(1.0f, static_cast<float>(tile.traffic - sim::kPathVisible) /
                                               static_cast<float>(sim::kPathTraffic));
                SDL_FColor c = shade(rgb(255, 255, 255), light);
                c.a = 0.3f + 0.55f * wear;
                any = true;
                if (trodden) addTexturedTile(cam, p, c);
                else addTile(cam, p, shade(rgb(126, 104, 78, c.a), light), 0.9f);
            }
        if (any) flushTiles(trodden);
    }

    // Filth is a second pass so it tints whatever is underneath.
    beginTiles();
    for (std::int32_t y = view.min.y; y < view.max.y; ++y)
        for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
            const auto& tile = w.map().at({x, y});
            if (tile.pollution <= core::kZero) continue;
            addTile(cam, {x, y}, rgb(90, 70, 40, std::min(0.55f, static_cast<float>(tile.pollution.toDouble()))));
        }
    flushTiles();
}

void Renderer::drawFoliage(const sim::World& w, const Camera& cam) {
    // Below five pixels a tuft is sub-pixel noise; the textured grass itself is
    // the foliage LOD there. Close up overlapping clumps make a continuous
    // meadow instead of a regular scattering of isolated plants.
    const int samples = cam.pixelsPerTile >= 24.0 ? 7 : cam.pixelsPerTile >= 12.0 ? 4
                                                                             : cam.pixelsPerTile >= 5.0 ? 2 : 0;
    if (samples == 0) return;

    constexpr std::array<const char*, 6> names{
        "foliage/grass_0", "foliage/grass_1", "foliage/grass_2",
        "foliage/grass_3", "foliage/grass_4", "foliage/grass_5"};
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());
    const float time = static_cast<float>(SDL_GetTicksNS()) * 0.000000001f;
    const float light = daylightFactor(w);

    // One geometry call per texture, irrespective of how far the grass runs.
    // Moving only the two top vertices bends each clump while its base stays
    // rooted. The shared low-frequency phase makes gusts travel across the map.
    for (int variant = 0; variant < static_cast<int>(names.size()); ++variant) {
        SDL_Texture* texture = sprite(names[variant]);
        if (!texture) continue;
        beginTiles();
        for (std::int32_t y = view.min.y; y < view.max.y; ++y) {
            for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
                const auto& tile = w.map().at({x, y});
                if (tile.terrain != sim::Terrain::Grass || tile.grass <= sim::kGrassBare) continue;

                const float abundance = std::clamp(
                    static_cast<float>(tile.grass - sim::kGrassBare) /
                        static_cast<float>(sim::kGrassFull - sim::kGrassBare),
                    0.0f, 1.0f);
                const int clumps = std::max(1, static_cast<int>(
                    std::ceil(abundance * static_cast<float>(samples))));
                for (int sample = variant; sample < clumps; sample += static_cast<int>(names.size())) {
                    const std::uint32_t random = foliageNoise(x, y, sample, 0);
                    const float jitterX = (static_cast<float>(random & 0xffu) / 255.0f - 0.5f) * 0.76f;
                    const float jitterY = (static_cast<float>((random >> 8) & 0xffu) / 255.0f - 0.5f) * 0.70f;
                    const float scale = 0.82f + static_cast<float>((random >> 16) & 0xffu) / 255.0f * 0.34f;
                    const double worldX = static_cast<double>(x) + 0.5 + jitterX;
                    const double worldY = static_cast<double>(y) + 0.5 + jitterY;
                    float sx, sy;
                    cam.worldToScreen(worldX, worldY, sx, sy);

                    const float height = static_cast<float>(cam.pixelsPerTile) * 0.58f * scale;
                    const float halfWidth = height * (0.38f +
                        static_cast<float>((random >> 24) & 0xffu) / 255.0f * 0.10f);
                    const float phase = static_cast<float>(worldX * 0.31 + worldY * 0.19) - time * 1.35f;
                    const float gust = std::sin(phase) * 0.72f + std::sin(phase * 0.43f - time * 0.91f) * 0.28f;
                    const float sway = gust * halfWidth * 0.26f;
                    const float bottom = sy + cam.tileRadiusY() * 0.32f;
                    const float top = bottom - height;
                    // Kenney's shaded cards are intentionally white masks. Tint
                    // their highlights and roots separately so they inherit the
                    // meadow's green while retaining the painted shading.
                    const float variation = static_cast<float>((random >> 20) & 0x0fu) / 15.0f;
                    const float alpha = 0.72f + abundance * 0.28f;
                    const SDL_FColor topColour{
                        (0.36f + variation * 0.07f) * light,
                        (0.61f + variation * 0.09f) * light,
                        (0.18f + variation * 0.05f) * light,
                        alpha};
                    const SDL_FColor bottomColour{
                        (0.20f + variation * 0.04f) * light,
                        (0.39f + variation * 0.07f) * light,
                        (0.09f + variation * 0.03f) * light,
                        alpha};

                    const auto base = static_cast<int>(vertices_.size());
                    vertices_.push_back({{sx - halfWidth + sway, top}, topColour, {0.0f, 0.0f}});
                    vertices_.push_back({{sx + halfWidth + sway, top}, topColour, {1.0f, 0.0f}});
                    vertices_.push_back({{sx + halfWidth * 0.76f, bottom}, bottomColour, {1.0f, 1.0f}});
                    vertices_.push_back({{sx - halfWidth * 0.76f, bottom}, bottomColour, {0.0f, 1.0f}});
                    for (int index : {0, 1, 2, 0, 2, 3}) indices_.push_back(base + index);
                }
            }
        }
        flushTiles(texture);
    }
}

// Which shore card a land tile wants, and how far to turn it. The cards are
// drawn with the water at the north (and, for an outer corner, at the north and
// east; for an inner corner, only in the north-east). Everything else is that
// same card turned - which is why the pieces are three and not fifteen.
namespace {

const char* shoreLandName(sim::Terrain t) {
    switch (t) {
        case sim::Terrain::Sand:  return "sand";
        case sim::Terrain::Marsh: return "marsh";
        case sim::Terrain::Rock:  return "rock";
        // Grass and forest both meet the water as an earth bank.
        default:                   return "dirt";
    }
}

// north, east, south, west - the order the turns go in.
bool waterAt(const sim::World& w, core::TilePos t, int dx, int dy) {
    const core::TilePos p{t.x + dx, t.y + dy};
    if (!w.map().inBounds(p)) return false;
    return w.map().at(p).terrain == sim::Terrain::Water;
}

} // namespace

void Renderer::drawShores(const sim::World& w, const Camera& cam) {
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());
    const float light = daylightFactor(w);
    const SDL_FColor lit = shade(rgb(255, 255, 255), light);

    // One batch per card, because a geometry call carries one texture.
    struct Piece { std::string name; std::vector<std::pair<core::TilePos, int>> at; };
    std::vector<Piece> pieces;
    const auto want = [&](const std::string& name, core::TilePos t, int turn) {
        for (auto& piece : pieces)
            if (piece.name == name) { piece.at.emplace_back(t, turn); return; }
        pieces.push_back({name, {{t, turn}}});
    };

    for (std::int32_t y = view.min.y; y < view.max.y; ++y)
        for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
            const core::TilePos t{x, y};
            const auto& tile = w.map().at(t);
            if (tile.terrain == sim::Terrain::Water) continue;

            const bool north = waterAt(w, t, 0, -1);
            const bool east = waterAt(w, t, 1, 0);
            const bool south = waterAt(w, t, 0, 1);
            const bool west = waterAt(w, t, -1, 0);
            const int sides = north + east + south + west;
            const std::string land = shoreLandName(tile.terrain);

            // A turn moves the water round the card the other way from the
            // clock: the card is drawn with the water at the north, and one turn
            // takes it to the west. Getting this backwards puts a bite of water
            // into the land instead of along its edge.
            const auto edgeTurn = [&]() {
                return north ? 0 : west ? 1 : south ? 2 : 3;
            };
            if (sides == 1) {
                want("terrain/shore_" + land + "_edge", t, edgeTurn());
            } else if (sides == 2 && ((north && east) || (west && north) || (south && west) ||
                                      (east && south))) {
                // The card has water on two adjacent sides: a spit of land with
                // the water coming round it.
                const int turn = (north && east) ? 0 : (west && north) ? 1 : (south && west) ? 2 : 3;
                want("terrain/shore_" + land + "_outer", t, turn);
            } else if (sides >= 2) {
                // Water on opposite sides, or on three: no card is drawn for
                // that, and a straight shore along one of them is the one thing
                // that cannot look broken.
                want("terrain/shore_" + land + "_edge", t, edgeTurn());
            } else {
                // No wet side, but a wet corner: the water bites into the land.
                const bool ne = waterAt(w, t, 1, -1);
                const bool se = waterAt(w, t, 1, 1);
                const bool sw = waterAt(w, t, -1, 1);
                const bool nw = waterAt(w, t, -1, -1);
                if (!(ne || se || sw || nw)) continue;
                const int turn = ne ? 0 : nw ? 1 : sw ? 2 : 3;
                want("terrain/shore_" + land + "_inner", t, turn);
            }
        }

    for (const auto& piece : pieces) {
        SDL_Texture* texture = sprite(piece.name);
        if (!texture) continue;
        beginTiles();
        for (const auto& [t, turn] : piece.at) addTexturedTile(cam, t, lit, false, 1.0f, turn);
        flushTiles(texture);
    }
}

void Renderer::drawFields(const sim::World& w, const Camera& cam) {
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());
    const float light = daylightFactor(w);
    const SDL_FColor lit = shade(rgb(255, 255, 255), light);

    // Broken ground with nothing in it yet.
    SDL_Texture* bare = sprite("terrain/tilled");
    const bool turnBare = !directional_.count("terrain/tilled");
    beginTiles();
    for (std::int32_t y = view.min.y; y < view.max.y; ++y)
        for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
            const core::TilePos p{x, y};
            const auto& tile = w.map().at(p);
            if (!tile.tilled || tile.crop.valid()) continue;
            if (bare) addTexturedTile(cam, p, lit, turnBare);
            else addTile(cam, p, shade(rgb(104, 78, 52), light), 0.94f);
        }
    flushTiles(bare);

    // A sown tile is drawn with the crop's own card, which the sheets draw as a
    // whole tile - soil, furrows and what is standing in them. Drawing it as a
    // sprite on top of the soil instead gave a floating box with its own patch
    // of earth inside the field, and a just-sown tile was indistinguishable from
    // bare ground because the card at that stage is bare ground.
    //
    // One batch per crop and stage: a geometry call carries one texture, and
    // there are at most a handful of both.
    for (const auto& crop : w.db().crops()) {
        for (int stage = 0; stage < kCropStages; ++stage) {
            const std::string name = "crops/" + crop.name + "_" + std::to_string(stage);
            SDL_Texture* texture = sprite(name, "crops/" + crop.name + "_0");
            const bool turn = !directional_.count(name);
            beginTiles();
            bool any = false;
            for (std::int32_t y = view.min.y; y < view.max.y; ++y)
                for (std::int32_t x = view.min.x; x < view.max.x; ++x) {
                    const core::TilePos p{x, y};
                    const auto& tile = w.map().at(p);
                    if (tile.crop != crop.id) continue;
                    if (cropStage(tile.cropGrowth) != stage) continue;
                    any = true;
                    if (texture) {
                        addTexturedTile(cam, p, lit, turn, kCropUvInset);
                    } else {
                        const float ripeness = static_cast<float>(tile.cropGrowth.toDouble());
                        addTile(cam, p,
                                shade({0.35f + 0.45f * ripeness, 0.55f + 0.28f * ripeness, 0.20f, 1.0f},
                                      light),
                                0.94f);
                    }
                }
            if (any) flushTiles(texture);
        }
    }
}

namespace {

void strokePolygon(SDL_Renderer* sdl, const client::Camera& cam,
                   const std::vector<core::WorldPos>& polygon, SDL_FColor colour) {
    if (polygon.size() < 2) return;
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl,
                           static_cast<Uint8>(std::clamp(colour.r, 0.0f, 1.0f) * 255.0f),
                           static_cast<Uint8>(std::clamp(colour.g, 0.0f, 1.0f) * 255.0f),
                           static_cast<Uint8>(std::clamp(colour.b, 0.0f, 1.0f) * 255.0f),
                           static_cast<Uint8>(std::clamp(colour.a, 0.0f, 1.0f) * 255.0f));
    for (std::size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        cam.worldToScreen(polygon[j].x.toDouble(), polygon[j].y.toDouble(), x0, y0);
        cam.worldToScreen(polygon[i].x.toDouble(), polygon[i].y.toDouble(), x1, y1);
        SDL_RenderLine(sdl, x0, y0, x1, y1);
    }
}

} // namespace

void Renderer::drawHomeBoundary(const sim::World& w, const Camera& cam) {
    const SDL_FColor edge = rgb(118, 205, 224, 0.72f);
    for (const auto& zone : w.zones()) {
        if (!zone.alive || zone.kind != sim::ZoneKind::Settlement) continue;
        for (const auto& polygon : zone.areas) strokePolygon(sdl_, cam, polygon, edge);

        float sx, sy;
        const core::WorldPos c = zone.centreWorld();
        cam.worldToScreen(c.x.toDouble(), c.y.toDouble(), sx, sy);
        SDL_SetRenderDrawColor(sdl_, 190, 235, 244, 210);
        SDL_RenderDebugText(sdl_, sx - 16.0f, sy - 4.0f, "HOME");
    }
}

void Renderer::drawZones(const sim::World& w, const Camera& cam) {
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());

    // Working areas are filled. The settlement area is not: it is the largest of
    // them by far, and filling it turned the whole map into fog through which
    // none of the others could be read.
    beginTiles();
    for (const auto& z : w.zones()) {
        if (!z.alive || z.kind == sim::ZoneKind::Settlement) continue;
        const SDL_FColor c = zoneColour(z.kind, z.mode);
        for (core::TilePos t : z.tiles)
            if (view.contains(t)) addTile(cam, t, c);
    }
    flushTiles();

    // Every area gets an outline from the world-space shape that defined it,
    // rather than from the cache tiles it happened to touch.
    for (const auto& z : w.zones()) {
        if (!z.alive) continue;
        SDL_FColor edge = zoneColour(z.kind, z.mode);
        edge.a = 0.85f;
        for (const auto& polygon : z.areas) strokePolygon(sdl_, cam, polygon, edge);
    }
}

// How tall each kind of natural feature stands, in metres. A date palm towering
// over a reed bed is most of what tells them apart at a glance.
// How wide the thing is, in tiles. A canopy may overhang the tile it grows on;
// a thicket does not fill one.
float resourceTilesWide(content::ResourceKind kind) {
    switch (kind) {
        case content::ResourceKind::Tree:      return 1.6f;
        case content::ResourceKind::Rock:      return 1.1f;
        case content::ResourceKind::Bush:      return 0.9f;
        case content::ResourceKind::WildPlant: return 0.9f;
        case content::ResourceKind::Game:      return 1.4f;
        default:                                return 1.0f;
    }
}

void Renderer::drawResources(const sim::World& w, const Camera& cam) {
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());
    const float light = daylightFactor(w);

    for (const auto& n : w.nodes()) {
        if (!n.alive || !view.contains(n.tile)) continue;
        const auto& def = w.db().resourceNode(n.def);

        float sx, sy;
        cam.tileToScreen(n.tile, sx, sy);

        // A worked-out vein or a cut stand has its own drawing where the art
        // provides one; otherwise it is the same drawing, dimmed.
        const std::string base = "nodes/" + def.name;
        std::string wanted = base;
        SDL_FColor tint = shade(rgb(255, 255, 255), light);
        if (n.depleted) {
            if (sprite(base + "_spent")) wanted = base + "_spent";
            else tint = shade(tint, 0.55f);
        }

        float across = resourceTilesWide(def.kind);
        if (n.depleted) across *= 0.75f;
        if (cam.pixelsPerTile >= kSpriteDetailPixelsPerTile &&
            queueSprite(cam, sx, sy, wanted, base, across, tint))
            continue;

        // No art for this definition yet: a flat tile still shows it is there.
        const float scale = resourceScale(def.kind);
        SDL_FColor c = resourceColour(def.kind);
        if (n.depleted) c = shade(c, 0.45f);
        addItem({sx, sy, cam.tileRadiusX() * scale, cam.tileRadiusY() * scale, shade(c, light)});
    }
}

// How wide a building is drawn, in tiles: exactly its footprint. A radius-0
// building covers one tile and is drawn one tile across, and its own drawing
// decides how tall it stands for that width.
float buildingTilesWide(const content::BuildingDef& def) {
    return static_cast<float>(def.footprintWidth);
}


void Renderer::drawBuildings(const sim::World& w, const Camera& cam) {
    const float light = daylightFactor(w);
    const SDL_FColor lit = shade(rgb(255, 255, 255), light);

    // Buildings whose art is a ground card - a dug channel - fill their tiles
    // like terrain. Drawn as sprites they came out as small separate puddles
    // sitting on the dirt instead of a channel cut into it. They go first, so
    // everything standing is drawn over them.
    for (const auto& b : w.buildings()) {
        if (!b.alive) continue;
        const std::string name = "buildings/" + w.db().building(b.def).name;
        if (!ground_.count(name)) continue;
        if (b.state != sim::BuildState::Complete) {
            // Being dug: bare turned earth, not water. A channel that showed
            // water before it was finished read as already done.
            beginTiles();
            for (core::TilePos t : b.footprintTiles(w.db()))
                if (w.map().inBounds(t)) addTile(cam, t, shade(rgb(74, 56, 38), light), 0.92f);
            flushTiles();
            continue;
        }
        SDL_Texture* texture = sprite(name);
        if (!texture) continue;
        beginTiles();
        for (core::TilePos t : b.footprintTiles(w.db()))
            if (w.map().inBounds(t)) addTexturedTile(cam, t, lit, !directional_.count(name));
        flushTiles(texture);
    }

    // Walls autotile, so a tile needs to know what stands beside it. Gathered
    // once for the frame; a wall is a few dozen tiles, not a few thousand.
    std::set<std::uint64_t> wallTiles;
    for (const auto& b : w.buildings()) {
        if (!b.alive) continue;
        if (w.db().building(b.def).kind != content::BuildingKind::Fortification) continue;
        for (core::TilePos t : b.footprintTiles(w.db()))
            wallTiles.insert((static_cast<std::uint64_t>(static_cast<std::uint32_t>(t.x)) << 32) |
                             static_cast<std::uint32_t>(t.y));
    }
    const auto wallAt = [&](std::int32_t x, std::int32_t y) {
        return wallTiles.count((static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) |
                               static_cast<std::uint32_t>(y)) != 0;
    };

    for (const auto& b : w.buildings()) {
        if (!b.alive) continue;
        const auto& def = w.db().building(b.def);
        if (ground_.count("buildings/" + def.name)) continue;   // already drawn as ground
        const bool complete = b.state == sim::BuildState::Complete;

        // The origin is the north-west corner of the footprint, so a sprite hung
        // on it sat up and to the left of the ground it occupies. It belongs
        // centred across the width, standing on the front row.
        float sx, sy;
        cam.worldToScreen(b.origin.x + (def.footprintWidth - 1) / 2.0,
                          static_cast<double>(b.origin.y + def.footprintDepth - 1), sx, sy);

        // A site has its own drawing where the art provides one - scaffolded
        // walls, an open frame. Otherwise the finished building rises as the work
        // goes in, so a half-built house reads as half-built either way.
        const std::string base = "buildings/" + def.name;
        std::string wanted = base;
        float across = buildingTilesWide(def);
        SDL_FColor tint = shade(rgb(255, 255, 255), light);

        // A stretch of wall reads as a stretch only if each tile is drawn for
        // the neighbours it has. Drawn all the same, a citadel came out as a row
        // of separate front-on segments with gaps at every turn.
        // The gate has one drawing of its own; only the wall itself is cut
        // into pieces to be chosen between.
        if (def.kind == content::BuildingKind::Fortification && def.blocksMovement) {
            const int cardinals = (wallAt(b.origin.x, b.origin.y - 1) ? kWallNorth : 0) |
                                  (wallAt(b.origin.x + 1, b.origin.y) ? kWallEast : 0) |
                                  (wallAt(b.origin.x, b.origin.y + 1) ? kWallSouth : 0) |
                                  (wallAt(b.origin.x - 1, b.origin.y) ? kWallWest : 0);
            wanted = base + wallVariant(cardinals);
        }

        if (!complete) {
            const double done = def.workAmount > core::kZero
                                        ? std::clamp(b.workDone.toDouble() / def.workAmount.toDouble(), 0.0, 1.0)
                                        : 0.0;
            if (sprite(base + "_site")) {
                wanted = base + "_site";
            } else {
                across *= static_cast<float>(0.55 + 0.45 * done);
                tint = shade(tint, 0.8f);
            }
        }
        if (queueSprite(cam, sx, sy, wanted, base, across, tint)) continue;

        const SDL_FColor c = shade(buildingColour(def.kind, complete), light);
        for (core::TilePos t : b.footprintTiles(w.db())) {
            float tx, ty;
            cam.tileToScreen(t, tx, ty);
            addItem({tx, ty, cam.tileRadiusX() * 0.99f, cam.tileRadiusY() * 0.99f, c});
        }
    }
}

void Renderer::drawStacks(const sim::World& w, const Camera& cam) {
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());
    const float light = daylightFactor(w);

    for (const auto& s : w.stacks()) {
        if (!s.alive || s.count <= 0) continue;
        if (s.where != sim::StackWhere::Ground && s.where != sim::StackWhere::InBuilding) continue;
        const core::TilePos at = s.where == sim::StackWhere::InBuilding ? w.building(s.building).origin : s.tile;
        if (!view.contains(at)) continue;

        const auto& def = w.db().item(s.def);
        // Fullness reads off the size of the pile, the way Stronghold stacks do.
        const double fullness = std::clamp(static_cast<double>(s.count) / std::max(1, def.stackLimit), 0.18, 1.0);
        const auto scale = static_cast<float>(0.24 + 0.40 * fullness);
        float sx, sy;
        cam.tileToScreen(at, sx, sy);

        // A tool lying on the ground is that tool, where the art gives it: a
        // heap of coloured tiles told the player how much of something was
        // there and nothing about what.
        if (queueSprite(cam, sx, sy, "items/" + def.name, {},
                        static_cast<float>(0.4 + 0.3 * fullness),
                        shade(rgb(255, 255, 255), light), Fit::LongestSide))
            continue;

        const SDL_FColor c = shade(itemColour(def.category), light);
        addItem({sx, sy, cam.tileRadiusX() * scale, cam.tileRadiusY() * scale, c});
    }
}

void Renderer::drawAnimals(const sim::World& w, const Camera& cam) {
    const bool detailed = cam.pixelsPerTile >= kSpriteDetailPixelsPerTile;
    for (const auto& a : w.animals()) {
        if (!a.alive) continue;
        float sx, sy;
        const auto* motion = animalMotion(a.id);
        cam.worldToScreen(motion ? motion->x : a.pos.x.toDouble(), motion ? motion->y : a.pos.y.toDouble(), sx, sy);
        if (sx < -30 || sy < -60 || sx > cam.viewportWidth + 30 || sy > cam.viewportHeight + 30) continue;
        if (!detailed) {
            const float r = std::max(1.2f, cam.tileRadiusX() * 0.8f);
            addItem({sx, sy, r, r, shade(rgb(226, 214, 186), daylightFactor(w))});
            continue;
        }

        // Every kind is drawn as itself, in the states the simulation tracks:
        // grown or young, in fleece or shorn. Which one is on screen is what
        // tells the player that the flock is due for shearing - and a beast
        // out of condition is drawn dull and a size down, because starving is
        // the other thing worth seeing from across the map.
        const auto& def = w.db().animal(a.def);
        const bool grown = a.adult(w.db());
        const bool shearable = def.shearIntervalDays > 0;
        const bool inFleece = shearable && w.tickCount() >= a.nextShearTick;
        const bool thin = a.condition < core::Fixed::ratio(1, 2);

        // Which way it is standing. The sheets draw every beast from the front,
        // from the side and from behind - the same three views the pawn rig
        // uses - so a herd walking towards the view is drawn walking towards
        // the view, and the side drawing is turned round for one heading west.
        const int facing = motion ? motion->facingFrame : 1;
        const std::string view = facing == 0 ? "_front" : facing == 2 ? "_back" : "";

        // Most specific drawing the sheet holds for it, walking back to the
        // kind itself: a boar is one boar at every age, a sheep is four.
        const std::string kind = "animals/" + def.name;
        const std::string age = grown ? "" : "_young";
        const std::string fleece = (shearable && !inFleece) ? "_shorn" : "";
        std::string name = kind;
        for (const std::string& candidate : {kind + age + fleece + view, kind + age + view,
                                             kind + view, kind + age + fleece, kind + age}) {
            if (sprite(candidate)) { name = candidate; break; }
        }

        const float width = static_cast<float>(def.sizeMetres.toDouble()) * (grown ? 1.0f : 0.66f) *
                            (thin ? 0.9f : 1.0f);
        const SDL_FColor tint = shade(thin ? rgb(206, 196, 178) : rgb(255, 255, 255),
                                      daylightFactor(w));
        if (queueSprite(cam, sx, sy, name, kind, width, tint, Fit::ByWidth,
                        view.empty() && motion && motion->flipHorizontal))
            continue;

        const float scale = 0.5f * width;
        const SDL_FColor c = thin ? rgb(190, 170, 150) : rgb(238, 234, 226);
        addItem({sx, sy, cam.tileRadiusX() * scale, cam.tileRadiusY() * scale, c});
    }
}

void Renderer::drawJobLines(const sim::World& w, const Camera& cam) {
    SDL_SetRenderDrawColor(sdl_, 240, 240, 200, 90);
    for (const auto& p : w.people()) {
        if (!p.alive || !p.job.valid()) continue;
        float px, py, tx, ty;
        const auto* motion = personMotion(p.id);
        cam.worldToScreen(motion ? motion->x : p.pos.x.toDouble(), motion ? motion->y : p.pos.y.toDouble(), px, py);
        cam.tileToScreen(p.job.target, tx, ty);
        SDL_RenderLine(sdl_, px, py, tx, ty);
    }
}

void Renderer::drawStackLabels(const sim::World& w, const Camera& cam) {
    if (cam.pixelsPerTile < 26.0) return;
    const auto view = cam.visibleTiles(w.map().width(), w.map().height());

    for (const auto& s : w.stacks()) {
        if (!s.alive || s.count <= 1) continue;
        if (s.where != sim::StackWhere::Ground && s.where != sim::StackWhere::InBuilding) continue;
        const core::TilePos at = s.where == sim::StackWhere::InBuilding ? w.building(s.building).origin : s.tile;
        if (!view.contains(at)) continue;

        float sx, sy;
        cam.tileToScreen(at, sx, sy);
        const std::string label = std::to_string(s.count);
        const float w0 = label.size() * 4.0f;
        SDL_SetRenderDrawColor(sdl_, 20, 18, 14, 220);
        SDL_RenderDebugText(sdl_, sx - w0 + 1, sy - 3, label.c_str());
        SDL_SetRenderDrawColor(sdl_, 250, 246, 232, 255);
        SDL_RenderDebugText(sdl_, sx - w0, sy - 4, label.c_str());
    }
}

void Renderer::drawPeople(const sim::World& w, const Camera& cam) {
    // Far enough out, a person is a mark. Drawing the five-layer rig at three
    // pixels a tile costs the same as drawing it at forty and shows nothing.
    const bool detailed = cam.pixelsPerTile >= kSpriteDetailPixelsPerTile;
    for (const auto& p : w.people()) {
        if (!p.alive) continue;
        if (!detailed) {
            float mx, my;
            const auto* motion = personMotion(p.id);
            if (motion) cam.worldToScreen(motion->x, motion->y, mx, my);
            else cam.tileToScreen(p.tile, mx, my);
            const float r = std::max(1.5f, cam.tileRadiusX() * 0.9f);
            addItem({mx, my, r, r, shade(rgb(238, 226, 196), daylightFactor(w))});
            continue;
        }
        float sx, sy;
        const auto* motion = personMotion(p.id);
        cam.worldToScreen(motion ? motion->x : p.pos.x.toDouble(), motion ? motion->y : p.pos.y.toDouble(), sx, sy);
        if (sx < -30 || sy < -80 || sx > cam.viewportWidth + 30 || sy > cam.viewportHeight + 30) continue;

        const bool child = p.stage == sim::LifeStage::Child;
        SDL_FColor c = rgb(236, 218, 194);
        if (p.stage == sim::LifeStage::Elder) c = rgb(210, 204, 200);
        if (p.asleep) c = rgb(150, 150, 178);
        // Health and hunger tint the body, so a community in trouble looks like it.
        if (p.satiety < core::Fixed::ratio(1, 4)) c = rgb(226, 152, 92);
        if (p.health < core::Fixed::ratio(1, 2)) c = rgb(208, 98, 98);

        const int frame = motion ? motion->facingFrame : 0;
        const SDL_FColor skin = shade(c, daylightFactor(w));
        const SDL_FColor cloth = shade(rgb(255, 255, 255), daylightFactor(w));
        if (p.settlement.valid() &&
            !sim::insideZoneOfKind(w, p.settlement, sim::ZoneKind::Settlement, p.tile)) {
            // Home is a preferred safe centre, not a prison. This makes workers
            // and the two permitted scouts outside it visible at a glance.
            addItem({sx, sy - 0.01f, cam.tileRadiusX() * 0.62f, cam.tileRadiusY() * 0.62f,
                     rgb(230, 116, 64, 0.48f)});
        }

        // Every layer of the rig goes into the same rectangle. The atlases share
        // one pivot, so alignment is a property of the art rather than something
        // this code reconstructs: the previous pass measured rectangles out of an
        // unrigged component sheet and scaled each part to the body's height
        // independently, which put a head on a stretched skirt with no torso
        // between them.
        const int ageRow = child ? 0 : p.stage == sim::LifeStage::Elder ? 2 : 1;
        const SDL_FRect cell{static_cast<float>(frame) * kPawnCell,
                             static_cast<float>(ageRow) * kPawnCell, kPawnCell, kPawnCell};
        const float side = static_cast<float>(cam.pixelsPerTile) * kPawnCellInTiles;
        const SDL_FRect destination{sx - side * 0.5f, sy - side * kPawnPivotY, side, side};

        DrawItem sprite;
        sprite.cx = sx;
        sprite.cy = sy;
        sprite.colour = skin;
        sprite.flipHorizontal = motion && motion->flipHorizontal;

        // One garment slot; the fuller thing worn is what shows. Which drawing
        // goes with which item is the rig's own mapping, so a loincloth under a
        // cloak is the cloak that is seen.
        SDL_Texture* garment = nullptr;
        bool onlyAKilt = true;
        for (core::ItemStackId id : p.worn) {
            if (!id.valid() || id.value >= w.stacks().size() || !w.stack(id).alive) continue;
            const auto worn = garmentOfItem_.find(w.db().item(w.stack(id).def).name);
            if (worn == garmentOfItem_.end()) continue;
            const auto layer = garmentLayers_.find(worn->second);
            if (layer == garmentLayers_.end()) continue;
            const bool kilt = worn->second == "garment_kilt";
            if (!garment || (onlyAKilt && !kilt)) {
                garment = layer->second;
                onlyAKilt = kilt;
            }
        }

        const auto push = [&](SDL_Texture* texture, SDL_FColor tint) {
            if (texture && sprite.layerCount < sprite.layers.size())
                sprite.layers[sprite.layerCount++] = {texture, tint, cell, destination};
        };
        // Arms over the body and over the garment - they are in front of the
        // chest, not behind it - and the head last of all, except from behind,
        // where the head is the furthest thing away.
        if (frame == 2) {
            push(pawnLayers_[kHead], skin);
            push(pawnLayers_[kBody], skin);
            push(garment, cloth);
            push(pawnLayers_[kArms], skin);
        } else {
            push(pawnLayers_[kBody], skin);
            push(garment, cloth);
            push(pawnLayers_[kArms], skin);
            push(pawnLayers_[kHead], skin);
        }

        // What is in the hands. The rig has arms for exactly this: a pawn on the
        // way to the field with a hoe should be carrying a hoe. Drawn last, so
        // it is in front of the figure, at the hand on the side it faces.
        if (p.equippedTool.valid() && p.equippedTool.value < w.stacks().size()) {
            const auto& held = w.stack(p.equippedTool);
            if (held.alive) {
                if (SDL_Texture* tool = Renderer::sprite("items/" + w.db().item(held.def).name)) {
                    float tw = 0.0f, th = 0.0f;
                    SDL_GetTextureSize(tool, &tw, &th);
                    if (tw > 0.0f && th > 0.0f) {
                        // Sized against the figure rather than the tile: a tool
                        // is about a third of a person long.
                        const float longest = side * kHeldToolOfFigure;
                        const float hw = th > tw ? longest * tw / th : longest;
                        const float hh = th > tw ? longest : longest * th / tw;
                        const float hand = sprite.flipHorizontal ? -kHeldToolHandX : kHeldToolHandX;
                        const SDL_FRect where{sx + side * hand - hw * 0.5f,
                                              sy - side * kHeldToolHandY - hh * 0.5f, hw, hh};
                        if (sprite.layerCount < sprite.layers.size())
                            sprite.layers[sprite.layerCount++] = {tool, cloth, {}, where};
                    }
                }
            }
        }

        addItem(sprite);
    }
}


void Renderer::drawSelection(const sim::World& w, const Camera& cam, const Selection& sel) {
    if (!sel.valid()) return;
    const SDL_FColor gold = rgb(255, 236, 130);
    const SDL_FColor dark = rgb(20, 18, 14, 0.85f);

    switch (sel.kind) {
        case SelectionKind::Building: {
            if (!sel.building.valid() || sel.building.value >= w.buildings().size()) return;
            for (core::TilePos t : w.buildings()[sel.building.value].footprintTiles(w.db())) {
                strokeTile(cam, t, dark, 1.02f);
                strokeTile(cam, t, gold, 0.98f);
            }
            return;
        }
        case SelectionKind::Person:
        case SelectionKind::Animal: {
            // Bodies stand between tiles, so the marker follows the body.
            double wx = 0.0, wy = 0.0;
            if (sel.kind == SelectionKind::Person && sel.person.value < w.people().size()) {
                const auto& person = w.people()[sel.person.value];
                const auto* motion = personMotion(person.id);
                wx = motion ? motion->x : person.pos.x.toDouble();
                wy = motion ? motion->y : person.pos.y.toDouble();
            } else if (sel.kind == SelectionKind::Animal && sel.animal.value < w.animals().size()) {
                const auto& animal = w.animals()[sel.animal.value];
                const auto* motion = animalMotion(animal.id);
                wx = motion ? motion->x : animal.pos.x.toDouble();
                wy = motion ? motion->y : animal.pos.y.toDouble();
            }
            float sx, sy;
            cam.worldToScreen(wx, wy, sx, sy);
            const float rx = cam.tileRadiusX() * 0.9f;
            const float ry = cam.tileRadiusY() * 0.9f;
            // A ring on the ground under the figure, not a box around it: the
            // figure now stands above its own tile.
            SDL_SetRenderDrawColorFloat(sdl_, gold.r, gold.g, gold.b, 1.0f);
            for (int i = 0; i < kTileCorners; ++i) {
                const int j = (i + 1) % kTileCorners;
                SDL_RenderLine(sdl_, sx + kCornerX[i] * rx, sy + kCornerY[i] * ry,
                               sx + kCornerX[j] * rx, sy + kCornerY[j] * ry);
            }
            return;
        }
        default:
            strokeTile(cam, sel.tile, dark, 1.02f);
            strokeTile(cam, sel.tile, gold, 0.98f);
            return;
    }
}

void Renderer::drawBrush(const Camera& cam, const BrushPreview& brush, const sim::World& w) {
    if (!brush.active) return;
    const std::vector<core::WorldPos> preview =
            core::discOutline(brush.centre, core::Fixed::fromInt(brush.radius) + core::Fixed::ratio(1, 2));
    beginTiles();
    const core::TilePos around = core::toTile(brush.centre);
    for (core::TilePos t : core::tilesWithin(around, brush.radius + 1)) {
        if (!w.map().inBounds(t)) continue;
        if (!core::polygonIntersectsRect(preview, core::tileBounds(t))) continue;
        addTile(cam, t, brush.colour);
    }
    flushTiles();
    for (core::TilePos t : core::tilesWithin(around, brush.radius + 1)) {
        if (!w.map().inBounds(t)) continue;
        if (!core::polygonIntersectsRect(preview, core::tileBounds(t))) continue;
        strokeTile(cam, t, brush.erasing ? rgb(240, 120, 120) : rgb(240, 240, 200, 0.8f));
    }
    strokePolygon(sdl_, cam, preview, brush.erasing ? rgb(240, 120, 120, 0.95f)
                                                     : rgb(240, 240, 200, 0.95f));
}


// The colour of each country. Chosen to be told apart at a glance the way an
// atlas is read: ice white, tundra a brown-grey, taiga dark, temperate forest
// green, steppe the colour of dry grass, mediterranean an olive, desert sand,
// savanna straw, tropics a deep green, alpine bare rock, river valleys and
// deltas the green of watered ground.
SDL_FColor climateColour(generation::Climate c) {
    switch (c) {
        case generation::Climate::Ice:             return rgb(232, 236, 240);
        case generation::Climate::Tundra:          return rgb(150, 148, 128);
        case generation::Climate::Taiga:           return rgb(62, 88, 68);
        case generation::Climate::TemperateForest: return rgb(84, 118, 66);
        case generation::Climate::Steppe:          return rgb(164, 158, 96);
        case generation::Climate::Mediterranean:   return rgb(134, 142, 78);
        case generation::Climate::Desert:          return rgb(214, 190, 138);
        case generation::Climate::Savanna:         return rgb(186, 172, 96);
        case generation::Climate::TropicalForest:  return rgb(48, 104, 56);
        case generation::Climate::Alpine:          return rgb(146, 138, 128);
        case generation::Climate::RiverValley:     return rgb(112, 140, 74);
        case generation::Climate::Delta:           return rgb(96, 138, 70);
        case generation::Climate::Count:           break;
    }
    return rgb(104, 134, 76);
}

SDL_Texture* Renderer::worldTexture(const sim::World& w) {
    const auto& world = w.worldMap();
    // Keyed on what the map is rather than on where it lives: looking at a
    // neighbour builds a second World with a copy of the same country in it,
    // and repainting a million cells for an identical picture would show as a
    // hitch every time the player steps in or out of somebody else's valley.
    const std::uint64_t key = world.seed ^ (std::uint64_t(std::uint32_t(world.width)) << 32) ^
                              std::uint32_t(world.height);
    if (worldMapTexture_ != nullptr && worldMapKey_ == key) return worldMapTexture_;
    if (worldMapTexture_ != nullptr) {
        SDL_DestroyTexture(worldMapTexture_);
        worldMapTexture_ = nullptr;
    }
    // Belt to the brace of holding the country at a stable address (D104): a
    // map whose cells do not match its size is not a map, and painting a million
    // pixels from one is how a dangling pointer became a crash rather than a
    // wrong picture.
    if (world.width <= 0 || world.height <= 0) return nullptr;
    if (world.cells.size() != static_cast<std::size_t>(world.width) * world.height) return nullptr;

    std::vector<std::uint32_t> pixels(world.cells.size(), 0);
    const auto heightAt = [&](std::int32_t x, std::int32_t y) {
        const core::TilePos p{std::clamp(x, 0, world.width - 1), std::clamp(y, 0, world.height - 1)};
        const auto& c = world.at(p);
        return static_cast<float>(c.sea ? 0 : c.elevation);
    };
    // How far out to sea a cell is: used to shade the shelf. A coast that goes
    // from beach to abyss in one cell is one of the things that made the world
    // look like flat pieces laid side by side. Grown outwards from the land in
    // one pass - asking each cell to look around itself instead is a couple of
    // billion reads on a world this size.
    std::vector<std::uint8_t> fromLand(world.cells.size(), 255);
    {
        std::vector<std::int32_t> wave;
        wave.reserve(world.cells.size() / 4);
        for (std::int32_t y = 0; y < world.height; ++y)
            for (std::int32_t x = 0; x < world.width; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * world.width + x;
                if (world.cells[i].sea) continue;
                fromLand[i] = 0;
                wave.push_back(static_cast<std::int32_t>(i));
            }
        std::size_t head = 0;
        while (head < wave.size()) {
            const std::int32_t index = wave[head++];
            const std::uint8_t step = fromLand[static_cast<std::size_t>(index)];
            if (step >= 20) continue;
            const core::TilePos p{index % world.width, index / world.width};
            for (int dir : core::kCardinalDirections) {
                const core::TilePos n = core::neighbour(p, dir);
                if (!world.inBounds(n)) continue;
                const std::size_t j = static_cast<std::size_t>(n.y) * world.width + n.x;
                if (fromLand[j] <= step + 1) continue;
                fromLand[j] = static_cast<std::uint8_t>(step + 1);
                wave.push_back(static_cast<std::int32_t>(j));
            }
        }
    }
    const auto mix = [](SDL_FColor a, SDL_FColor b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return SDL_FColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.0f};
    };

    for (std::int32_t cy = 0; cy < world.height; ++cy) {
        for (std::int32_t cx = 0; cx < world.width; ++cx) {
            const std::size_t index = static_cast<std::size_t>(cy) * world.width + cx;
            const auto& c = world.cells[index];
            SDL_FColor colour;
            if (c.sea) {
                // Shelf, then slope, then deep water: depth read off how far the
                // cell is from the nearest shore.
                const float out = std::clamp(static_cast<float>(fromLand[index]) / 14.0f, 0.0f, 1.0f);
                colour = mix(rgb(62, 110, 146), rgb(20, 46, 84), out);
            } else {
                // Painted by what country this is (D96). Colouring straight
                // from a rain figure gave a map that could not say "desert" or
                // "taiga" - only "wetter here than there" - and a world whose
                // parts cannot be named is a world nobody can be placed in.
                colour = climateColour(c.climate);
                // Height still shows through, so ranges read as ranges and the
                // tops of them go bare and then white.
                const float h = static_cast<float>(c.elevation) / 255.0f;
                colour = mix(colour, rgb(150, 142, 132), std::clamp((h - 0.55f) * 1.8f, 0.0f, 1.0f));
                colour = mix(colour, rgb(240, 242, 246), std::clamp((h - 0.82f) * 4.0f, 0.0f, 1.0f));
                // And a river is drawn as water, whatever country it crosses.
                if (c.river) colour = mix(colour, rgb(72, 112, 152), 0.75f);
            }

            // Hill shading over eight neighbours rather than two: with two, the
            // light is computed from a pair of steps and the ground comes out
            // faceted - which is exactly the "someone laid tiles down" look.
            float lit = 1.0f;
            if (!c.sea) {
                const float dzdx = (heightAt(cx + 1, cy - 1) + 2 * heightAt(cx + 1, cy) +
                                    heightAt(cx + 1, cy + 1)) -
                                   (heightAt(cx - 1, cy - 1) + 2 * heightAt(cx - 1, cy) +
                                    heightAt(cx - 1, cy + 1));
                const float dzdy = (heightAt(cx - 1, cy + 1) + 2 * heightAt(cx, cy + 1) +
                                    heightAt(cx + 1, cy + 1)) -
                                   (heightAt(cx - 1, cy - 1) + 2 * heightAt(cx, cy - 1) +
                                    heightAt(cx + 1, cy - 1));
                lit = std::clamp(1.0f + (dzdx + dzdy) * 0.0045f, 0.72f, 1.26f);
            }
            const auto channel = [&](float v) {
                return static_cast<std::uint32_t>(std::clamp(v * lit, 0.0f, 1.0f) * 255.0f);
            };
            pixels[index] =
                    0xFF000000u | (channel(colour.r) << 16) | (channel(colour.g) << 8) |
                    channel(colour.b);
        }
    }

    worldMapTexture_ = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, world.width, world.height);
    if (worldMapTexture_ == nullptr) return nullptr;
    SDL_UpdateTexture(worldMapTexture_, nullptr, pixels.data(),
                      static_cast<int>(world.width * sizeof(std::uint32_t)));
    // Smoothed, because at any zoom where the whole country is on screen a cell
    // is smaller than a pixel and nearest-neighbour makes the coastline crawl
    // as the camera moves.
    SDL_SetTextureScaleMode(worldMapTexture_, SDL_SCALEMODE_LINEAR);
    worldMapKey_ = key;
    return worldMapTexture_;
}

void Renderer::drawWorldMap(const sim::World& w, const Camera& cam) {
    const auto& world = w.worldMap();
    if (world.cells.empty()) return;

    // A world cell is kMetresPerCell of ground and a tile is a metre, so this
    // is how many tiles wide a cell is drawn. The world is laid over exactly the
    // ground it covers, with the played cell under the local map, so pulling the
    // camera back walks out of the settlement and into the country around it
    // rather than cutting to a different picture.
    //
    // It used to be the local map's own width, from the day a cell was a local
    // map (D84). A cell is now half a kilometre and a local map is a piece of
    // one (D116), and taking the two as equal drew the whole continent at a
    // third of its size - which reads as a blurry green smear, because the
    // camera is then looking at four cells rather than at a country.
    const double cell = generation::kMetresPerCell;
    const double originX = -w.localBlock().x * cell;
    const double originY = -w.localBlock().y * cell;

    const float light = daylightFactor(w);
    if (SDL_Texture* texture = worldTexture(w)) {
        float x0, y0, x1, y1;
        cam.worldToScreen(originX, originY, x0, y0);
        cam.worldToScreen(originX + world.width * cell, originY + world.height * cell, x1, y1);
        const SDL_FRect destination{x0, y0, x1 - x0, y1 - y0};
        // Night barely touches it. This is a map of a country, not a view of
        // ground from above: at midnight the local map should go dark and the
        // country should still be readable.
        const float k = std::clamp(0.72f + light * 0.28f, 0.0f, 1.0f);
        SDL_SetTextureColorModFloat(texture, k, k, k);
        SDL_RenderTexture(sdl_, texture, nullptr, &destination);
    }

    // The ground the local map covers, outlined: at this distance it is a
    // square a fraction of a pixel across, so it is drawn no smaller than a
    // mark that can actually be seen.
    {
        // The ground the local map actually covers, which is its own width -
        // a piece of a cell, not a cell.
        const double side = static_cast<double>(w.map().width());
        float sx, sy;
        cam.worldToScreen(originX + (w.localBlock().x * cell) + side * 0.5,
                          originY + (w.localBlock().y * cell) + side * 0.5, sx, sy);
        const float half = std::max(3.0f, static_cast<float>(side * cam.pixelsPerTile * 0.5));
        SDL_SetRenderDrawColorFloat(sdl_, 1.0f, 0.94f, 0.55f, 0.95f);
        const SDL_FRect box{sx - half, sy - half, half * 2, half * 2};
        SDL_RenderRect(sdl_, &box);
    }

    // Every settled site is a marker sized by how many people live there, and
    // its name if there is room for one. This is the whole point of the view: at
    // this distance a settlement is a place on a map, not a hundred roofs. A
    // thousand markers all the same size is a rash; sized, the map shows where
    // the country actually is - the cities on the rivers, the specks on the
    // hills.
    const bool named = cell * cam.pixelsPerTile > 8.0;
    // Which community is being looked at right now: every one of them is
    // simulated, so "ours" is wherever the camera is (D95).
    const core::TilePos here = w.localCell();
    for (const auto& site : world.sites) {
        float sx, sy;
        cam.worldToScreen(originX + (site.cell.x + 0.5) * cell,
                          originY + (site.cell.y + 0.5) * cell, sx, sy);
        if (sx < -60 || sy < -60 || sx > cam.viewportWidth + 60 || sy > cam.viewportHeight + 60)
            continue;
        // Square root, because a town of four hundred is not ten times the mark
        // of a hamlet of forty - it is three times, the way a map draws it.
        const float people = static_cast<float>(std::max(1, site.population));
        const bool isHere = site.cell == here;
        const float r = isHere ? 5.0f : std::clamp(std::sqrt(people) * 0.7f, 3.0f, 6.0f);
        // Told apart by their people: this is the only place the map says that
        // the country has more than one nation in it.
        const SDL_FColor mark = isHere              ? rgb(255, 240, 120)
                                : site.ethnos == "sumerian" ? rgb(226, 116, 74)
                                                            : rgb(196, 84, 132);
        beginTiles();
        addQuadAt(sx, sy, r + 1.0f, r + 1.0f, rgb(24, 20, 16, 0.9f));
        addQuadAt(sx, sy, r, r, mark);
        flushTiles();
        if (!named) continue;
        const std::string label = site.name + "  " + std::to_string(site.population);
        SDL_SetRenderDrawColor(sdl_, 20, 18, 14, 220);
        SDL_RenderDebugText(sdl_, sx + r + 4, sy - 3, label.c_str());
        SDL_SetRenderDrawColor(sdl_, 250, 246, 232, 255);
        SDL_RenderDebugText(sdl_, sx + r + 3, sy - 4, label.c_str());
    }
}

void Renderer::draw(const sim::World& w, const Camera& cam, Overlay overlay, const Selection& selected,
                    const BrushPreview& brush) {
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_, 18, 20, 24, 255);
    SDL_RenderClear(sdl_);

    // Pulled back past tile scale there is nothing left to read on the ground,
    // so the camera shows the country instead: one cell to a local map, every
    // settlement a marker (D84).
    if (cam.pixelsPerTile < kWorldViewPixelsPerTile) {
        drawWorldMap(w, cam);
        return;
    }

    // The ground is flat and drawn first. Everything standing on it goes into one
    // list, gets sorted back to front, and comes out in a single call - which is
    // what makes a person walk in front of a hut rather than through it.
    drawTerrain(w, cam);
    drawFoliage(w, cam);
    drawShores(w, cam);
    drawFields(w, cam);
    drawHomeBoundary(w, cam);
    if (overlay == Overlay::Zones) drawZones(w, cam);

    items_.clear();
    drawResources(w, cam);
    drawBuildings(w, cam);
    if (cam.pixelsPerTile >= kSpriteDetailPixelsPerTile) drawStacks(w, cam);
    drawAnimals(w, cam);
    drawPeople(w, cam);
    flushItems();

    if (overlay == Overlay::Jobs) drawJobLines(w, cam);
    drawStackLabels(w, cam);
    drawSelection(w, cam, selected);
    drawBrush(cam, brush, w);
}

} // namespace client
