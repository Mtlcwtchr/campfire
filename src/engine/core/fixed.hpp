#pragma once
// Q32.32 fixed-point arithmetic.
//
// The simulation contains no floating point at all: every quantity that can
// influence a decision must produce bit-identical results on every machine and
// compiler so that lockstep multiplayer (GDD 11) and headless replay from a seed
// stay in sync. Floats do not guarantee that; int64 does.

#include <cstdint>
#include <compare>
#include <string>

// Inlined even when the compiler was told not to optimise.
//
// Every operator below is two or three instructions of work wrapped in a
// function call, and the simulation and the terrain do millions of them. At
// -O0 nothing is inlined by default - that is the whole point of -O0, because
// a debugger can only step what still exists - so each of those becomes a
// prologue, a call, a return and a stack frame, and a debug build runs the
// engine an order of magnitude slower than the code it was written as.
//
// This is the one place where the trade is clearly worth taking. Nobody steps
// INTO a fixed-point multiply: what you want to see at a breakpoint is the
// value, and the value is still there. So these are forced open at every
// optimisation level, and the debugger keeps everything worth having.
//
// It does NOT help an indirect call - a std::function, a virtual - because the
// target of one is not known until it runs. Those cost what they cost until
// the signature changes.
#if defined(__clang__) || defined(__GNUC__)
#define ASR_ALWAYS_INLINE [[gnu::always_inline]] inline
#elif defined(_MSC_VER)
#define ASR_ALWAYS_INLINE __forceinline
#else
#define ASR_ALWAYS_INLINE inline
#endif

namespace core {

struct Fixed {
    using Raw = std::int64_t;
    using Wide = __int128;

    static constexpr int kFracBits = 32;
    static constexpr Raw kOne = Raw(1) << kFracBits;
    static constexpr Raw kHalf = kOne >> 1;

    Raw raw = 0;

    constexpr Fixed() = default;
    ASR_ALWAYS_INLINE static constexpr Fixed fromRaw(Raw r) { Fixed f; f.raw = r; return f; }
    ASR_ALWAYS_INLINE static constexpr Fixed fromInt(std::int64_t v) { return fromRaw(v << kFracBits); }
    // Exact ratio, e.g. ratio(1, 3) is the closest representable third.
    static constexpr Fixed ratio(std::int64_t num, std::int64_t den) {
        return fromRaw(static_cast<Raw>((Wide(num) << kFracBits) / den));
    }
    // Only for reading authored content and for the renderer. Never call from
    // simulation code paths that feed a decision.
    static Fixed fromDoubleForContent(double v) {
        return fromRaw(static_cast<Raw>(v * static_cast<double>(kOne)));
    }

    ASR_ALWAYS_INLINE constexpr std::int64_t toInt() const { return raw >> kFracBits; }       // floor
    constexpr std::int64_t roundToInt() const { return (raw + kHalf) >> kFracBits; }
    double toDouble() const { return static_cast<double>(raw) / static_cast<double>(kOne); }

    ASR_ALWAYS_INLINE constexpr Fixed operator-() const { return fromRaw(-raw); }
    ASR_ALWAYS_INLINE constexpr Fixed& operator+=(Fixed o) { raw += o.raw; return *this; }
    ASR_ALWAYS_INLINE constexpr Fixed& operator-=(Fixed o) { raw -= o.raw; return *this; }
    ASR_ALWAYS_INLINE constexpr Fixed& operator*=(Fixed o) {
        raw = static_cast<Raw>((Wide(raw) * Wide(o.raw)) >> kFracBits);
        return *this;
    }
    ASR_ALWAYS_INLINE constexpr Fixed& operator/=(Fixed o) {
        raw = static_cast<Raw>((Wide(raw) << kFracBits) / Wide(o.raw));
        return *this;
    }

    ASR_ALWAYS_INLINE friend constexpr Fixed operator+(Fixed a, Fixed b) { return a += b; }
    ASR_ALWAYS_INLINE friend constexpr Fixed operator-(Fixed a, Fixed b) { return a -= b; }
    ASR_ALWAYS_INLINE friend constexpr Fixed operator*(Fixed a, Fixed b) { return a *= b; }
    ASR_ALWAYS_INLINE friend constexpr Fixed operator/(Fixed a, Fixed b) { return a /= b; }

    // Scaling by a plain integer avoids the shift round-trip and cannot overflow
    // the way a Fixed multiply can.
    ASR_ALWAYS_INLINE friend constexpr Fixed operator*(Fixed a, std::int64_t b) { return fromRaw(a.raw * b); }
    ASR_ALWAYS_INLINE friend constexpr Fixed operator*(std::int64_t a, Fixed b) { return fromRaw(a * b.raw); }
    ASR_ALWAYS_INLINE friend constexpr Fixed operator/(Fixed a, std::int64_t b) { return fromRaw(a.raw / b); }

    ASR_ALWAYS_INLINE friend constexpr auto operator<=>(Fixed a, Fixed b) { return a.raw <=> b.raw; }
    ASR_ALWAYS_INLINE friend constexpr bool operator==(Fixed a, Fixed b) { return a.raw == b.raw; }
};

inline constexpr Fixed kZero = Fixed::fromRaw(0);
inline constexpr Fixed kOne = Fixed::fromRaw(Fixed::kOne);

constexpr Fixed operator""_fx(unsigned long long v) { return Fixed::fromInt(static_cast<std::int64_t>(v)); }

ASR_ALWAYS_INLINE constexpr Fixed abs(Fixed a) { return a.raw < 0 ? -a : a; }
ASR_ALWAYS_INLINE constexpr Fixed min(Fixed a, Fixed b) { return a.raw < b.raw ? a : b; }
ASR_ALWAYS_INLINE constexpr Fixed max(Fixed a, Fixed b) { return a.raw > b.raw ? a : b; }
ASR_ALWAYS_INLINE constexpr Fixed clamp(Fixed v, Fixed lo, Fixed hi) { return min(max(v, lo), hi); }
ASR_ALWAYS_INLINE constexpr Fixed saturate(Fixed v) { return clamp(v, kZero, kOne); }
ASR_ALWAYS_INLINE constexpr Fixed lerp(Fixed a, Fixed b, Fixed t) { return a + (b - a) * t; }

// Integer Newton on the widened radicand; deterministic and exact to one ulp.
Fixed sqrt(Fixed v);
// Euclidean length without the intermediate overflow of squaring large coords.
Fixed hypot(Fixed dx, Fixed dy);

std::string toString(Fixed v, int decimals = 3);

} // namespace core
