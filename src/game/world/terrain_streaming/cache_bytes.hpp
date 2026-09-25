#pragma once
// The byte primitives every terrain cache record is written with.
//
// Nothing here knows what a tile or a river is. It exists so that the tile
// cache and the hydrology cache cannot disagree about what a little-endian
// u32 or a varint is: two copies of a serialization primitive is two formats
// that read each other's files until the day one of them is fixed.
//
// Everything is explicitly little-endian and byte-at-a-time. No struct is ever
// dumped as its native layout, so a cache written on one machine is readable
// on another whatever the alignment, padding or endianness of the writer.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace world::streaming {

template <class UInt>
void writeUnsigned(std::vector<std::uint8_t>& bytes, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8u)));
}

template <class Int>
void writeInteger(std::vector<std::uint8_t>& bytes, Int value) {
    using UInt = std::make_unsigned_t<Int>;
    writeUnsigned(bytes, static_cast<UInt>(value));
}

template <class UInt>
bool readUnsigned(const std::vector<std::uint8_t>& bytes, std::size_t& cursor, UInt& out) {
    static_assert(std::is_unsigned_v<UInt>);
    if (cursor > bytes.size() || bytes.size() - cursor < sizeof(UInt)) return false;
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i)
        value |= static_cast<UInt>(bytes[cursor++]) << (i * 8u);
    out = value;
    return true;
}

template <class Int>
bool readInteger(const std::vector<std::uint8_t>& bytes, std::size_t& cursor, Int& out) {
    using UInt = std::make_unsigned_t<Int>;
    UInt value = 0;
    if (!readUnsigned(bytes, cursor, value)) return false;
    out = static_cast<Int>(value);
    return true;
}

inline void writeVarUInt(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    do {
        std::uint8_t next = static_cast<std::uint8_t>(value & 0x7fu);
        value >>= 7u;
        if (value != 0) next |= 0x80u;
        bytes.push_back(next);
    } while (value != 0);
}

inline bool readVarUInt(const std::vector<std::uint8_t>& bytes, std::size_t& cursor,
                        std::uint32_t& out) {
    std::uint32_t value = 0;
    for (unsigned shift = 0; shift < 35; shift += 7) {
        if (cursor == bytes.size()) return false;
        const auto next = bytes[cursor++];
        value |= std::uint32_t(next & 0x7fu) << shift;
        if ((next & 0x80u) == 0) {
            out = value;
            return true;
        }
    }
    return false;
}

inline std::uint32_t zigZag(std::int32_t value) {
    return (static_cast<std::uint32_t>(value) << 1u) ^ static_cast<std::uint32_t>(value >> 31u);
}

inline std::int32_t unZigZag(std::uint32_t value) {
    return static_cast<std::int32_t>((value >> 1u) ^ (0u - (value & 1u)));
}

} // namespace world::streaming
