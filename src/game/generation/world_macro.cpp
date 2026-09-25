#include "game/generation/world_macro.hpp"

#include "engine/core/rng.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace generation {
namespace {

// Smoothstep-interpolated value noise on a unit lattice. Smoothstep rather than
// linear because the Lipschitz bound below is stated for it, and because a
// linear lattice leaves creases along every cell edge that the ridge fold then
// turns into false mountain fronts.
double lattice(std::uint64_t seed, std::int64_t x, std::int64_t y) {
    // One finalizer over a combined input, not three chained ones. A page is
    // seventeen thousand samples and a sample is eleven octaves of four
    // corners, so this hash is called a hundred and thirty times per sample and
    // its cost IS the cost of the source: three chained mixes measured 260 ns a
    // sample, and streaming a world is paid in exactly that number. splitmix64
    // is a finalizer with full avalanche, and two odd multipliers keep distinct
    // lattice coordinates distinct over any range a world can address.
    const auto h = core::splitmix64(seed + std::uint64_t(x) * 0x9e3779b97f4a7c15ULL +
                                    std::uint64_t(y) * 0xc2b2ae3d27d4eb4fULL);
    return double(h >> 11) * 0x1p-53;
}

double smooth(double t) { return t * t * (3 - 2 * t); }

double valueNoise(std::uint64_t seed, double x, double y) {
    const auto ix = std::int64_t(std::floor(x)), iy = std::int64_t(std::floor(y));
    const double u = smooth(x - double(ix)), v = smooth(y - double(iy));
    const double a = lattice(seed, ix, iy), b = lattice(seed, ix + 1, iy);
    const double c = lattice(seed, ix, iy + 1), d = lattice(seed, ix + 1, iy + 1);
    return std::lerp(std::lerp(a, b, u), std::lerp(c, d, u), v);
}

// The fold that makes a crest a crease instead of a dome. Doubles the slope,
// which the Lipschitz bound accounts for.
double ridge(double value) { return 1 - std::abs(2 * value - 1); }

// A smoothstep lattice reaches its full amplitude over one cell with a peak
// slope of 1.5; ridging doubles it. Stated here once so the constant and its
// reason travel together.
constexpr double kLatticeSlope = 1.5;

} // namespace

std::size_t MacroOverview::bytes() const {
    return cells() * 5 * sizeof(float);
}

std::size_t MacroOverview::indexAt(double x, double y) const {
    if (!columns || !rows) throw std::logic_error("empty macro overview");
    const auto column = std::clamp(std::int64_t(std::floor(x / cellWidthMetres)),
                                   std::int64_t(0), std::int64_t(columns) - 1);
    const auto row = std::clamp(std::int64_t(std::floor(y / cellHeightMetres)),
                                std::int64_t(0), std::int64_t(rows) - 1);
    return std::size_t(row) * columns + std::size_t(column);
}

