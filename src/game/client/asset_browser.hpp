#pragma once
// A window for choosing what the ground is made of from what the build has:
// every packed ground texture on disk (assets/terrain/**/packed.json, whether
// the ground's texture list names it yet or not), every scene model
// (assets/generated/scene_models/manifest.json) and every entry of the terrain
// libraries (content/config/terrain: soils, rocks, ground cover, plants,
// props, decals, forest, water and decor biomes). Each shows a picture where
// the build has one: a texture's albedo, a model's baked impostor, a library
// entry through the texture or model it stands on.
//
// The Ground tab (terrain_panel.hpp) opens it for one choice and is told what
// was chosen; the window itself writes nothing.
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/ui/ui.hpp"

struct SDL_Texture;
struct SDL_Renderer;

namespace client {

enum class AssetKind : std::uint8_t { Texture, Model, Soil, Rock, Foliage, Plant, Prop, Decal, Forest, Water, Decor };
inline constexpr std::size_t kAssetKinds = 11;
inline constexpr const char* kAssetKindLabels[kAssetKinds] = {
        "Textures", "Models", "Soils", "Rocks", "Ground cover", "Plants", "Props", "Decals", "Forests", "Waters", "Decor"};

struct AssetItem {
    AssetKind kind = AssetKind::Texture;
    std::string name;                 // what a config names it by
    std::string group;                // a texture's folder, a model's role, a library's id...
    std::string detail;               // a line or two about it
    std::filesystem::path thumbnail;  // empty: a swatch of `swatch`
    std::array<float, 3> swatch{0.45f, 0.45f, 0.45f};
    // Textures only: the packed stem relative to assets/, the ground one turn
    // covers, and whether layers.json names it yet (choosing one that it does
    // not adds it - a new layer of the texture array, one shader rebuild).
    std::string stem;
    double metres = 2.0;
    bool registered = true;
};

class AssetCatalog {
public:
    // Reads the build and the terrain libraries again.
    void refresh();
    [[nodiscard]] const std::vector<AssetItem>& items(AssetKind kind) const { return items_[std::size_t(kind)]; }
    [[nodiscard]] const AssetItem* find(AssetKind kind, const std::string& name) const;
    [[nodiscard]] static std::filesystem::path assetsDirectory();

private:
    void scanTextures(const std::filesystem::path& assets);
    void scanModels(const std::filesystem::path& assets);
    void readLibraries();
    std::array<std::vector<AssetItem>, kAssetKinds> items_;
};

class AssetBrowser {
public:
    // What one choice offers. `extras` are choices that are not assets -
    // "(inherit)", "none" - shown as buttons; `apply` is handed the asset
    // chosen (a copy), or null and the index of the extra.
    struct Request {
        std::string title;
        std::vector<AssetKind> kinds;
        std::string current;
        std::vector<std::string> extras;
        std::function<void(const AssetItem* item, int extra)> apply;
    };

    void open(Request request, const AssetCatalog& catalog);
    void close() { open_ = false; request_ = {}; }
    [[nodiscard]] bool isOpen() const { return open_; }
    [[nodiscard]] const std::string& title() const { return request_.title; }

    // The window, inside `area` (the screen beside the edit panel). Calls
    // `apply` and closes when a choice is made.
    void draw(ui::Ui& ui, const ui::Rect& area);

private:
    SDL_Texture* thumbnail(ui::Ui& ui, const std::filesystem::path& path);
    void choose(const AssetItem* item, int extra);
    [[nodiscard]] std::vector<const AssetItem*> shown() const;

    bool open_ = false;
    Request request_;
    const AssetCatalog* catalog_ = nullptr;
    std::size_t tab_ = 0;
    std::string search_;
    std::string selected_;            // name of the selected item on this tab
    // Pictures, loaded a few a frame so a first open does not stall.
    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<std::string, SDL_Texture*> pictures_;
    int loadsThisFrame_ = 0;
};

} // namespace client

