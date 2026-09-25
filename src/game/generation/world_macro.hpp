#pragma once
// Bounded global composition for the scale-aware generator.
//
// This is the SOURCE of the new mode's large shape: continents, ocean basins
// and orogenic belts over the whole normalized domain, evaluated in physical
// metres. It is not a dense 540 m grid, it never becomes a WorldMapData, and
// nothing here allocates anything proportional to the world's area.
//
// Two scale classes on purpose, because they answer different questions:
//
// - The continental term is world-relative. A world reads as a world at any
//   size: widening the map widens the continent rather than turning it into an
//   archipelago of the old one.
// - Everything below it is semi-relative in metres. A mountain belt is about as
//   wide in a small world as in a large one, so a hundred-kilometre world holds
//   a few systems and a ten-thousand-kilometre world holds hundreds of them.
//   That is what makes a large world more regional hierarchy rather than the
//   same picture stretched.
//
// The field is a closed form: it can be asked for one point, a rectangle or a
// bounded overview without any of those costing more than the samples asked
// for, and its answer at a point never depends on what was asked before. That
// is the property the sparse source tree and the land mask both need.
#include "game/generation/world_scale_policy.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace generation {

// What one point of the composition is, before any regional process runs.
struct MacroSample {
    float elevationMetres = 0;  // relative to sea level; negative is ocean floor
    float continent = 0;        // 0 oceanic crust, 1 deep continental interior
    float uplift = 0;           // 0..1 orogenic belt strength, the mountain skeleton
};

// The continuous composition. Cheap to copy, holds no samples, and is the only
// thing that decides the shape: the overview below is a view of it, not a
// separate truth that could disagree.
class MacroField {
public:
    explicit MacroField(const WorldDescriptor&);
    const WorldDescriptor& descriptor() const { return descriptor_; }

    MacroSample at(double x, double y) const;
    float elevationAt(double x, double y) const { return at(x, y).elevationMetres; }

    // Sea level is NOT a constant of the field. It is the height that leaves
    // `kLandShare` of the domain above water, taken from the bounded overview
    // - which is the whole world, at a resolution that does not grow with it.
    //
    // It has to be measured rather than fixed because the continental term is
    // world-relative: a hundred-kilometre world spans little more than one of
    // its wavelengths, so the entire land-and-sea split of a small world rests
    // on a handful of lattice values. With a fixed threshold that made the land
    // share a lottery - changing the hash alone moved it from a third of the
    // world to a twenty-fifth - and a world that is nine tenths ocean is the
    // failure the plan names outright. Measured, it is a design parameter at
    // every size and seed.
    static constexpr double kLandShare = 0.38;
    // The shelf profile in physical metres. These are the DEEPEST and highest
    // the profile may go, not what every world gets: a hundred-kilometre map
    // cannot hold a continental margin, which is a hundred kilometres wide in
    // the world it was measured in, so asking it to fall four kilometres gives
    // a cliff rather than a slope. The margin's vertical extent is therefore
    // capped by `kMarginSlope` and only a large world reaches these numbers,
    // which is the same reasoning as a coarse start growing with the domain.
    static constexpr double kAbyssalDepth = 3900;    // ocean floor below sea level
    static constexpr double kShelfDepth = 130;       // the shelf just off a coast
    static constexpr double kInteriorHeight = 780;   // base height of a continental interior
    // How steep a continental margin is allowed to be, as a gradient. Real ones
    // run between one and six degrees; this is a ceiling, not a target.
    static constexpr double kMarginSlope = 0.09;
    // Where the coast sits on the crust, and how wide each band of the profile
    // is in crust units. The crust runs around a half, so the coast sits just
    // below that and the land share follows from this number alone.
    static constexpr double kCoastCrust = 0.485;
    static constexpr double kMarginBand = 0.220;    // abyssal floor to shelf break
    static constexpr double kCoastBand = 0.045;     // shelf break to the waterline
    static constexpr double kInteriorBand = 0.260;  // waterline to the interior
    static constexpr double kLandGateBand = 0.140;  // where relief fades in across the coast
    // How far the metric crust octaves may move the world-relative crust. Large
    // enough to cut a big world into regions, small enough that a small world
    // is still the composition its world-relative term drew.
    static constexpr double kRegionalCrust = 0.30;
    // The three relief layers, in metres above the shelf profile.
    static constexpr double kChainRelief = 2100;     // a range, where there is one
    static constexpr double kPlainRelief = 120;      // rolling ground, everywhere else
    static constexpr double kPlatformRelief = 260;   // regional basins and plateaus
    // Where along the belt noise a range starts and how quickly. A narrow, high
    // gate is what makes a chain a chain: most of a continent is outside it.
    static constexpr double kChainGateStart = 0.55;
    static constexpr double kChainGateWidth = 0.30;
    // Where on the crust an orogen sits, how far its influence reaches, and how
    // much uplift survives far from any margin. Slightly inland of the coast,
    // because a range stands behind a coastal plain rather than in the surf.
    static constexpr double kOrogenCrust = 0.560;
    static constexpr double kOrogenBand = 0.170;
    static constexpr double kOrogenInterior = 0.30;
    static constexpr double kContinentalRelief = kAbyssalDepth + kInteriorHeight;

