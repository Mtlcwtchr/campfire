#include "game/client/explore_menu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/client/world_dials.hpp"

namespace client {
namespace {

// Two passes, dark then light: the debug font has no outline of its own and a
// letter on top of a hillside is unreadable without one.
void text(SDL_Renderer* sdl, float x, float y, const std::string& s, int r, int g, int b) {
    SDL_SetRenderDrawColor(sdl, 10, 10, 12, 230);
    SDL_RenderDebugText(sdl, x + 1, y + 1, s.c_str());
    SDL_SetRenderDrawColor(sdl, r, g, b, 255);
    SDL_RenderDebugText(sdl, x, y, s.c_str());
}

} // namespace

// The four numbers of a material a person may move, and how far one press moves
// them. Named here rather than in the drawing, because the names are what the
// person reads and the steps are what they feel.
namespace {
struct Knob {
    const char* label;
    float step;
    float low, high;
    float& (*of)(content::GroundMaterial&);
};
const Knob kKnobs[] = {
        {"metres to a turn", 0.1f, 0.5f, 200.0f,
         [](content::GroundMaterial& m) -> float& { return m.metresPerTurn; }},
        {"border width", 0.02f, 0.0f, 1.0f,
         [](content::GroundMaterial& m) -> float& { return m.blendWidth; }},
        {"tearing", 0.02f, 0.0f, 1.5f,
         [](content::GroundMaterial& m) -> float& { return m.tear; }},
        {"tear size, m", 0.2f, 0.2f, 40.0f,
         [](content::GroundMaterial& m) -> float& { return m.tearMetres; }},
};
constexpr std::size_t kKnobCount = sizeof(kKnobs) / sizeof(kKnobs[0]);
}  // namespace

void ExploreMenu::open(const generation::WorldMapParams& showing,
                       std::vector<content::GroundMaterial>* ground,
                       std::filesystem::path groundFile) {
    ground_ = ground;
    groundFile_ = std::move(groundFile);
    if (ground_) asLoaded_ = *ground_;
    std::filesystem::path content = "content";
    for (int up = 0; up < 5 && !std::filesystem::exists(content); ++up) content = ".." / content;
    presets_ = generation::loadWorldPresets(content / "config" / "world_presets.json");
    params_ = showing;
    // Whichever preset the world on screen was built from, if any: the menu
    // opens showing what is actually out there rather than its own first entry.
    for (std::size_t i = 0; i < presets_.size(); ++i)
        if (presets_[i].params.seaPercent == showing.seaPercent &&
            presets_[i].params.erosionPasses == showing.erosionPasses &&
            presets_[i].params.rainfallPercent == showing.rainfallPercent)
            preset_ = i;
    dirty_ = true;
}

void ExploreMenu::applyPreset(std::size_t index) {
    if (presets_.empty()) return;
    preset_ = index % presets_.size();
    const std::uint64_t keepSeed = params_.seed;
    params_ = presets_[preset_].params;
    params_.seed = keepSeed;
    dirty_ = true;
}

bool ExploreMenu::handle(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN) return false;
    const SDL_Keycode key = event.key.key;
    // The draw distance steps by about a sixth, and repeats while held.
    if (key == SDLK_COMMA || key == SDLK_PERIOD) {
        drawDistance(drawDistance() * (key == SDLK_PERIOD ? 1.18 : 1.0 / 1.18));
        return true;
    }
    if (event.key.repeat) return false;
    // Graphics settings window and the scene view (freeze the cull camera).
    if (key == SDLK_O) { togglePanel(); return true; }
    if (key == SDLK_F) { panel_.sceneViewToggled = true; return true; }
    if ((event.key.mod & SDL_KMOD_SHIFT) && event.key.scancode>=SDL_SCANCODE_1 && event.key.scancode<=SDL_SCANCODE_8) {
        if (stagesAvailable_) terrainStage_=generation::TerrainStage(event.key.scancode-SDL_SCANCODE_1);
        else note_="This legacy world has no saved generation stages";
        dirty_=true;
        return true;
    }
    if (key >= SDLK_F1 && key <= SDLK_F8) {
        mapView_ = static_cast<world::MapView>(key - SDLK_F1 + 1);
        if (mapView_ == world::MapView::Flood) floodRequested_ = true;
        dirty_ = true;
        return true;
    }
    if (key == SDLK_0) {
        mapView_ = world::MapView::Natural;
        dirty_ = true;
        return true;
    }
    if (key == SDLK_PERIOD && mapView_ == world::MapView::EnvMasks) {
        environmentChannel_ = (environmentChannel_ + 1) % 8;
        dirty_ = true;
        return true;
    }
    if (key == SDLK_M) {
        mapView_ = static_cast<world::MapView>(
                (static_cast<int>(mapView_) + 1) % static_cast<int>(world::MapView::Count));
        if (mapView_ == world::MapView::Flood) floodRequested_ = true;
        dirty_ = true;
        return true;
    }
    if (key == SDLK_F11) {
        iceVisible_ = !iceVisible_;
        dirty_ = true;
        return true;
    }
    if (key == SDLK_F9) {
        weatherPaused_ = !weatherPaused_;
        return true;
    }
    if (key == SDLK_F10) {
        weatherDay_ += std::max(1, calendar_.daysPerSeason);
        return true;
    }
    if (key == SDLK_F12) {
        weatherSpeed_ = weatherSpeed_ >= 8.0 ? 0.25 : weatherSpeed_ * 2.0;
        return true;
    }
    if (key == SDLK_H) {
        soilTreatment_ = world::SoilState::Treatment::Harvest;
        return true;
    }
    if (key == SDLK_G) {
        soilTreatment_ = world::SoilState::Treatment::Pasture;
        return true;
    }
    if (key == SDLK_J) {
        soilTreatment_ = world::SoilState::Treatment::Ash;
        return true;
    }
    if (key == SDLK_N) {
        soil_.advanceDays(30);
        return true;
    }
    if (key == SDLK_T) {
        weatherPreset_ = (weatherPreset_ + 1) % static_cast<int>(world::weather::kPresets.size());
        conditions_.rainfall = weatherPreset_ == 3 ? core::Fixed::ratio(3, 2)
                                                   : weatherPreset_ == 4 ? core::Fixed::ratio(1, 4)
                                                                          : core::Fixed::ratio(3, 4);
        return true;
    }
    if (key == SDLK_R && mapView_ == world::MapView::Flood) {
        floodRequested_ = true;
        return true;
    }
    if (key == SDLK_B && mapView_ != world::MapView::Natural && !open_) {
        potentialOnly_ = !potentialOnly_;
        return true;
    }
    if (key == SDLK_TAB) { toggle(); return true; }
    if (!open_) return false;
    if (key == SDLK_P) {
        page_ = page_ == Page::World ? Page::Ground : Page::World;
        dirty_ = true;
        return true;
    }
    if (page_ == Page::Ground) return handleGround(key);
    switch (key) {
        case SDLK_UP:
            field_ = (field_ + dials().size() - 1) % dials().size();
            break;
        case SDLK_DOWN:
            field_ = (field_ + 1) % dials().size();
            break;
        case SDLK_LEFT:
            dials()[field_].move(params_, -1);
            break;
        case SDLK_RIGHT:
            dials()[field_].move(params_, +1);
            break;
        case SDLK_LEFTBRACKET:
            applyPreset(preset_ + presets_.size() - 1);
            break;
        case SDLK_RIGHTBRACKET:
            applyPreset(preset_ + 1);
            break;
        case SDLK_R:
            params_.seed = params_.seed + 1;
            break;
        case SDLK_RETURN:
            wanted_ = true;
            note_ = "building ...";
            break;
        default:
            return false;
    }
    dirty_ = true;
    return true;
}

