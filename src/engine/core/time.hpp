#pragma once
// Game calendar.
//
// The GDD (12) explicitly leaves the mapping of ticks to days, seasons and years
// open, tied to the "2-4 real hours to the first natural succession" target from
// section 10. The numbers below are the provisional defaults recorded in
// doc/DECISIONS.md D5; they live in content/config/time.json and are loaded, not
// hardcoded, so retuning the generational pace never touches simulation code.

#include <cstdint>
#include <string_view>

namespace core {

enum class Season : std::uint8_t { Spring = 0, Summer = 1, Autumn = 2, Winter = 3 };

std::string_view seasonName(Season s);

struct TimeConfig {
    std::int32_t ticksPerHour = 10;     // 10 ticks per in-world hour
    std::int32_t hoursPerDay = 24;
    std::int32_t daysPerSeason = 15;
    std::int32_t seasonsPerYear = 4;

    constexpr std::int64_t ticksPerDay() const { return std::int64_t(ticksPerHour) * hoursPerDay; }
    constexpr std::int64_t ticksPerSeason() const { return ticksPerDay() * daysPerSeason; }
    constexpr std::int64_t ticksPerYear() const { return ticksPerSeason() * seasonsPerYear; }
};

// A point on the calendar, derived from the absolute tick counter. Nothing stores
// a date: the tick is the single source of truth, so a save is one integer.
struct DateTime {
    std::int64_t tick = 0;
    std::int32_t year = 0;
    Season season = Season::Spring;
    std::int32_t dayOfSeason = 0;   // 0-based
    std::int32_t hour = 0;          // 0-based, 0..hoursPerDay-1
    std::int32_t tickOfHour = 0;

    // Daylight drives what work is available and how fast pawns move.
    bool isDaylight = true;
};

DateTime decompose(std::int64_t tick, const TimeConfig& cfg);

// Sunrise/sunset shift with the season; winter days are short enough to matter
// for the food chain, which is the point of modelling it at all.
bool isDaylightHour(std::int32_t hour, Season season, const TimeConfig& cfg);

} // namespace core
