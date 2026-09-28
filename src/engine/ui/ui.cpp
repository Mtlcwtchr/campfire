#include "engine/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ui {
namespace {

// The corner arc, as a handful of segments. Rounded rectangles are drawn as one
// triangle fan rather than as a rect with corner pieces laid over it: laid over,
// the seams show wherever the panel is translucent, and every panel here is.
constexpr int kCornerSteps = 4;
constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0f;

} // namespace

std::uint64_t widgetId(const char* name, int index) {
    std::uint64_t h = 1469598103934665603ULL;          // FNV-1a, good enough for a HUD
    for (const char* p = name; *p != '\0'; ++p) {
        h ^= static_cast<std::uint8_t>(*p);
        h *= 1099511628211ULL;
    }
    h ^= static_cast<std::uint64_t>(index) + 0x9e3779b9;
    h *= 1099511628211ULL;
    return h;
}

bool Ui::init(SDL_Renderer* renderer) {
    sdl_ = renderer;
    buildGlyphs();
    return glyphs_.texture != nullptr;
}

void Ui::shutdown() {
    if (glyphs_.texture) SDL_DestroyTexture(glyphs_.texture);
    glyphs_.texture = nullptr;
    sdl_ = nullptr;
}

// One font, drawn once into a texture, then blitted at any size. SDL's debug
// text is fixed at eight pixels; a HUD needs a title as well as a caption, and
// scaling a glyph atlas is the cheapest way to have both without shipping a font
// and a shaper.
void Ui::buildGlyphs() {
    const int columns = 16, rows = 6;                 // ASCII 32..127
    const int cell = 8;
    SDL_Texture* target = SDL_CreateTexture(sdl_, SDL_PIXELFORMAT_RGBA32,
                                            SDL_TEXTUREACCESS_TARGET, columns * cell, rows * cell);
    if (!target) return;
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);
    SDL_Texture* was = SDL_GetRenderTarget(sdl_);
    SDL_SetRenderTarget(sdl_, target);
    SDL_SetRenderDrawColor(sdl_, 0, 0, 0, 0);
    SDL_RenderClear(sdl_);
    SDL_SetRenderDrawColor(sdl_, 255, 255, 255, 255);
    for (int i = 0; i < columns * rows; ++i) {
        const char c = static_cast<char>(32 + i);
        if (c < 32) continue;
        const char s[2] = {c, '\0'};
        SDL_RenderDebugText(sdl_, static_cast<float>((i % columns) * cell),
                            static_cast<float>((i / columns) * cell), s);
    }
    SDL_SetRenderTarget(sdl_, was);
    SDL_SetTextureScaleMode(target, SDL_SCALEMODE_NEAREST);
    glyphs_.texture = target;
    glyphs_.cellWidth = cell;
    glyphs_.cellHeight = cell;
    glyphs_.columns = columns;
}

void Ui::begin(Input& input, int viewportWidth, int viewportHeight) {
    painting_ = true;
    input_ = &input;
    width_ = viewportWidth;
    height_ = viewportHeight;
    input_->overUi = false;
    input_->takenByUi = false;
    hot_ = 0;
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
}

void Ui::end() {
    if (input_ && input_->released) active_ = 0;
    input_ = nullptr;
}

void Ui::setColour(const Colour& c) {
    SDL_SetRenderDrawColorFloat(sdl_, c.r, c.g, c.b, c.a);
}

void Ui::rect(const Rect& r, const Colour& c) {
    if (!painting_) return;
    setColour(c);
    const SDL_FRect box = r.sdl();
    SDL_RenderFillRect(sdl_, &box);
}