bool ExploreMenu::handleGround(SDL_Keycode key) {
    if (ground_ == nullptr || ground_->empty()) return false;
    const std::size_t most = std::min<std::size_t>(content::kBlendedMaterials, ground_->size());
    switch (key) {
        case SDLK_UP:
            material_ = (material_ + most - 1) % most;
            break;
        case SDLK_DOWN:
            material_ = (material_ + 1) % most;
            break;
        case SDLK_LEFTBRACKET:
            number_ = (number_ + kKnobCount - 1) % kKnobCount;
            break;
        case SDLK_RIGHTBRACKET:
            number_ = (number_ + 1) % kKnobCount;
            break;
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            const Knob& knob = kKnobs[number_];
            float& value = knob.of((*ground_)[material_]);
            value = std::clamp(value + (key == SDLK_RIGHT ? knob.step : -knob.step), knob.low,
                               knob.high);
            break;
        }
        case SDLK_S:
            note_ = content::saveGroundMaterials(groundFile_, *ground_)
                            ? "written to " + groundFile_.filename().string() +
                                      " (first save kept a .bak)"
                            : "could not write " + groundFile_.string();
            break;
        case SDLK_B:
            *ground_ = asLoaded_;
            note_ = "back to what was loaded";
            break;
        default:
            return false;
    }
    dirty_ = true;
    return true;
}

void ExploreMenu::groundReread() {
    // What "back to what was loaded" means moves with it: the numbers in the
    // file are now what is on screen, and going back to a copy taken before the
    // editor was ever opened would undo somebody else's work rather than one's
    // own.
    if (ground_) asLoaded_ = *ground_;
    note_ = "ground.json changed - taken from the file";
    dirty_ = true;
}

