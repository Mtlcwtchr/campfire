#pragma once
// A mountain range built on a plate boundary, as a function of where you are.
//
// The boundary IS the axis. That is the whole idea, and it is what the flood
// fill this replaces could not do: a fill spreads a scalar inland and produces
// a broad smooth swell - "somewhere around here is higher" - with no line in it
// anywhere. A range has a line. It has a crest that wanders, peaks along it,
// passes between them, spurs running off it, and a foot where it meets the
// plain. None of that is a property of how much stress there is; all of it is a
// property of the axis.
//
// So:
//
//   1. The margin says how much and what kind - collision, cordillera, arc,
//      trench, rift. That is the envelope, not the shape.
//   2. The crest line is the boundary, WANDERED: a Voronoi bisector is a
//      diagram, and a range that follows one looks like a diagram. The wander
//      is two scales, one that swings whole sections and one that frays.
//   3. Across the axis the section is TRIANGULAR, not a dome. A dome is what
//      `q * q` gives - zero slope at the summit - and it is why the mountains
//      in this world look like loaves. A real range has its steepest ground
//      just below the crest and eases only at the foot, so the profile is
//      linear in distance and softened over the last quarter of it.
//   4. Along the axis a ridged multifractal gives peaks, passes and spurs. It
//      is ridged rather than summed because summed noise is smooth everywhere
//      and would round off what step 3 just sharpened.
//
// Nothing is stored and nothing is world-sized. Two chunks, two levels of
// detail and two machines get the same range.
#include <cstdint>

#include "game/generation/plate_field.hpp"

namespace generation {

struct RangeSample {
    // Metres above (or below) what the plate would be without the margin.
    double metres = 0;
    // How far this point is from the wandered crest line, in metres.
    double crestMetres = 0;
    // What the section alone says, 0 to 1: one on the crest, zero past the
    // foot. Useful to anything that wants to know it is on a mountain without
    // caring how tall.
    double flank = 0;
};

struct RangeSettings {
    // How tall each kind of margin builds, in metres, at full stress.
    double collisionMetres = 3600;
    double cordilleraMetres = 2900;
    double arcMetres = 1500;
    double trenchMetres = -4200;
    double riftMetres = -900;
    double oceanRidgeMetres = 1100;

    // Half the width of the range at the foot, in metres. Varied along the axis
    // by noise, because a range of constant width is a wall.
    double halfWidthMetres = 22000;
    double widthVariation = 0.45;

    // How far the crest wanders off the bisector, and at what scales.
    double wanderMetres = 9000;
    double wanderBroadMetres = 120000;
    double wanderFineMetres = 26000;

    // The scale of the peaks and passes along the range.
    double crestScaleMetres = 14000;
    int crestOctaves = 5;
    // How much of the height the ridged field owns. The rest is the section,
    // so a range never disappears between its peaks - a pass is lower, not a
    // gap.
    double crestShare = 0.62;
};

class RangeField {
public:
    RangeField(std::uint64_t seed, const PlateField& plates, RangeSettings settings = {});

    [[nodiscard]] RangeSample at(double x, double y) const;
    [[nodiscard]] const RangeSettings& settings() const { return settings_; }

private:
    std::uint64_t seed_ = 0;
    const PlateField* plates_ = nullptr;
    RangeSettings settings_;
};

} // namespace generation
