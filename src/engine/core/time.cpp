#include "engine/core/time.hpp"

namespace core {

std::string_view seasonName(Season s) {
    switch (s) {
        case Season::Spring: return "spring";
        case Season::Summer: return "summer";
        case Season::Autumn: return "autumn";
        case Season::Winter: return "winter";
    }
    return "unknown";
}

bool isDaylightHour(std::int32_t hour, Season season, const TimeConfig& cfg) {
    // Expressed as a fraction of the day so a modded hoursPerDay still works.
    std::int32_t sunriseNum = 6, sunsetNum = 18;   // out of 24, equinox baseline
    switch (season) {
        case Season::Spring: sunriseNum = 6;  sunsetNum = 19; break;
        case Season::Summer: sunriseNum = 4;  sunsetNum = 21; break;
        case Season::Autumn: sunriseNum = 6;  sunsetNum = 18; break;
        case Season::Winter: sunriseNum = 8;  sunsetNum = 16; break;
    }
    const std::int32_t sunrise = sunriseNum * cfg.hoursPerDay / 24;
    const std::int32_t sunset = sunsetNum * cfg.hoursPerDay / 24;
    return hour >= sunrise && hour < sunset;
}

DateTime decompose(std::int64_t tick, const TimeConfig& cfg) {
    DateTime dt;
    dt.tick = tick;

    const std::int64_t perYear = cfg.ticksPerYear();
    const std::int64_t perSeason = cfg.ticksPerSeason();
    const std::int64_t perDay = cfg.ticksPerDay();

    dt.year = static_cast<std::int32_t>(tick / perYear);
    std::int64_t rem = tick % perYear;

    dt.season = static_cast<Season>(rem / perSeason);
    rem %= perSeason;

    dt.dayOfSeason = static_cast<std::int32_t>(rem / perDay);
    rem %= perDay;

    dt.hour = static_cast<std::int32_t>(rem / cfg.ticksPerHour);
    dt.tickOfHour = static_cast<std::int32_t>(rem % cfg.ticksPerHour);

    dt.isDaylight = isDaylightHour(dt.hour, dt.season, cfg);
    return dt;
}

} // namespace core
