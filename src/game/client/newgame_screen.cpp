#include "game/client/newgame_screen.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "game/client/world_dials.hpp"
#include "game/simulation/savegame.hpp"

namespace client {
namespace {

void text(SDL_Renderer* sdl, float x, float y, const std::string& s, int r, int g, int b) {
    SDL_SetRenderDrawColor(sdl, 12, 12, 14, 220);
    SDL_RenderDebugText(sdl, x + 1, y + 1, s.c_str());
    SDL_SetRenderDrawColor(sdl, r, g, b, 255);
    SDL_RenderDebugText(sdl, x, y, s.c_str());
}

} // namespace

void NewGameScreen::open(SDL_Renderer* sdl, const std::filesystem::path& contentDir,
                         const std::filesystem::path& saveDir) {
    presets_ = generation::loadWorldPresets(contentDir / "config" / "world_presets.json");
    applyPreset(0);

    saves_.clear();
    std::error_code ec;
    if (std::filesystem::exists(saveDir, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(saveDir, ec)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".save") continue;
            const sim::SaveSummary summary = sim::inspectSave(entry.path());
            SaveEntry line;
            line.file = entry.path();
            line.line = entry.path().stem().string() + "  ";
            if (summary.readable) {
                line.line += "seed " + std::to_string(summary.seed) + ", day " +
                             std::to_string(summary.tick / 240) + ", " +
                             std::to_string(summary.communities) + " communities";
            } else {
                line.line += "unreadable (" + summary.note + ")";
            }
            saves_.push_back(std::move(line));
        }
        std::sort(saves_.begin(), saves_.end(),
                  [](const SaveEntry& a, const SaveEntry& b) { return a.file < b.file; });
    }
    dirty_ = true;
    regenerate(sdl);
}

void NewGameScreen::close() {
    if (image_) SDL_DestroyTexture(image_);
    image_ = nullptr;
}

void NewGameScreen::applyPreset(std::size_t index) {
    if (presets_.empty()) return;
    preset_ = index % presets_.size();
    const std::uint64_t keepSeed = params_.seed;
    params_ = presets_[preset_].params;
    if (keepSeed > 1) params_.seed = keepSeed;
    dirty_ = true;
}

void NewGameScreen::adjust(int direction) {
    if (field_ < dials().size()) {
        dials()[field_].move(params_, direction);
        dirty_ = true;
    }
}