    // A true bound on how fast the elevation can change, in metres per metre.
    //
    // Every octave is a value-noise lattice with a known amplitude and
    // wavelength, and a smoothstep-interpolated lattice cannot climb faster
    // than 1.5 amplitudes per wavelength. Summing that over the octaves gives a
    // Lipschitz constant, which is what lets a rectangle be PROVEN to hold no
    // land instead of merely looking like it holds none at the sample spacing.
    // Deliberately an over-estimate: too large costs work, too small loses an
    // island.
    double elevationLipschitz() const { return lipschitz_; }

    // The same bound, but for a rectangle whose crust is known to lie within
    // [low, high]. A global constant has to assume the worst thing the field
    // does anywhere - a mountain front - and then every rectangle of deep
    // ocean is charged for mountains it cannot contain. Over open water the
    // relief terms are multiplied by zero, and being able to say so is the
    // difference between proving a rectangle empty and giving up on it.
    double elevationLipschitz(double crustLow, double crustHigh) const;
    // How fast the crust itself can change, which is what bounds the range
    // above before the elevation bound can use it.
    double crustLipschitz() const { return crustSlope_; }

    // Largest elevation possible anywhere, used to stop a query early.
    double elevationCeiling() const { return ceiling_; }

    // How wide one orogenic belt is, in metres. Semi-relative, so it grows far
    // more slowly than the world does: this is the number that decides whether
    // a larger map is more regions or the same picture stretched.
    double beltWavelengthMetres() const { return beltWavelength_; }

private:
    enum class Layer { Chain, Plain, Platform };
    struct Octave {
        double wavelengthMetres = 1;
        double amplitudeMetres = 0;
        std::uint64_t seed = 0;
        Layer layer = Layer::Plain;
    };
    WorldDescriptor descriptor_;
    std::vector<Octave> octaves_;      // the relief above the continental shelf
    double continentalWavelengthX_ = 1, continentalWavelengthY_ = 1;
    double regionWavelength_ = 1;
    double warpMetres_ = 0, warpWavelength_ = 1;
    double beltWavelength_ = 1;
    // How much of the full shelf profile this world is wide enough to hold.
    double profileScale_ = 1;
    double crustSlope_ = 0, reliefSlope_ = 0, chainSlope_ = 0, amplitude_ = 0;
    double lipschitz_ = 0, ceiling_ = 0;
    std::uint64_t seed_ = 0, upliftSeed_ = 0;
    std::array<std::uint64_t, 5> crustSeeds_{};
    std::array<std::uint64_t, 2> warpSeeds_{};

    double continentAt(double x, double y) const;
    double upliftAt(double x, double y, double crust) const;
};

// A bounded sampled view of the field, at the descriptor's overview size.
//
// The bounds are the point of it. Each cell carries the lowest and the highest
// the field can reach ANYWHERE inside it, not the value at its corner, so a
// query over a rectangle answers for the continuous world rather than for the
// lattice the overview happens to use. An island between two overview nodes
// raises the ceiling of the cell that contains it.
struct MacroOverview {
    WorldDescriptor descriptor;
    std::uint32_t columns = 0, rows = 0;
    double cellWidthMetres = 0, cellHeightMetres = 0;
    std::vector<float> elevation;   // at the cell centre
    std::vector<float> floors;      // conservative minimum over the cell
    std::vector<float> ceilings;    // conservative maximum over the cell
    std::vector<float> continent;   // at the cell centre
    std::vector<float> uplift;      // at the cell centre
    // The height that leaves kLandShare of the cell centres above it. Every
    // other elevation here is raw, so a consumer either compares against this
    // or subtracts it; nothing else in the pipeline may invent a sea level.
    float seaLevelMetres = 0;

    std::size_t cells() const { return std::size_t(columns) * rows; }
    std::size_t bytes() const;
    // Cell containing a point, clamped to the overview: halo queries are legal.
    std::size_t indexAt(double x, double y) const;
};

// Build the overview. Cost is exactly `columns * rows * supersamples` field
// evaluations and one allocation of `bytes()`; neither grows with world area,
// because the descriptor's overview size does not.
MacroOverview buildMacroOverview(const MacroField&);

} // namespace generation
