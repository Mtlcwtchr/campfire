#pragma once
// A TrueType face for the interface, and its glyphs baked for one renderer.
//
// The interface used SDL's eight-pixel debug font, scaled. It is legible at a
// glance and tiring to read: every letter is the same width, nothing is
// antialiased, and on a dense screen it is either tiny or a mosaic. A real face
// is a file and a rasteriser (stb_truetype, third_party/stb), and the glyphs are
// baked once per pixel size into a texture of the renderer that draws them - so
// text is drawn at exactly the pixels it lands on, however the points map to
// pixels on this display.
//
// Latin, Latin-1, Cyrillic and the handful of typographic marks an interface
// writes (dashes, quotes, the ellipsis, the bullet, arrows). A codepoint outside
// them draws as a question mark rather than as nothing.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

namespace ui {

// One face, read from its file once and shared by every renderer that bakes it.
class FontFace {
public:
    // Nothing when the file cannot be read or is not a font.
    static std::shared_ptr<FontFace> load(const std::filesystem::path& file);
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;

    // The face's vertical metrics as fractions of its pixel height.
    float ascent() const { return ascent_; }
    float descent() const { return descent_; }   // negative
    float lineGap() const { return lineGap_; }
    const void* info() const { return info_.get(); }

private:
    FontFace() = default;
    std::vector<unsigned char> bytes_;
    std::shared_ptr<void> info_;
    float ascent_ = 0.8f, descent_ = -0.2f, lineGap_ = 0;
};

// The glyphs of one face at one pixel size, in one renderer's texture.
class FontAtlas {
public:
    struct Glyph {
        SDL_FRect from{};          // in the atlas, pixels
        float left = 0, top = 0;   // from the pen on the baseline to the bitmap's corner, pixels
        float advance = 0;         // pixels
        int index = 0;             // the face's glyph index, for kerning
    };
    FontAtlas(std::shared_ptr<FontFace> face, SDL_Renderer* renderer, int pixelSize);
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    explicit operator bool() const { return texture_ != nullptr; }
    int pixelSize() const { return size_; }
    float ascent() const { return ascent_; }       // pixels above the baseline
    float lineHeight() const { return line_; }     // pixels from one baseline to the next
    SDL_Texture* texture() const { return texture_; }
    // The glyph for a codepoint, or the question mark's.
    const Glyph& glyph(char32_t codepoint) const;
    float kerning(const Glyph& left, const Glyph& right) const;
    // How wide this text is, in pixels, laid out as draw() lays it out.
    float width(std::string_view utf8) const;

private:
    std::shared_ptr<FontFace> face_;
    SDL_Texture* texture_ = nullptr;
    int size_ = 0;
    float scale_ = 1, ascent_ = 0, line_ = 0;
    std::unordered_map<char32_t, Glyph> glyphs_;
    Glyph missing_{};
};

// UTF-8, one codepoint at a time. Malformed bytes come out as U+FFFD, one each,
// so a broken string is drawn broken rather than swallowing what follows.
char32_t nextCodepoint(std::string_view text, std::size_t& at);
// The byte length of the last codepoint of a UTF-8 string (what a backspace
// removes), or 0 for an empty one.
std::size_t lastCodepointBytes(std::string_view text);

} // namespace ui