MacroField::MacroField(const WorldDescriptor& descriptor) : descriptor_(descriptor) {
    descriptor_.validate();
    seed_ = core::splitmix64(descriptor_.seed ^ 0x6d6163726fULL);
    const auto& domain = descriptor_.domain;

    // The continent follows the world: a couple of wavelengths across the side,
    // so any world is a composition rather than a crop of a larger one. The map
    // is not periodic, so this is a wavelength and not a tiling.
    //
    // Two rather than one, because one wavelength means the entire shape of a
    // small world rests on four lattice values, and four values is a lottery
    // rather than a composition: it decided whether a world was mostly land or
    // mostly ocean, and it decided it differently for every seed. Two and a bit
    // still reads as one arrangement and has enough samples to be an
    // arrangement rather than a coin.
    continentalWavelengthX_ = double(domain.widthMetres()) / 2.2;
    continentalWavelengthY_ = double(domain.heightMetres()) / 2.2;

    // Everything else is semi-relative: a belt is a belt at any world size, and
    // a world ten times wider holds more of them rather than wider ones. Thirty
    // kilometres puts three or four systems across a hundred-kilometre map,
    // which is the density the plan asks for, and sixty across ten thousand.
    const ScaleRule belts{ScaleClass::SemiRelative, 30000, 0.35, 12000, 260000};
    const ScaleRule warp{ScaleClass::SemiRelative, 150000, 0.35, 40000, 900000};
    beltWavelength_ = belts.metres(domain)[0];
    warpWavelength_ = warp.metres(domain)[0];

    // A world-relative continent alone gives every size the same silhouette
    // with more detail painted on, which is the "same picture stretched" this
    // rework exists to avoid. So the crust also carries a metric term whose
    // wavelength grows far more slowly than the world: at a hundred kilometres
    // it is nearly the whole map and adds nothing but a tilt, and at ten
    // thousand it cuts the landmass into regions - inland seas, gulfs, separate
    // masses. Clamped to the world so a small map can never be all ocean
    // because one feature happened to land on it.
    const ScaleRule regions{ScaleClass::SemiRelative, 320000, 0.35, 90000, 4000000};
    const double shortestSide = double(std::min(domain.widthMetres(), domain.heightMetres()));
    regionWavelength_ = std::clamp(regions.metres(domain)[0], shortestSide / 24, shortestSide / 1.2);

    // The warp displaces the sampling point, which is what turns concentric
    // blobs into margins that swing and interlock. Capped against the
    // continental wavelength: at belt scale alone a hundred-kilometre world was
    // displaced by a third of itself, which made its coastline a lottery and
    // shifted its land share away from every other size. Its slope is folded
    // into the Lipschitz bound through the chain rule below.
    warpMetres_ = std::min(beltWavelength_ * 0.45, regionWavelength_ * 0.10);

    // The crust's own slope has to be known before the profile that rides on
    // it can be sized, so it is computed here and reused by the bound below.
    const double warpSlope0 = 1 + 2 * kLatticeSlope * warpMetres_ / warpWavelength_;
    const double shortestWavelength = std::min(continentalWavelengthX_, continentalWavelengthY_);
    const double crust = warpSlope0 * kLatticeSlope *
        ((0.62 + 0.26 * 2.3 + 0.12 * 5.1) / shortestWavelength +
         (kRegionalCrust + kRegionalCrust * 0.42 * 2.7) / regionWavelength_);
    // A margin falls `kContinentalRelief * profileScale_` over the crust band
    // `kMarginBand`, and that fall must not exceed `kMarginSlope`.
    profileScale_ = std::clamp(kMarginSlope * kMarginBand /
                               (kLatticeSlope * crust * kContinentalRelief), 0.0, 1.0);

    // Relief in three layers rather than one stack of octaves, because a stack
    // of octaves is a crumpled sheet: every part of every continent equally
    // rough, no plain, no range, no basin. What a composition owes the stages
    // below it is WHERE the mountains are.
    //
    // - Chains: ridged octaves that only exist inside the belt skeleton, so
    //   they read as connected ranges with saddles rather than as noise.
    // - Plains: a low, rounded layer over all land, which is most of a continent.
    // - Platform: a broad regional tilt - basins and plateaus, hundreds of
    //   metres over hundreds of kilometres.
    octaves_ = {
        {beltWavelength_ * 0.62, kChainRelief * 0.54, core::splitmix64(seed_ ^ 1), Layer::Chain},
        {beltWavelength_ * 0.29, kChainRelief * 0.29, core::splitmix64(seed_ ^ 2), Layer::Chain},
        {beltWavelength_ * 0.13, kChainRelief * 0.17, core::splitmix64(seed_ ^ 3), Layer::Chain},
        {beltWavelength_ * 0.34, kPlainRelief * 0.62, core::splitmix64(seed_ ^ 4), Layer::Plain},
        {beltWavelength_ * 0.12, kPlainRelief * 0.38, core::splitmix64(seed_ ^ 5), Layer::Plain},
        {regionWavelength_ * 0.55, kPlatformRelief, core::splitmix64(seed_ ^ 6), Layer::Platform},
    };

    // A bound, not a measurement, and a product rule rather than a sum: the
    // relief is gated by how continental the crust is and by how strong the
    // belt is, and a gate that opens over a tenth of a unit of crust is itself
    // a slope. Leaving those terms out understates the bound by an order of
    // magnitude, and an understated bound is an island quietly deleted.
    //
    // The warp displaces the point every term below reads, so its own slope
    // multiplies all of them; the displacement runs over [-1,1], which doubles
    // the lattice slope before it is scaled by the displacement distance.
    const double warpSlope = warpSlope0;
    const double crustSlope = crust;
    // |grad belt|, ridged, so twice the lattice slope; and the chain gate is a
    // smoothstep over it, which multiplies that again over its own width.
    // |grad belt| now also carries the margin gate, whose own slope in crust
    // units is at most two over its band: d(1-d^2)/dd = -2d and d <= 1.
    const double beltSlope = warpSlope * 2 * kLatticeSlope / beltWavelength_ +
                             (1 - kOrogenInterior) * 2 / kOrogenBand * crust;
    const double chainGateSlope = kLatticeSlope / kChainGateWidth * beltSlope;

    double amplitude = 0, reliefSlope = 0, chainAmplitude = 0;
    for (const auto& octave : octaves_) {
        amplitude += octave.amplitudeMetres;
        const double fold = octave.layer == Layer::Chain ? 2 : 1;
        const double own = fold * kLatticeSlope * warpSlope / octave.wavelengthMetres;
        // d(value * gate) = gate * d(value) + value * d(gate).
        const double gate = octave.layer == Layer::Chain ? chainGateSlope : 0;
        reliefSlope += octave.amplitudeMetres * (own + gate);
        chainAmplitude += octave.layer == Layer::Chain ? octave.amplitudeMetres : 0;
    }
    // Derived once. These were splitmix64 calls inside `at`, which is a hot
    // loop: six of them per sample, paid for nothing.
    crustSeeds_ = {core::splitmix64(seed_ ^ 0x11), core::splitmix64(seed_ ^ 0x12),
                   core::splitmix64(seed_ ^ 0x13), core::splitmix64(seed_ ^ 0x14),
                   core::splitmix64(seed_ ^ 0x15)};
    upliftSeed_ = core::splitmix64(seed_ ^ 0x21);
    warpSeeds_ = {core::splitmix64(seed_ ^ 0x31), core::splitmix64(seed_ ^ 0x32)};
    crustSlope_ = crustSlope;
    reliefSlope_ = reliefSlope;
    chainSlope_ = chainAmplitude;
    amplitude_ = amplitude;

    // The continental slope, the shelf, the interior and the land gate are all
    // smoothsteps over the crust, so each turns the crust's slope into its own
    // over its own width. The narrowest of them - the coast - dominates, which
    // is correct: a coast IS the steepest thing the composition draws.
    const double shelfSlope = profileScale_ * ((kAbyssalDepth - kShelfDepth) * kLatticeSlope /
                                               kMarginBand * crustSlope +
                                               kShelfDepth * kLatticeSlope / kCoastBand * crustSlope);
    const double interiorSlope = profileScale_ * kInteriorHeight * kLatticeSlope /
                                 kInteriorBand * crustSlope;
    const double landGateSlope = kLatticeSlope / kLandGateBand * crustSlope;

    lipschitz_ = shelfSlope + interiorSlope + reliefSlope + amplitude * landGateSlope;
    ceiling_ = kInteriorHeight * profileScale_ + amplitude;
}

