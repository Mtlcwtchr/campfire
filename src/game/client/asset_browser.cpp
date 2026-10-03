#include "game/client/asset_browser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <optional>
#include <set>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <nlohmann/json.hpp>

#include "engine/biomes/registry.hpp"

namespace client {
namespace fs = std::filesystem;
namespace eb = engine::biomes;
using Json = nlohmann::json;

namespace {

std::string format(const char* pattern, double a, double b = 0.0) {
    char text[96];
    std::snprintf(text, sizeof text, pattern, a, b);
    return text;
}

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool endsWith(const std::string& s, const std::string& tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

Json readJson(const fs::path& path) {
    std::ifstream in(path);
    return in ? Json::parse(in, nullptr, false) : Json();
}

std::array<float, 3> swatchOf(const eb::Rgb& rgb, std::array<float, 3> base) {
    return {float(rgb[0]) * base[0], float(rgb[1]) * base[1], float(rgb[2]) * base[2]};
}

// The names of a weighted list, the heaviest first, as "a, b, c".
std::string names(const eb::Weighted& list, std::size_t most = 3) {
    auto sorted = list;
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string out;
    for (std::size_t i = 0; i < sorted.size() && i < most; ++i) out += (i ? ", " : "") + sorted[i].first;
    if (sorted.size() > most) out += ", ...";
    return out.empty() ? std::string("none") : out;
}

AssetItem makeItem(AssetKind kind, std::string name, std::string group = {}) {
    AssetItem item;
    item.kind = kind;
    item.name = std::move(name);
    item.group = std::move(group);
    return item;
}

std::string heaviest(const eb::Weighted& list) {
    const auto best = std::max_element(list.begin(), list.end(),
                                       [](const auto& a, const auto& b) { return a.second < b.second; });
    return best == list.end() ? std::string() : best->first;
}

} // namespace

// --- the catalogue ------------------------------------------------------------

fs::path AssetCatalog::assetsDirectory() {
    // content/config/terrain -> the project's root -> assets.
    return eb::defaultDirectory().parent_path().parent_path().parent_path() / "assets";
}

const AssetItem* AssetCatalog::find(AssetKind kind, const std::string& name) const {
    for (const auto& item : items_[std::size_t(kind)])
        if (item.name == name) return &item;
    return nullptr;
}

void AssetCatalog::refresh() {
    for (auto& list : items_) list.clear();
    const auto assets = assetsDirectory();
    scanTextures(assets);
    scanModels(assets);
    readLibraries();
}

void AssetCatalog::scanTextures(const fs::path& assets) {
    const auto registry = eb::active();
    // The texture list's own names, by the stem each one loads.
    std::vector<std::pair<std::string, const eb::TextureLayer*>> listed;
    std::set<std::string> taken;
    if (registry)
        for (const auto& layer : registry->textureLayers()) {
            listed.emplace_back(layer.path.empty() ? "terrain/ph/" + layer.name + "/ph_" + layer.name : layer.path, &layer);
            taken.insert(layer.name);
        }
    auto& out = items_[std::size_t(AssetKind::Texture)];
    const auto terrain = assets / "terrain";
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(terrain, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (it->path().filename() != "packed.json") continue;
        const auto dir = it->path().parent_path();
        const auto relative = fs::relative(dir, terrain, ec).generic_string();
        // Water normals and foam are the water's, not ground.
        if (relative.rfind("water", 0) == 0 || relative.find("/src") != std::string::npos) continue;
        const Json packed = readJson(it->path());
        if (!packed.is_object()) continue;
        std::string albedo;
        if (packed.contains("runtime_files") && packed["runtime_files"].is_object())
            albedo = packed["runtime_files"].value("albedo", std::string());
        if (!endsWith(albedo, "_albedo.png") || !fs::exists(dir / albedo, ec)) continue;
        const std::string file = albedo.substr(0, albedo.size() - std::string("_albedo.png").size());
        AssetItem item;
        item.kind = AssetKind::Texture;
        item.stem = "terrain/" + relative + "/" + file;
        item.group = relative.substr(0, relative.find('/'));
        for (const char* picture : {"_albedo@8.png", "_albedo@16.png", "_albedo@4.png", "_albedo.png"})
            if (fs::exists(dir / (file + picture), ec)) {
                item.thumbnail = dir / (file + picture);
                break;
            }
        item.metres = packed.value("world_scale", 0.0);
        if (item.metres <= 0 && packed.contains("source") && packed["source"].is_object())
            item.metres = packed["source"].value("runtime_world_scale", 0.0);
        if (item.metres <= 0) item.metres = 2.0;
        std::string role;
        const auto known = std::find_if(listed.begin(), listed.end(), [&](const auto& l) { return l.first == item.stem; });
        if (known != listed.end()) {
            item.name = known->second->name;
            item.metres = known->second->metres;
            role = known->second->role;
            item.registered = true;
        } else {
            std::string id = packed.value("id", file);
            if (id.rfind("ph_", 0) == 0) id = id.substr(3);
            std::string name = id;
            for (int n = 2; taken.count(name); ++n) name = id + "_" + std::to_string(n);
            taken.insert(name);
            item.name = name;
            item.registered = false;
        }
        item.detail = item.stem + "\n" + format("%.2f m a turn", item.metres) +
                      (item.registered ? (role.empty() ? std::string() : "\n" + role)
                                       : std::string("\nnot in the ground's texture list yet"));
        out.push_back(std::move(item));
    }
    std::sort(out.begin(), out.end(), [](const AssetItem& a, const AssetItem& b) {
        return a.group != b.group ? a.group < b.group : a.name < b.name;
    });
}

void AssetCatalog::scanModels(const fs::path& assets) {
    const auto dir = assets / "generated" / "scene_models";
    const Json manifest = readJson(dir / "manifest.json");
    if (!manifest.is_object() || !manifest.contains("models") || !manifest["models"].is_array()) return;
    const Json* colours = manifest.contains("colours") && manifest["colours"].is_array() ? &manifest["colours"] : nullptr;
    auto& out = items_[std::size_t(AssetKind::Model)];
    std::error_code ec;
    for (const auto& m : manifest["models"]) {
        if (!m.is_object()) continue;
        AssetItem item;
        item.kind = AssetKind::Model;
        item.name = m.value("name", std::string());
        if (item.name.empty()) continue;
        const bool vegetation = m.value("vegetation", false);
        item.group = vegetation ? "vegetation" : "solid";
        // The baked impostor's front view; failing that, its card sheet.
        const int view = m.contains("impostor") && m["impostor"].is_number_integer() ? m["impostor"].get<int>() : -1;
        std::vector<fs::path> pictures;
        if (view >= 0 && colours && std::size_t(view) < colours->size() && (*colours)[std::size_t(view)].is_string())
            pictures.push_back(dir / (*colours)[std::size_t(view)].get<std::string>());
        if (view >= 0) pictures.push_back(dir / ("colour-" + std::to_string(view) + ".png"));
        pictures.push_back(dir / (item.name + "-sheet.png"));
        for (const auto& p : pictures)
            if (fs::exists(p, ec)) {
                item.thumbnail = p;
                break;
            }
        const std::string lod = m.value("lod", std::string(m.contains("levels") ? "clusters" : "single"));
        item.detail = format("%.1f m wide, %.1f m high", m.value("width", 0.0), m.value("height", 0.0)) + "\n" +
                      std::to_string(m.value("triangles", 0)) + " triangles, distance: " + lod +
                      (vegetation ? "\na tree, shrub or plant" : "\na solid prop");
        out.push_back(std::move(item));
    }
}

void AssetCatalog::readLibraries() {
    const auto registry = eb::active();
    if (!registry) return;
    const auto textureThumb = [&](const std::string& layer) {
        const AssetItem* t = find(AssetKind::Texture, layer);
        return t ? t->thumbnail : fs::path();
    };
    const auto modelThumb = [&](const std::string& model) {
        const AssetItem* m = find(AssetKind::Model, model);
        return m ? m->thumbnail : fs::path();
    };
    for (const auto& s : registry->soils()) {
        AssetItem item = makeItem(AssetKind::Soil, s.name);
        item.thumbnail = textureThumb(heaviest(s.layers));
        item.swatch = swatchOf(s.tint, {0.42f, 0.36f, 0.28f});
        std::string layers;
        for (const auto& [layer, share] : s.layers)
            layers += (layers.empty() ? "" : ", ") + layer + format(" %.0f %%", share * 100.0);
        item.detail = layers + "\nmixed by " + eb::kNoiseKindNames[std::size_t(s.noise.kind)] +
                      format(", patches %.1f m", s.noise.metres);
        items_[std::size_t(AssetKind::Soil)].push_back(std::move(item));
    }
    for (const auto& r : registry->rocks()) {
        AssetItem item = makeItem(AssetKind::Rock, r.name);
        item.thumbnail = textureThumb(r.layer);
        item.swatch = swatchOf(r.tint, {0.5f, 0.5f, 0.5f});
        item.detail = "face: " + r.layer + (r.strataMetres > 0 ? format(", strata %.1f m", r.strataMetres) : "") +
                      "\nscree: " + (r.scree.empty() ? std::string("none") : r.scree);
        items_[std::size_t(AssetKind::Rock)].push_back(std::move(item));
    }
    for (const auto& f : registry->foliage()) {
        AssetItem item = makeItem(AssetKind::Foliage, f.name);
        item.swatch = swatchOf(f.tint, {0.36f * (1.0f + float(f.dryness) * 0.8f), 0.52f, 0.20f});
        item.detail = format("density x%.2f, height x%.2f", f.density, f.height) +
                      format("\ndryness %.0f %%, flowers x%.2f", f.dryness * 100.0, f.flowers);
        items_[std::size_t(AssetKind::Foliage)].push_back(std::move(item));
    }
    for (const auto& p : registry->plants()) {
        AssetItem item = makeItem(AssetKind::Plant, p.name, p.model);
        item.thumbnail = modelThumb(p.model);
        item.swatch = swatchOf(p.tint, {0.25f, 0.42f, 0.18f});
        item.detail = "model " + p.model + format("\nheight x%.2f - x%.2f", p.heightMin, p.heightMax);
        items_[std::size_t(AssetKind::Plant)].push_back(std::move(item));
    }
    for (const auto& p : registry->props()) {
        AssetItem item = makeItem(AssetKind::Prop, p.name, p.model);
        item.thumbnail = modelThumb(p.model);
        item.detail = "model " + p.model + format("\nscale x%.2f - x%.2f", p.scaleMin, p.scaleMax);
        items_[std::size_t(AssetKind::Prop)].push_back(std::move(item));
    }
    for (const auto& d : registry->decals()) {
        AssetItem item = makeItem(AssetKind::Decal, d.name, eb::kDecalKindNames[std::size_t(d.kind)]);
        if (d.kind == eb::DecalKind::Texture) item.thumbnail = textureThumb(d.texture);
        if (d.kind == eb::DecalKind::Instance) item.thumbnail = modelThumb(d.model);
        item.swatch = {float(d.colour[0]), float(d.colour[1]), float(d.colour[2])};
        item.detail = std::string(eb::kDecalKindNames[std::size_t(d.kind)]) +
                      (d.texture.empty() ? "" : ": " + d.texture) + (d.model.empty() ? "" : ": " + d.model) +
                      format("\none a %.1f m cell, %.0f %% of cells", d.cellMetres, d.density * 100.0) +
                      format("\n%.2f - %.2f m across", d.sizeMetres[0], d.sizeMetres[1]);
        items_[std::size_t(AssetKind::Decal)].push_back(std::move(item));
    }
    const auto plantThumb = [&](const std::string& plant) {
        const AssetItem* p = find(AssetKind::Plant, plant);
        return p ? p->thumbnail : fs::path();
    };
    const auto propThumb = [&](const std::string& prop) {
        const AssetItem* p = find(AssetKind::Prop, prop);
        return p ? p->thumbnail : fs::path();
    };
    for (const auto& f : registry->forestBiomes()) {
        if (f.id == 0) continue;
        AssetItem item = makeItem(AssetKind::Forest, f.name, "id " + std::to_string(f.id));
        item.thumbnail = plantThumb(heaviest(f.trees));
        if (item.thumbnail.empty()) item.thumbnail = plantThumb(heaviest(f.shrubs));
        item.swatch = {0.20f, 0.34f, 0.16f};
        item.detail = "trees: " + names(f.trees) + "\nshrubs: " + names(f.shrubs) +
                      "\nundergrowth: " + (f.undergrowth.empty() ? std::string("as the ground") : f.undergrowth);
        items_[std::size_t(AssetKind::Forest)].push_back(std::move(item));
    }
    for (const auto& w : registry->waterBiomes()) {
        if (w.id == 0) continue;
        AssetItem item = makeItem(AssetKind::Water, w.name, "id " + std::to_string(w.id));
        item.swatch = w.colour ? std::array<float, 3>{float((*w.colour)[0]), float((*w.colour)[1]), float((*w.colour)[2])}
                               : std::array<float, 3>{0.12f, 0.30f, 0.38f};
        item.detail = (w.turbidity >= 0 ? format("turbidity %.0f %%", w.turbidity * 100.0) : std::string("clear as it is")) +
                      format("\nscum %.0f %%, foam x%.2f", w.scum * 100.0, w.foam) +
                      (w.emissive > 0 ? format("\nglows %.2f", w.emissive) : "");
        items_[std::size_t(AssetKind::Water)].push_back(std::move(item));
    }
    for (const auto& d : registry->decorBiomes()) {
        if (d.id == 0) continue;
        AssetItem item = makeItem(AssetKind::Decor, d.name, "id " + std::to_string(d.id));
        item.thumbnail = propThumb(heaviest(d.props));
        if (item.thumbnail.empty())
            if (const AssetItem* decal = find(AssetKind::Decal, heaviest(d.decals))) item.thumbnail = decal->thumbnail;
        item.swatch = {0.40f, 0.34f, 0.26f};
        item.detail = "props: " + names(d.props) + "\ndecals: " + names(d.decals);
        items_[std::size_t(AssetKind::Decor)].push_back(std::move(item));
    }
}

// --- the window ---------------------------------------------------------------

void AssetBrowser::open(Request request, const AssetCatalog& catalog) {
    request_ = std::move(request);
    if (request_.kinds.empty()) request_.kinds.push_back(AssetKind::Texture);
    catalog_ = &catalog;
    tab_ = 0;
    for (std::size_t i = 0; i < request_.kinds.size(); ++i)
        if (catalog.find(request_.kinds[i], request_.current)) {
            tab_ = i;
            break;
        }
    selected_ = request_.current;
    search_.clear();
    open_ = true;
}

std::vector<const AssetItem*> AssetBrowser::shown() const {
    std::vector<const AssetItem*> out;
    if (!catalog_) return out;
    const auto wanted = lower(search_);
    for (const auto& item : catalog_->items(request_.kinds[tab_]))
        if (wanted.empty() || lower(item.name + " " + item.group + " " + item.detail).find(wanted) != std::string::npos)
            out.push_back(&item);
    return out;
}

SDL_Texture* AssetBrowser::thumbnail(ui::Ui& ui, const fs::path& path) {
    if (path.empty() || !ui.renderer()) return nullptr;
    const auto key = path.string();
    if (const auto found = pictures_.find(key); found != pictures_.end()) return found->second;
    // A few a frame: the first look at a tab of sixty pictures is a short
    // fill, not a stall.
    if (loadsThisFrame_ >= 6) return nullptr;
    ++loadsThisFrame_;
    SDL_Texture* texture = IMG_LoadTexture(ui.renderer(), key.c_str());
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    pictures_.emplace(key, texture);
    return texture;
}

void AssetBrowser::choose(const AssetItem* item, int extra) {
    std::optional<AssetItem> copy;
    if (item) copy = *item;
    auto apply = std::move(request_.apply);
    close();
    if (apply) apply(copy ? &*copy : nullptr, extra);
}

void AssetBrowser::draw(ui::Ui& ui, const ui::Rect& area) {
    if (!open_ || !catalog_) return;
    // Pictures belong to the renderer that made them.
    if (renderer_ != ui.renderer()) {
        pictures_.clear();
        renderer_ = ui.renderer();
    }
    loadsThisFrame_ = 0;
    const auto& t = ui.theme();
    const auto wid = [](const char* name, int index = 0) { return ui::widgetId(name, index); };
    const ui::Rect win{area.x, area.y, std::min(area.w, 940.0f), std::min(area.h, 720.0f)};
    if (win.w < 320 || win.h < 260) return;
    ui.panel(win);
    const float x = win.x + 16, w = win.w - 32;
    float y = win.y + 14;

    ui.text(x, y, ui.fit(request_.title, w - 110, 0.95f, true), t.accent, 0.95f, true);
    if (ui.button(wid("assets.cancel"), {win.right() - 16 - 96, y - 5, 96, 28}, request_.apply ? "Cancel" : "Close") ||
        ui.input().escape) {
        close();
        return;
    }
    y += 34;

    if (request_.kinds.size() > 1) {
        float tx = x;
        for (std::size_t i = 0; i < request_.kinds.size(); ++i) {
            const auto kind = request_.kinds[i];
            const std::string label = std::string(kAssetKindLabels[std::size_t(kind)]) + "  " +
                                      std::to_string(catalog_->items(kind).size());
            const float tw = ui.textWidth(label) + 26;
            if (ui.tab(wid("assets.tab", int(i)), {tx, y, tw, 30}, label, i == tab_) && i != tab_) {
                tab_ = i;
                selected_ = catalog_->find(kind, request_.current) ? request_.current : std::string();
            }
            tx += tw + 4;
        }
        y += 38;
    }

    // The search, and the choices that are not assets.
    float right = x + w;
    for (int e = int(request_.extras.size()) - 1; e >= 0; --e) {
        const auto& label = request_.extras[std::size_t(e)];
        const float bw = ui.textWidth(label, 0.9f) + 30;
        right -= bw;
        if (ui.button(wid("assets.extra", e), {right, y, bw, 28}, label)) {
            choose(nullptr, e);
            return;
        }
        right -= 6;
    }
    ui.textField(wid("assets.search"), {x, y, right - x - 6, 28}, search_, "search by name, folder or what it holds", 64);
    y += 40;

    const auto items = shown();
    const AssetKind kind = request_.kinds[tab_];
    const float side = std::floor(std::min(250.0f, w * 0.32f));
    const ui::Rect grid{x, y, w - side - 14, win.bottom() - 14 - y};
    const ui::Rect pane{grid.right() + 14, y, side, grid.h};

    // The grid: click to look, click the one already marked (or Choose) to take it.
    const AssetItem* hovered = nullptr;
    const AssetItem* selected = nullptr;
    const AssetItem* taken = nullptr;
    const ui::Rect content = ui.beginScroll(wid("assets.scroll", int(kind)), grid);
    const float gap = 8, caption = ui.textHeight(0.85f) + 8;
    const int columns = std::max(1, int((content.w + gap) / (118 + gap)));
    const float cell = std::floor((content.w - gap * float(columns - 1)) / float(columns));
    float bottom = content.y;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const AssetItem& item = *items[i];
        const int col = int(i) % columns, row = int(i) / columns;
        const ui::Rect r{content.x + float(col) * (cell + gap), content.y + 4 + float(row) * (cell + caption + gap), cell,
                         cell + caption};
        bottom = r.bottom();
        if (item.name == selected_) selected = &item;
        const bool visible = r.bottom() > grid.y && r.y < grid.bottom();
        if (!visible) continue;
        SDL_Texture* picture = thumbnail(ui, item.thumbnail);
        const std::uint64_t cardId = ui::widgetId(("assets.card." + item.name).c_str(), int(kind));
        if (ui.card(cardId, r, picture, item.name, item.name == selected_)) {
            if (item.name == selected_ && request_.apply) taken = &item;
            else selected_ = item.name;
        }
        const ui::Rect image{r.x + 4, r.y + 4, r.w - 8, r.h - caption - 6};
        if (!picture) {
            const bool waiting = !item.thumbnail.empty();
            ui.roundRect(image.inset(8), waiting ? t.slotEdge.withAlpha(0.35f)
                                                 : ui::Colour(item.swatch[0], item.swatch[1], item.swatch[2]), 6.0f);
        }
        if (item.kind == AssetKind::Texture && !item.registered)
            ui.text(r.x + 7, r.y + 5, "new", t.warn, 0.72f, true);
        if (item.name == request_.current)
            ui.textRight(r.right() - 7, r.y + 5, "in use", t.good, 0.72f, true);
        if (ui.hovered(r)) hovered = &item;
    }
    if (items.empty())
        ui.text(content.x + 4, content.y + 8,
                search_.empty() ? "The build has none of these." : "Nothing here matches \"" + search_ + "\".",
                t.labelSoft, 0.85f);
    ui.endScroll(bottom + 8);
    if (taken) {
        choose(taken, -1);
        return;
    }

