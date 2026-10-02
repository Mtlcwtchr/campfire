#include "engine/world_source/png_io.hpp"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>

#include "engine/world_store/atomic_file.hpp"

namespace engine::world_source {
namespace {

constexpr std::array<std::uint8_t, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};

bool fail(std::string* why, const char* text) {
    if (why) *why = text;
    return false;
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(std::uint8_t(v >> 24)); out.push_back(std::uint8_t(v >> 16));
    out.push_back(std::uint8_t(v >> 8)); out.push_back(std::uint8_t(v));
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return std::uint8_t(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

} // namespace

std::optional<Image> readPng(const std::filesystem::path& file, std::string* why) {
    const auto bytes = world_store::readFileBytes(file, why);
    if (!bytes) return std::nullopt;
    const auto& data = *bytes;
    if (data.size() < 8 || !std::equal(kSignature.begin(), kSignature.end(), data.begin())) {
        fail(why, "not a PNG file");
        return std::nullopt;
    }
    std::uint32_t width = 0, height = 0;
    std::uint8_t depth = 0, colour = 0, interlace = 0;
    std::vector<std::uint8_t> idat, palette, transparency;
    for (std::size_t at = 8; at + 12 <= data.size();) {
        const std::uint32_t length = be32(&data[at]);
        if (at + 12 + std::size_t(length) > data.size()) { fail(why, "PNG chunk runs past the file"); return std::nullopt; }
        const std::string type(reinterpret_cast<const char*>(&data[at + 4]), 4);
        const std::uint8_t* body = &data[at + 8];
        if (type == "IHDR" && length >= 13) {
            width = be32(body); height = be32(body + 4);
            depth = body[8]; colour = body[9]; interlace = body[12];
        } else if (type == "PLTE") {
            palette.assign(body, body + length);
        } else if (type == "tRNS") {
            transparency.assign(body, body + length);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), body, body + length);
        } else if (type == "IEND") {
            break;
        }
        at += 12 + std::size_t(length);
    }
    if (!width || !height || width > 65536 || height > 65536) { fail(why, "PNG has no usable size"); return std::nullopt; }
    if (interlace != 0) { fail(why, "interlaced PNG is not supported: save it without interlacing"); return std::nullopt; }
    int stored = 0;   // channels in the file
    switch (colour) {
        case 0: stored = 1; break;
        case 2: stored = 3; break;
        case 3: stored = 1; break;
        case 4: stored = 2; break;
        case 6: stored = 4; break;
        default: fail(why, "unknown PNG colour type"); return std::nullopt;
    }
    if (colour == 3 ? depth != 8 : (depth != 8 && depth != 16)) {
        fail(why, "PNG must be 8 or 16 bits a channel (palette images 8 bits)");
        return std::nullopt;
    }
    const std::size_t bytesPerPixel = std::size_t(stored) * (depth / 8);
    const std::size_t stride = bytesPerPixel * width;
    std::vector<std::uint8_t> raw((stride + 1) * height);
    z_stream z{};
    if (inflateInit(&z) != Z_OK) { fail(why, "zlib would not start"); return std::nullopt; }
    z.next_in = idat.data();
    z.avail_in = uInt(idat.size());
    z.next_out = raw.data();
    z.avail_out = uInt(raw.size());
    const int result = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (result != Z_STREAM_END || z.avail_out != 0) { fail(why, "PNG image data is damaged or short"); return std::nullopt; }
    // Unfilter in place, row by row.
    std::vector<std::uint8_t> previous(stride, 0);
    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = &raw[y * (stride + 1)];
        const std::uint8_t filter = row[0];
        std::uint8_t* line = row + 1;
        for (std::size_t i = 0; i < stride; ++i) {
            const int left = i >= bytesPerPixel ? line[i - bytesPerPixel] : 0;
            const int up = previous[i];
            const int corner = i >= bytesPerPixel ? previous[i - bytesPerPixel] : 0;
            switch (filter) {
                case 0: break;
                case 1: line[i] = std::uint8_t(line[i] + left); break;
                case 2: line[i] = std::uint8_t(line[i] + up); break;
                case 3: line[i] = std::uint8_t(line[i] + ((left + up) >> 1)); break;
                case 4: line[i] = std::uint8_t(line[i] + paeth(left, up, corner)); break;
                default: fail(why, "PNG row has an unknown filter"); return std::nullopt;
            }
        }
        std::memcpy(previous.data(), line, stride);
    }
    Image image;
    image.width = width;
    image.height = height;
    image.bits = colour == 3 ? 8 : depth;
    image.channels = std::uint8_t(colour == 3 ? (transparency.empty() ? 3 : 4) : stored);
    image.allocate();
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t* line = &raw[y * (stride + 1) + 1];
        for (std::uint32_t x = 0; x < width; ++x) {
            if (colour == 3) {
                const std::size_t index = line[x];
                if (index * 3 + 2 >= palette.size()) { fail(why, "PNG palette index out of range"); return std::nullopt; }
                for (std::uint8_t c = 0; c < 3; ++c) image.set(x, y, c, palette[index * 3 + c]);
                if (image.channels == 4) image.set(x, y, 3, index < transparency.size() ? transparency[index] : 255);
                continue;
            }
            for (int c = 0; c < stored; ++c)
                image.set(x, y, std::uint8_t(c),
                          depth == 16 ? std::uint16_t((line[(x * stored + std::uint32_t(c)) * 2] << 8) |
                                                      line[(x * stored + std::uint32_t(c)) * 2 + 1])
                                      : line[x * std::uint32_t(stored) + std::uint32_t(c)]);
        }
    }
    return image;
}