void NewGameScreen::regenerate(SDL_Renderer* sdl) {
    const auto started = std::chrono::steady_clock::now();
    preview_ = generation::generateWorldMap(params_);
    builtInSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    dirty_ = false;

    // The picture: one pixel a cell, sea by depth, land shaded by height with a
    // hint of what grows on it, rivers drawn over the top. The same reading as
    // the world view in the game, so the map a player chooses is the map they
    // then live on.
    const int w = preview_.width, h = preview_.height;
    if (w <= 0 || h <= 0) return;
    SDL_Surface* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surface) return;
    auto* pixels = static_cast<std::uint32_t*>(surface->pixels);
    const int pitch = surface->pitch / 4;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const generation::WorldCell& c = preview_.at({x, y});
            int r, g, b;
            if (c.sea) {
                const int deep = 30 + c.elevation / 3;
                r = 14 + deep / 4;
                g = 38 + deep / 2;
                b = 78 + deep;
            } else {
                // Dry ground is sand, wet ground is green, and both go to bare
                // grey rock as the land rises. Reading the two axes separately
                // is what makes a rain shadow legible on the map: the same
                // height either side of a range, one green and one sand.
                const float height = static_cast<float>(c.elevation) / 255.0f;
                const float wet = std::clamp(static_cast<float>(c.moisture) / 190.0f, 0.0f, 1.0f);
                const float bare = std::clamp((height - 0.55f) / 0.45f, 0.0f, 1.0f);
                const float dry[3] = {206.0f, 184.0f, 132.0f};
                const float green[3] = {86.0f, 124.0f, 66.0f};
                const float rock[3] = {150.0f, 146.0f, 140.0f};
                float mix[3];
                for (int k = 0; k < 3; ++k) {
                    const float ground = dry[k] + (green[k] - dry[k]) * wet;
                    mix[k] = ground + (rock[k] - ground) * bare;
                }
                r = static_cast<int>(mix[0]);
                g = static_cast<int>(mix[1]);
                b = static_cast<int>(mix[2]);
                // Hill shading, so ranges read as ranges rather than as colour.
                const auto at = [&](int hx, int hy) {
                    const core::TilePos q{std::clamp(hx, 0, w - 1), std::clamp(hy, 0, h - 1)};
                    return static_cast<float>(preview_.at(q).elevation);
                };
                const float lit = std::clamp(1.0f + ((at(x + 1, y) - at(x - 1, y)) +
                                                     (at(x, y + 1) - at(x, y - 1))) * 0.012f,
                                             0.6f, 1.35f);
                r = static_cast<int>(r * lit);
                g = static_cast<int>(g * lit);
                b = static_cast<int>(b * lit);
            }
            if (c.river && !c.sea) { r = 62; g = 104; b = 150; }
            pixels[y * pitch + x] = static_cast<std::uint32_t>(0xff000000u |
                                                              (std::clamp(b, 0, 255) << 16) |
                                                              (std::clamp(g, 0, 255) << 8) |
                                                              std::clamp(r, 0, 255));
        }
    }
    if (image_) SDL_DestroyTexture(image_);
    image_ = SDL_CreateTextureFromSurface(sdl, surface);
    SDL_SetTextureScaleMode(image_, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(surface);
}

NewGameScreen::Outcome NewGameScreen::handle(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) return Outcome::Quit;
    if (e.type != SDL_EVENT_KEY_DOWN) return Outcome::Staying;
    switch (e.key.key) {
        case SDLK_ESCAPE: return Outcome::Quit;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (onSaves_ && !saves_.empty()) return Outcome::Load;
            return Outcome::Start;
        case SDLK_TAB:
            if (!saves_.empty()) onSaves_ = !onSaves_;
            return Outcome::Staying;
        case SDLK_UP:
            if (onSaves_) save_ = save_ == 0 ? saves_.size() - 1 : save_ - 1;
            else field_ = field_ == 0 ? dials().size() - 1 : field_ - 1;
            return Outcome::Staying;
        case SDLK_DOWN:
            if (onSaves_) save_ = saves_.empty() ? 0 : (save_ + 1) % saves_.size();
            else field_ = (field_ + 1) % dials().size();
            return Outcome::Staying;
        case SDLK_LEFT: adjust(-1); return Outcome::Staying;
        case SDLK_RIGHT: adjust(1); return Outcome::Staying;
        case SDLK_LEFTBRACKET: applyPreset(preset_ == 0 ? presets_.size() - 1 : preset_ - 1); return Outcome::Staying;
        case SDLK_RIGHTBRACKET: applyPreset(preset_ + 1); return Outcome::Staying;
        case SDLK_R:
            params_.seed = params_.seed * 6364136223846793005ULL + 1442695040888963407ULL;
            if (params_.seed == 0) params_.seed = 1;
            dirty_ = true;
            return Outcome::Staying;
        default: return Outcome::Staying;
    }
}

