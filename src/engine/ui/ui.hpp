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

#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

namespace ui {

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
    // Set by the interface: the world must not act on a click the HUD has taken.
    bool overUi = false;
    bool takenByUi = false;
};

// A handle for one widget, so hover and press survive between frames without a
// tree. Made from a name and an index: "build.card" #7.
std::uint64_t widgetId(const char* name, int index = 0);

class Ui {
public:
    bool init(SDL_Renderer* renderer);
    void shutdown();

    Theme& theme() { return theme_; }
    const Theme& theme() const { return theme_; }

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

    // --- drawing ---------------------------------------------------------
    void rect(const Rect& r, const Colour& c);
    void roundRect(const Rect& r, const Colour& c, float radius = -1.0f);
    void border(const Rect& r, const Colour& c, float thickness = 1.0f, float radius = -1.0f);
    void line(float x0, float y0, float x1, float y1, const Colour& c);
    void text(float x, float y, const std::string& s, const Colour& c, float scale = 1.0f);
    void textRight(float rightX, float y, const std::string& s, const Colour& c, float scale = 1.0f);
    void textCentred(const Rect& in, const std::string& s, const Colour& c, float scale = 1.0f);
    float textWidth(const std::string& s, float scale = 1.0f) const;
    // As much of the text as fits the width, with a full stop where it was cut.
    // A caption that runs past its card is worse than one that stops.
    std::string fit(const std::string& s, float width, float scale = 1.0f) const;
    float textHeight(float scale = 1.0f) const;
    // A picture from the sprite library, fitted inside the rectangle.
    void image(const Rect& r, SDL_Texture* texture, const Colour& tint = Colour{});

    // --- widgets ---------------------------------------------------------
    // Every one of them takes the rectangle it lives in: layout is the caller's
    // business, because a HUD's layout is a design, not a algorithm.
    void panel(const Rect& r, bool paper = false);
    bool button(std::uint64_t id, const Rect& r, const std::string& label, bool enabled = true);
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

    SDL_Renderer* sdl_ = nullptr;
    bool painting_ = true;
    Theme theme_;
    Input* input_ = nullptr;
    int width_ = 0, height_ = 0;
    Glyphs glyphs_;
    std::uint64_t hot_ = 0;        // under the pointer
    std::uint64_t active_ = 0;     // being pressed
    std::uint64_t openMenu_ = 0;   // the dropdown that is showing its options
};

} // namespace ui
