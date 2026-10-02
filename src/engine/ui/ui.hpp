#pragma once
// The interface layer: drawing, input and widgets.
//
// Everything the player touches goes through this. It is immediate mode - the
// HUD is rebuilt every frame from the state of the world - but it keeps what
// immediate mode needs to feel solid: hover and press are remembered between
// frames, the panel under the cursor swallows the click before the world sees
// it, and the look of every part is a value in a theme rather than a number
// buried in a draw call.
//
// There is no widget tree and no retained layout on purpose. A settlement's HUD
// is a handful of panels whose contents change every frame; a tree would have to
// be rebuilt anyway, and a tree that is rebuilt every frame is a draw list with
// extra steps.
//
// Writing is SDL's eight-pixel debug font unless a TrueType face is set
// (setFont, font.hpp); then it is that face, baked at the pixels it lands on.
// Layout is always in points: a surface with more pixels than points (a Retina
// display) is drawn with the renderer's scale set to match, and the face is
// baked for the pixels, so text stays sharp at any density.
//
// Every primitive also folds what it would draw into `signature()`, painting or
// not. A caller that keeps its picture between frames builds the interface with
// painting off, compares the number, and paints only when the picture changed.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

namespace ui {

class FontFace;
class FontAtlas;

struct Colour {
    float r = 1, g = 1, b = 1, a = 1;
    constexpr Colour() = default;
    constexpr Colour(float rr, float gg, float bb, float aa = 1.0f) : r(rr), g(gg), b(bb), a(aa) {}
    // From the eight-bit values a designer actually writes down.
    static constexpr Colour rgb(int r, int g, int b, float a = 1.0f) {
        return {r / 255.0f, g / 255.0f, b / 255.0f, a};
    }
    constexpr Colour withAlpha(float alpha) const { return {r, g, b, alpha}; }
    Colour lerp(const Colour& other, float t) const {
        return {r + (other.r - r) * t, g + (other.g - g) * t, b + (other.b - b) * t,
                a + (other.a - a) * t};
    }
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    Rect inset(float by) const { return {x + by, y + by, w - by * 2, h - by * 2}; }
    Rect inset(float left, float top, float right, float bottom) const {
        return {x + left, y + top, w - left - right, h - top - bottom};
    }
    SDL_FRect sdl() const { return {x, y, w, h}; }
};

// How the whole interface looks. One place, so that changing the game's palette
// is changing a value here (or, later, a file) rather than hunting through draw
// calls - and so the editor can put a colour picker on every one of them.
struct Theme {
    // The dark furniture: the bars along the top and bottom.
    Colour barTop = Colour::rgb(28, 24, 21, 0.96f);
    Colour barBottom = Colour::rgb(24, 21, 18, 0.96f);
    Colour barEdge = Colour::rgb(96, 82, 62);
    Colour slot = Colour::rgb(46, 40, 34);
    Colour slotHot = Colour::rgb(66, 57, 46);
    Colour slotDown = Colour::rgb(86, 74, 58);
    Colour slotEdge = Colour::rgb(104, 88, 66);
    // Parchment: the panels that carry writing.
    Colour paper = Colour::rgb(232, 224, 203);
    Colour paperEdge = Colour::rgb(120, 104, 78);
    Colour paperShade = Colour::rgb(214, 203, 178);
    // Words.
    Colour ink = Colour::rgb(38, 32, 25);
    Colour inkSoft = Colour::rgb(94, 84, 68);
    Colour label = Colour::rgb(226, 216, 196);
    Colour labelSoft = Colour::rgb(150, 138, 120);
    Colour accent = Colour::rgb(228, 178, 74);
    Colour danger = Colour::rgb(178, 66, 52);
    // Bars that say how much of something there is.
    Colour good = Colour::rgb(112, 158, 74);
    Colour warn = Colour::rgb(214, 158, 58);
    Colour bad = Colour::rgb(178, 74, 56);
    Colour gauge = Colour::rgb(58, 52, 44);
    Colour selection = Colour::rgb(255, 246, 214);

