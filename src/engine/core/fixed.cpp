#include "engine/core/fixed.hpp"

#include <cstdio>

namespace core {

Fixed sqrt(Fixed v) {
    if (v.raw <= 0) return kZero;
    // sqrt(x) in Q32.32 == isqrt(raw << 32), computed in 128 bits.
    using Wide = Fixed::Wide;
    const Wide radicand = Wide(v.raw) << Fixed::kFracBits;

    // Seed from the bit length so Newton converges in a bounded number of steps.
    int bits = 0;
    for (Wide t = radicand; t > 0; t >>= 1) ++bits;
    Wide x = Wide(1) << ((bits + 1) / 2);
    for (int i = 0; i < 8; ++i) {
        Wide next = (x + radicand / x) >> 1;
        if (next == x) break;
        x = next;
    }
    while (x * x > radicand) --x;
    return Fixed::fromRaw(static_cast<Fixed::Raw>(x));
}

Fixed hypot(Fixed dx, Fixed dy) {
    // Squaring in Q32.32 would overflow past ~46 000 units, so keep the squares
    // in raw 128-bit space and take one sqrt at the end.
    using Wide = Fixed::Wide;
    const Wide a = Wide(dx.raw) * Wide(dx.raw);
    const Wide b = Wide(dy.raw) * Wide(dy.raw);
    Wide radicand = a + b;
    if (radicand <= 0) return kZero;

    int bits = 0;
    for (Wide t = radicand; t > 0; t >>= 1) ++bits;
    Wide x = Wide(1) << ((bits + 1) / 2);
    for (int i = 0; i < 12; ++i) {
        Wide next = (x + radicand / x) >> 1;
        if (next == x) break;
        x = next;
    }
    while (x * x > radicand) --x;
    return Fixed::fromRaw(static_cast<Fixed::Raw>(x));
}

std::string toString(Fixed v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v.toDouble());
    return buf;
}

} // namespace core
