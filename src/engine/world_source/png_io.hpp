#pragma once
// PNG, eight and sixteen bits a channel, both ways.
//
// The authoring package's rasters are PNG (world_authoring_import_export_spec
// §2): a sixteen-bit grey heightmap, RGBA control maps, a categorical id map.
// The image loaders the renderer uses hand back eight bits whatever the file
// holds, which is two hundred and fifty-six heights for a mountain range - so
// this reads and writes the format itself, over zlib. Non-interlaced images
// only (what every paint program writes by default); palette images are
// expanded to RGB(A).
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace engine::world_source {

struct Image {
    std::uint32_t width = 0, height = 0;
    std::uint8_t channels = 0;       // 1 grey, 2 grey+alpha, 3 RGB, 4 RGBA
    std::uint8_t bits = 8;           // 8 or 16 a channel
    // Row-major, channels interleaved, in whichever of the two the depth is:
    // an 8k RGBA control map is a quarter of a gigabyte as bytes and half of
    // one widened.
    std::vector<std::uint8_t> bytes;      // bits == 8
    std::vector<std::uint16_t> words;     // bits == 16 (host order)

    void allocate() {
        const std::size_t n = std::size_t(width) * height * channels;
        if (bits == 16) { words.assign(n, 0); bytes.clear(); } else { bytes.assign(n, 0); words.clear(); }
    }
    [[nodiscard]] std::uint16_t at(std::uint32_t x, std::uint32_t y, std::uint8_t channel) const {
        const std::size_t i = (std::size_t(y) * width + x) * channels + channel;
        return bits == 16 ? words[i] : bytes[i];
    }
    void set(std::uint32_t x, std::uint32_t y, std::uint8_t channel, std::uint16_t value) {
        const std::size_t i = (std::size_t(y) * width + x) * channels + channel;
        if (bits == 16) words[i] = value; else bytes[i] = std::uint8_t(value);
    }
    // The largest value a sample can hold at this depth.
    [[nodiscard]] std::uint32_t maximum() const { return bits == 16 ? 65535u : 255u; }
};

std::optional<Image> readPng(const std::filesystem::path& file, std::string* why = nullptr);
// `image.bits` 8 or 16, `channels` 1..4. Written atomically.
bool writePng(const std::filesystem::path& file, const Image& image, std::string* why = nullptr);

} // namespace engine::world_source