double MacroField::elevationLipschitz(double crustLow, double crustHigh) const {
    if (!std::isfinite(crustLow) || !std::isfinite(crustHigh) || crustHigh < crustLow)
        return lipschitz_;
    // A smoothstep's derivative is zero outside its own band, so a band the
    // crust range never enters contributes nothing. Every term below is the
    // same one the global bound sums; the only difference is that it is allowed
    // to notice where it is.
    const auto crosses = [&](double start, double width) {
        return crustHigh >= start && crustLow <= start + width;
    };
    double bound = 0;
    if (crosses(kCoastCrust - kMarginBand - kCoastBand, kMarginBand))
        bound += profileScale_ * (kAbyssalDepth - kShelfDepth) * kLatticeSlope / kMarginBand * crustSlope_;
    if (crosses(kCoastCrust - kCoastBand, kCoastBand))
        bound += profileScale_ * kShelfDepth * kLatticeSlope / kCoastBand * crustSlope_;
    if (crosses(kCoastCrust, kInteriorBand))
        bound += profileScale_ * kInteriorHeight * kLatticeSlope / kInteriorBand * crustSlope_;
    // The relief is multiplied by the land gate, so the most it can move is set
    // by how far open that gate gets anywhere in the range.
    const double gateStart = kCoastCrust - kLandGateBand * 0.5;
    const double land = std::clamp((crustHigh - gateStart) / kLandGateBand, 0.0, 1.0);
    const double open = land * land * (3 - 2 * land);
    bound += reliefSlope_ * open;
    if (crosses(gateStart, kLandGateBand))
        bound += amplitude_ * kLatticeSlope / kLandGateBand * crustSlope_;
    return std::min(bound, lipschitz_);
}