void NewGameScreen::draw(SDL_Renderer* sdl, int viewportWidth, int viewportHeight) {
    if (dirty_) regenerate(sdl);

    SDL_SetRenderDrawColor(sdl, 16, 17, 20, 255);
    SDL_RenderClear(sdl);

    // The map fills the right of the screen, square and as large as it fits.
    const float panel = 380.0f;
    const float side = std::min(static_cast<float>(viewportWidth) - panel - 40.0f,
                                static_cast<float>(viewportHeight) - 40.0f);
    const SDL_FRect box{panel + 20.0f, (viewportHeight - side) * 0.5f, side, side};
    if (image_) SDL_RenderTexture(sdl, image_, nullptr, &box);
    SDL_SetRenderDrawColor(sdl, 90, 88, 82, 255);
    SDL_RenderRect(sdl, &box);

    // Every site a founding community could be put on, and the one that is ours.
    for (const auto& site : preview_.sites) {
        const float x = box.x + box.w * static_cast<float>(site.cell.x) / std::max(1, preview_.width);
        const float y = box.y + box.h * static_cast<float>(site.cell.y) / std::max(1, preview_.height);
        const float r = site.played ? 4.0f : 2.5f;
        SDL_SetRenderDrawColor(sdl, 20, 16, 12, 220);
        const SDL_FRect halo{x - r - 1, y - r - 1, (r + 1) * 2, (r + 1) * 2};
        SDL_RenderFillRect(sdl, &halo);
        if (site.played) SDL_SetRenderDrawColor(sdl, 255, 236, 120, 255);
        else SDL_SetRenderDrawColor(sdl, 226, 96, 74, 255);
        const SDL_FRect dot{x - r, y - r, r * 2, r * 2};
        SDL_RenderFillRect(sdl, &dot);
    }

    float y = 24.0f;
    text(sdl, 24, y, "CAMPFIRE - a world to settle", 250, 246, 232);
    y += 26;
    if (!presets_.empty()) {
        text(sdl, 24, y, "[ ] " + presets_[preset_].label, 245, 226, 150);
        y += 14;
        // The note wraps by hand: there is one font and no measuring in it.
        const std::string& note = presets_[preset_].note;
        for (std::size_t at = 0; at < note.size(); at += 40) {
            text(sdl, 24, y, "    " + note.substr(at, 40), 150, 148, 140);
            y += 12;
        }
    }
    y += 10;

    for (std::size_t i = 0; i < dials().size(); ++i) {
        const bool on = !onSaves_ && i == field_;
        const std::string line = std::string(on ? "> " : "  ") + dials()[i].label + ": " +
                                 dials()[i].show(params_);
        text(sdl, 24, y, line, on ? 255 : 214, on ? 236 : 210, on ? 150 : 196);
        y += 13;
        if (on) {
            text(sdl, 24, y, std::string("    ") + dials()[i].note, 150, 148, 140);
            y += 13;
        }
    }

    y += 8;
    std::int64_t land = 0;
    for (const auto& c : preview_.cells)
        if (!c.sea) ++land;
    const std::int64_t cells = std::max<std::int64_t>(1, preview_.cells.size());
    char summary[160];
    std::snprintf(summary, sizeof summary, "%lld%% land, %zu communities, built in %.1f s",
                  static_cast<long long>(land * 100 / cells), preview_.sites.size(),
                  builtInSeconds_);
    text(sdl, 24, y, summary, 190, 206, 190);
    y += 20;

    if (!saves_.empty()) {
        text(sdl, 24, y, onSaves_ ? "> saved games (tab to leave)" : "  saved games (tab)",
             onSaves_ ? 255 : 190, onSaves_ ? 236 : 190, onSaves_ ? 150 : 190);
        y += 14;
        for (std::size_t i = 0; i < saves_.size() && i < 8; ++i) {
            const bool on = onSaves_ && i == save_;
            text(sdl, 24, y, std::string(on ? "  > " : "    ") + saves_[i].line,
                 on ? 255 : 200, on ? 236 : 198, on ? 150 : 186);
            y += 12;
        }
        y += 8;
    }

    text(sdl, 24, static_cast<float>(viewportHeight) - 66,
         "up/down choose   left/right change", 170, 168, 160);
    text(sdl, 24, static_cast<float>(viewportHeight) - 52,
         "[ ] preset   r re-roll the seed", 170, 168, 160);
    text(sdl, 24, static_cast<float>(viewportHeight) - 38,
         "enter start   tab saves   esc quit", 170, 168, 160);
}

} // namespace client
