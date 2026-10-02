#pragma once
// Addressing shared by the uploader and the future page-backed vertex path.
// One atlas holds one dataset, not one geometry LOD. Stored samples include
// both endpoints of the 512 m page and the baker's normal/filtering halo.

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "game/world/terrain_streaming/tile_layout.hpp"

namespace world::terrain {

struct HeightPageAddress {
    std::size_t slot = 0;
    std::uint32_t x = 0, y = 0;
    // uv = uvOrigin + pageLocalMetres * uvPerMetre, in texel centres.
    std::array<float, 2> uvOrigin{};
    std::array<float, 2> uvPerMetre{};
    // R16_UNORM is decoded without requantising the authoritative page.
    float heightLow = 0, heightRange = 0;
};

class HeightPageLayout {
public:
    HeightPageLayout(std::int32_t sampleMetres, std::uint32_t columns,
                     std::uint32_t rows,
                     std::uint16_t padding = streaming::kDefaultPaddingSamples)
        : step_(sampleMetres), columns_(columns), rows_(rows), padding_(padding) {
        if ((step_ != 4 && step_ != 8 && step_ != 16 && step_ != 64) || !columns_ || !rows_)
            throw std::invalid_argument("height atlas needs H4, H8, H16 or H64 and nonzero slots");
        stored_ = std::uint32_t(streaming::interiorSamples(step_)) + 2u * padding_;
        const auto width = std::uint64_t(stored_) * columns_;
        const auto height = std::uint64_t(stored_) * rows_;
        if (width > std::numeric_limits<std::uint32_t>::max() ||
            height > std::numeric_limits<std::uint32_t>::max() ||
            width > std::numeric_limits<std::size_t>::max() / height / sizeof(std::uint16_t))
            throw std::invalid_argument("height atlas dimensions overflow");
    }

    [[nodiscard]] std::int32_t sampleMetres() const { return step_; }
    [[nodiscard]] std::uint8_t level() const { return step_ == 4 ? 0 : step_ == 8 ? 1 : step_ == 16 ? 2 : 4; }
    // The levels this atlas holds: its own, and for H64 every coarser one -
    // H256 and H1024 pages are nine samples a side like H64's, so they share
    // its slots (tile_layout.hpp, pageMetresForSpacing).
    [[nodiscard]] bool holds(std::uint8_t level) const {
        return level == this->level() || (step_ == 64 && level > 4 && level <= 8);
    }
    [[nodiscard]] std::uint32_t storedSamples() const { return stored_; }
    [[nodiscard]] std::uint32_t width() const { return stored_ * columns_; }
    [[nodiscard]] std::uint32_t height() const { return stored_ * rows_; }
    [[nodiscard]] std::size_t capacity() const { return std::size_t(columns_) * rows_; }
    [[nodiscard]] std::size_t bytes() const { return std::size_t(width()) * height() * 2; }

    [[nodiscard]] bool accepts(const streaming::BaseTile& page) const {
        return holds(page.key.level) && page.sampleMetres == (4 << page.key.level) &&
               page.width == streaming::interiorSamples(step_) && page.height == page.width &&
               page.padding == padding_ && page.elevationMax > page.elevationMin && page.valid();
    }

    [[nodiscard]] HeightPageAddress address(std::size_t slot,
                                           const streaming::BaseTile& page) const {
        if (slot >= capacity()) throw std::out_of_range("height atlas slot");
        if (!accepts(page)) throw std::invalid_argument("incompatible height page");
        HeightPageAddress out;
        out.slot = slot;
        out.x = static_cast<std::uint32_t>(slot % columns_) * stored_;
        out.y = static_cast<std::uint32_t>(slot / columns_) * stored_;
        out.uvOrigin = {(float(out.x) + padding_ + 0.5f) / float(width()),
                        (float(out.y) + padding_ + 0.5f) / float(height())};
        out.uvPerMetre = {1.0f / (float(page.sampleMetres) * float(width())),
                          1.0f / (float(page.sampleMetres) * float(height()))};
        out.heightLow = static_cast<float>(page.elevationMin.toDouble());
        out.heightRange = static_cast<float>(page.elevationMax.toDouble() -
                                              page.elevationMin.toDouble());
        return out;
    }

private:
    std::int32_t step_;
    std::uint32_t columns_, rows_, stored_ = 0;
    std::uint16_t padding_;
};

// All 64 coordinate bits are identity, including negative page coordinates.
// The dataset is owned by the atlas; XOR-ing its level into x would alias keys.
// An atlas holding more than one level (H64 with H256 and H1024) keys them
// apart by the level in the top bits: page coordinates stay far inside 28 bits.
inline std::int64_t heightPageKey(streaming::TileKey key) {
    if (key.level <= 4) {
        const auto xy = (std::uint64_t(static_cast<std::uint32_t>(key.x)) << 32) |
                        static_cast<std::uint32_t>(key.y);
        return std::bit_cast<std::int64_t>(xy);
    }
    const auto xy = (std::uint64_t(key.level) << 56) |
                    ((std::uint64_t(static_cast<std::uint32_t>(key.x)) & 0x0fffffffu) << 28) |
                    (std::uint64_t(static_cast<std::uint32_t>(key.y)) & 0x0fffffffu);
    return std::bit_cast<std::int64_t>(xy);
}

} // namespace world::terrain