    float radius = 5.0f;          // how round a panel's corners are
    float rowHeight = 22.0f;
    float pad = 8.0f;
    float textScale = 1.0f;       // the base size of a line of writing
};

// What the mouse and keyboard did this frame, in one place. Filled by the client
// from SDL events before the interface is built.
struct Input {
    float mouseX = 0, mouseY = 0;
    float wheel = 0;
    bool down = false;            // left button held
    bool pressed = false;         // went down this frame
    bool released = false;        // came up this frame
    bool rightPressed = false;
    bool rightDown = false;
    bool middleDown = false;
    bool shift = false;
    bool ctrl = false;
    // The left button's press this frame was the second of a double click.
    bool doubleClick = false;
    // Typing, for the text field that has the focus: what was typed this frame
    // (UTF-8) and the keys that edit or leave it.
    std::string text;
    bool backspace = false, enter = false, escape = false, tab = false;
    // Set by the interface: the world must not act on a click the HUD has taken.
    bool overUi = false;
    bool takenByUi = false;
};

// A handle for one widget, so hover and press survive between frames without a
// tree. Made from a name and an index: "build.card" #7.
std::uint64_t widgetId(const char* name, int index = 0);

class Ui {
public:
    Ui();
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    bool init(SDL_Renderer* renderer);
    void shutdown();

    Theme& theme() { return theme_; }
    const Theme& theme() const { return theme_; }

    // A TrueType face (font.hpp) for the writing, regular and bold: a line at
    // scale 1 is `points` tall in points, and a point is `pixelsPerPoint`
    // pixels on the surface this draws into (the renderer's scale is the
    // caller's to set to the same). Faces are shared; glyphs are baked per
    // renderer and size on first use.
    void setFont(std::shared_ptr<FontFace> regular, std::shared_ptr<FontFace> bold, float points,
                 float pixelsPerPoint = 1.0f);
    bool hasFont() const { return regular_ != nullptr; }
    float pixelsPerPoint() const { return pixelsPerPoint_; }

    // A frame: hand it the input, build the interface, then finish.
    void begin(Input& input, int viewportWidth, int viewportHeight);
    void end();

    const Input& input() const { return *input_; }
    int width() const { return width_; }
    int height() const { return height_; }
    SDL_Renderer* renderer() const { return sdl_; }
    // Retained surfaces can reuse their pixels while widgets still process input.
    // The caller owns invalidation; ordinary immediate-mode callers leave this on.
    void painting(bool enabled) { painting_ = enabled; }
    // What this frame drew (or would have drawn), as one number.
    std::uint64_t signature() const { return signature_; }
    // A widget holds the pointer (a button being pressed, a slider dragged):
    // the world must not act on the drag even off the widget.
    bool capturing() const { return active_ != 0; }
    // A text field has the keyboard: keys are words, not commands.
    bool typing() const { return focus_ != 0; }
    // Every line of writing this frame, where it went and what it said, into
    // `sink` (null: none), and every control's rectangle (its text starts
    // with \x01). What a layout check reads: two lines on top of each other,
    // two controls on top of each other, or either off the edge, is a layout
    // bug nobody has to see.
    struct TextBox { Rect box; std::string text; };
    void audit(std::vector<TextBox>* sink) { audit_ = sink; }
    void unfocus() { focus_ = 0; }
    void focus(std::uint64_t id) { focus_ = id; }

    // --- where the picture changed ---------------------------------------
    // With a tile size set (points; 0 is off), every primitive also folds
    // itself into the signature of each tile its pixels may land on, so two
    // frames' tiles say not only THAT the picture changed but WHERE. A caller
    // that keeps its picture repaints only there (changedAreas, paintOnly): a
    // lit button or a number in the status bar costs its own few tiles, not a
    // whole window of software blending.
    void trackTiles(float points) { tile_ = points > 0 ? points : 0; }
    float tileSize() const { return tile_; }
    int tileColumns() const { return tileColumns_; }
    int tileRows() const { return tileRows_; }
    const std::vector<std::uint64_t>& tileSignatures() const { return tiles_; }
    // Painting clipped to this rectangle (points), and whatever lies wholly
    // outside it not drawn at all; null paints everywhere again. Kept across
    // frames until changed.
    void paintOnly(const Rect* area);

