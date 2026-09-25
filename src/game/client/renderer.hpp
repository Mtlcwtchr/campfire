#pragma once
// Top-down tile renderer. Reads the world, never writes to it.
//
// Sprites are looked up by the name of the content definition they belong to -
// `buildings/mudbrick_house`, `nodes/date_palm`, `crops/emmer_2` - out of the
// library `assets/sprites/sprites.json` describes. Nothing about which art file
// holds which drawing lives in this code: an earlier pass hardcoded eight file
// names and eight rectangles here, keyed by render category, so a reed hut and a
// mud-brick house shared one drawing and four of the eight rectangles turned out
// to be pieces of the printed captions on the source sheets.
//
// Falling back is deliberate: a definition with no sprite is drawn as a flat
// coloured tile rather than not at all, so new content is visible before its art
// exists.

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/client/camera.hpp"
#include "game/client/selection.hpp"
#include "game/simulation/world.hpp"

namespace client {

// What extra information is painted over the world.
enum class Overlay { None, Zones, Jobs, Relief, Count };
const char* overlayName(Overlay o);

// While the player is drawing an area, the renderer shows the brush and the
// tiles it would take.
struct BrushPreview {
    bool active = false;
    core::WorldPos centre;
    std::int32_t radius = 1;
    bool erasing = false;
    SDL_FColor colour{1.0f, 1.0f, 1.0f, 0.35f};
};

class Renderer {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool init(SDL_Renderer* sdl, const std::filesystem::path& spriteDirectory);
    void shutdown();
    void updatePresentation(const sim::World& w, double frameSeconds, double requestedTicksPerSecond);
    void draw(const sim::World& w, const Camera& cam, Overlay overlay, const Selection& selected,
              const BrushPreview& brush);

    // The picture for a thing, by the name its content definition carries. The
    // HUD wants these for its cards and its inspector: a building's card shows
    // the building.
    SDL_Texture* pictureOf(const std::string& name) const { return sprite(name); }

private:
    enum PawnLayer { kBody = 0, kHead, kArms, kPawnLayerCount };

    struct SpriteLayer {
        SDL_Texture* texture = nullptr;
        SDL_FColor colour{1.0f, 1.0f, 1.0f, 1.0f};
        SDL_FRect source{};
        SDL_FRect destination{};
    };

    // A flat placeholder or sprite anchored to a point on the ground. All items
    // share one painter-ordered queue, so overlap supplies depth without 3D.
    struct DrawItem {
        float cx = 0, cy = 0;      // centre of the base, in screen pixels
        float rx = 0, ry = 0;      // base radii
        SDL_FColor colour{};
        std::array<SpriteLayer, 4> layers{};
        std::size_t layerCount = 0;
        bool flipHorizontal = false;
    };

    // --- the sprite library ---------------------------------------------
    bool loadSpriteLibrary(const std::filesystem::path& spriteDirectory);
    // The sprite for a name, or nullptr. `fallback` is tried second, which is how
    // a definition with no art of its own still gets its category's drawing.
    SDL_Texture* sprite(const std::string& name, const std::string& fallback = {}) const;
    // How a sprite's given size is read: as its width across the ground, or as
    // its longest side. A spear is three times as tall as it is wide, so sizing
    // it by width drew it two and a half tiles high.
    enum class Fit { ByWidth, LongestSide };
    // Queues a sprite standing on the ground at (sx, sy), sized in tiles, and
    // returns whether there was one to queue.
    bool queueSprite(const Camera& cam, float sx, float sy, const std::string& name,
                     const std::string& fallback, float tiles, SDL_FColor tint,
                     Fit fit = Fit::ByWidth, bool flip = false);
    // How brightly each corner of a tile is lit, given the lie of the land
    // around it. The map is drawn flat; this is what makes it read as ground
    // with a slope in it rather than a coloured plan.
    std::array<float, 4> cornerLight(const sim::World& w, core::TilePos t) const;
    // The country this map is one cell of, drawn when the camera is pulled back
    // past tile scale: heights, rivers, and every settled site a marker (D84).
    void drawWorldMap(const sim::World& w, const Camera& cam);
    // The country, baked into one texture a pixel to the cell. The world is a
    // million cells and never changes, so it is painted once and then drawn as
    // a single stretched image: a quad per cell was six million vertices a
    // frame for a picture that is identical to the last one.
    SDL_Texture* worldTexture(const sim::World& w);
    // The shoreline: one card per boundary tile, turned to face the water.
    void drawShores(const sim::World& w, const Camera& cam);

