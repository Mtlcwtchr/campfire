#pragma once
// A picture of the interface, drawn on the processor: a surface the size of
// the window in pixels and a software renderer over it.
//
// Whoever draws into it says where a new picture is ready (`painted`); whoever
// puts it on the screen keeps it as tiles of kTile pixels and uploads a tile
// only when that tile's revision moved. An interface that is not changing
// costs one comparison a frame, and one that changes in a corner - a lit
// button, a number in a status bar - costs that corner's tiles, not the
// window: uploading a whole two-pixel-per-point window is twenty megabytes.
//
// The pixels are BGRA32: blue, green, red, alpha in memory. That is SDL's
// ARGB8888 on every machine this runs on, the one 32-bit layout its software
// renderer has a dedicated blending path for - every other layout blends
// through a per-pixel format lookup, several times slower - and it is the
// card's B8G8R8A8.
#include <algorithm>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>

namespace ui {

class Canvas {
public:
    static constexpr SDL_PixelFormat kFormat = SDL_PIXELFORMAT_BGRA32;
    static constexpr int kTile = 256;   // pixels

    Canvas() = default;
    ~Canvas() { release(); }
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    // Makes the surface this many pixels. True when a new one was made - the
    // renderer is new too, and anything holding textures of the old one (a
    // ui::Ui) has to be shut down BEFORE this and initialised after.
    bool size(int width, int height) {
        width = width < 1 ? 1 : width;
        height = height < 1 ? 1 : height;
        if (surface_ && width == width_ && height == height_) return false;
        release();
        surface_ = SDL_CreateSurface(width, height, kFormat);
        if (!surface_) return false;
        renderer_ = SDL_CreateSoftwareRenderer(surface_);
        if (!renderer_) { release(); return false; }
        width_ = width;
        height_ = height;
        columns_ = (width + kTile - 1) / kTile;
        rows_ = (height + kTile - 1) / kTile;
        tiles_.assign(std::size_t(columns_) * std::size_t(rows_), ++revision_);
        return true;
    }
    bool wouldResize(int width, int height) const { return !surface_ || width != width_ || height != height_; }
    void release() {
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (surface_) SDL_DestroySurface(surface_);
        renderer_ = nullptr;
        surface_ = nullptr;
        width_ = height_ = columns_ = rows_ = 0;
        tiles_.clear();
    }

    SDL_Renderer* renderer() const { return renderer_; }
    SDL_Surface* surface() const { return surface_; }
    int width() const { return width_; }
    int height() const { return height_; }
    explicit operator bool() const { return renderer_ != nullptr; }

    // Moves whenever any of it was painted.
    std::uint64_t revision() const { return revision_; }
    int columns() const { return columns_; }
    int rows() const { return rows_; }
    std::uint64_t tileRevision(int column, int row) const {
        return tiles_[std::size_t(row) * std::size_t(columns_) + std::size_t(column)];
    }
    // All of it has a new picture.
    void painted() {
        ++revision_;
        std::fill(tiles_.begin(), tiles_.end(), revision_);
    }
    // The pixels in this rectangle have (and nothing else has).
    void painted(const SDL_Rect& pixels) {
        if (tiles_.empty() || pixels.w <= 0 || pixels.h <= 0) return;
        ++revision_;
        const int c0 = std::clamp(pixels.x / kTile, 0, columns_ - 1);
        const int c1 = std::clamp((pixels.x + pixels.w - 1) / kTile, 0, columns_ - 1);
        const int r0 = std::clamp(pixels.y / kTile, 0, rows_ - 1);
        const int r1 = std::clamp((pixels.y + pixels.h - 1) / kTile, 0, rows_ - 1);
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c)
                tiles_[std::size_t(r) * std::size_t(columns_) + std::size_t(c)] = revision_;
    }

private:
    SDL_Surface* surface_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    int width_ = 0, height_ = 0;
    int columns_ = 0, rows_ = 0;
    std::vector<std::uint64_t> tiles_;
    std::uint64_t revision_ = 1;
};

} // namespace ui