    // --- drawing ---------------------------------------------------------
    void rect(const Rect& r, const Colour& c);
    void roundRect(const Rect& r, const Colour& c, float radius = -1.0f);
    void border(const Rect& r, const Colour& c, float thickness = 1.0f, float radius = -1.0f);
    void line(float x0, float y0, float x1, float y1, const Colour& c);
    void text(float x, float y, const std::string& s, const Colour& c, float scale = 1.0f);
    void text(float x, float y, const std::string& s, const Colour& c, float scale, bool bold);
    void textRight(float rightX, float y, const std::string& s, const Colour& c, float scale = 1.0f,
                   bool bold = false);
    void textCentred(const Rect& in, const std::string& s, const Colour& c, float scale = 1.0f,
                     bool bold = false);
    float textWidth(const std::string& s, float scale = 1.0f, bool bold = false) const;
    // As much of the text as fits the width, with a full stop where it was cut.
    // A caption that runs past its card is worse than one that stops.
    std::string fit(const std::string& s, float width, float scale = 1.0f, bool bold = false) const;
    // The height of a line of writing: the glyph cell of the debug font, or
    // the face's ascent to descent plus its line gap.
    float textHeight(float scale = 1.0f) const;
    // Words laid out over as many lines as they need inside `width`; returns
    // the height they took. Breaks at spaces only.
    float paragraph(float x, float y, float width, const std::string& s, const Colour& c, float scale = 1.0f);
    // A picture from the sprite library, fitted inside the rectangle.
    void image(const Rect& r, SDL_Texture* texture, const Colour& tint = Colour{});

    // --- widgets ---------------------------------------------------------
    // Every one of them takes the rectangle it lives in: layout is the caller's
    // business, because a HUD's layout is a design, not a algorithm.
    void panel(const Rect& r, bool paper = false);
    bool button(std::uint64_t id, const Rect& r, const std::string& label, bool enabled = true);
    // The same, in colours of the caller's: the one action a screen is for
    // (accent), or the one that destroys something (danger).
    bool button(std::uint64_t id, const Rect& r, const std::string& label, const Colour& face, const Colour& ink,
                bool enabled = true);
    // A button that stays lit while `active`: a tool, a tab of a strip, one
    // choice of several.
    bool toggle(std::uint64_t id, const Rect& r, const std::string& label, bool active, bool enabled = true);
    bool iconButton(std::uint64_t id, const Rect& r, SDL_Texture* icon, const std::string& fallback,
                    bool active = false);
    bool tab(std::uint64_t id, const Rect& r, const std::string& label, bool active);
    bool card(std::uint64_t id, const Rect& r, SDL_Texture* icon, const std::string& label,
              bool active);
    bool checkbox(std::uint64_t id, const Rect& r, const std::string& label, bool& value);
    void gauge(const Rect& r, float fraction, const Colour& fill, const Colour& back);
    void gaugeLabelled(const Rect& r, const std::string& label, float fraction, const Colour& fill);
    // Returns the index chosen, or -1. Draws as a row of choices in a box.
    int dropdown(std::uint64_t id, const Rect& r, const std::vector<std::string>& options,
                 int chosen);
    // A track with a knob, `value` between `low` and `high` (logarithmically
    // for ranges that span orders of magnitude). True while it moved.
    bool slider(std::uint64_t id, const Rect& r, float& value, float low, float high, bool logarithmic = false);
    // One line of text to type into. Clicking it takes the keyboard; Enter,
    // Escape, Tab or a click elsewhere gives it back. True when it changed.
    bool textField(std::uint64_t id, const Rect& r, std::string& value, const std::string& placeholder = {},
                   std::size_t maxBytes = 64);
    // A whole number between `low` and `high`, with a button either side to
    // step it (held: repeating) and the number between them. True when it
    // changed.
    bool stepper(std::uint64_t id, const Rect& r, std::int32_t& value, std::int32_t low, std::int32_t high,
                 std::int32_t step = 1);
    // A row of a list: lit under the pointer, marked when selected. The
    // caller writes its contents over it. True when clicked.
    bool selectable(std::uint64_t id, const Rect& r, bool selected);
    // --- scrolling ---------------------------------------------------------
    // A region with more in it than it has room for. Between these two calls
    // the caller lays its widgets out from the returned rectangle's top, as if
    // there were room - `content.w` wide, which leaves the bar its strip when
    // there is anything to scroll - and ends it with the lowest point it laid
    // anything at. What falls outside `frame` is neither drawn nor pointed at
    // (and costs nothing to repaint), the wheel over the frame moves it, and a
    // bar down its right edge can be dragged or clicked. The offset is kept
    // per id from frame to frame. They nest.
    //
    //     const ui::Rect content = ui.beginScroll(id, frame);
    //     float y = content.y;
    //     ... widgets at y, y += ...
    //     ui.endScroll(y);
    static constexpr float kScrollBar = 10.0f;
    Rect beginScroll(std::uint64_t id, const Rect& frame);
    void endScroll(float contentBottom);
    // How far a region is scrolled, and scrolled to (clamped when next built).
    float scrollOffset(std::uint64_t id) const;
    void scrollTo(std::uint64_t id, float offset);
    // Brings the band [top, bottom) of a region's content (as laid out, in the
    // coordinates beginScroll handed out) into view, the least way.
    void scrollIntoView(std::uint64_t id, float top, float bottom);