    // What the pointer is over, else what is marked.
    const AssetItem* about = hovered ? hovered : selected;
    float py = pane.y;
    ui.roundRect({pane.x, py, pane.w, pane.w}, t.slot, 6.0f);
    if (about) {
        const ui::Rect image{pane.x + 6, py + 6, pane.w - 12, pane.w - 12};
        if (SDL_Texture* picture = thumbnail(ui, about->thumbnail)) ui.image(image, picture);
        else ui.roundRect(image.inset(10), ui::Colour(about->swatch[0], about->swatch[1], about->swatch[2]), 6.0f);
    }
    py += pane.w + 10;
    if (about) {
        ui.text(pane.x, py, ui.fit(about->name, pane.w, 0.95f, true), t.label, 0.95f, true);
        py += ui.textHeight(0.95f) + 4;
        if (!about->group.empty()) {
            ui.text(pane.x, py, ui.fit(about->group, pane.w, 0.8f), t.labelSoft, 0.8f);
            py += ui.textHeight(0.8f) + 6;
        }
        std::size_t from = 0;
        while (from <= about->detail.size()) {
            const auto to = about->detail.find('\n', from);
            const auto line = about->detail.substr(from, to == std::string::npos ? std::string::npos : to - from);
            py += ui.paragraph(pane.x, py, pane.w, line, t.labelSoft, 0.78f) + 2;
            if (to == std::string::npos) break;
            from = to + 1;
        }
        py += 6;
        if (about->kind == AssetKind::Texture && !about->registered)
            py += ui.paragraph(pane.x, py, pane.w,
                               "Choosing it adds it to the ground's texture list (one shader rebuild).", t.warn, 0.78f) + 6;
        if (about->name == request_.current) py += ui.paragraph(pane.x, py, pane.w, "In use now.", t.good, 0.78f) + 6;
    } else {
        py += ui.paragraph(pane.x, py, pane.w, "Point at a picture to read about it; click it to mark it.", t.labelSoft,
                           0.8f) + 6;
    }
    if (!request_.apply) return;   // only to look at
    const ui::Rect take{pane.x, pane.bottom() - 32, pane.w, 32};
    const std::string label = selected ? "Choose " + selected->name : std::string("Choose");
    if (ui.button(wid("assets.choose"), take, ui.fit(label, pane.w - 16, 0.9f), t.accent, t.ink, selected != nullptr) ||
        (selected && ui.input().enter)) {
        choose(selected, -1);
        return;
    }
}

} // namespace client