double MacroField::continentAt(double x, double y) const {
    // Three world-relative octaves: where the land is at all. Two drew an oval.
    const double u = x / continentalWavelengthX_, v = y / continentalWavelengthY_;
    const double a = valueNoise(crustSeeds_[0], u, v);
    const double b = valueNoise(crustSeeds_[1], u * 2.3, v * 2.3);
    const double c = valueNoise(crustSeeds_[2], u * 5.1, v * 5.1);
    const double world = a * 0.62 + b * 0.26 + c * 0.12;
    // Two metric octaves on top, centred so they move the crust either way and
    // leave the average where it was: the land share is a property of the
    // composition, not of how big the map happens to be.
    const double p = x / regionWavelength_, q = y / regionWavelength_;
    const double r = valueNoise(crustSeeds_[3], p, q) - 0.5;
    const double t = valueNoise(crustSeeds_[4], p * 2.7, q * 2.7) - 0.5;
    return std::clamp(world + r * kRegionalCrust + t * (kRegionalCrust * 0.42), 0.0, 1.0);
}

double MacroField::upliftAt(double x, double y, double crust) const {
    // Ridged noise alone gives a labyrinth: crests in every direction, equally
    // strong everywhere, which reads as a crumpled sheet rather than as ranges.
    // Real belts are linear because they follow a margin, so the skeleton is
    // the ridge noise gated by how close this point is to one - the crust
    // transition - with a floor so continental interiors still get their own
    // older, lower ranges instead of being billiard tables.
    const double belt = ridge(valueNoise(upliftSeed_, x / beltWavelength_, y / beltWavelength_));
    const double distance = std::abs(crust - kOrogenCrust) / kOrogenBand;
    const double margin = std::clamp(1 - distance * distance, 0.0, 1.0);
    return std::clamp(belt * (kOrogenInterior + (1 - kOrogenInterior) * margin), 0.0, 1.0);
}

MacroSample MacroField::at(double x, double y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("macro sample is not finite");

    // Domain warp first: everything below reads the displaced point, which is
    // what interlocks the margins instead of leaving concentric rings.
    const double wx = valueNoise(warpSeeds_[0], x / warpWavelength_, y / warpWavelength_);
    const double wy = valueNoise(warpSeeds_[1], x / warpWavelength_, y / warpWavelength_);
    const double px = x + (wx * 2 - 1) * warpMetres_, py = y + (wy * 2 - 1) * warpMetres_;

    MacroSample sample;
    sample.continent = float(continentAt(px, py));

    // The shelf profile, as named breakpoints on the crust rather than as two
    // smoothsteps whose product happened to put the coast wherever it put it.
    // The crust runs around a half, so the coast sits just below that: land
    // share is decided here and nowhere else, and it is the same at every size.
    const double c = sample.continent;
    const double slope = smooth(std::clamp((c - kCoastCrust + kMarginBand + kCoastBand) /
                                           kMarginBand, 0.0, 1.0));           // abyss -> shelf break
    const double shelf = smooth(std::clamp((c - kCoastCrust + kCoastBand) / kCoastBand, 0.0, 1.0));
    const double interior = smooth(std::clamp((c - kCoastCrust) / kInteriorBand, 0.0, 1.0));
    double elevation = profileScale_ * (-kAbyssalDepth + (kAbyssalDepth - kShelfDepth) * slope +
                                        kShelfDepth * shelf + kInteriorHeight * interior);

    // Relief rides on land and fades out under water: an abyssal plain with
    // alpine ridges on it is noise, not composition. The gate is wide enough
    // that the shelf carries the near-shore shape instead of the relief
    // switching on at the waterline - a knife-edge gate makes every coastal
    // rectangle undecidable for the land mask and buys nothing.
    const double land = smooth(std::clamp((c - kCoastCrust + kLandGateBand * 0.5) /
                                         kLandGateBand, 0.0, 1.0));
    // The chain gate is the skeleton: only the crests of the belt noise carry
    // mountains, so ranges are connected lines with saddles between them and
    // the ground between the ranges is plain.
    const double belt = upliftAt(px, py, c);
    const double chain = smooth(std::clamp((belt - kChainGateStart) / kChainGateWidth, 0.0, 1.0));
    sample.uplift = float(chain * land);
    double amplitude = 0;
    for (const auto& octave : octaves_) {
        const double raw = valueNoise(octave.seed, px / octave.wavelengthMetres,
                                      py / octave.wavelengthMetres);
        switch (octave.layer) {
        case Layer::Chain: amplitude += ridge(raw) * octave.amplitudeMetres * chain; break;
        case Layer::Plain: amplitude += (raw * 2 - 1) * octave.amplitudeMetres; break;
        case Layer::Platform: amplitude += (raw - 0.5) * 2 * octave.amplitudeMetres; break;
        }
    }
    elevation += amplitude * land;
    sample.elevationMetres = float(elevation);
    return sample;
}