    std::unordered_map<std::string, SDL_Texture*> sprites_;
    // Ground cards whose drawing has a direction in it and so must not be turned
    // per tile - a ploughed field's furrows have to run one way.
    std::set<std::string> directional_;
    // Cards that are ground rather than something standing on it: a sown field,
    // a dug channel. They fill their tile instead of being drawn as sprites.
    std::set<std::string> ground_;
    // The rig's layers, in the order tools/build_pawn_rig.py writes them. All of
    // them share one pivot, so alignment is a property of the art.
    std::array<SDL_Texture*, kPawnLayerCount> pawnLayers_{};
    // The clothes, by the layer name the rig gives them, and which garment each
    // thing a person can wear is drawn as. Both come out of pawn.json, so a new
    // garment is a row on a sheet and a line in the rig's manifest.
    std::unordered_map<std::string, SDL_Texture*> garmentLayers_;
    std::unordered_map<std::string, std::string> garmentOfItem_;
    // The baked world map, and the world it was baked from: a client can be
    // handed a different world (a new seed) without being rebuilt.
    SDL_Texture* worldMapTexture_ = nullptr;
    std::uint64_t worldMapKey_ = 0;

    void beginTiles();
    void addTile(const Camera& cam, core::TilePos t, SDL_FColor c, float scale = 1.0f);
    void addQuadAt(float cx, float cy, float rx, float ry, SDL_FColor c);
    // Same quad, but carrying texture coordinates so a ground tile can fill it.
    void addTexturedTile(const Camera& cam, core::TilePos t, SDL_FColor c, bool mayTurn = true,
                         float uvInset = 1.0f, int turn = -1,
                         const std::array<float, 4>* cornerShade = nullptr);
    void flushTiles(SDL_Texture* texture = nullptr);
    void strokeTile(const Camera& cam, core::TilePos t, SDL_FColor c, float scale = 1.0f);
    // One edge of a tile, the one facing the given neighbour direction. An area
    // outline is made of these; outlining whole tiles draws a chain of squares.
    void strokeTileEdge(const Camera& cam, core::TilePos t, int direction, SDL_FColor c,
                       float scale = 1.0f);

    // Collected over a frame, sorted back to front, then emitted in one call so
    // painter's order comes out right without one draw per object.
    void addItem(const DrawItem& item);
    void flushItems();
    std::vector<DrawItem> items_;

    void drawTerrain(const sim::World& w, const Camera& cam);
    void drawFoliage(const sim::World& w, const Camera& cam);
    void drawFields(const sim::World& w, const Camera& cam);
    void drawHomeBoundary(const sim::World& w, const Camera& cam);
    void drawZones(const sim::World& w, const Camera& cam);
    void drawResources(const sim::World& w, const Camera& cam);
    void drawBuildings(const sim::World& w, const Camera& cam);
    void drawStacks(const sim::World& w, const Camera& cam);
    void drawAnimals(const sim::World& w, const Camera& cam);
    void drawPeople(const sim::World& w, const Camera& cam);
    void drawJobLines(const sim::World& w, const Camera& cam);
    void drawSelection(const sim::World& w, const Camera& cam, const Selection& selected);
    void drawBrush(const Camera& cam, const BrushPreview& brush, const sim::World& w);
    void drawStackLabels(const sim::World& w, const Camera& cam);

    struct MotionState {
        float x = 0, y = 0;
        float fromX = 0, fromY = 0;
        float targetX = 0, targetY = 0;
        double elapsed = 0, duration = 0;
        int facingFrame = 0;
        bool flipHorizontal = false;
        bool initialized = false;
    };

    static void updateMotion(MotionState& state, float targetX, float targetY,
                             double frameSeconds, double tickSeconds);
    const MotionState* personMotion(core::PersonId id) const;
    const MotionState* animalMotion(core::AnimalId id) const;

    SDL_Renderer* sdl_ = nullptr;
    std::vector<MotionState> peopleMotion_;
    std::vector<MotionState> animalMotion_;
    std::vector<SDL_Vertex> vertices_;
    std::vector<int> indices_;
};

} // namespace client
