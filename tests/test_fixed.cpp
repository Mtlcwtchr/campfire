#include "framework.hpp"

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "engine/core/rng.hpp"
#include "engine/core/time.hpp"

using core::Fixed;

TEST(fixed_basic_arithmetic) {
    const Fixed a = Fixed::fromInt(3);
    const Fixed b = Fixed::ratio(1, 2);
    CHECK((a + b).toDouble() == 3.5);
    CHECK((a - b).toDouble() == 2.5);
    CHECK((a * b).toDouble() == 1.5);
    CHECK((a / b).toDouble() == 6.0);
    CHECK(core::abs(-a) == a);
}

TEST(fixed_ratio_is_exact_where_it_can_be) {
    CHECK(Fixed::ratio(1, 4) * Fixed::fromInt(4) == Fixed::fromInt(1));
    CHECK(Fixed::ratio(3, 8) * Fixed::fromInt(8) == Fixed::fromInt(3));
}

TEST(fixed_sqrt_and_hypot) {
    CHECK(core::sqrt(Fixed::fromInt(16)) == Fixed::fromInt(4));
    CHECK(core::sqrt(Fixed::fromInt(0)) == core::kZero);
    // 3-4-5 triangle, exact in fixed point.
    CHECK(core::hypot(Fixed::fromInt(3), Fixed::fromInt(4)) == Fixed::fromInt(5));
    // Large coordinates must not overflow the intermediate squares.
    const Fixed big = core::hypot(Fixed::fromInt(30000), Fixed::fromInt(40000));
    CHECK(big == Fixed::fromInt(50000));
}

TEST(fixed_saturate_and_clamp) {
    CHECK(core::saturate(Fixed::fromInt(3)) == core::kOne);
    CHECK(core::saturate(Fixed::fromInt(-3)) == core::kZero);
    CHECK(core::clamp(Fixed::fromInt(5), Fixed::fromInt(1), Fixed::fromInt(4)) == Fixed::fromInt(4));
}

TEST(geometry_tile_round_trip) {
    for (std::int32_t x = -5; x <= 5; ++x) {
        for (std::int32_t y = -5; y <= 5; ++y) {
            const core::TilePos t{x, y};
            CHECK(core::toTile(core::tileCentre(t)) == t);
        }
    }
}

TEST(rng_is_reproducible_and_uniform_enough) {
    core::Rng a(42, 1), b(42, 1);
    for (int i = 0; i < 1000; ++i) CHECK_EQ(a.nextU64(), b.nextU64());

    // Different streams from one seed must not track each other.
    core::Rng s1(42, 1), s2(42, 2);
    bool differed = false;
    for (int i = 0; i < 100; ++i) if (s1.nextU64() != s2.nextU64()) { differed = true; break; }
    CHECK(differed);

    core::Rng u(7, 1);
    int buckets[10] = {};
    for (int i = 0; i < 10000; ++i) buckets[u.below(10)]++;
    for (int i = 0; i < 10; ++i) CHECK(buckets[i] > 700 && buckets[i] < 1300);
}

TEST(calendar_decomposes_ticks) {
    core::TimeConfig cfg;   // 10 ticks/h, 24 h/day, 15 d/season, 4 seasons
    CHECK_EQ(cfg.ticksPerDay(), 240);
    CHECK_EQ(cfg.ticksPerSeason(), 3600);
    CHECK_EQ(cfg.ticksPerYear(), 14400);

    const auto start = core::decompose(0, cfg);
    CHECK(start.year == 0 && start.season == core::Season::Spring && start.dayOfSeason == 0 && start.hour == 0);

    const auto later = core::decompose(cfg.ticksPerYear() + cfg.ticksPerSeason() * 2 + cfg.ticksPerDay() * 3 + 50, cfg);
    CHECK_EQ(later.year, 1);
    CHECK(later.season == core::Season::Autumn);
    CHECK_EQ(later.dayOfSeason, 3);
    CHECK_EQ(later.hour, 5);
}
