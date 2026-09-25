#pragma once
// Reading and writing the game's own binary form.
//
// Everything the simulation saves goes through these two: a writer that appends
// to a byte buffer and a reader that walks one. Both are explicit about width
// and byte order, so a save written on one machine is read on another, and both
// carry a failure flag rather than throwing - a truncated file is a thing that
// happens, and the answer to it is "this save is not readable", not a crash.
//
// There is no reflection and no schema here on purpose. Each entity writes its
// own fields in its own order (World::write and friends), and a version number
// on every section says which order that was.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "engine/core/fixed.hpp"

namespace core {

class BinaryWriter {
public:
    void u8(std::uint8_t v) { bytes_.push_back(v); }
    void i8(std::int8_t v) { u8(static_cast<std::uint8_t>(v)); }
    void u16(std::uint16_t v) { raw(&v, sizeof v); }
    void i16(std::int16_t v) { raw(&v, sizeof v); }
    void u32(std::uint32_t v) { raw(&v, sizeof v); }
    void i32(std::int32_t v) { raw(&v, sizeof v); }
    void u64(std::uint64_t v) { raw(&v, sizeof v); }
    void i64(std::int64_t v) { raw(&v, sizeof v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    // Fixed point is stored as the integer it is, not as a double: the whole
    // point of it is that the bits are the value (D2).
    void fixed(Fixed v) { i64(v.raw); }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }

    std::size_t size() const { return bytes_.size(); }
    const std::vector<std::uint8_t>& data() const { return bytes_; }
    std::vector<std::uint8_t> take() { return std::move(bytes_); }

private:
    // Little-endian, whatever the machine is: a save is a file format, not a
    // memory dump.
    void raw(const void* p, std::size_t n) {
        const auto* src = static_cast<const std::uint8_t*>(p);
        std::uint64_t value = 0;
        std::memcpy(&value, src, n);
        for (std::size_t i = 0; i < n; ++i) bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    std::vector<std::uint8_t> bytes_;
};

class BinaryReader {
public:
    BinaryReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    explicit BinaryReader(const std::vector<std::uint8_t>& bytes)
        : data_(bytes.data()), size_(bytes.size()) {}

    std::uint8_t u8() {
        if (at_ + 1 > size_) { fail(); return 0; }
        return data_[at_++];
    }
    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }
    std::uint16_t u16() { return static_cast<std::uint16_t>(raw(2)); }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(raw(4)); }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::uint64_t u64() { return raw(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    bool boolean() { return u8() != 0; }
    Fixed fixed() { return Fixed::fromRaw(i64()); }
    std::string str() {
        const std::uint32_t n = u32();
        if (at_ + n > size_) { fail(); return {}; }
        std::string out(reinterpret_cast<const char*>(data_ + at_), n);
        at_ += n;
        return out;
    }

    // A section may be longer than this build knows how to read - a save from a
    // later version - so the reader can be told to stand at a known offset again
    // rather than trusting that it consumed exactly the right number of bytes.
    std::size_t position() const { return at_; }
    void seek(std::size_t to) {
        if (to > size_) { fail(); return; }
        at_ = to;
    }
    std::size_t remaining() const { return size_ - at_; }

    bool ok() const { return ok_; }
    void fail() { ok_ = false; }

private:
    std::uint64_t raw(std::size_t n) {
        if (at_ + n > size_) { fail(); return 0; }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < n; ++i) value |= std::uint64_t(data_[at_ + i]) << (8 * i);
        at_ += n;
        return value;
    }
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t at_ = 0;
    bool ok_ = true;
};

} // namespace core
