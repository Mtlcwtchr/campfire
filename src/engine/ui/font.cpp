#include "engine/ui/font.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace ui {
namespace {

const stbtt_fontinfo& infoOf(const FontFace& face) {
    return *static_cast<const stbtt_fontinfo*>(face.info());
}

// What an interface writes. Contiguous runs, so each is one packing range.
struct Run { char32_t first, last; };
constexpr Run kRuns[] = {
    {0x0020, 0x007E},   // ASCII
    {0x00A0, 0x00FF},   // Latin-1: no-break space, degree, middle dot, times, accents
    {0x0400, 0x045F},   // Cyrillic
    {0x2009, 0x2027},   // thin space, dashes, quotes, bullet, ellipsis
    {0x2190, 0x2193},   // arrows
    {0x2212, 0x2212},   // minus
};

} // namespace

std::shared_ptr<FontFace> FontFace::load(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    std::shared_ptr<FontFace> face(new FontFace());
    face->bytes_.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    if (face->bytes_.size() < 12) return nullptr;
    auto info = std::make_shared<stbtt_fontinfo>();
    const int offset = stbtt_GetFontOffsetForIndex(face->bytes_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info.get(), face->bytes_.data(), offset)) return nullptr;
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(info.get(), &ascent, &descent, &gap);
    const float em = stbtt_ScaleForMappingEmToPixels(info.get(), 1.0f);
    face->ascent_ = float(ascent) * em;
    face->descent_ = float(descent) * em;
    face->lineGap_ = float(gap) * em;
    face->info_ = std::move(info);
    return face;
}

FontFace::~FontFace() = default;

FontAtlas::FontAtlas(std::shared_ptr<FontFace> face, SDL_Renderer* renderer, int pixelSize)
    : face_(std::move(face)), size_(std::max(4, pixelSize)) {
    if (!face_ || !renderer) return;
    const auto& info = infoOf(*face_);
    scale_ = stbtt_ScaleForMappingEmToPixels(&info, float(size_));
    ascent_ = face_->ascent() * float(size_);
    line_ = (face_->ascent() - face_->descent() + face_->lineGap()) * float(size_);

    std::vector<stbtt_pack_range> ranges;
    std::vector<std::vector<stbtt_packedchar>> packed;
    packed.reserve(std::size(kRuns));
    for (const Run& run : kRuns) {
        packed.emplace_back(std::size_t(run.last - run.first + 1));
        stbtt_pack_range range{};
        range.font_size = STBTT_POINT_SIZE(float(size_));
        range.first_unicode_codepoint_in_range = int(run.first);
        range.num_chars = int(run.last - run.first + 1);
        range.chardata_for_range = packed.back().data();
        ranges.push_back(range);
    }
    // As small a square as holds them: a page of text at a large size needs
    // more room than a caption, and a texture much bigger than its glyphs is
    // memory for nothing.
    int side = 256;
    std::vector<unsigned char> alpha;
    for (;; side *= 2) {
        if (side > 4096) return;
        alpha.assign(std::size_t(side) * side, 0);
        stbtt_pack_context context;
        if (!stbtt_PackBegin(&context, alpha.data(), side, side, 0, 1, nullptr)) return;
        stbtt_PackSetOversampling(&context, 1, 1);
        const bool all = stbtt_PackFontRanges(&context, info.data, 0, ranges.data(), int(ranges.size())) != 0;
        stbtt_PackEnd(&context);
        if (all) break;
    }

    for (std::size_t r = 0; r < std::size(kRuns); ++r)
        for (char32_t cp = kRuns[r].first; cp <= kRuns[r].last; ++cp) {
            const int index = stbtt_FindGlyphIndex(&info, int(cp));
            // Not in this face: left out, so it falls back to the question mark
            // rather than to an empty box of the right width.
            if (index == 0 && cp != U' ') continue;
            const stbtt_packedchar& c = packed[r][std::size_t(cp - kRuns[r].first)];
            Glyph g;
            g.from = {float(c.x0), float(c.y0), float(c.x1 - c.x0), float(c.y1 - c.y0)};
            g.left = c.xoff;
            g.top = c.yoff;
            g.advance = c.xadvance;
            g.index = index;
            glyphs_.emplace(cp, g);
        }
    if (const auto q = glyphs_.find(U'?'); q != glyphs_.end()) missing_ = q->second;

    // White, with the coverage as alpha: the colour is the draw's.
    std::vector<std::uint32_t> pixels(alpha.size());
    for (std::size_t i = 0; i < alpha.size(); ++i)
        pixels[i] = 0x00FFFFFFu | (std::uint32_t(alpha[i]) << 24);
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, side, side);
    if (!texture_) return;
    SDL_UpdateTexture(texture_, nullptr, pixels.data(), side * 4);
    SDL_SetTextureBlendMode(texture_, SDL_BLENDMODE_BLEND);
    // Drawn at the pixels it was baked for: nothing to filter.
    SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_NEAREST);
}

FontAtlas::~FontAtlas() {
    if (texture_) SDL_DestroyTexture(texture_);
}

const FontAtlas::Glyph& FontAtlas::glyph(char32_t codepoint) const {
    const auto found = glyphs_.find(codepoint);
    return found == glyphs_.end() ? missing_ : found->second;
}

float FontAtlas::kerning(const Glyph& left, const Glyph& right) const {
    if (!face_ || left.index == 0 || right.index == 0) return 0;
    return float(stbtt_GetGlyphKernAdvance(&infoOf(*face_), left.index, right.index)) * scale_;
}

float FontAtlas::width(std::string_view utf8) const {
    float pen = 0;
    const Glyph* previous = nullptr;
    for (std::size_t at = 0; at < utf8.size();) {
        const Glyph& g = glyph(nextCodepoint(utf8, at));
        if (previous) pen += kerning(*previous, g);
        pen += g.advance;
        previous = &g;
    }
    return pen;
}

char32_t nextCodepoint(std::string_view text, std::size_t& at) {
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    int extra = 0;
    char32_t cp = 0;
    if (lead < 0x80) { ++at; return lead; }
    if ((lead & 0xE0) == 0xC0) { extra = 1; cp = lead & 0x1F; }
    else if ((lead & 0xF0) == 0xE0) { extra = 2; cp = lead & 0x0F; }
    else if ((lead & 0xF8) == 0xF0) { extra = 3; cp = lead & 0x07; }
    else { ++at; return 0xFFFD; }
    if (at + std::size_t(extra) >= text.size()) {   // cut short
        ++at;
        return 0xFFFD;
    }
    for (int i = 1; i <= extra; ++i) {
        const unsigned char next = byte(at + std::size_t(i));
        if ((next & 0xC0) != 0x80) { ++at; return 0xFFFD; }
        cp = (cp << 6) | (next & 0x3F);
    }
    at += std::size_t(extra) + 1;
    return cp;
}

std::size_t lastCodepointBytes(std::string_view text) {
    if (text.empty()) return 0;
    std::size_t start = text.size() - 1;
    // Back over continuation bytes to the lead, at most three of them.
    while (start > 0 && text.size() - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80)
        --start;
    return text.size() - start;
}

} // namespace ui