void Ui::roundRect(const Rect& r, const Colour& c, float radius) {
    if (!painting_) return;
    const float rad = std::min({radius < 0 ? theme_.radius : radius, r.w * 0.5f, r.h * 0.5f});
    if (rad <= 0.5f) { rect(r, c); return; }

    std::vector<SDL_Vertex> vertices;
    std::vector<int> indices;
    const SDL_FColor colour{c.r, c.g, c.b, c.a};
    const SDL_FPoint centre{r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    vertices.push_back({centre, colour, {0, 0}});

    const SDL_FPoint corners[4] = {{r.x + r.w - rad, r.y + rad},
                                   {r.x + r.w - rad, r.y + r.h - rad},
                                   {r.x + rad, r.y + r.h - rad},
                                   {r.x + rad, r.y + rad}};
    for (int corner = 0; corner < 4; ++corner) {
        const float from = -kHalfPi + static_cast<float>(corner) * kHalfPi;
        for (int step = 0; step <= kCornerSteps; ++step) {
            const float angle = from + kHalfPi * step / kCornerSteps;
            vertices.push_back({{corners[corner].x + std::cos(angle) * rad,
                                 corners[corner].y + std::sin(angle) * rad},
                                colour,
                                {0, 0}});
        }
    }
    const int rim = static_cast<int>(vertices.size()) - 1;
    for (int i = 0; i < rim; ++i) {
        indices.push_back(0);
        indices.push_back(1 + i);
        indices.push_back(1 + (i + 1) % rim);
    }
    SDL_RenderGeometry(sdl_, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                       indices.data(), static_cast<int>(indices.size()));
}

void Ui::border(const Rect& r, const Colour& c, float thickness, float radius) {
    if (!painting_) return;
    (void)radius;
    setColour(c);
    for (float i = 0; i < thickness; ++i) {
        const SDL_FRect box{r.x + i, r.y + i, r.w - i * 2, r.h - i * 2};
        SDL_RenderRect(sdl_, &box);
    }
}

void Ui::line(float x0, float y0, float x1, float y1, const Colour& c) {
    if (!painting_) return;
    setColour(c);
    SDL_RenderLine(sdl_, x0, y0, x1, y1);
}

float Ui::textWidth(const std::string& s, float scale) const {
    return static_cast<float>(s.size()) * glyphs_.cellWidth * scale * theme_.textScale;
}
float Ui::textHeight(float scale) const {
    return glyphs_.cellHeight * scale * theme_.textScale;
}

std::string Ui::fit(const std::string& s, float width, float scale) const {
    const float each = glyphs_.cellWidth * scale * theme_.textScale;
    if (each <= 0) return s;
    const std::size_t room = static_cast<std::size_t>(std::max(1.0f, width / each));
    if (s.size() <= room) return s;
    if (room <= 1) return ".";
    return s.substr(0, room - 1) + ".";
}

void Ui::text(float x, float y, const std::string& s, const Colour& c, float scale) {
    if (!painting_ || !glyphs_.texture) return;
    const float size = scale * theme_.textScale;
    const float w = glyphs_.cellWidth * size, h = glyphs_.cellHeight * size;
    SDL_SetTextureColorModFloat(glyphs_.texture, c.r, c.g, c.b);
    SDL_SetTextureAlphaModFloat(glyphs_.texture, c.a);
    float pen = x;
    for (unsigned char ch : s) {
        if (ch >= 32 && ch < 128) {
            const int index = ch - 32;
            const SDL_FRect from{static_cast<float>((index % glyphs_.columns) * glyphs_.cellWidth),
                                 static_cast<float>((index / glyphs_.columns) * glyphs_.cellHeight),
                                 static_cast<float>(glyphs_.cellWidth),
                                 static_cast<float>(glyphs_.cellHeight)};
            const SDL_FRect to{pen, y, w, h};
            SDL_RenderTexture(sdl_, glyphs_.texture, &from, &to);
        }
        pen += w;
    }
}

void Ui::textRight(float rightX, float y, const std::string& s, const Colour& c, float scale) {
    text(rightX - textWidth(s, scale), y, s, c, scale);
}

void Ui::textCentred(const Rect& in, const std::string& s, const Colour& c, float scale) {
    text(in.x + (in.w - textWidth(s, scale)) * 0.5f, in.y + (in.h - textHeight(scale)) * 0.5f, s, c,
         scale);
}

void Ui::image(const Rect& r, SDL_Texture* texture, const Colour& tint) {
    if (!painting_ || !texture) return;
    float tw = 0, th = 0;
    SDL_GetTextureSize(texture, &tw, &th);
    if (tw <= 0 || th <= 0) return;
    const float scale = std::min(r.w / tw, r.h / th);
    const SDL_FRect to{r.x + (r.w - tw * scale) * 0.5f, r.y + (r.h - th * scale) * 0.5f,
                       tw * scale, th * scale};
    SDL_SetTextureColorModFloat(texture, tint.r, tint.g, tint.b);
    SDL_SetTextureAlphaModFloat(texture, tint.a);
    SDL_RenderTexture(sdl_, texture, nullptr, &to);
}

bool Ui::hovered(const Rect& r) const {
    return input_ != nullptr && r.contains(input_->mouseX, input_->mouseY);
}

void Ui::claim(const Rect& r) {
    if (input_ != nullptr && r.contains(input_->mouseX, input_->mouseY)) input_->overUi = true;
}

void Ui::panel(const Rect& r, bool paper) {
    // A shadow first, so a panel sits over the world instead of being painted on
    // it. Two soft rectangles rather than a blur: at this size nobody can tell.
    roundRect({r.x + 2, r.y + 3, r.w, r.h}, Colour::rgb(0, 0, 0, 0.30f));
    roundRect(r, paper ? theme_.paper : theme_.barTop);
    border(r, paper ? theme_.paperEdge : theme_.barEdge);
    claim(r);
}

bool Ui::button(std::uint64_t id, const Rect& r, const std::string& label, bool enabled) {
    const bool over = enabled && hovered(r);
    if (over) hot_ = id;
    bool clicked = false;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    if (active_ == id && input_->released && over) clicked = true;

    const Colour face = !enabled ? theme_.slot.withAlpha(0.5f)
                       : active_ == id && over ? theme_.slotDown
                       : over ? theme_.slotHot
                              : theme_.slot;
    roundRect(r, face, 4.0f);
    border(r, over ? theme_.accent.withAlpha(0.8f) : theme_.slotEdge);
    textCentred(r, label, enabled ? theme_.label : theme_.labelSoft);
    claim(r);
    return clicked;
}

bool Ui::iconButton(std::uint64_t id, const Rect& r, SDL_Texture* icon, const std::string& fallback,
                    bool active) {
    const bool over = hovered(r);
    if (over) hot_ = id;
    bool clicked = false;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    if (active_ == id && input_->released && over) clicked = true;

    roundRect(r, active ? theme_.slotDown : over ? theme_.slotHot : theme_.slot, 4.0f);
    border(r, active ? theme_.accent : theme_.slotEdge);
    if (icon) image(r.inset(3), icon);
    else textCentred(r, fallback, theme_.label);
    claim(r);
    return clicked;
}

bool Ui::tab(std::uint64_t id, const Rect& r, const std::string& label, bool active) {
    const bool over = hovered(r);
    if (over) hot_ = id;
    bool clicked = false;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    if (active_ == id && input_->released && over) clicked = true;

    // A tab is part of the bar it belongs to: square at the bottom, lit along the
    // top when it is the one showing.
    rect(r, active ? theme_.slotHot : over ? theme_.slot : Colour::rgb(0, 0, 0, 0.0f));
    if (active) rect({r.x, r.y, r.w, 2}, theme_.accent);
    textCentred(r, label, active ? theme_.label : theme_.labelSoft);
    claim(r);
    return clicked;
}

bool Ui::card(std::uint64_t id, const Rect& r, SDL_Texture* icon, const std::string& label,
              bool active) {
    const bool over = hovered(r);
    if (over) hot_ = id;
    bool clicked = false;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    if (active_ == id && input_->released && over) clicked = true;

    roundRect(r, active ? theme_.slotDown : over ? theme_.slotHot : theme_.slot, 4.0f);
    border(r, active ? theme_.accent : theme_.slotEdge);
    const float captionHeight = textHeight(0.85f) + 8.0f;
    if (icon) image({r.x + 4, r.y + 4, r.w - 8, r.h - captionHeight - 6}, icon);
    const Rect caption{r.x + 3, r.bottom() - captionHeight, r.w - 6, captionHeight};
    textCentred(caption, fit(label, caption.w, 0.85f),
                over || active ? theme_.label : theme_.labelSoft, 0.85f);
    claim(r);
    return clicked;
}

bool Ui::checkbox(std::uint64_t id, const Rect& r, const std::string& label, bool& value) {
    const bool over = hovered(r);
    if (over) hot_ = id;
    bool changed = false;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    if (active_ == id && input_->released && over) {
        value = !value;
        changed = true;
    }
    const float side = std::min(r.h - 4.0f, 14.0f);
    const Rect box{r.x, r.y + (r.h - side) * 0.5f, side, side};
    roundRect(box, value ? theme_.good : theme_.paperShade, 3.0f);
    border(box, theme_.paperEdge);
    if (value) {
        line(box.x + 3, box.y + side * 0.55f, box.x + side * 0.45f, box.bottom() - 3,
             Colour::rgb(255, 255, 255));
        line(box.x + side * 0.45f, box.bottom() - 3, box.right() - 3, box.y + 3,
             Colour::rgb(255, 255, 255));
    }
    text(box.right() + 6, r.y + (r.h - textHeight()) * 0.5f, label, theme_.ink);
    claim(r);
    return changed;
}

void Ui::gauge(const Rect& r, float fraction, const Colour& fill, const Colour& back) {
    roundRect(r, back, r.h * 0.5f);
    const float clamped = std::clamp(fraction, 0.0f, 1.0f);
    if (clamped > 0.01f)
        roundRect({r.x, r.y, std::max(r.h, r.w * clamped), r.h}, fill, r.h * 0.5f);
    border(r, theme_.slotEdge.withAlpha(0.6f));
}

void Ui::gaugeLabelled(const Rect& r, const std::string& label, float fraction,
                       const Colour& fill) {
    const float line = textHeight();
    text(r.x, r.y, label, theme_.labelSoft, 0.85f);
    gauge({r.x, r.y + line + 2, r.w, 6}, fraction, fill, theme_.gauge);
}

int Ui::dropdown(std::uint64_t id, const Rect& r, const std::vector<std::string>& options,
                 int chosen) {
    const bool over = hovered(r);
    if (over) hot_ = id;
    if (over && input_->pressed) {
        openMenu_ = openMenu_ == id ? 0 : id;
        input_->takenByUi = true;
    }
    roundRect(r, theme_.paperShade, 3.0f);
    border(r, theme_.paperEdge);
    if (chosen >= 0 && chosen < static_cast<int>(options.size()))
        text(r.x + 6, r.y + (r.h - textHeight()) * 0.5f, options[chosen], theme_.ink);
    // The little arrow.
    const float cx = r.right() - 12, cy = r.y + r.h * 0.5f;
    line(cx - 4, cy - 2, cx, cy + 2, theme_.ink);
    line(cx, cy + 2, cx + 4, cy - 2, theme_.ink);
    claim(r);

    int picked = -1;
    if (openMenu_ == id) {
        const float rowHeight = r.h;
        const Rect list{r.x, r.bottom() + 2, r.w, rowHeight * options.size()};
        panel(list, true);
        for (std::size_t i = 0; i < options.size(); ++i) {
            const Rect row{list.x + 2, list.y + rowHeight * i, list.w - 4, rowHeight};
            const bool rowOver = hovered(row);
            if (rowOver) rect(row, theme_.paperShade);
            text(row.x + 6, row.y + (row.h - textHeight()) * 0.5f, options[i], theme_.ink);
            if (rowOver && input_->pressed) {
                picked = static_cast<int>(i);
                openMenu_ = 0;
                input_->takenByUi = true;
            }
        }
        claim(list);
    }
    return picked;
}

} // namespace ui