void ExploreMenu::built(double seconds) {
    wanted_ = false;
    char said[64];
    std::snprintf(said, sizeof(said), "built in %.2f s", seconds);
    note_ = said;
    dirty_ = true;
}

double ExploreMenu::distanceAt(float x) {
    const double t = std::clamp(double(x - kTrackLeft) / double(kTrackRight - kTrackLeft), 0.0, 1.0);
    return kMinDrawDistance * std::pow(kMaxDrawDistance / kMinDrawDistance, t);
}

float ExploreMenu::trackAt(double metres) {
    const double t = std::log(std::clamp(metres, kMinDrawDistance, kMaxDrawDistance) / kMinDrawDistance) /
                     std::log(kMaxDrawDistance / kMinDrawDistance);
    return float(kTrackLeft + t * (kTrackRight - kTrackLeft));
}

bool ExploreMenu::pointer(float x, float y, bool down) {
    const bool pressed = down && !pointerWasDown_;
    pointerWasDown_ = down;
    // Two toolbar buttons on the status strip, for a mouse-only person.
    if (pressed && y >= kButtonY && y < kButtonY + 14) {
        if (x >= kGraphicsButtonX && x < kGraphicsButtonX + kButtonW) { togglePanel(); dirty_ = true; return true; }
        if (x >= kSceneButtonX && x < kSceneButtonX + kButtonW) { panel_.sceneViewToggled = true; dirty_ = true; return true; }
    }
    // And the switch between looking at the world and making it.
    if (pressed && y >= kEditButtonY && y < kEditButtonY + 14 &&
        x >= kEditButtonX && x < kEditButtonX + kButtonW) {
        toggleEditor();
        return true;
    }
    if (!down) { const bool was = dragging_; dragging_ = false; return was; }
    // Grabbed anywhere on the slider's row, then held until released even if
    // the pointer leaves it: a drag that drops when it strays is no slider.
    if (!dragging_ && !(x >= kTrackLeft - 6 && x <= kTrackRight + 6 && y >= kTrackY - 9 && y <= kTrackY + 9))
        return false;
    dragging_ = true;
    drawDistance(distanceAt(x));
    return true;
}