MacroOverview buildMacroOverview(const MacroField& field) {
    const auto& descriptor = field.descriptor();
    descriptor.validate();
    MacroOverview overview;
    overview.descriptor = descriptor;
    overview.columns = descriptor.overviewSamples[0];
    overview.rows = descriptor.overviewSamples[1];
    overview.cellWidthMetres = double(descriptor.domain.widthMetres()) / overview.columns;
    overview.cellHeightMetres = double(descriptor.domain.heightMetres()) / overview.rows;

    const auto count = overview.cells();
    overview.elevation.resize(count);
    overview.floors.resize(count);
    overview.ceilings.resize(count);
    overview.continent.resize(count);
    overview.uplift.resize(count);

    // Supersampling narrows the bound; the Lipschitz term is what makes it
    // sound. Four by four keeps the build near sixteen field evaluations a
    // cell, which is what a bounded overview can afford at any world size.
    constexpr int kSupersamples = 4;
    const double halfDiagonal = std::hypot(overview.cellWidthMetres, overview.cellHeightMetres) * 0.5;
    // Each supersample covers its own sub-cell, so the allowance is the
    // sub-cell's half-diagonal, not the whole cell's.
    const double reach = halfDiagonal / kSupersamples * field.elevationLipschitz();

    // Rows are independent and the field is a closed form, so this parallelises
    // exactly: the result is the same bits whatever the thread count, which is
    // the property a source has to have if two machines are to agree about a
    // world. Bands rather than rows so the scheduling overhead does not eat a
    // small overview.
    const auto band = [&](std::uint32_t first, std::uint32_t last) {
        for (std::uint32_t row = first; row < last; ++row)
            for (std::uint32_t column = 0; column < overview.columns; ++column) {
                const double x0 = column * overview.cellWidthMetres;
                const double y0 = row * overview.cellHeightMetres;
                const auto centre = field.at(x0 + overview.cellWidthMetres * 0.5,
                                             y0 + overview.cellHeightMetres * 0.5);
                double low = centre.elevationMetres, high = centre.elevationMetres;
                for (int sy = 0; sy < kSupersamples; ++sy)
                    for (int sx = 0; sx < kSupersamples; ++sx) {
                        const double x = x0 + (sx + 0.5) * overview.cellWidthMetres / kSupersamples;
                        const double y = y0 + (sy + 0.5) * overview.cellHeightMetres / kSupersamples;
                        const double h = field.elevationAt(x, y);
                        low = std::min(low, h);
                        high = std::max(high, h);
                    }
                const auto index = std::size_t(row) * overview.columns + column;
                overview.elevation[index] = centre.elevationMetres;
                overview.continent[index] = centre.continent;
                overview.uplift[index] = centre.uplift;
                overview.floors[index] = float(low - reach);
                overview.ceilings[index] = float(high + reach);
            }
    };
    const auto threads = std::max(1u, std::min(std::thread::hardware_concurrency(),
                                               overview.rows / 16 + 1));
    if (threads <= 1) {
        band(0, overview.rows);
    } else {
        std::vector<std::future<void>> workers;
        const std::uint32_t span = (overview.rows + threads - 1) / threads;
        for (std::uint32_t first = 0; first < overview.rows; first += span)
            workers.push_back(std::async(std::launch::async, band, first,
                                         std::min(first + span, overview.rows)));
        for (auto& worker : workers) worker.get();
    }
    // The sea level of THIS world, from the overview that is already built.
    // Cell centres rather than the supersamples: the bound arrays exist to be
    // conservative, and a conservative bound is the wrong thing to take a
    // percentile of.
    auto sorted = overview.elevation;
    std::sort(sorted.begin(), sorted.end());
    const auto at = std::size_t((1.0 - MacroField::kLandShare) * double(sorted.size() - 1));
    overview.seaLevelMetres = sorted.empty() ? 0.0f : sorted[at];
    return overview;
}

} // namespace generation