    // Is the pointer inside this rectangle and not over something in front?
    bool hovered(const Rect& r) const;
    // Tells the world that this area belongs to the interface.
    void claim(const Rect& r);

private:
    struct Glyphs {
        SDL_Texture* texture = nullptr;
        int cellWidth = 8, cellHeight = 8;
        int columns = 16;
    };
    void buildGlyphs();
    void setColour(const Colour& c);
    const FontAtlas* atlas(float scale, bool bold) const;
    void mix(std::uint64_t value);
    // One primitive: its hash into the signature and into every tile under
    // `bounds`; then whether to draw it (painting, and not clipped away).
    bool record(const Rect& bounds, std::uint64_t hash);
    Rect textBounds(float x, float y, const std::string& s, float scale, bool bold) const;
    bool pressedOn(std::uint64_t id, bool over);
    void note(const Rect& r, const std::string& label);

    SDL_Renderer* sdl_ = nullptr;
    bool painting_ = true;
    Theme theme_;
    Input* input_ = nullptr;
    int width_ = 0, height_ = 0;
    Glyphs glyphs_;
    std::uint64_t hot_ = 0;        // under the pointer
    std::uint64_t active_ = 0;     // being pressed
    std::uint64_t openMenu_ = 0;   // the dropdown that is showing its options
    std::uint64_t focus_ = 0;      // the text field with the keyboard
    bool focusSeen_ = false;       // it was built this frame
    std::uint64_t signature_ = 0;
    std::vector<TextBox>* audit_ = nullptr;
    // Tiles and the clip.
    float tile_ = 0;
    int tileColumns_ = 0, tileRows_ = 0;
    std::vector<std::uint64_t> tiles_;
    bool clipped_ = false;
    Rect clip_{};
    // The scrolled regions being built, innermost last: each one's frame cut
    // by the ones around it.
    struct Scroll {
        float offset = 0, content = 0, frame = 0;   // as last built
        float grab = -1;                            // where on the thumb it was taken
    };
    struct OpenScroll {
        std::uint64_t id;
        Rect frame;       // on screen
        Rect clip;        // the frame, cut by everything around it
        float top;        // where its content starts, as laid out
    };
    std::map<std::uint64_t, Scroll> scrolls_;
    std::vector<OpenScroll> open_;
    // Where drawing may land now: paintOnly and every open scroll, together.
    void applyClip();
    bool inClip(float x, float y) const;
    std::vector<SDL_FRect> spans_;   // a rounded rectangle's rows, reused

    std::shared_ptr<FontFace> regular_, bold_;
    float fontPoints_ = 14.0f, pixelsPerPoint_ = 1.0f;
    mutable std::map<std::pair<bool, int>, std::unique_ptr<FontAtlas>> atlases_;
};

// Where two frames' tiles (Ui::tileSignatures) differ, as a few rectangles in
// points: the runs of changed tiles along each row, merged down the rows while
// they line up, cut to `width` x `height`. Tiles of different frames that do
// not match in number are all changed. More than `most` rectangles become the
// one that holds them: past that, the passes cost more than the pixels.
std::vector<Rect> changedAreas(const std::vector<std::uint64_t>& before, const std::vector<std::uint64_t>& after,
                               int columns, int rows, float tile, float width, float height, std::size_t most = 12);

} // namespace ui