void ExploreMenu::draw(SDL_Renderer* into, int width, int height) {
    dirty_ = false;
    SDL_SetRenderDrawBlendMode(into, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(into, 0, 0, 0, 0);
    SDL_RenderClear(into);
    SDL_SetRenderDrawBlendMode(into, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(into, 14, 14, 18, 215);
    const SDL_FRect panel{0, 0, static_cast<float>(width), static_cast<float>(open_ ? height : kStatusHigh)};
    SDL_RenderFillRect(into, &panel);

    text(into,10,8,"View: " + viewName_,220,225,240);
    text(into,10,22,std::string("Stage: ") + (stagesAvailable_ ?
        generation::kTerrainStageNames[std::size_t(terrainStage_)] : "Final (legacy; stages unavailable)"),220,225,240);
    text(into,10,36,std::string("Display: ") + world::kMapTitles[std::size_t(mapView_)],190,210,230);
    text(into,10,52,"V view   M display   0 natural",150,150,160);
    text(into,10,64,"Shift+1..8 stage  Tab world  ` edit  O gfx  F scene",150,150,160);
    {
        char said[64];
        std::snprintf(said, sizeof(said), "Draw distance / fog: %.1f km   , .", drawDistance() / 1000.0);
        text(into, 10, 78, said, 220, 225, 240);
        const SDL_FRect track{kTrackLeft, kTrackY - 1, kTrackRight - kTrackLeft, 3};
        SDL_SetRenderDrawColor(into, 90, 96, 110, 255);
        SDL_RenderFillRect(into, &track);
        const float at = trackAt(drawDistance());
        const SDL_FRect filled{kTrackLeft, kTrackY - 1, at - kTrackLeft, 3};
        SDL_SetRenderDrawColor(into, 170, 200, 225, 255);
        SDL_RenderFillRect(into, &filled);
        const SDL_FRect knob{at - 4, kTrackY - 6, 8, 12};
        SDL_SetRenderDrawColor(into, dragging_ ? 250 : 225, dragging_ ? 235 : 230, dragging_ ? 160 : 240, 255);
        SDL_RenderFillRect(into, &knob);
        const auto button = [&](float x, const char* label, bool on) {
            const SDL_FRect box{x, kButtonY, kButtonW, 14};
            SDL_SetRenderDrawColor(into, on ? 120 : 46, on ? 96 : 52, on ? 40 : 64, 255);
            SDL_RenderFillRect(into, &box);
            text(into, x + 6, kButtonY + 3, label, 230, 232, 240);
        };
        button(kGraphicsButtonX, "Graphics  O", panelOpen_);
        button(kSceneButtonX, "Scene view F", panel_.sceneView);
        {
            const bool on = editor_.active();
            const SDL_FRect box{kEditButtonX, kEditButtonY, kButtonW, 14};
            SDL_SetRenderDrawColor(into, on ? 150 : 46, on ? 104 : 52, on ? 34 : 64, 255);
            SDL_RenderFillRect(into, &box);
            text(into, kEditButtonX + 6, kEditButtonY + 3, on ? "EDIT mode   `" : "Explore     `", 230, 232, 240);
        }
    }
    if (!open_) return;
    float y = kStatusHigh + 4;
    if (page_ == Page::World) drawWorld(into, width, y);
    else drawGround(into, width, y);
    if (!note_.empty()) {
        y += 13;
        text(into, 10, y, note_, 200, 220, 200);
    }
}

void ExploreMenu::drawWorld(SDL_Renderer* into, int width, float& y) {
    (void)width;
    text(into, 10, y, "THE WORLD", 235, 220, 170);
    y += 14;
    if (!presets_.empty()) {
        text(into, 10, y, "[ " + presets_[preset_].label + " ]", 200, 200, 210);
        y += 12;
        // The note wraps at the panel's width, and on spaces: broken on the
        // count alone it splits words down the middle, which reads as a fault in
        // the text rather than as a line ending.
        const std::string& said = presets_[preset_].note;
        for (std::size_t at = 0; at < said.size();) {
            std::size_t take = std::min<std::size_t>(52, said.size() - at);
            if (at + take < said.size()) {
                const std::size_t space = said.rfind(' ', at + take);
                if (space != std::string::npos && space > at) take = space - at;
            }
            text(into, 18, y, said.substr(at, take), 130, 130, 140);
            y += 10;
            at += take;
            while (at < said.size() && said[at] == ' ') ++at;
        }
    }
    y += 6;
    for (std::size_t i = 0; i < dials().size(); ++i) {
        const bool here = i == field_;
        const std::string line = std::string(here ? "> " : "  ") + dials()[i].label + ": " +
                                 dials()[i].show(params_);
        text(into, 10, y, line, here ? 245 : 190, here ? 230 : 190, here ? 150 : 200);
        y += 12;
    }
    y += 4;
    text(into, 10, y, dials()[field_].note, 130, 130, 140);
    y += 14;
    text(into, 10, y, "up/down choose   left/right change   [ ] preset", 150, 150, 160);
    y += 11;
    text(into, 10, y, "r  new seed   enter  build it   p  ground   tab  close", 150, 150, 160);
}

// The ground, as a table: a row to a material and a column to a number, with
// the one being moved marked. A table rather than one value at a time because
// what a person is doing here is comparing - grass against dirt, this border
// against that one - and a dial that shows one number at a time hides exactly
// the comparison they are making.
void ExploreMenu::drawGround(SDL_Renderer* into, int width, float& y) {
    (void)width;
    text(into, 10, y, "THE GROUND", 235, 220, 170);
    y += 14;
    if (ground_ == nullptr || ground_->empty()) {
        text(into, 10, y, "no ground table loaded", 200, 150, 150);
        return;
    }
    text(into, 10, y, "material     turn  width   tear   size", 150, 150, 160);
    y += 12;
    const std::size_t most = std::min<std::size_t>(content::kBlendedMaterials, ground_->size());
    for (std::size_t i = 0; i < most; ++i) {
        const content::GroundMaterial& m = (*ground_)[i];
        char line[96];
        std::snprintf(line, sizeof(line), "%c%-11s %5.0f  %5.2f  %5.2f  %5.1f",
                      i == material_ ? '>' : ' ', m.name.c_str(), m.metresPerTurn, m.blendWidth,
                      m.tear, m.tearMetres);
        const bool here = i == material_;
        text(into, 10, y, line, here ? 245 : 190, here ? 230 : 190, here ? 150 : 200);
        // Which column is being moved, marked under it rather than by colour:
        // one row is already picked out by colour and two colours in one table
        // is a puzzle.
        if (here) {
            const float column = 10 + 8.0f * (13 + 7 * static_cast<float>(number_));
            text(into, column, y + 9, "^^^^", 245, 230, 150);
        }
        y += here ? 22 : 12;
    }
    y += 4;
    text(into, 10, y, std::string("moving: ") + kKnobs[number_].label, 130, 130, 140);
    y += 14;
    text(into, 10, y, "up/down material   [ ] number   left/right move", 150, 150, 160);
    y += 11;
    text(into, 10, y, "s  save   b  back to loaded   p  world   tab  close", 150, 150, 160);
    y += 11;
    text(into, 10, y, "set and variant are baked, not live: re-bake for those", 120, 120, 130);
}

} // namespace client
