#pragma once
// Historical drainage (doc/plan_procedural_environment_2026-10-03.md, part D).
//
// Today's water is not the only thing that shaped a valley. A broad trench
// through a forest with a trickle at the bottom was cut by more water than it
// carries now, and the forest grows along it because of that. So the
// environment keeps a drainage of its own beside the world's rivers: a micro
// network of flow worked out from the ground over a window, once under the
// climate of the past and once under today's, and the ratio of the two says
// what kind of channel each reach is.
//
// The world's hydrology (rivers, lakes) stays the authority on water. This
// network carries none: it is where recipes look for gullies, old channels
// and wet hollows.
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "engine/environment/fields.hpp"
#include "engine/environment/recipe.hpp"

namespace engine::environment {

struct DrainageSettings {
    double step = 16.0;                // metres between flow cells
    // Upstream area a channel begins at, square metres, before erodibility.
    double channelArea = 60000.0;
    double minSlope = 0.002;
    // Today's flow over the past's at which a reach counts as each class.
    double seasonalRatio = 0.55, ephemeralRatio = 0.25, abandonedRatio = 0.08;
    // Today's upstream area that holds water all year, square metres.
    double permanentArea = 4.0e6;
    // Below this much of today's upstream area a channel runs only after rain,
    // however wet the past: the first-order heads of every network.
    double seasonalArea = 6.0e5;
    // Widths from upstream area: outer = k * sqrt(area)^0.5 ... see drainage.cpp.
    double outerWidthPerRootArea = 0.9, innerWidthPerRootArea = 0.22, waterWidthPerRootArea = 0.05;
    double smoothing = 2;              // Chaikin passes over each reach
    double minReach = 48.0;            // metres; shorter reaches are dropped
};

// How much more (or less) water fell here in the past than now, and how
// easily the ground gives way. The game decides both; defaults are neutral.
//
// They change over hundreds of metres, so they are asked on a grid four times
// coarser than the flow's and interpolated. The heights are asked for the
// whole window at once when `heights` is given: a source that can answer a
// grid shares its work between neighbours.
struct DrainageClimate {
    std::function<double(double x, double y)> pastRain;     // relative, 1 = today's
    std::function<double(double x, double y)> rainToday;    // relative
    std::function<double(double x, double y)> erodibility;  // 0..1, 1 gives way easily
    // Heights of `columns` x `rows` points `step` apart from (x0, y0), row-major.
    std::function<void(double x0, double y0, double step, int columns, int rows, std::vector<double>& out)> heights;
};

struct ChannelReach {
    std::vector<std::array<double, 2>> points;   // downstream order
    std::vector<double> pastArea, todayArea;     // square metres, per point
    ChannelClass kind = ChannelClass::Paleochannel;
    double length = 0;
    double outerWidth = 0, innerWidth = 0, waterWidth = 0;   // at the downstream end
    double sinuosity = 1;
};

// A bend sharp enough that the channel may have cut it off: a candidate for
// an oxbow or a wet hollow.
struct Meander {
    double x = 0, y = 0;
    double radius = 0;
    ChannelClass kind = ChannelClass::Paleochannel;
};

struct DrainageNetwork {
    double x0 = 0, y0 = 0, step = 16;
    int columns = 0, rows = 0;
    std::vector<float> pastArea;    // per cell, square metres
    std::vector<float> todayArea;
    std::vector<std::int8_t> down;  // D8 direction index, -1 for an outlet
    std::vector<ChannelReach> reaches;
    std::vector<Meander> meanders;

    [[nodiscard]] float pastAt(double x, double y) const;
};

// The network over a rectangle: `columns` x `rows` cells of settings.step
// starting at (x0, y0). Pure: the same window gives the same network.
DrainageNetwork buildDrainage(const HeightAt& height, const DrainageClimate& climate,
                              double x0, double y0, int columns, int rows, const DrainageSettings& settings);

ChannelClass classifyChannel(double pastArea, double todayArea, const DrainageSettings& settings);

} // namespace engine::environment