bool writePng(const std::filesystem::path& file, const Image& image, std::string* why) {
    if (!image.width || !image.height || image.channels < 1 || image.channels > 4 ||
        (image.bits != 8 && image.bits != 16) ||
        (image.bits == 16 ? image.words.size() : image.bytes.size()) !=
                std::size_t(image.width) * image.height * image.channels)
        return fail(why, "image to write is malformed");
    static constexpr std::uint8_t kColour[5]{0, 0, 4, 2, 6};
    const std::size_t bytesPerPixel = std::size_t(image.channels) * (image.bits / 8);
    const std::size_t stride = bytesPerPixel * image.width;
    // Each row with the Sub or Up filter, whichever leaves smaller numbers:
    // heights and control fields are smooth, and the deltas deflate well.
    std::vector<std::uint8_t> raw;
    raw.reserve((stride + 1) * image.height);
    std::vector<std::uint8_t> line(stride), previous(stride, 0), sub(stride), up(stride);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x)
            for (std::uint8_t c = 0; c < image.channels; ++c) {
                const std::uint16_t v = image.at(x, y, c);
                if (image.bits == 16) {
                    line[(x * image.channels + c) * 2] = std::uint8_t(v >> 8);
                    line[(x * image.channels + c) * 2 + 1] = std::uint8_t(v);
                } else {
                    line[x * image.channels + c] = std::uint8_t(v);
                }
            }
        std::uint64_t subCost = 0, upCost = 0;
        for (std::size_t i = 0; i < stride; ++i) {
            sub[i] = std::uint8_t(line[i] - (i >= bytesPerPixel ? line[i - bytesPerPixel] : 0));
            up[i] = std::uint8_t(line[i] - previous[i]);
            subCost += std::min<int>(sub[i], 256 - sub[i]);
            upCost += std::min<int>(up[i], 256 - up[i]);
        }
        const bool useUp = y > 0 && upCost < subCost;
        raw.push_back(useUp ? 2 : 1);
        const auto& chosen = useUp ? up : sub;
        raw.insert(raw.end(), chosen.begin(), chosen.end());
        previous = line;
    }
    uLongf packedSize = compressBound(uLong(raw.size()));
    std::vector<std::uint8_t> packed(packedSize);
    if (compress2(packed.data(), &packedSize, raw.data(), uLong(raw.size()), 6) != Z_OK)
        return fail(why, "zlib could not compress the image");
    packed.resize(packedSize);

    std::vector<std::uint8_t> out(kSignature.begin(), kSignature.end());
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& body) {
        put32(out, std::uint32_t(body.size()));
        const std::size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), body.begin(), body.end());
        const auto crc = crc32(0L, &out[start], uInt(out.size() - start));
        put32(out, std::uint32_t(crc));
    };
    std::vector<std::uint8_t> header;
    put32(header, image.width);
    put32(header, image.height);
    header.push_back(image.bits);
    header.push_back(kColour[image.channels]);
    header.push_back(0); header.push_back(0); header.push_back(0);
    chunk("IHDR", header);
    // IDAT in pieces of a few megabytes: some readers dislike one huge chunk.
    constexpr std::size_t kPiece = std::size_t(1) << 22;
    for (std::size_t at = 0; at < packed.size(); at += kPiece)
        chunk("IDAT", std::vector<std::uint8_t>(packed.begin() + std::ptrdiff_t(at),
                                                packed.begin() + std::ptrdiff_t(std::min(packed.size(), at + kPiece))));
    chunk("IEND", {});
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    return world_store::writeFileAtomic(file, out, why);
}

} // namespace engine::world_source
