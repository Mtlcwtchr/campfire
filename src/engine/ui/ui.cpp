#include "engine/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "engine/ui/font.hpp"

namespace ui {
namespace {

// A rounded rectangle's corners are cut row by row, in whole pixels: each row
// of a corner one span, inset by the arc, and the rows between one rectangle.
// Nothing overlaps, so a translucent panel has no seams - which is why it was
// a triangle fan - and every row goes through the software renderer's blended
// rectangle fill, which is several times cheaper per pixel than its triangle
// rasteriser. On a two-pixel-per-point window a large panel went from tens of
// milliseconds to a few.
constexpr float kSpanSlack = 0.01f;   // so the renderer's truncation lands on the pixel meant

std::uint64_t bitsOf(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

// What one primitive draws, as one number.
struct Hash {
    std::uint64_t h = 1469598103934665603ULL;
    Hash& add(std::uint64_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        return *this;
    }
    Hash& add(float v) { return add(bitsOf(v)); }
    Hash& add(const Rect& r) { return add(r.x).add(r.y).add(r.w).add(r.h); }
    Hash& add(const Colour& c) { return add(c.r).add(c.g).add(c.b).add(c.a); }
    Hash& add(const std::string& s) {
        std::uint64_t f = 1469598103934665603ULL;
        for (const unsigned char ch : s) { f ^= ch; f *= 1099511628211ULL; }
        return add(f);
    }
};

bool overlaps(const Rect& a, const Rect& b) {
    return a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

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

Ui::Ui() = default;
// Nothing of SDL's is touched here: a Ui may outlive the renderer it drew
// with, and whoever owns both calls shutdown() while the renderer is alive.
Ui::~Ui() = default;

bool Ui::init(SDL_Renderer* renderer) {
    atlases_.clear();
    sdl_ = renderer;
    buildGlyphs();
    return glyphs_.texture != nullptr;
}

void Ui::shutdown() {
    atlases_.clear();
    if (glyphs_.texture) SDL_DestroyTexture(glyphs_.texture);
    glyphs_.texture = nullptr;
    sdl_ = nullptr;
}

void Ui::setFont(std::shared_ptr<FontFace> regular, std::shared_ptr<FontFace> bold, float points,
                 float pixelsPerPoint) {
    const bool same = regular == regular_ && bold == bold_ && points == fontPoints_ &&
                      pixelsPerPoint == pixelsPerPoint_;
    regular_ = std::move(regular);
    bold_ = std::move(bold);
    fontPoints_ = std::max(4.0f, points);
    pixelsPerPoint_ = std::max(0.25f, pixelsPerPoint);
    if (!same) atlases_.clear();
}

const FontAtlas* Ui::atlas(float scale, bool bold) const {
    if (!regular_ || !sdl_) return nullptr;
    const int pixels = std::max(4, int(std::lround(fontPoints_ * scale * theme_.textScale * pixelsPerPoint_)));
    const bool heavy = bold && bold_ != nullptr;
    auto& slot = atlases_[{heavy, pixels}];
    if (!slot) slot = std::make_unique<FontAtlas>(heavy ? bold_ : regular_, sdl_, pixels);
    return *slot ? slot.get() : nullptr;
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
    signature_ = 0;
    focusSeen_ = false;
    open_.clear();
    mix(std::uint64_t(viewportWidth) << 32 | std::uint32_t(viewportHeight));
    if (tile_ > 0) {
        tileColumns_ = std::max(1, int(std::ceil(float(viewportWidth) / tile_)));
        tileRows_ = std::max(1, int(std::ceil(float(viewportHeight) / tile_)));
        tiles_.assign(std::size_t(tileColumns_) * std::size_t(tileRows_), signature_);
    } else {
        tileColumns_ = tileRows_ = 0;
        tiles_.clear();
    }
    SDL_SetRenderDrawBlendMode(sdl_, SDL_BLENDMODE_BLEND);
}

void Ui::end() {
    if (input_ && input_->released) active_ = 0;
    // A field that was not built this frame is not on the screen to type into.
    if (!focusSeen_) focus_ = 0;
    input_ = nullptr;
}

void Ui::mix(std::uint64_t value) {
    signature_ ^= value + 0x9e3779b97f4a7c15ULL + (signature_ << 6) + (signature_ >> 2);
}

bool Ui::record(const Rect& bounds, std::uint64_t hash) {
    mix(hash);
    // A point of slack all round: the renderer rounds to whole pixels, and a
    // tile that lost a pixel row of a primitive has changed as much as one
    // that lost all of it.
    Rect b = bounds.inset(-1.0f);
    // Inside a scrolled region only what shows in it is anywhere at all: a
    // row scrolled out of the frame marks no tile and is not drawn.
    if (!open_.empty()) {
        const Rect& c = open_.back().clip;
        if (!overlaps(b, c)) return false;
        const float x0 = std::max(b.x, c.x), y0 = std::max(b.y, c.y);
        b = {x0, y0, std::min(b.right(), c.right()) - x0, std::min(b.bottom(), c.bottom()) - y0};
    }
    if (tile_ > 0 && !tiles_.empty() && b.right() > 0 && b.bottom() > 0 && b.x < float(width_) &&
        b.y < float(height_)) {
        const int c0 = std::clamp(int(std::floor(b.x / tile_)), 0, tileColumns_ - 1);
        const int c1 = std::clamp(int(std::floor(b.right() / tile_)), 0, tileColumns_ - 1);
        const int r0 = std::clamp(int(std::floor(b.y / tile_)), 0, tileRows_ - 1);
        const int r1 = std::clamp(int(std::floor(b.bottom() / tile_)), 0, tileRows_ - 1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                auto& t = tiles_[std::size_t(r) * std::size_t(tileColumns_) + std::size_t(c)];
                t ^= hash + 0x9e3779b97f4a7c15ULL + (t << 6) + (t >> 2);
            }
    }
    return painting_ && (!clipped_ || overlaps(b, clip_));
}

void Ui::paintOnly(const Rect* area) {
    clipped_ = area != nullptr;
    if (area) clip_ = *area;
    applyClip();
}

void Ui::applyClip() {
    if (!sdl_) return;
    if (!clipped_ && open_.empty()) {
        SDL_SetRenderClipRect(sdl_, nullptr);
        return;
    }
    Rect area = clipped_ ? clip_ : open_.back().clip;
    if (clipped_ && !open_.empty()) {
        const Rect& c = open_.back().clip;
        const float x0 = std::max(area.x, c.x), y0 = std::max(area.y, c.y);
        area = {x0, y0, std::max(0.0f, std::min(area.right(), c.right()) - x0),
                std::max(0.0f, std::min(area.bottom(), c.bottom()) - y0)};
    }
    // In points, as the renderer's scale takes them.
    const int x0 = int(std::floor(area.x)), y0 = int(std::floor(area.y));
    const int x1 = int(std::ceil(area.right())), y1 = int(std::ceil(area.bottom()));
    const SDL_Rect r{x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0)};
    SDL_SetRenderClipRect(sdl_, &r);
}

bool Ui::inClip(float x, float y) const {
    return open_.empty() || open_.back().clip.contains(x, y);
}

Rect Ui::beginScroll(std::uint64_t id, const Rect& frame) {
    Scroll& state = scrolls_[id];
    Rect clip = frame;
    if (!open_.empty()) {
        const Rect& outer = open_.back().clip;
        const float x0 = std::max(clip.x, outer.x), y0 = std::max(clip.y, outer.y);
        clip = {x0, y0, std::max(0.0f, std::min(clip.right(), outer.right()) - x0),
                std::max(0.0f, std::min(clip.bottom(), outer.bottom()) - y0)};
    }
    const float room = std::max(0.0f, state.content - frame.h);
    // The wheel over the frame is this region's, and nobody else's: taken, so
    // the world under it does not zoom and a region around it does not move.
    if (input_ && input_->wheel != 0 && room > 0 && clip.contains(input_->mouseX, input_->mouseY)) {
        state.offset -= input_->wheel * 48.0f;
        input_->wheel = 0;
    }
    state.offset = std::clamp(state.offset, 0.0f, room);
    state.frame = frame.h;
    mix(Hash().add(std::uint64_t(17)).add(id).add(state.offset).add(frame).h);
    const float top = frame.y - state.offset;
    open_.push_back({id, frame, clip, top});
    applyClip();
    return {frame.x, top, frame.w - (room > 0 ? kScrollBar : 0.0f), std::max(frame.h, state.content)};
}

void Ui::endScroll(float contentBottom) {
    if (open_.empty()) return;
    const OpenScroll done = open_.back();
    open_.pop_back();
    applyClip();
    Scroll& state = scrolls_[done.id];
    state.content = std::max(0.0f, contentBottom - done.top);
    const float room = std::max(0.0f, state.content - done.frame.h);
    claim(done.clip);
    if (room <= 0) {
        state.offset = 0;
        return;
    }
    // The bar: where in the content the frame is, and how much of it shows.
    const Rect track{done.frame.right() - kScrollBar + 3, done.frame.y + 2, kScrollBar - 5, done.frame.h - 4};
    const float shown = std::clamp(done.frame.h / state.content, 0.08f, 1.0f);
    const float thumbH = std::max(18.0f, track.h * shown);
    const float travel = std::max(1.0f, track.h - thumbH);
    const Rect thumb{track.x, track.y + travel * (state.offset / room), track.w, thumbH};
    const std::uint64_t thumbId = done.id ^ 0x5C501BA11ull;
    const bool over = input_ && inClip(input_->mouseX, input_->mouseY) &&
                      Rect{track.x - 3, track.y, track.w + 6, track.h}.contains(input_->mouseX, input_->mouseY);
    if (over) hot_ = thumbId;
    if (over && input_->pressed) {
        active_ = thumbId;
        input_->takenByUi = true;
        // On the thumb it is held where it was taken; on the track the thumb
        // jumps there first.
        state.grab = thumb.contains(input_->mouseX, input_->mouseY) ? input_->mouseY - thumb.y : thumbH * 0.5f;
    }
    if (active_ == thumbId && input_ && input_->down && state.grab >= 0) {
        const float at = std::clamp((input_->mouseY - state.grab - track.y) / travel, 0.0f, 1.0f);
        state.offset = at * room;
    }
    const Rect lit{track.x, track.y + travel * (state.offset / room), track.w, thumbH};
    roundRect(track, theme_.slot.withAlpha(0.8f), track.w * 0.5f);
    roundRect(lit, active_ == thumbId || over ? theme_.accent.withAlpha(0.85f) : theme_.slotEdge, track.w * 0.5f);
}

float Ui::scrollOffset(std::uint64_t id) const {
    const auto found = scrolls_.find(id);
    return found == scrolls_.end() ? 0.0f : found->second.offset;
}

void Ui::scrollTo(std::uint64_t id, float offset) { scrolls_[id].offset = std::max(0.0f, offset); }

void Ui::scrollIntoView(std::uint64_t id, float top, float bottom) {
    Scroll& state = scrolls_[id];
    // `top` and `bottom` are in the coordinates of the last build, which
    // began at frame.y - offset; as distances into the content:
    for (const auto& o : open_)
        if (o.id == id) {
            top -= o.top;
            bottom -= o.top;
            if (top < state.offset) state.offset = top;
            else if (bottom > state.offset + state.frame) state.offset = bottom - state.frame;
            return;
        }
}

std::vector<Rect> changedAreas(const std::vector<std::uint64_t>& before, const std::vector<std::uint64_t>& after,
                               int columns, int rows, float tile, float width, float height, std::size_t most) {
    const Rect whole{0, 0, width, height};
    if (columns <= 0 || rows <= 0 || tile <= 0 || after.size() != std::size_t(columns) * std::size_t(rows) ||
        before.size() != after.size())
        return {whole};
    const auto cut = [&](Rect r) {
        r.w = std::min(r.right(), width) - r.x;
        r.h = std::min(r.bottom(), height) - r.y;
        return r;
    };
    // Runs along each row, as [first, last] columns; a run that lines up with
    // one in the row above makes that one taller.
    struct Open { int c0, c1, r0, r1; };
    std::vector<Open> done, open, next;
    for (int r = 0; r < rows; ++r) {
        next.clear();
        for (int c = 0; c < columns;) {
            const std::size_t i = std::size_t(r) * std::size_t(columns) + std::size_t(c);
            if (before[i] == after[i]) { ++c; continue; }
            int end = c;
            while (end + 1 < columns && before[i + std::size_t(end + 1 - c)] != after[i + std::size_t(end + 1 - c)])
                ++end;
            bool grown = false;
            for (auto& o : open)
                if (o.c0 == c && o.c1 == end && o.r1 == r - 1) {
                    o.r1 = r;
                    next.push_back(o);
                    o.r1 = -2;   // taken
                    grown = true;
                    break;
                }
            if (!grown) next.push_back({c, end, r, r});
            c = end + 1;
        }
        for (const auto& o : open)
            if (o.r1 != -2) done.push_back(o);
        open.swap(next);
    }
    done.insert(done.end(), open.begin(), open.end());
    std::vector<Rect> out;
    out.reserve(done.size());
    for (const auto& o : done)
        out.push_back(cut({float(o.c0) * tile, float(o.r0) * tile, float(o.c1 - o.c0 + 1) * tile,
                           float(o.r1 - o.r0 + 1) * tile}));
    if (out.size() > most) {
        float x0 = width, y0 = height, x1 = 0, y1 = 0;
        for (const auto& r : out) {
            x0 = std::min(x0, r.x); y0 = std::min(y0, r.y);
            x1 = std::max(x1, r.right()); y1 = std::max(y1, r.bottom());
        }
        out = {cut({x0, y0, x1 - x0, y1 - y0})};
    }
    return out;
}

void Ui::setColour(const Colour& c) {
    SDL_SetRenderDrawColorFloat(sdl_, c.r, c.g, c.b, c.a);
}

void Ui::rect(const Rect& r, const Colour& c) {
    if (!record(r, Hash().add(std::uint64_t(1)).add(r).add(c).h)) return;
    setColour(c);
    const SDL_FRect box = r.sdl();
    SDL_RenderFillRect(sdl_, &box);
}

void Ui::roundRect(const Rect& r, const Colour& c, float radius) {
    if (!record(r, Hash().add(std::uint64_t(2)).add(r).add(c).add(radius).h)) return;
    const float rad = std::min({radius < 0 ? theme_.radius : radius, r.w * 0.5f, r.h * 0.5f});
    setColour(c);
    if (rad <= 0.5f) {
        const SDL_FRect box = r.sdl();
        SDL_RenderFillRect(sdl_, &box);
        return;
    }
    // In whole pixels of the surface.
    const float ppp = pixelsPerPoint_;
    const int x0 = int(std::lround(r.x * ppp)), x1 = int(std::lround(r.right() * ppp));
    const int y0 = int(std::lround(r.y * ppp)), y1 = int(std::lround(r.bottom() * ppp));
    if (x1 <= x0 || y1 <= y0) return;
    const float arc = std::min(rad * ppp, float(std::min(x1 - x0, y1 - y0)) * 0.5f);
    const int corner = int(std::ceil(arc));
    // How far in the arc is on the row `d` pixels from the straight edge.
    const auto inset = [&](int d) {
        const float dy = arc - (float(d) + 0.5f);
        if (dy <= 0) return 0;
        return int(std::lround(arc - std::sqrt(std::max(0.0f, arc * arc - dy * dy))));
    };
    spans_.clear();
    const auto span = [&](int row0, int rows, int in) {
        const int w = x1 - x0 - 2 * in;
        if (w <= 0 || rows <= 0) return;
        spans_.push_back({(float(x0 + in) + kSpanSlack) / ppp, (float(row0) + kSpanSlack) / ppp,
                          (float(w) + kSpanSlack) / ppp, (float(rows) + kSpanSlack) / ppp});
    };
    const int top = std::min(corner, (y1 - y0 + 1) / 2);
    const int bottom = std::min(corner, (y1 - y0) - top);
    for (int d = 0; d < top; ++d) span(y0 + d, 1, inset(d));
    span(y0 + top, (y1 - bottom) - (y0 + top), 0);
    for (int d = 0; d < bottom; ++d) span(y1 - 1 - d, 1, inset(d));
    if (!spans_.empty()) SDL_RenderFillRects(sdl_, spans_.data(), int(spans_.size()));
}

void Ui::border(const Rect& r, const Colour& c, float thickness, float radius) {
    if (!record(r, Hash().add(std::uint64_t(3)).add(r).add(c).add(thickness).h)) return;
    (void)radius;
    setColour(c);
    // Whole pixels: a one-point line on a two-pixel-per-point surface is two
    // pixels, drawn as two rings rather than one smeared across both.
    const float step = 1.0f / std::max(1.0f, std::round(pixelsPerPoint_));
    for (float i = 0; i < thickness; i += step) {
        const SDL_FRect box{r.x + i, r.y + i, r.w - i * 2, r.h - i * 2};
        SDL_RenderRect(sdl_, &box);
    }
}

void Ui::line(float x0, float y0, float x1, float y1, const Colour& c) {
    const Rect bounds{std::min(x0, x1), std::min(y0, y1), std::abs(x1 - x0), std::abs(y1 - y0)};
    if (!record(bounds, Hash().add(std::uint64_t(4)).add(x0).add(y0).add(x1).add(y1).add(c).h)) return;
    setColour(c);
    SDL_RenderLine(sdl_, x0, y0, x1, y1);
}

float Ui::textWidth(const std::string& s, float scale, bool bold) const {
    if (const FontAtlas* font = atlas(scale, bold)) return font->width(s) / pixelsPerPoint_;
    return static_cast<float>(s.size()) * glyphs_.cellWidth * scale * theme_.textScale;
}

float Ui::textHeight(float scale) const {
    if (const FontAtlas* font = atlas(scale, false)) {
        // Capital top to descender foot: what the eye reads as the line.
        const float cap = -font->glyph(U'H').top;
        const float descent = font->lineHeight() - font->ascent();
        return (cap + descent) / pixelsPerPoint_;
    }
    return glyphs_.cellHeight * scale * theme_.textScale;
}

std::string Ui::fit(const std::string& s, float width, float scale, bool bold) const {
    if (textWidth(s, scale, bold) <= width) return s;
    if (hasFont()) {
        // Whole codepoints off the end until it fits with its full stop.
        std::string cut = s;
        while (!cut.empty()) {
            cut.erase(cut.size() - lastCodepointBytes(cut));
            while (!cut.empty() && cut.back() == ' ') cut.pop_back();
            if (textWidth(cut + ".", scale, bold) <= width) return cut + ".";
        }
        return ".";
    }
    const float each = glyphs_.cellWidth * scale * theme_.textScale;
    if (each <= 0) return s;
    const std::size_t room = static_cast<std::size_t>(std::max(1.0f, width / each));
    if (s.size() <= room) return s;
    if (room <= 1) return ".";
    return s.substr(0, room - 1) + ".";
}

void Ui::text(float x, float y, const std::string& s, const Colour& c, float scale) {
    text(x, y, s, c, scale, false);
}

void Ui::text(float x, float y, const std::string& s, const Colour& c, float scale, bool bold) {
    if (audit_ && !s.empty()) {
        Rect seen{x, y, textWidth(s, scale, bold), textHeight(scale)};
        bool shows = true;
        if (!open_.empty()) {
            const Rect& c = open_.back().clip;
            shows = overlaps(seen, c);
            const float x0 = std::max(seen.x, c.x), y0 = std::max(seen.y, c.y);
            seen = {x0, y0, std::min(seen.right(), c.right()) - x0, std::min(seen.bottom(), c.bottom()) - y0};
        }
        if (shows) audit_->push_back({seen, s});
    }
    // Where it may land is only worth measuring when somebody asks.
    const bool placed = tile_ > 0 || clipped_ || !open_.empty();
    const Rect bounds = placed ? textBounds(x, y, s, scale, bold) : Rect{x, y, 0, 0};
    if (!record(bounds, Hash().add(std::uint64_t(5)).add(x).add(y).add(s).add(c).add(scale).add(std::uint64_t(bold)).h))
        return;
    if (const FontAtlas* font = atlas(scale, bold)) {
        // In pixels, on whole pixels: the glyphs were baked for exactly these.
        const float ppp = pixelsPerPoint_;
        const float cap = -font->glyph(U'H').top;
        float pen = std::round(x * ppp);
        const float baseline = std::round(y * ppp + cap);
        SDL_SetTextureColorModFloat(font->texture(), c.r, c.g, c.b);
        SDL_SetTextureAlphaModFloat(font->texture(), c.a);
        const FontAtlas::Glyph* previous = nullptr;
        for (std::size_t at = 0; at < s.size();) {
            const FontAtlas::Glyph& g = font->glyph(nextCodepoint(s, at));
            if (previous) pen += font->kerning(*previous, g);
            if (g.from.w > 0 && g.from.h > 0) {
                const SDL_FRect to{(std::round(pen) + g.left) / ppp, (baseline + g.top) / ppp,
                                   g.from.w / ppp, g.from.h / ppp};
                SDL_RenderTexture(sdl_, font->texture(), &g.from, &to);
            }
            pen += g.advance;
            previous = &g;
        }
        return;
    }
    if (!glyphs_.texture) return;
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

void Ui::textRight(float rightX, float y, const std::string& s, const Colour& c, float scale, bool bold) {
    text(rightX - textWidth(s, scale, bold), y, s, c, scale, bold);
}

Rect Ui::textBounds(float x, float y, const std::string& s, float scale, bool bold) const {
    const float w = textWidth(s, scale, bold), h = textHeight(scale);
    if (!hasFont()) return {x, y, w, h};
    // The line is capital top to descender foot; accents stand above the
    // capitals and a first letter may overhang to the left, so the box is
    // generous rather than exact - a tile too many costs nothing.
    return {x - 3, y - h * 0.6f, w + 6, h * 1.9f};
}

void Ui::textCentred(const Rect& in, const std::string& s, const Colour& c, float scale, bool bold) {
    float height = textHeight(scale);
    if (const FontAtlas* font = atlas(scale, bold)) {
        // The capitals centred, not the line: a label sits in the middle of
        // its button whatever its descenders do.
        height = -font->glyph(U'H').top / pixelsPerPoint_;
    }
    text(in.x + (in.w - textWidth(s, scale, bold)) * 0.5f, in.y + (in.h - height) * 0.5f, s, c, scale, bold);
}

float Ui::paragraph(float x, float y, float width, const std::string& s, const Colour& c, float scale) {
    const float advance = hasFont() ? [&] {
        const FontAtlas* font = atlas(scale, false);
        return font ? font->lineHeight() / pixelsPerPoint_ : textHeight(scale) * 1.3f;
    }() : textHeight(scale) * 1.4f;
    float top = y;
    std::string line;
    std::size_t at = 0;
    const auto flush = [&] {
        if (line.empty()) return;
        text(x, top, line, c, scale);
        top += advance;
        line.clear();
    };
    while (at <= s.size()) {
        const std::size_t space = s.find_first_of(" \n", at);
        const std::size_t end = space == std::string::npos ? s.size() : space;
        const std::string word = s.substr(at, end - at);
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && textWidth(tried, scale) > width) {
            flush();
            line = word;
        } else {
            line = tried;
        }
        if (space != std::string::npos && s[space] == '\n') flush();
        if (space == std::string::npos) break;
        at = space + 1;
    }
    flush();
    return top - y;
}

void Ui::image(const Rect& r, SDL_Texture* texture, const Colour& tint) {
    if (!record(r, Hash().add(std::uint64_t(6)).add(r).add(std::uint64_t(reinterpret_cast<std::uintptr_t>(texture)))
                           .add(tint).h) ||
        !texture)
        return;
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
    return input_ != nullptr && r.contains(input_->mouseX, input_->mouseY) && inClip(input_->mouseX, input_->mouseY);
}

void Ui::claim(const Rect& r) {
    if (input_ != nullptr && r.contains(input_->mouseX, input_->mouseY) && inClip(input_->mouseX, input_->mouseY))
        input_->overUi = true;
}

void Ui::note(const Rect& r, const std::string& label) {
    if (!audit_) return;
    // What a scrolled region hides is not on the screen to lie on anything:
    // the audit sees what shows, cut to the frame.
    Rect seen = r;
    if (!open_.empty()) {
        const Rect& c = open_.back().clip;
        if (!overlaps(r, c)) return;
        const float x0 = std::max(r.x, c.x), y0 = std::max(r.y, c.y);
        seen = {x0, y0, std::min(r.right(), c.right()) - x0, std::min(r.bottom(), c.bottom()) - y0};
    }
    audit_->push_back({seen, "\x01" + label});
}

bool Ui::pressedOn(std::uint64_t id, bool over) {
    if (over) hot_ = id;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    return active_ == id && input_->released && over;
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
    note(r, label);
    const bool over = enabled && hovered(r);
    const bool clicked = enabled && pressedOn(id, over);

    const Colour face = !enabled ? theme_.slot.withAlpha(0.5f)
                       : active_ == id && over ? theme_.slotDown
                       : over ? theme_.slotHot
                              : theme_.slot;
    roundRect(r, face, 4.0f);
    border(r, over ? theme_.accent.withAlpha(0.8f) : theme_.slotEdge);
    textCentred(r, fit(label, r.w - 8), enabled ? theme_.label : theme_.labelSoft);
    claim(r);
    return clicked;
}

bool Ui::button(std::uint64_t id, const Rect& r, const std::string& label, const Colour& face, const Colour& ink,
                bool enabled) {
    note(r, label);
    const bool over = enabled && hovered(r);
    const bool clicked = enabled && pressedOn(id, over);
    const Colour lit = face.lerp(Colour::rgb(255, 255, 255, face.a), 0.14f);
    const Colour down = face.lerp(Colour::rgb(0, 0, 0, face.a), 0.18f);
    const Colour fill = !enabled ? face.withAlpha(face.a * 0.45f) : active_ == id && over ? down : over ? lit : face;
    roundRect(r, fill, 4.0f);
    border(r, over ? Colour::rgb(255, 246, 214, 0.9f) : down);
    textCentred(r, fit(label, r.w - 8), enabled ? ink : ink.withAlpha(0.55f), 1.0f, true);
    claim(r);
    return clicked;
}

bool Ui::stepper(std::uint64_t id, const Rect& r, std::int32_t& value, std::int32_t low, std::int32_t high,
                 std::int32_t step) {
    const std::int32_t was = value;
    const float side = std::min(r.h, std::floor(r.w / 3.0f));
    if (button(id ^ 0x5E7D0001ull, {r.x, r.y, side, r.h}, "\u2212", value > low)) value = std::max(low, value - step);
    if (button(id ^ 0x5E7D0002ull, {r.right() - side, r.y, side, r.h}, "+", value < high))
        value = std::min(high, value + step);
    const Rect middle{r.x + side + 4, r.y, r.w - 2 * side - 8, r.h};
    roundRect(middle, theme_.slot.withAlpha(0.6f), 4.0f);
    textCentred(middle, std::to_string(value), theme_.label, 1.0f, true);
    note(middle, std::to_string(value));
    return value != was;
}

bool Ui::toggle(std::uint64_t id, const Rect& r, const std::string& label, bool active, bool enabled) {
    note(r, label);
    const bool over = enabled && hovered(r);
    const bool clicked = enabled && pressedOn(id, over);
    const Colour face = !enabled ? theme_.slot.withAlpha(0.5f)
                       : active ? theme_.slotDown
                       : active_ == id && over ? theme_.slotDown
                       : over ? theme_.slotHot
                              : theme_.slot;
    roundRect(r, face, 4.0f);
    border(r, active ? theme_.accent : over ? theme_.accent.withAlpha(0.6f) : theme_.slotEdge);
    textCentred(r, fit(label, r.w - 8), !enabled ? theme_.labelSoft : active ? theme_.accent : theme_.label, 1.0f,
                active);
    claim(r);
    return clicked;
}

bool Ui::iconButton(std::uint64_t id, const Rect& r, SDL_Texture* icon, const std::string& fallback,
                    bool active) {
    const bool over = hovered(r);
    const bool clicked = pressedOn(id, over);
    roundRect(r, active ? theme_.slotDown : over ? theme_.slotHot : theme_.slot, 4.0f);
    border(r, active ? theme_.accent : theme_.slotEdge);
    if (icon) image(r.inset(3), icon);
    else textCentred(r, fallback, theme_.label);
    claim(r);
    return clicked;
}

bool Ui::tab(std::uint64_t id, const Rect& r, const std::string& label, bool active) {
    note(r, label);
    const bool over = hovered(r);
    const bool clicked = pressedOn(id, over);

    // A tab is part of the bar it belongs to: square at the bottom, lit along the
    // top when it is the one showing.
    rect(r, active ? theme_.slotHot : over ? theme_.slot : Colour::rgb(0, 0, 0, 0.0f));
    if (active) rect({r.x, r.y, r.w, 2}, theme_.accent);
    textCentred(r, fit(label, r.w - 6), active ? theme_.label : theme_.labelSoft);
    claim(r);
    return clicked;
}

bool Ui::card(std::uint64_t id, const Rect& r, SDL_Texture* icon, const std::string& label,
              bool active) {
    const bool over = hovered(r);
    const bool clicked = pressedOn(id, over);

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
    bool changed = false;
    if (pressedOn(id, over)) {
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
        text(r.x + 6, r.y + (r.h - textHeight()) * 0.5f, fit(options[std::size_t(chosen)], r.w - 26),
             theme_.ink);
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
            text(row.x + 6, row.y + (row.h - textHeight()) * 0.5f, fit(options[i], row.w - 10), theme_.ink);
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

bool Ui::slider(std::uint64_t id, const Rect& r, float& value, float low, float high, bool logarithmic) {
    note(r, "slider");
    const bool over = hovered(r);
    if (over) hot_ = id;
    if (over && input_->pressed) {
        active_ = id;
        input_->takenByUi = true;
    }
    const bool log = logarithmic && low > 0 && high > low;
    const float knob = std::min(12.0f, r.h);
    bool changed = false;
    if (active_ == id && input_->down) {
        const float t = std::clamp((input_->mouseX - r.x - knob * 0.5f) / std::max(1.0f, r.w - knob), 0.0f, 1.0f);
        const float next = log ? low * std::pow(high / low, t) : low + (high - low) * t;
        if (next != value) {
            value = next;
            changed = true;
        }
        input_->takenByUi = true;
    }
    const float span = log ? std::log(high / low) : high - low;
    const float t = span > 0 ? std::clamp(log ? std::log(std::max(value, low) / low) / span : (value - low) / span,
                                          0.0f, 1.0f)
                             : 0.0f;
    const float cy = r.y + r.h * 0.5f;
    const Rect track{r.x, cy - 2, r.w, 4};
    roundRect(track, theme_.gauge, 2);
    roundRect({track.x, track.y, knob * 0.5f + (r.w - knob) * t, track.h}, theme_.accent.withAlpha(0.85f), 2);
    const Rect handle{r.x + (r.w - knob) * t, cy - knob * 0.5f, knob, knob};
    roundRect(handle, active_ == id ? theme_.selection : over ? theme_.label : theme_.paperShade, knob * 0.5f);
    border(handle, theme_.slotEdge);
    claim(r);
    return changed;
}

bool Ui::textField(std::uint64_t id, const Rect& r, std::string& value, const std::string& placeholder,
                   std::size_t maxBytes) {
    note(r, "field " + placeholder);
    const bool over = hovered(r);
    if (over) hot_ = id;
    if (input_->pressed) {
        if (over) {
            focus_ = id;
            input_->takenByUi = true;
        } else if (focus_ == id) {
            focus_ = 0;
        }
    }
    bool changed = false;
    const bool focused = focus_ == id;
    if (focused) {
        focusSeen_ = true;
        for (std::size_t at = 0; at < input_->text.size();) {
            const std::size_t from = at;
            const char32_t cp = nextCodepoint(input_->text, at);
            if (cp < 32 || cp == 127) continue;
            if (value.size() + (at - from) > maxBytes) break;
            value.append(input_->text, from, at - from);
            changed = true;
        }
        if (input_->backspace && !value.empty()) {
            value.erase(value.size() - lastCodepointBytes(value));
            changed = true;
        }
        if (input_->enter || input_->escape || input_->tab) focus_ = 0;
    }
    roundRect(r, focused ? theme_.paper : theme_.paperShade, 3.0f);
    border(r, focused ? theme_.accent : over ? theme_.inkSoft : theme_.paperEdge, focused ? 2.0f : 1.0f);
    const float pad = 8;
    const float room = r.w - pad * 2 - 4;
    const float y = r.y + (r.h - textHeight()) * 0.5f;
    if (value.empty()) {
        if (!focused) text(r.x + pad, y, fit(placeholder, room), theme_.inkSoft);
    } else {
        // The end of what is being typed, when it is longer than the box.
        std::string shown = value;
        while (!shown.empty() && textWidth(shown) > room) {
            std::size_t at = 0;
            nextCodepoint(shown, at);
            shown.erase(0, at);
        }
        text(r.x + pad, y, shown, theme_.ink);
    }
    if (focused) {
        const float caret = r.x + pad + (value.empty() ? 0.0f : std::min(room, textWidth(value))) + 1;
        rect({caret, r.y + 6, 2, r.h - 12}, theme_.ink);
    }
    claim(r);
    return changed;
}

bool Ui::selectable(std::uint64_t id, const Rect& r, bool selected) {
    const bool over = hovered(r);
    const bool clicked = pressedOn(id, over);
    if (selected || over)
        roundRect(r, selected ? theme_.slotDown : theme_.slotHot.withAlpha(0.7f), 4.0f);
    if (selected) {
        border(r, theme_.accent.withAlpha(0.7f));
        roundRect({r.x, r.y + 4, 3, r.h - 8}, theme_.accent, 1.5f);
    }
    claim(r);
    return clicked;
}

} // namespace ui

