#include "engine/biomes/category_field.hpp"
#include "engine/biomes/patch_field.hpp"
#include "engine/environment/feature_layer.hpp"
#include "game/world/height_field.hpp"

#include "game/world/terrain_streaming/hydrology_builder.hpp"

#include <algorithm>

#include "engine/core/rng.hpp"
#include "game/generation/hybrid_terrain.hpp"
#include "game/generation/mountain_shape.hpp"
#include "game/generation/world_map_gen.hpp"

namespace world {

struct HeightField::QueryCache::State {
    template<class Value, std::size_t Count> struct Memo {
        struct Entry {
            std::int64_t x = 0, y = 0, stride = 0;
            std::uint64_t epoch = 0;
            Value value{};
            bool matches(std::int64_t sx, std::int64_t sy, std::int64_t step,
                         std::uint64_t version) const {
                return epoch == version && x == sx && y == sy && stride == step;
            }
            Value put(std::int64_t sx, std::int64_t sy, std::int64_t step,
                      std::uint64_t version, Value result) {
                x = sx; y = sy; stride = step; epoch = version; value = result;
                return result;
            }
        };
        std::array<Entry, Count> entries{};
        Entry& at(std::int64_t x, std::int64_t y, std::int64_t stride) {
            const auto h = core::splitmix64(std::uint64_t(x) * 0x9e3779b97f4a7c15ull ^
                std::uint64_t(y) * 0xc2b2ae3d27d4eb4full ^ std::uint64_t(stride));
            return entries[h & (Count - 1)];
        }
    };
    Memo<core::Fixed, 4096> heights;
    Memo<core::Fixed, 2048> slopes;
    Memo<MaterialWeights, 2048> materials;
    const EditLayer* edits = nullptr;
    std::uint64_t revision = 0, epoch = 1, features = 0;
    generation::TerrainStage stage = generation::TerrainStage::Final;
    void prepare(const HeightField& field) {
        const auto current = field.edits_ ? field.edits_->revision() : 0;
        const auto terrainStage = field.coarse_ ? field.coarse_->terrainStage : generation::TerrainStage::Final;
        const auto layer = field.features_ ? field.features_->generation() * 1000003ull + std::uint64_t(field.featureStride_) : 0;
        if (edits != field.edits_ || revision != current || stage != terrainStage || features != layer) {
            edits = field.edits_; revision = current; stage = terrainStage; features = layer; ++epoch;
        }
    }
};

thread_local HeightField::QueryCache* HeightField::QueryCache::active_ = nullptr;

HeightField::QueryCache::QueryCache(HeightField& field)
    : field_(field), previous_(active_), state_(std::make_unique<State>()) {
    active_ = this;
}
HeightField::QueryCache::~QueryCache() { active_ = previous_; }

HeightField::QueryCache::State* HeightField::cachedQueries() const {
    // A copied field must not inherit another field's active cache. Workers
    // and nested scopes have independent lifetimes, even when seeds match.
    for (auto* scope = QueryCache::active_; scope; scope = scope->previous_) {
        if (&scope->field_ == this) {
            scope->state_->prepare(*this);
            return scope->state_.get();
        }
    }
    return nullptr;
}

// How far down the open sea's floor lies. Shallow on purpose: it is something
// for the water to sit on rather than a bathymetry, and nothing goes down there.
constexpr std::int64_t kSeaBedMetres = generation::HybridTerrain::kSeaBedMetres;

namespace {

using core::Fixed;
using core::WorldPos;

// Integer value noise on a global lattice. The same function the local
// generator has always used, lifted here because it is the one part of terrain
// generation that must be identical whoever asks and from wherever: it is keyed
// on a global coordinate and nothing else.
std::int32_t valueNoise(std::uint64_t seed, std::int64_t x, std::int64_t y) {
    std::uint64_t h = seed;
    h = core::splitmix64(h ^ (static_cast<std::uint64_t>(x) * 0x9e3779b97f4a7c15ULL));
    h = core::splitmix64(h ^ (static_cast<std::uint64_t>(y) * 0xc2b2ae3d27d4eb4fULL));
    return static_cast<std::int32_t>(h & 1023);
}

// Eases a fraction so the interpolation between two lattice values arrives and
// leaves flat: 3t^2 - 2t^3. Straight bilinear is continuous but kinked, and a
// kink is a ridge - under any shading at all, plain bilinear noise draws the
// lattice it was built on as a grid of creases across the whole country.
Fixed ease(Fixed t) { return t * t * (Fixed::fromInt(3) - Fixed::fromInt(2) * t); }

// A curve through four values, reading the two beyond the span it is
// interpolating: Catmull-Rom. Where a fraction is eased between two values the
// surface arrives flat at every one of them, which turns a lattice of heights
// into a lattice of domes - correct in the small and, over a mountain range, a
// field of identical bumps with the valleys ruled along the axes. A curve that
// takes its slope from the neighbours carries a rise through a cell instead of
// levelling it off, so a range comes out as ridges and spurs.
Fixed catmull(Fixed p0, Fixed p1, Fixed p2, Fixed p3, Fixed t) {
    const Fixed t2 = t * t;
    const Fixed t3 = t2 * t;
    const Fixed a = p1 * Fixed::fromInt(2);
    const Fixed b = (p2 - p0) * t;
    const Fixed c = (p0 * Fixed::fromInt(2) - p1 * Fixed::fromInt(5) + p2 * Fixed::fromInt(4) - p3) * t2;
    const Fixed d = (p1 * Fixed::fromInt(3) - p0 - p2 * Fixed::fromInt(3) + p3) * t3;
    return (a + b + c + d) / Fixed::fromInt(2);
}

// Noise with a crease in it: folded about its middle, so instead of rolling
// hills it gives crests and gullies. What mountains are made of, and the one
// thing plain value noise cannot produce at any amplitude.
Fixed ridgeNoise(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t scale);
Fixed gradientNoise(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t scale);

// Gradient noise, of `scale` metres. Deterministic: the gradient at a lattice
// point is chosen by hashing its global coordinate, and everything between is
// interpolation.
//
// Not value noise, which is what this was. Value noise picks a number at each
// lattice point and eases between them, so every lattice point is a flat spot -
// a little plateau at the top of a bump - and a country made of it comes out as
// a tiling of rounded rectangles with flat tops. It is the most recognisable
// artefact in generated terrain and no amount of octaves hides it. Gradient
// noise is nought at every lattice point and slopes away from it, so the
// lattice leaves no mark at all.
Fixed gradientNoise(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t scale) {
    struct Direction { Fixed x, y; };
    // Sixteen ways round the circle. Enough that the directions do not read as
    // a pattern, few enough to be a table.
    static const Direction kDirections[16] = {
        {Fixed::ratio(10000, 10000), Fixed::ratio(0, 10000)},
        {Fixed::ratio(9239, 10000), Fixed::ratio(3827, 10000)},
        {Fixed::ratio(7071, 10000), Fixed::ratio(7071, 10000)},
        {Fixed::ratio(3827, 10000), Fixed::ratio(9239, 10000)},
        {Fixed::ratio(0, 10000), Fixed::ratio(10000, 10000)},
        {Fixed::ratio(-3827, 10000), Fixed::ratio(9239, 10000)},
        {Fixed::ratio(-7071, 10000), Fixed::ratio(7071, 10000)},
        {Fixed::ratio(-9239, 10000), Fixed::ratio(3827, 10000)},
        {Fixed::ratio(-10000, 10000), Fixed::ratio(0, 10000)},
        {Fixed::ratio(-9239, 10000), Fixed::ratio(-3827, 10000)},
        {Fixed::ratio(-7071, 10000), Fixed::ratio(-7071, 10000)},
        {Fixed::ratio(-3827, 10000), Fixed::ratio(-9239, 10000)},
        {Fixed::ratio(0, 10000), Fixed::ratio(-10000, 10000)},
        {Fixed::ratio(3827, 10000), Fixed::ratio(-9239, 10000)},
        {Fixed::ratio(7071, 10000), Fixed::ratio(-7071, 10000)},
        {Fixed::ratio(9239, 10000), Fixed::ratio(-3827, 10000)}
    };
    const std::int64_t cx = floorDiv(x, scale), cy = floorDiv(y, scale);
    const Fixed fx = Fixed::ratio(floorMod(x, scale), scale);
    const Fixed fy = Fixed::ratio(floorMod(y, scale), scale);
    const auto corner = [&](std::int64_t ax, std::int64_t ay, Fixed dx, Fixed dy) {
        const Direction& g = kDirections[valueNoise(seed, ax, ay) % 16];
        return g.x * dx + g.y * dy;
    };
    const Fixed n00 = corner(cx, cy, fx, fy);
    const Fixed n10 = corner(cx + 1, cy, fx - core::kOne, fy);
    const Fixed n01 = corner(cx, cy + 1, fx, fy - core::kOne);
    const Fixed n11 = corner(cx + 1, cy + 1, fx - core::kOne, fy - core::kOne);
    const Fixed ex = fx, ey = fy;   // EASE-OFF
    const Fixed top = n00 + (n10 - n00) * ex;
    const Fixed bottom = n01 + (n11 - n01) * ex;
    // Gradient noise of this kind reaches about seven tenths either way; scaled
    // so that what comes out of here is about minus one to one.
    return (top + (bottom - top) * ey) * Fixed::ratio(14, 10);
}

// Smoothed to blobs of `scale` metres. Floor division throughout, so the noise
// is continuous through the origin instead of folding along the axes.
Fixed smoothNoise(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t scale) {
    const std::int64_t cx = floorDiv(x, scale), cy = floorDiv(y, scale);
    const Fixed fx = ease(Fixed::ratio(floorMod(x, scale), scale));
    const Fixed fy = ease(Fixed::ratio(floorMod(y, scale), scale));
    const auto value = [&](std::int64_t ax, std::int64_t ay) {
        return Fixed::ratio(valueNoise(seed, ax, ay), 1024);
    };
    const Fixed v00 = value(cx, cy), v10 = value(cx + 1, cy);
    const Fixed v01 = value(cx, cy + 1), v11 = value(cx + 1, cy + 1);
    const Fixed top = v00 + (v10 - v00) * fx;
    const Fixed bottom = v01 + (v11 - v01) * fx;
    return top + (bottom - top) * fy;
}

// The detail layer, in metres about zero.
//
// Each octave's amplitude is tied to its own wavelength, because what matters
// on the ground is not how tall a bump is but how steep it is: a hundred metres
// of rise over six hundred is a hillside, and the same hundred over sixty is a
// wall. An octave given amplitude out of a shared budget, the usual way, is
// exactly how a fractal ends up making every four metres of the world a cliff.
//
// `steepness` is the most each octave may contribute to the slope. They add, so
// the three of them together form the walkable background. Steep highland
// faces are a separate bounded layer, not a global increase in roughness.
Fixed detailMetres(std::uint64_t seed, std::int64_t x, std::int64_t y, Fixed scale, Fixed crease,
                   std::int64_t finestWave) {
    struct Octave { std::int64_t metres; core::Fixed steepness; };
    const Octave octaves[] = {{620, Fixed::ratio(11, 100)},
                              {210, Fixed::ratio(9, 100)},
                              {70, Fixed::ratio(6, 100)}};
    Fixed sum = core::kZero;
    std::uint64_t stream = seed;
    for (const Octave& o : octaves) {
        // Shorter than two of the samples being taken: it cannot be drawn by
        // them, only aliased by them, so it is left out rather than turned down.
        // Left out it also costs nothing, which is the whole point - two noises
        // and a hash chain per octave, at every sample of a continent.
        if (finestWave > 0 && o.metres < finestWave) {
            stream = core::splitmix64(stream);
            continue;
        }
        // Mountain detail is subordinate to the long ridge profile below, not
        // another complete range at every octave. Leave lowland rolling alone.
        const auto mountainShare = Fixed::ratio(o.metres >= 620 ? 60 : o.metres >= 210 ? 18 : 8, 100);
        const Fixed swing = Fixed::fromInt(o.metres) * o.steepness *
                            core::lerp(core::kOne, mountainShare, crease);
        // Rolling and creased, mixed by how broken the country is: a flood
        // plain gets none of the crease, an upland is mostly crease. Ridges are
        // what makes a range read as rock rather than as dunes.
        const Fixed rolling = gradientNoise(stream, x, y, o.metres);
        // Half amplitude for the creased part, because a crease is twice as
        // steep for the same height: folding the noise about zero doubles its
        // slope at the fold, and given the same swing as the rolling noise it
        // made every mountain in the world a face that has to be climbed. Half
        // puts a ridge and a hillside at the same angle, which is what they are.
        const Fixed crest = ridgeNoise(stream ^ 0x9d21, x, y, o.metres) * Fixed::ratio(1, 2);
        const Fixed ridgeShare = o.metres >= 620 ? crease * Fixed::ratio(1, 3) : core::kZero;
        const Fixed mixed = rolling + (crest - rolling) * ridgeShare;
        sum += mixed * swing;
        stream = core::splitmix64(stream);
    }
    return sum * scale;
}

Fixed ridgeNoise(std::uint64_t seed, std::int64_t x, std::int64_t y, std::int64_t scale) {
    // Folded about zero and turned over: what was a slope through nought
    // becomes a crest. Centred afterwards so it swings either way like the
    // rolling noise it is mixed with, rather than sitting above it.
    const Fixed n = gradientNoise(seed, x, y, scale);
    const Fixed folded = core::kOne - Fixed::fromRaw(n.raw < 0 ? -n.raw : n.raw);
    return (folded - Fixed::ratio(1, 2)) * Fixed::fromInt(2);
}

// The same shape, as a fraction from nought to one, for the things that want a
// pattern rather than a height: which way a patch of ground leans, how wet it
// is, where the sand gives out.
Fixed fractalNoise(std::uint64_t seed, std::int64_t x, std::int64_t y) {
    return (smoothNoise(seed, x, y, 600) * Fixed::fromInt(8) +
            smoothNoise(seed + 7, x, y, 190) * Fixed::fromInt(4) +
            smoothNoise(seed + 13, x, y, 60)) /
           Fixed::fromInt(13);
}

// A weight that fades in over a band rather than switching at a threshold. This
// is what keeps a material transition off the lattice: at the edge of a band the
// weight is a fraction, and the fraction is different at every sample.
Fixed ramp(std::int32_t value, std::int32_t from, std::int32_t to) {
    if (from == to) return value >= to ? core::kOne : core::kZero;
    const std::int32_t lo = std::min(from, to), hi = std::max(from, to);
    const std::int32_t clamped = std::clamp(value, lo, hi);
    Fixed t = Fixed::ratio(clamped - lo, hi - lo);
    return from <= to ? t : core::kOne - t;
}

Fixed rampFixed(Fixed value, Fixed from, Fixed to) {
    if (from.raw == to.raw) return value.raw >= to.raw ? core::kOne : core::kZero;
    const Fixed lo = from.raw < to.raw ? from : to;
    const Fixed hi = from.raw < to.raw ? to : from;
    Fixed clamped = value;
    if (clamped.raw < lo.raw) clamped = lo;
    if (clamped.raw > hi.raw) clamped = hi;
    const Fixed t = (clamped - lo) / (hi - lo);
    return from.raw <= to.raw ? t : core::kOne - t;
}

Fixed alpineMetres(std::uint64_t seed, std::int64_t x, std::int64_t y,
                   Fixed elevation, Fixed relief, Fixed seaShare) {
    const Fixed land = core::kOne - core::saturate(seaShare);
    const Fixed mask = ease(rampFixed(elevation, Fixed::fromInt(280), Fixed::fromInt(900))) *
        ease(rampFixed(relief, Fixed::fromInt(80), Fixed::fromInt(260))) * land * land;
    if (mask <= core::kZero) return core::kZero;
    // A few coherent spines with kilometre-scale saddles, not intersections of
    // 730/470/170 m noises. Rivers still own their beds through GraphCarver.
    const auto shape = generation::mountains::worldSample(seed, Fixed::fromInt(x), Fixed::fromInt(y));
    // Bounded by [-30,+407] m, independent of LOD and inside the shared margin.
    return mask * (shape.height() * Fixed::fromInt(380) - Fixed::fromInt(30));
}

// The shape between sixty-four metres and four.
//
// H64 is the geometry authority and it is a grid of sixty-four metres. Between
// its samples there was nothing at all but the reconstruction - and that is the
// whole of why the ground in this world reads as plasticine. There is no shape
// at the scale a person actually looks at, and no amount of shading hides an
// absence of form.
//
// What goes there is EROSION, not noise, and the difference is the whole point.
//
// Noise laid over a hillside knows nothing about the hillside. Ridged, summed,
// warped, gated on slope - every version of it was tried here and every version
// reads the same way, because a pattern that does not know which way is downhill
// cannot put a gully anywhere a gully would be. That is where the circuit-board
// look comes from: the eye finds the lattice the pattern was built on, because
// there is nothing else in it to find.
//
// This is a filter instead: stripes aligned to the fall line, so what it cuts
// runs downhill because that is the only direction it is able to cut in. Four
// octaves, and between each the gradient is RE-READ from the gullies already
// cut - so the small ones run down the flanks of the big ones, at the angle
// those flanks actually lie at, and the result branches. Branching is not
// decoration; it is the thing that says water did this.
//
// Two properties it has to keep, and it keeps them by construction:
//
// It is a function of position and nothing else. No iteration, no neighbours,
// no world-sized array - so it is the same ground however the camera got here,
// at any stride, in any order, in any thread. That is what lets the geometry
// authority be a formula rather than a stored field.
//
// And it only ever REMOVES. The wave is offset so its crest is zero and its
// trough is the full depth, which is what erosion does and is also what keeps
// this out of trouble: a layer that adds ridges dams the ground into closed
// hollows, and every one of those fills with water. That is exactly what put
// the whole country under puddles before.
//
// After: Rune Skovbo Johansen, "Fast and gorgeous erosion filter".

// A length, to four per cent, without a square root.
//
// core::hypot is a hundred-and-twenty-eight-bit Newton iteration with a divide
// in every step, and the erosion filter below wanted eight of them for every
// height in the world: measured, that one function was most of the fourteen
// hundred nanoseconds a sample cost, which is a page of ground taking a
// twentieth of a second to build.
//
// Alpha-max-plus-beta-min. What the length is used for here is the DIRECTION of
// a stripe and how much of it to lay down, and neither of those can tell four
// per cent from exact - they are a noise field's steering, not a measurement of
// the ground. Where a real length is wanted, hypot is still what is called.
Fixed roughLength(Fixed x, Fixed y) {
    const Fixed a = core::abs(x), b = core::abs(y);
    return core::max(a, b) + core::min(a, b) * Fixed::ratio(7, 16);
}

// A parabolic stand-in for a sine, because there is no sine in fixed point here
// and a lookup table is a lookup table. Over one period it is continuous in
// value and in slope, including across the wrap, and it is within a few per
// cent of the real thing - which is far closer than a stripe pattern needs.
Fixed waveOf(Fixed turns) {
    const auto period = Fixed::fromInt(2);
    const Fixed u = Fixed::fromRaw(floorMod((turns + core::kOne).raw, period.raw)) - core::kOne;
    return u * (core::kOne - core::abs(u)) * Fixed::fromInt(4);
}

// One octave of gullies, cut across the given fall line.
//
// The phase is measured from a PIVOT jittered inside each lattice cell, and
// four of them are blended - which is what stops the stripes being one comb
// ruled across the whole world. Two waves of the same frequency at different
// phases blend into another wave of that frequency with a smaller amplitude, so
// the pattern loses coherence at cell boundaries and finds it again inside
// them: gullies that start, run, and stop, rather than a corduroy.
//
// The pair is then pushed back out towards the unit circle, or everything near
// a boundary comes out shallower than everything else and the blending shows up
// as a grid of faint patches - the lattice again, by the back door.
struct Stripe { Fixed height, across; };
Stripe stripeAt(std::uint64_t seed, Fixed x, Fixed y, Fixed ax, Fixed ay, std::int64_t wave) {
    const auto span = Fixed::fromInt(wave);
    const std::int64_t cx = floorDiv(x.toInt(), wave), cy = floorDiv(y.toInt(), wave);
    const Fixed tx = Fixed::ratio(floorMod(x.toInt(), wave), wave);
    const Fixed ty = Fixed::ratio(floorMod(y.toInt(), wave), wave);
    Fixed height, across;
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            std::uint64_t h = core::splitmix64(seed ^ std::uint64_t(cx + i) * 0x9e3779b97f4a7c15ULL);
            h = core::splitmix64(h ^ std::uint64_t(cy + j) * 0xc2b2ae3d27d4eb4fULL);
            const Fixed px = Fixed::fromInt((cx + i) * wave) + Fixed::ratio(std::int64_t(h & 1023), 1024) * span;
            const Fixed py = Fixed::fromInt((cy + j) * wave) +
                             Fixed::ratio(std::int64_t((h >> 12) & 1023), 1024) * span;
            // Twice the offset over the wavelength, because one period of the
            // wave above spans two of its own units.
            const Fixed turns = ((x - px) * ax + (y - py) * ay) * 2 / span;
            const Fixed weight = (i ? tx : core::kOne - tx) * (j ? ty : core::kOne - ty);
            height += waveOf(turns + core::kOne / 2) * weight;
            across += waveOf(turns) * weight;
        }
    // Pushed back out towards the unit circle, but only so far. The gain is
    // also a gain on the wave's SLOPE - at two it doubles it - and four octaves
    // of that stack: measured, faces of a metre and a half of fall per metre,
    // fifty-six degrees, which is a cliff and not a gully. Three halves keeps
    // the depth without the wall.
    const Fixed magnitude = roughLength(height, across);
    const Fixed cap = Fixed::ratio(3, 2);
    const Fixed gain = magnitude > Fixed::ratio(2, 3) ? core::min(cap, core::kOne / magnitude) : cap;
    return {core::clamp(height * gain, -core::kOne, core::kOne),
            core::clamp(across * gain, -core::kOne, core::kOne)};
}

// What the filter cut, and what that did to the ground.
//
// The gradient and the wall fraction come out of the same arithmetic that cuts
// the gullies, so asking for them costs nothing - and the alternative is four
// more evaluations of the whole filter to difference it, per material sample.
//
// They matter because the material pass was painting the mountains off the
// sixty-four-metre foundation and could therefore not see any of this: every
// gully wall this cuts was being painted as the gentle hillside it was cut into.
struct Sculpt {
    Fixed cut;            // metres, never positive
    Fixed slopeX, slopeY; // what the gullies added to the fall line
    Fixed wall;           // nought on a crest or a floor, one on a gully wall
};

Sculpt foundationSculpt(std::uint64_t seed, const generation::TerrainFoundation& foundation,
                        generation::TerrainStage stage, Fixed x, Fixed y, Fixed here) {
    // The fall line, read over a quarter of a kilometre either side. That is
    // the landform's own slope rather than the sixty-four-metre one: what the
    // first and widest gullies should run down is the shape of the hill, not
    // the shape of the grid it was stored on.
    const auto reach = Fixed::fromInt(std::int64_t(foundation.step) * 4);
    const Fixed east = foundation.sample(x + reach, y, stage) - foundation.sample(x - reach, y, stage);
    const Fixed north = foundation.sample(x, y + reach, stage) - foundation.sample(x, y - reach, stage);
    // Divided as an INTEGER, not as a Fixed. A Fixed by a Fixed is a
    // hundred-and-twenty-eight bit division - a library call here - and the
    // divisor is a whole number of metres, so the cheap overload is the exact
    // same answer.
    const std::int64_t span = std::int64_t(foundation.step) * 8;
    Fixed slopeX = east / span, slopeY = north / span;

    // Nothing at all at the waterline.
    //
    // Ground that crosses sea level with a metre of relief on it does not make
    // a beach, it makes a stipple of islets and puddles a few metres across -
    // and because the water is a plane and the ground is within centimetres of
    // it over a wide flat shore, that stipple is where the picture crawls. The
    // cut fades out six metres either side of the water and is fully back by
    // forty, so a coast is a line rather than a rash.
    const Fixed beach = ease(rampFixed(core::abs(here), Fixed::fromInt(6), Fixed::fromInt(40)));
    if (beach <= core::kZero) return {};
    const Fixed baseX = slopeX, baseY = slopeY;

    // How deep a gully of a given width may be, as an ANGLE rather than as a
    // number of metres: a fifth is about eleven degrees of wall, which reads as
    // cut ground and not as a trench. The depth then follows from the width at
    // every scale, so no octave can produce a face steeper than any other one.
    //
    // And each octave bites less than the one above it. They all cut at their
    // own wavelength, so without this their slopes simply add and the fourth
    // one is standing on three others: it is the sum that has to stay under
    // repose, not each term of it.
    const auto bite = Fixed::ratio(14, 100);
    Fixed share = core::kOne;
    Fixed cut, wall;
    std::int64_t wave = 960;
    for (int octave = 0; octave < 4; ++octave, wave /= 2, share = share * Fixed::ratio(3, 4)) {
        const Fixed steep = roughLength(slopeX, slopeY);
        if (steep <= Fixed::ratio(1, 200)) break;   // flat ground has no fall line to follow
        Fixed ax = -slopeY / steep, ay = slopeX / steep;

        // The fall line, turned by a few degrees that wander over a couple of
        // kilometres.
        //
        // The gradient is read as differences along the two axes of a grid, off
        // a surface reconstructed from that same grid, so on smooth country it
        // points very nearly north or east far more often than it should - and
        // stripes laid across it inherit that. The linter measures it directly:
        // on one seed a hundred and six of the steep steps ran east-west
        // against twenty-six north-south, which is the lattice showing through
        // the erosion. A slow turn costs nothing and the water still runs
        // downhill, because a few degrees is not a direction.
        const Fixed turn = (smoothNoise(seed ^ 0x70A7ull, x.toInt(), y.toInt(), 1700) * 2 - core::kOne) *
                           Fixed::ratio(25, 100);
        const Fixed shrink = core::kOne - turn * turn / 2;   // the small-angle normalisation
        const Fixed rx = (ax - ay * turn) * shrink, ry = (ay + ax * turn) * shrink;
        ax = rx;
        ay = ry;

        // And how much of it this ground gets. Squared from the far end rather
        // than ramped, so it arrives without a crease: there is no line in the
        // landscape where the erosion switches on, which is what a plain gate
        // leaves behind.
        const Fixed shallow = core::kOne - core::saturate(steep / Fixed::ratio(30, 100));
        const Fixed fade = (core::kOne - shallow * shallow) * beach;

        const Stripe stripe = stripeAt(seed ^ (0x9E37ull + std::uint64_t(octave) * 0x2545F491ull),
                                       x, y, ax, ay, wave);
        // Offset so the crest is zero: the surface is the ground, and the
        // gullies hang below it.
        cut += (stripe.height - core::kOne) / 2 * Fixed::fromInt(wave) * bite * share / 4 * fade;
        // What this octave did to the slope, so the next one runs down the
        // walls it just cut instead of down the hill it found.
        const Fixed turned = stripe.across * bite * share * fade;
        slopeX += ax * turned;
        slopeY += ay * turned;
        // Where on the wave this point sits. The pair is a circle: the height
        // term is one at a crest and minus one in a gully floor, and the across
        // term is largest exactly halfway between them, which is the wall. That
        // is where soil does not stay and rock is what you see.
        wall += core::abs(stripe.across) * share * fade;
    }
    return {cut, slopeX - baseX, slopeY - baseY, core::saturate(wall * Fixed::ratio(2, 3))};
}

Fixed foundationDetail(std::uint64_t seed, const generation::TerrainFoundation& foundation,
                       generation::TerrainStage stage, Fixed x, Fixed y, Fixed here) {
    return foundationSculpt(seed, foundation, stage, x, y, here).cut;
}

} // namespace

void MaterialWeights::normalise() {
    Fixed total = core::kZero;
    for (Fixed w : weight) total += w;
    if (total.raw <= 0) {
        // Nothing claimed this ground. Bare earth is the honest answer, and it
        // keeps the invariant that the weights sum to one.
        weight = {};
        weight[static_cast<std::size_t>(Material::Dirt)] = core::kOne;
        return;
    }
    for (Fixed& w : weight) w = w / total;
}

Material MaterialWeights::strongest() const {
    std::size_t best = 0;
    for (std::size_t i = 1; i < weight.size(); ++i)
        if (weight[i].raw > weight[best].raw) best = i;
    return static_cast<Material>(best);
}

const char* travelName(Travel t) {
    switch (t) {
        case Travel::Walk: return "walk";
        case Travel::Scramble: return "scramble";
        case Travel::Climb: return "climb";
        case Travel::Ford: return "ford";
        case Travel::Swim: return "swim";
        case Travel::None: break;
    }
    return "no way";
}

HeightField::HeightField(const generation::WorldMapData* coarse, std::uint64_t seed)
    : coarse_(coarse), seed_(seed) {
    macro_.attach(coarse);
}

HeightField::HeightField(const generation::WorldMapData* coarse, std::uint64_t seed,
                         std::shared_ptr<const MacroWorld::Resolved> resolved)
    : coarse_(coarse), seed_(seed) {
    macro_.attach(coarse, std::move(resolved));
}

std::array<Fixed, 4> HeightField::foliageAt(WorldPos p) const {
    return surfaceClimateAt(p).foliage;
}

HeightField::CoarseLookup HeightField::coarseLookup(WorldPos p) const {
    const Fixed perCell = Fixed::fromInt(generation::kMetresPerCell);
    const Fixed reach = perCell * Fixed::ratio(45, 100);
    const Fixed wx = (smoothNoise(seed_ ^ 0x5eed, p.x.toInt(), p.y.toInt(), 520) - Fixed::ratio(1, 2)) *
                     Fixed::fromInt(2) * reach;
    const Fixed wy = (smoothNoise(seed_ ^ 0xb1a5, p.x.toInt(), p.y.toInt(), 520) - Fixed::ratio(1, 2)) *
                     Fixed::fromInt(2) * reach;
    const Fixed x = (p.x + wx) / perCell, y = (p.y + wy) / perCell;
    return {floorDiv(x.raw, core::kOne.raw), floorDiv(y.raw, core::kOne.raw),
            Fixed::fromRaw(floorMod(x.raw, core::kOne.raw)),
            Fixed::fromRaw(floorMod(y.raw, core::kOne.raw))};
}

HeightField::SurfaceClimate HeightField::surfaceClimateAt(WorldPos p) const {
    using generation::Climate;
    using Weights = std::array<Fixed, 5>;
    if (!coarse_ || coarse_->width <= 0 || coarse_->height <= 0)
        return {{core::kZero, core::kZero, core::kOne, core::kZero}, core::kZero,
                {Fixed::ratio(150,255), Fixed::ratio(1,2), Fixed::ratio(1,2),
                 kPrevailingWindX, kPrevailingWindY, Fixed::ratio(1,2)}, core::kOne};
    const auto profile = [](const generation::WorldCell& cell) -> Weights {
        if (cell.sea) return {};
        const Fixed one = core::kOne, zero = core::kZero;
        switch (cell.climate) {
        case Climate::Steppe: return {one, zero, zero, zero};
        case Climate::Taiga: return {zero, one, zero, zero};
        case Climate::TemperateForest: return {zero, zero, one, zero};
        case Climate::TropicalForest: return {zero, zero, zero, one};
        case Climate::Mediterranean: return {Fixed::ratio(3, 4), zero, Fixed::ratio(1, 4), zero};
        case Climate::Savanna: return {Fixed::ratio(3, 4), zero, zero, Fixed::ratio(1, 4)};
        case Climate::Tundra: return {zero, Fixed::ratio(3, 10), zero, zero};
        case Climate::Alpine: return {Fixed::ratio(1, 10), Fixed::ratio(1, 5), zero, zero};
        case Climate::Desert: return {Fixed::ratio(1, 10), zero, zero, zero, one};
        case Climate::Ice: return {};
        case Climate::RiverValley:
        case Climate::Delta:
            // A river is not a latitude. Keep its surrounding climate's palette.
            if (cell.temperature < 90) return {zero, one, zero, zero};
            if (cell.temperature > 185) return {zero, zero, zero, one};
            return {zero, zero, one, zero};
        case Climate::Count: return {};
        }
        return {};
    };
    // Match coarseExact's lookup warp, but interpolate weights linearly:
    // categorical climate IDs cannot be interpolated, and Catmull-Rom weights
    // would overshoot into negative species probabilities at biome edges.
    const auto [cx, cy, tx, ty] = coarseLookup(p);
    const auto at = [&](std::int64_t dx, std::int64_t dy) {
        const core::TilePos tile{
            static_cast<std::int32_t>(std::clamp<std::int64_t>(cx+dx,0,coarse_->width-1)),
            static_cast<std::int32_t>(std::clamp<std::int64_t>(cy+dy,0,coarse_->height-1))};
        const auto& cell = coarse_->at(tile);
        const auto cover = profile(cell);
        const auto index = static_cast<std::size_t>(tile.y)*coarse_->width+tile.x;
        Fixed vx = kPrevailingWindX, vy = kPrevailingWindY;
        if (index < coarse_->prevailingWindField.size() && index < coarse_->windStrengthField.size()) {
            const auto wind = coarse_->prevailingWindField[index];
            vx = Fixed::fromInt(wind.x); vy = Fixed::fromInt(wind.y);
            const auto length = core::hypot(vx,vy);
            const auto strength = Fixed::ratio(std::clamp(coarse_->windStrengthField[index],0,255),150);
            if (length > core::kZero) { vx = vx/length*strength; vy = vy/length*strength; }
        }
        const auto drainage = index < coarse_->soilDrainageField.size()
            ? Fixed::ratio(std::clamp(coarse_->soilDrainageField[index],0,255),255) : Fixed::ratio(1,2);
        Fixed woodland=core::kZero;
        if (!cell.sea) switch (cell.climate) {
        case Climate::TemperateForest: case Climate::Taiga: case Climate::TropicalForest:
            woodland=core::kOne;break;
        case Climate::Mediterranean: case Climate::Savanna: woodland=Fixed::ratio(1,8);break;
        case Climate::RiverValley: woodland=Fixed::ratio(1,6);break;
        case Climate::Delta: woodland=Fixed::ratio(1,16);break;
        default: break; // steppe, tundra, alpine, desert and ice stay open
        }
        return std::array<Fixed,12>{cover[0],cover[1],cover[2],cover[3],cover[4],
            Fixed::ratio(cell.temperature,255),Fixed::ratio(cell.fertility,255),
            Fixed::ratio(cell.moisture,255),vx,vy,drainage,woodland};
    };
    const auto a = at(0, 0), b = at(1, 0), c = at(0, 1), d = at(1, 1);
    std::array<Fixed,12> out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const Fixed top = a[i] + (b[i] - a[i]) * tx;
        out[i] = top + (c[i] + (d[i] - c[i]) * tx - top) * ty;
    }
    // Over open sea every channel the barren ground scales is nought already
    // (a sea cell has no cover and no woodland), and the landform sample is
    // the dearest thing here: seven samples in ten of a world's climate are
    // taken over water.
    const bool allSea = coarse_->at({static_cast<std::int32_t>(std::clamp<std::int64_t>(cx, 0, coarse_->width - 1)),
                                     static_cast<std::int32_t>(std::clamp<std::int64_t>(cy, 0, coarse_->height - 1))}).sea &&
                        out[0] == core::kZero && out[1] == core::kZero && out[2] == core::kZero &&
                        out[3] == core::kZero && out[11] == core::kZero;
    if (coarse_->hybridTerrain && !allSea) {
        const auto side = Fixed::fromInt(generation::kMetresPerCell);
        const auto barren = coarse_->hybridTerrain->sample(coarse_->terrainFoundation?p.x:(Fixed::fromInt(cx)+tx)*side,
                                                          coarse_->terrainFoundation?p.y:(Fixed::fromInt(cy)+ty)*side).barren;
        for (std::size_t i = 0; i < 4; ++i) out[i] *= core::kOne - barren;
        out[11] *= core::kOne - barren;
    }
    return {{out[0],out[1],out[2],out[3]},out[4],{out[5],out[6],out[7],out[8],out[9],out[10]},out[11]};
}

std::array<Fixed, 2> HeightField::windAt(WorldPos p) const {
    constexpr std::int64_t spacing = 64;
    const auto side = Fixed::fromInt(spacing);
    const auto x = floorDiv(p.x.raw, side.raw), y = floorDiv(p.y.raw, side.raw);
    const auto u = Fixed::fromRaw(floorMod(p.x.raw, side.raw)) / side;
    const auto v = Fixed::fromRaw(floorMod(p.y.raw, side.raw)) / side;
    const auto corner = [&](std::int64_t cx, std::int64_t cy) {
        const auto hash = static_cast<std::uint64_t>(cx) * 73856093u ^
                          static_cast<std::uint64_t>(cy) * 19349663u;
        auto& memo = windCorners_[hash % windCorners_.size()];
        if (memo.valid && memo.x == cx && memo.y == cy) return memo.velocity;
        const WorldPos at{Fixed::fromInt(cx * spacing), Fixed::fromInt(cy * spacing)};
        const auto climate = surfaceClimateAt(at).environment;
        const auto speed = core::hypot(climate[3], climate[4]);
        std::array<Fixed, 2> velocity{climate[3], climate[4]};
        if (speed > core::kZero) {
            const auto dx = climate[3] / speed, dy = climate[4] / speed;
            const auto height = heightAt(at);
            Fixed horizon;
            for (int metres : {32, 96, 256}) {
                const auto distance = Fixed::fromInt(metres);
                const WorldPos upwind{at.x - dx * distance, at.y - dy * distance};
                horizon = std::max(horizon, (heightAt(upwind) - height) / distance);
            }
            const auto exposure = core::kOne - Fixed::ratio(4, 5) *
                ease(rampFixed(horizon, Fixed::ratio(4, 100), Fixed::ratio(24, 100)));
            velocity[0] *= exposure;
            velocity[1] *= exposure;
        }
        memo = {true, cx, cy, velocity};
        return velocity;
    };
    const auto a = corner(x, y), b = corner(x + 1, y);
    const auto c = corner(x, y + 1), d = corner(x + 1, y + 1);
    std::array<Fixed, 2> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = core::lerp(core::lerp(a[i], b[i], u), core::lerp(c[i], d[i], u), v);
    return result;
}

Fixed HeightField::sandWindExposureAt(WorldPos p, Fixed height) const {
    Fixed horizon = core::kZero;
    for (const int metres : {16, 40, 96}) {
        const Fixed distance = Fixed::fromInt(metres);
        const auto sx = floorDiv((p.x - kPrevailingWindX * distance).toInt(), kSampleMetres);
        const auto sy = floorDiv((p.y - kPrevailingWindY * distance).toInt(), kSampleMetres);
        const Fixed rise = sampleHeight(sx, sy) - height;
        horizon = std::max(horizon, rise / distance);
    }
    return core::kOne - ease(rampFixed(horizon, Fixed::ratio(4, 100), Fixed::ratio(24, 100)));
}

HeightField::Coarse HeightField::coarseExact(Fixed worldX, Fixed worldY) const {
    Coarse out{};
    if (coarse_ == nullptr || coarse_->width <= 0) {
        // No country above: invent one, so the field is usable on its own.
        const Fixed n = fractalNoise(seed_ ^ 0x51ed, worldX.toInt(), worldY.toInt());
        out.elevation = Fixed::fromInt(40) + n * Fixed::fromInt(120);
        out.moisture = 90 + static_cast<std::int32_t>((n * Fixed::fromInt(160)).toInt());
        out.temperature = 150;
        out.relief = Fixed::fromInt(30);
        return out;
    }

    // Where this point falls on the coarse map, and interpolation between the
    // four cells around it. In fixed point the whole way: thousandths of a cell
    // and whole metres were enough quantisation to terrace the country into
    // one-metre contours, which read as ripples round every hill.
    //
    // Outside the map the edge cell carries on, so the field is defined
    // everywhere rather than falling off the end of the country.
    const generation::WorldMapData& world = *coarse_;
    const std::int64_t nx = worldX.toInt(), ny = worldY.toInt();

    // Ask the coarse map about somewhere slightly else, and how far else is
    // itself noise. Without this the country is read off a lattice of
    // hundred-and-eighty-metre cells and interpolated, and interpolation of a
    // lattice looks like a lattice: in a range, where neighbouring cells differ
    // by hundreds of metres, it comes out as a regular field of bumps with
    // valleys ruled north-south and east-west. Bending the lookup turns the
    // same cells into ridges and spurs, and it bends the coastline off the cell
    // edges at the same time, for the same reason.
    // Raw fractions: the curve below is smooth by construction and does not
    // want them eased. Easing them as well is what levels the surface off at
    // every cell and puts a dome in each one.
    const auto [cx, cy, tx, ty] = coarseLookup({worldX, worldY});

    const auto cell = [&](std::int64_t ox, std::int64_t oy) -> const generation::WorldCell& {
        const core::TilePos at{
                static_cast<std::int32_t>(std::clamp<std::int64_t>(cx + ox, 0, world.width - 1)),
                static_cast<std::int32_t>(std::clamp<std::int64_t>(cy + oy, 0, world.height - 1))};
        return world.at(at);
    };

    // The block this point sits in, remembered from the last time it was asked
    // for. Consecutive samples of a patch fall in the same cell, so this is
    // filled once and read a few hundred times.
    if (!block_.valid || block_.cx != cx || block_.cy != cy) {
        const auto reliefAround = [&](std::int64_t ox, std::int64_t oy) -> Fixed {
            std::int64_t lowest = 1 << 30, highest = -(1 << 30);
            for (std::int64_t dy = -1; dy <= 1; ++dy)
                for (std::int64_t dx = -1; dx <= 1; ++dx) {
                    const generation::WorldCell& c = cell(ox + dx, oy + dy);
                    const std::int64_t h =
                            c.sea ? 0
                                  : static_cast<std::int64_t>(c.elevation) *
                                            generation::kMetresPerElevationStep;
                    lowest = std::min(lowest, h);
                    highest = std::max(highest, h);
                }
            return Fixed::fromInt(std::clamp<std::int64_t>(highest - lowest, 4, 300));
        };
        for (std::int64_t j = 0; j < 4; ++j)
            for (std::int64_t i = 0; i < 4; ++i) {
                const generation::WorldCell& c = cell(i - 1, j - 1);
                // No cut for a river here. The coarse map used to drop a river
                // cell by sixty metres, from the days when nothing else made a
                // valley - and fed into a spline that is a sixty-metre step
                // every time a river cell meets one without a river, which is a
                // crease along the cell boundaries of every watercourse in the
                // world. The valley is the macro layer's business now (D116),
                // and it cuts along the course rather than around the cell.
                // A filled basin is a lake, so the ground here is its floor -
                // the fill taken back out - and the water stands at the level
                // the fill reached. Everything downstream of the drainage still
                // sees the filled land it was given; only the picture changes.
                const std::size_t at = static_cast<std::size_t>(
                        std::clamp<std::int64_t>(cy + j - 1, 0, world.height - 1)) *
                                       static_cast<std::size_t>(world.width) +
                               static_cast<std::size_t>(
                                       std::clamp<std::int64_t>(cx + i - 1, 0, world.width - 1));
                const std::int64_t sunk =
                        (!c.sea && at < world.lakeDepthField.size())
                                ? static_cast<std::int64_t>(world.lakeDepthField[at])
                                : 0;
                block_.elevation[j][i] =
                        c.sea ? Fixed::fromInt(kSeaBedMetres)
                              : Fixed::fromInt((static_cast<std::int64_t>(c.elevation) - sunk) *
                                               generation::kMetresPerElevationStep);
                // The level the fill reached, as it was recorded then - not the
                // cell's elevation now. Erosion cuts the elevation down after
                // the fill, so reading it back gave a surface well below the
                // outlet and the lakes came out as puddles: measured, one per
                // cent of the basin under two metres of water.
                const std::int64_t brim =
                        (!c.sea && at < world.lakeLevelField.size() && sunk > 0)
                                ? static_cast<std::int64_t>(world.lakeLevelField[at])
                                : static_cast<std::int64_t>(c.elevation);
                block_.lakeLevel[j][i] =
                        Fixed::fromInt(brim * generation::kMetresPerElevationStep);
                block_.lakeDeep[j][i] = Fixed::fromInt(sunk * generation::kMetresPerElevationStep);
                block_.moisture[j][i] = Fixed::fromInt(c.moisture);
                block_.temperature[j][i] = Fixed::fromInt(c.temperature);
                block_.sea[j][i] = c.sea ? core::kOne : core::kZero;
                block_.river[j][i] = c.river && !c.sea ? core::kOne : core::kZero;
                block_.relief[j][i] = reliefAround(i - 1, j - 1);
            }
        block_.cx = cx;
        block_.cy = cy;
        block_.valid = true;
    }
    // Sixteen cells rather than four: the curve needs the neighbours beyond the
    // span to know which way the ground was already going.
    const auto blend = [&](const Fixed (&values)[4][4]) -> Fixed {
        Fixed rows[4];
        for (std::int64_t j = 0; j < 4; ++j)
            rows[j] = catmull(values[j][0], values[j][1], values[j][2], values[j][3], tx);
        return catmull(rows[0], rows[1], rows[2], rows[3], ty);
    };

    // Elevation comes off the coarse map in steps of nine metres. The sea is a
    // floor rather than a flag: blended against the land around it, the height
    // crosses sea level somewhere in between, and that crossing is the
    // coastline. A flag instead drew the shore as the outline of a map cell - a
    // hundred and eighty metres of ruler-straight beach.
    out.elevation = blend(block_.elevation);
    out.moisture = static_cast<std::int32_t>(blend(block_.moisture).toInt());
    out.temperature = static_cast<std::int32_t>(blend(block_.temperature).toInt());
    // How much of each is around here, rather than what the cell under the point
    // happens to be: both decide where water stands, and both have to move
    // continuously or the water's edge is a cell boundary again.
    out.seaShare = blend(block_.sea);
    out.riverShare = blend(block_.river);
    // A lake's head is its own, and it is not shared with the lake next door.
    //
    // How much of a basin is filled here blends, because the fill runs out at
    // the shore and a step in it would be a step in the ground. The head does
    // not. Two basins that meet - one at two hundred and seventy metres, one
    // at four hundred and fifty - have a bank between them, not a ramp, and
    // averaging their heads by distance puts the water at a height that
    // belongs to neither of them: three hundred and sixty metres of lake
    // standing over the rim of one basin and under the surface of the other.
    //
    // So the head is taken, not mixed: whichever filled corner this point
    // stands nearest to is the basin it is in, and its head is the answer.
    // Inside one lake every corner carries the same head and the choice
    // cannot be seen; between two it falls where the bank does.
    Fixed nearest;
    bool anyFill = false;
    for (int j=0;j<2;++j) for (int i=0;i<2;++i) {
        const auto weight=(i?tx:core::kOne-tx)*(j?ty:core::kOne-ty);
        const auto deep=block_.lakeDeep[j+1][i+1];
        out.lakeDeep+=deep*weight;
        if (deep>core::kZero && (!anyFill || weight>nearest)) {
            nearest=weight;
            out.lakeLevel=block_.lakeLevel[j+1][i+1];
            anyFill=true;
        }
    }

    // How broken the country is here, blended like everything else: read off
    // the cell instead and the amplitude of the detail steps at every cell
    // boundary, which is a step in the ground.
    out.relief = blend(block_.relief);

    // And past the last cell, open sea.
    //
    // The field has to answer everywhere - a patch on the border reads samples
    // beyond it, and the camera can look past the edge - and what it used to
    // answer was the edge cell, carried on for ever, because that is what
    // clamping a lookup does. Measured across the west border: the country
    // climbs from two hundred and fifty metres at the edge to thirteen hundred
    // four kilometres in, and outside it sits at two hundred and twenty for as
    // far as anybody cares to walk, moving only with the detail noise. That is
    // the world's last row of cells extruded into a shelf, and it is on every
    // generation because it is not a mistake in any of them - it is what the
    // map not existing looks like when you draw it anyway.
    //
    // Sunk instead, over a couple of kilometres, so the country ends in water
    // and the horizon is a sea rather than a ledge. The fade is by distance and
    // nothing else, so it is the same pure function of position everything else
    // here is, and it needs no agreement with anybody.
    const Fixed outside = std::max(
            std::max(-worldX, worldX - Fixed::fromInt(std::int64_t(world.width) *
                                                      generation::kMetresPerCell)),
            std::max(-worldY, worldY - Fixed::fromInt(std::int64_t(world.height) *
                                                      generation::kMetresPerCell)));
    // Where the shelf falls away, wandered a little. The border is a rectangle
    // and it cannot help being one, but a rectangle is only visible if the water
    // over it goes deep along a ruled line - which it did, and which read as the
    // edge of a table. Moved in and out by a few hundred metres, on the same
    // noise everything else here uses, it reads as the edge of a shelf.
    const Fixed wander = (smoothNoise(seed_ ^ 0xed6e, nx, ny, 900) - Fixed::ratio(1, 2)) *
                         Fixed::fromInt(kBeyondTheMapMetres * 2 / 3);
    if ((outside + wander).raw > 0) {
        const Fixed gone = rampFixed(outside + wander, core::kZero,
                                     Fixed::fromInt(kBeyondTheMapMetres));
        out.elevation = out.elevation + (Fixed::fromInt(-120) - out.elevation) * gone;
        out.seaShare = out.seaShare + (core::kOne - out.seaShare) * gone;
        out.riverShare = out.riverShare - out.riverShare * gone;
        out.relief = out.relief - (out.relief - Fixed::fromInt(4)) * gone;
        out.lakeDeep = out.lakeDeep - out.lakeDeep * gone;
    }

    // The coast, brought in from where the bare curve puts it.
    //
    // SWEEP MARKER
    const Fixed ashore = rampFixed(out.seaShare, core::kZero, Fixed::ratio(90, 100));
    out.elevation = out.elevation + (Fixed::fromInt(-60) - out.elevation) * ashore;
    return out;
}

const HeightField::Coarse& HeightField::coarseCorner(std::int64_t lx, std::int64_t ly,
                                                     std::int64_t lattice) const {
    // Direct-mapped on the corner's own coordinate rather than searched.
    //
    // A ring of sixteen searched end to end was right while the samples were
    // four metres apart: eight of them share a corner, so the corner asked for
    // is nearly always the one asked for last. It stops being right the moment
    // the level is coarse - at thirty-two metres to the sample no two samples
    // share a corner along a row, at a kilometre no two share one at all, and
    // the ring then holds sixteen corners that will never be wanted again while
    // missing every one that will. That, measured, is most of why a coarse
    // sample cost five times a fine one: the same coarse map, evaluated four
    // times over for every sample instead of once for every eight.
    const std::uint64_t mix = static_cast<std::uint64_t>(lx) * 0x9e3779b97f4a7c15ULL ^
                              static_cast<std::uint64_t>(ly) * 0xc2b2ae3d27d4eb4fULL ^
                              static_cast<std::uint64_t>(lattice) * 0x94d049bb133111ebULL;
    Corner& slot = corners_[(mix ^ (mix >> 29)) & (kCornersKept - 1)];
    if (slot.valid && slot.lx == lx && slot.ly == ly && slot.lattice == lattice)
        return slot.value;
    slot.valid = true;
    slot.lx = lx;
    slot.ly = ly;
    slot.lattice = lattice;
    slot.value = coarseExact(Fixed::fromInt(lx * lattice), Fixed::fromInt(ly * lattice));
    return slot.value;
}

HeightField::Coarse HeightField::coarseAt(Fixed worldX, Fixed worldY,
                                          std::int64_t lattice) const {
    const Fixed step = Fixed::fromInt(lattice);
    const std::int64_t lx = floorDiv(worldX.raw, step.raw);
    const std::int64_t ly = floorDiv(worldY.raw, step.raw);
    const Fixed fx = Fixed::fromRaw(floorMod(worldX.raw, step.raw)) / step;
    const Fixed fy = Fixed::fromRaw(floorMod(worldY.raw, step.raw)) / step;

    // Read at the spacing being asked for, once it is wider than the coarse
    // lattice. Interpolating a thirty-two metre lattice for samples a kilometre
    // apart is four evaluations of the coarse map for every sample, none of
    // them shared with any other sample, to place a point between two corners
    // that are thirty times closer together than the thing being drawn. Widened,
    // each corner is shared by the four samples around it and the map is
    // evaluated once a sample instead of four times.
    // By value, not by reference, and this is not a style choice.
    //
    // coarseCorner hands back a reference into a direct-mapped table, and the
    // next call to it may land in the same slot and overwrite what the last
    // reference points to. Two of these four corners collide now and then - one
    // slot in five hundred and twelve, so rarely enough to look like something
    // else entirely - and when they do, the interpolation reads one corner's
    // value where another's should be. What that looks like on the ground is a
    // step of twenty-odd metres along a line of the thirty-two metre lattice:
    // a cut with a straight edge, as if the country had been sliced into tiles.
    // Measured across it: 222.66 m at one sample and 246.80 half a metre later,
    // with the detail layer and the drainage both smooth through the same point.
    //
    // The ring of sixteen this replaced was safe by luck: four misses in a row
    // could not wrap it.
    const Coarse a = coarseCorner(lx, ly, lattice);
    const Coarse b = coarseCorner(lx + 1, ly, lattice);
    const Coarse c = coarseCorner(lx, ly + 1, lattice);
    const Coarse d = coarseCorner(lx + 1, ly + 1, lattice);
    // Straight, not eased.
    //
    // Bilinear interpolation is continuous and kinked, and the kink shows on
    // steep ground: the linter measured up to two and a half metres of slope
    // break on the thirty-two metre period, so it was eased - and easing was
    // the wrong cure, in the way this file warns about two hundred lines above
    // about value noise. A fraction that arrives and leaves flat makes every
    // lattice point a flat spot, and the country becomes a lattice of domes.
    //
    // On the heights that was subtle. On what the ground is made of it was not:
    // the material rules read moisture, warmth and height, so a flat spot at
    // every corner became a pocket of different material at every corner - and
    // since grass is scattered where its weight crosses a threshold, the field
    // came out as evenly spaced tufts of grass in rock, on a thirty-two metre
    // grid. A stencil, not a meadow. Measured on the same ground: with easing,
    // three-by-three pockets of grass every eight or nine cells; without it,
    // continuous country.
    //
    // The right cure is a curve that is smooth without flat spots - Catmull-Rom,
    // which is what coarseExact already uses between cells for exactly this
    // reason - but here it would read sixteen corners of the coarse map per
    // sample instead of four, and cutting that from four to one is most of what
    // D135 bought. So the crease stays, and it is in the gaps.
    const auto mix = [&](Fixed p, Fixed q, Fixed r, Fixed s) {
        const Fixed top = p + (q - p) * fx;
        const Fixed bottom = r + (s - r) * fx;
        return top + (bottom - top) * fy;
    };
    const auto mixInt = [&](std::int32_t p, std::int32_t q, std::int32_t r, std::int32_t s) {
        return static_cast<std::int32_t>(mix(Fixed::fromInt(p), Fixed::fromInt(q),
                                             Fixed::fromInt(r), Fixed::fromInt(s))
                                                 .toInt());
    };

    Coarse out{};
    out.elevation = mix(a.elevation, b.elevation, c.elevation, d.elevation);
    out.moisture = mixInt(a.moisture, b.moisture, c.moisture, d.moisture);
    out.temperature = mixInt(a.temperature, b.temperature, c.temperature, d.temperature);
    out.seaShare = mix(a.seaShare, b.seaShare, c.seaShare, d.seaShare);
    out.lakeLevel = mix(a.lakeLevel, b.lakeLevel, c.lakeLevel, d.lakeLevel);
    out.lakeDeep = mix(a.lakeDeep, b.lakeDeep, c.lakeDeep, d.lakeDeep);
    out.riverShare = mix(a.riverShare, b.riverShare, c.riverShare, d.riverShare);
    out.relief = mix(a.relief, b.relief, c.relief, d.relief);
    return out;
}

const streaming::HydrologyGraph* HeightField::graph() const {
    if (!askedForGraph_) {
        askedForGraph_ = true;
        // No coarse map, no drainage: the tests invent a country from the seed,
        // and a country with no rivers in it has none to read.
        if (coarse_ != nullptr) hydrology_ = streaming::sharedHydrologyGraph(*coarse_);
    }
    return hydrology_.get();
}

bool HeightField::hasBogPools() const {
    return coarse_ != nullptr && coarse_->categories && !coarse_->categories->empty();
}

Fixed HeightField::bogPoolDepth(Fixed x, Fixed y) const {
    if (!hasBogPools()) return core::kZero;
    // The category ids of moor_marsh, peat_plain and river_fen (content/config/terrain/categories.json).
    const auto bog = [](std::uint8_t id) { return id == 32 || id == 33 || id == 34; };
    const auto pair = coarse_->categories->groundPair(x.toDouble(), y.toDouble());
    const double share = (bog(pair.a) ? 1.0 - pair.share : 0.0) + (bog(pair.b) ? pair.share : 0.0);
    if (share <= 0.35) return core::kZero;
    // The one field of islands (engine/biomes/patch_field.hpp): the soil puts
    // moss where it is high and mud where it is low, and water stands where it
    // is lowest - so the pools lie between the moss islands, not beside them.
    const double field = engine::biomes::patchField(x.toDouble(), y.toDouble());
    const double pit = std::clamp((0.37 - field) / 0.12, 0.0, 1.0);
    if (pit <= 0.0) return core::kZero;
    const Fixed inside = rampFixed(Fixed::fromDoubleForContent(share), Fixed::ratio(35, 100), Fixed::ratio(80, 100));
    return Fixed::fromDoubleForContent(pit * pit * (3.0 - 2.0 * pit)) * inside * Fixed::ratio(60, 100);
}

streaming::CarvedSample HeightField::carved(WorldPos at, Fixed country, Fixed detail) const {
    if (coarse_ && coarse_->terrainFoundation && coarse_->terrainStage < generation::TerrainStage::Water) {
        streaming::CarvedSample dry;
        dry.floor=country+detail; dry.bankDistance=Fixed::fromInt(1 << 20);
        return dry;
    }
    if (graph() == nullptr) {
        // No map, no drainage: the ground is what the country and its detail
        // make it, and nothing stands on it.
        streaming::CarvedSample dry;
        dry.floor = country + detail;
        dry.bankDistance = Fixed::fromInt(1 << 20);
        return dry;
    }
    // A window wide enough that walking a mesh stays inside it, and small
    // enough that what it gathered is a handful of reaches. Snapped to a grid
    // of its own size so that neighbouring queries name the same window rather
    // than each building one centred on itself.
    constexpr std::int64_t kWindow = 1024;   // metres
    constexpr std::int32_t kHalo = 900;      // the widest valley the graph draws
    if (!carver_ || at.x < carverArea_.min.x || at.y < carverArea_.min.y ||
        at.x >= carverArea_.max.x || at.y >= carverArea_.max.y) {
        const std::int64_t x = floorDiv(at.x.toInt(), kWindow) * kWindow;
        const std::int64_t y = floorDiv(at.y.toInt(), kWindow) * kWindow;
        carverArea_ = {{Fixed::fromInt(x), Fixed::fromInt(y)},
                       {Fixed::fromInt(x + kWindow), Fixed::fromInt(y + kWindow)}};
        carver_.emplace(*hydrology_, carverArea_, kHalo);
    }
    auto result=carver_->carve(at, country, detail);
    if (coarse_ && coarse_->terrainFoundation && coarse_->terrainStage==generation::TerrainStage::Water) {
        result.floor=country+detail;
        result.wet=result.wet && !result.reach && result.surface>result.floor;
    }
    return result;
}

HeightField::Residual HeightField::residualAt(WorldPos p) const {
    if (coarse_ && coarse_->terrainFoundation) return {}; // sub-H8 detail is material/normal only
    // Read at the same lattice the country is, so the two bands answer for a
    // world coordinate and nothing else - a residual that depended on which
    // level asked for it would be a different terrain per level, which is the
    // fault this whole layer exists to avoid.
    const Coarse country = coarseAt(p.x, p.y, kCoarseLatticeMetres);
    // The same relief scaling and the same crease the detail layer uses: a
    // flood plain takes a fraction of it, a broken upland takes all of it, and
    // an upland gets ridges where a plain gets swells.
    const Fixed scale = rampFixed(country.relief, Fixed::fromInt(10), Fixed::fromInt(220)) *
                                Fixed::ratio(9, 10) +
                        Fixed::ratio(1, 10);
    const Fixed crease = rampFixed(country.relief, Fixed::fromInt(60), Fixed::fromInt(260));
    // Under the open sea the same damping the detail layer gets: a sea bed
    // does not need ridges coming through the surface in lumps.
    const Fixed damp = core::kOne - country.seaShare * Fixed::ratio(4, 5);

    struct Octave { std::int64_t metres; Fixed steepness; };
    const auto band = [&](const Octave* octaves, std::size_t count, std::uint64_t stream) {
        Fixed sum = core::kZero;
        for (std::size_t i = 0; i < count; ++i) {
            const Octave& o = octaves[i];
            const Fixed swing = Fixed::fromInt(o.metres) * o.steepness;
            const Fixed rolling = gradientNoise(stream, p.x.toInt(), p.y.toInt(), o.metres);
            const Fixed crest =
                    ridgeNoise(stream ^ 0x9d21, p.x.toInt(), p.y.toInt(), o.metres) *
                    Fixed::ratio(1, 2);
            sum += (rolling + (crest - rolling) * crease) * swing;
            stream = core::splitmix64(stream);
        }
        // Do not turn metre-scale surface texture into another field of peaks
        // on a mountain. This is a geographic mask, never a camera/LOD switch.
        return sum * scale * damp * (core::kOne - crease * Fixed::ratio(7, 10));
    };
    // Amplitude tied to wavelength, as the octaves above it are: what decides
    // whether ground is ground is the angle, not the height of a bump.
    static constexpr Octave kLarge[] = {{64, Fixed::ratio(55, 1000)}, {32, Fixed::ratio(50, 1000)}};
    static constexpr Octave kMedium[] = {{16, Fixed::ratio(45, 1000)}, {8, Fixed::ratio(40, 1000)}};
    return {band(kLarge, 2, seed_ ^ 0x5eed1a11ull), band(kMedium, 2, seed_ ^ 0x0dd0c7a4ull)};
}

HeightField::Pieces HeightField::piecesAt(Fixed x, Fixed y, std::int64_t strideMetres) const {
    Pieces pieces = piecesWithout(x, y, strideMetres);
    // The environment's features, on top of the detail: what they move is
    // landscape, and the carve below still makes it give way to a channel.
    if (features_ && !features_->empty())
        pieces.moved += features_->at({x, y}, pieces.country + pieces.moved, std::max(strideMetres, featureStride_));
    return pieces;
}

HeightField::Pieces HeightField::piecesWithout(Fixed x, Fixed y, std::int64_t strideMetres) const {
    if (coarse_ && coarse_->terrainFoundation) {
        const bool outside=x<core::kZero || y<core::kZero ||
            x>=Fixed::fromInt(std::int64_t(coarse_->width)*generation::kMetresPerCell) ||
            y>=Fixed::fromInt(std::int64_t(coarse_->height)*generation::kMetresPerCell);
        if (outside) return {Fixed::fromInt(-60), {}, {}, {}};
        const auto& foundation=*coarse_->terrainFoundation;
        const auto stage=coarse_->terrainStage;
        // The generator's ground, plus whatever anybody dug into it. Added to
        // the BASE rather than to the detail, so the erosion filter below still
        // reads the fall line of the shape a brush just made - a dug valley
        // grows its own gullies instead of keeping the ones the hill had.
        const Fixed base=foundation.sample(x,y,stage)+
            (edits_ && !edits_->empty() ? edits_->at(x,y) : core::kZero);
        return {base,foundationDetail(seed_,foundation,stage,x,y,base),{},{}};
    }
    const Detail wanted = detailFor(strideMetres);
    Coarse country = coarseAt(x, y, wanted.lattice);
    // What anybody dug, added to the country as in the foundation path above:
    // the level the water is measured against moves with the ground.
    if (edits_ && !edits_->empty()) country.elevation += edits_->at(x, y);

    // How much detail this country wants: a flood plain takes a fraction of it,
    // a broken upland takes all of it. It scales the whole layer rather than any
    // one octave, so the ground gets rougher without getting steeper.
    const Fixed scale = rampFixed(country.relief, Fixed::fromInt(10), Fixed::fromInt(220)) *
                                Fixed::ratio(9, 10) +
                        Fixed::ratio(1, 10);
    const Fixed crease = rampFixed(country.relief, Fixed::fromInt(60), Fixed::fromInt(260));
    Fixed detail =
            detailMetres(seed_ ^ 0x9e37, x.toInt(), y.toInt(), scale, crease, 0);

    // Macro dune flow is geography, not a clock-driven vertex displacement.
    // Broad asymmetric crests, warped across the wind; unchanged by near LODs.
    // Drainage still carves this layer below, so dunes cannot fill a river bed.
    if (country.moisture < 100 && country.seaShare < core::kOne) {
        const Fixed desert = surfaceClimateAt({x, y}).desert;
        if (desert > core::kZero) {
            const Fixed along = x * kPrevailingWindX + y * kPrevailingWindY;
            const Fixed across = -x * kPrevailingWindY + y * kPrevailingWindX;
            const Fixed warp = smoothNoise(seed_ ^ 0xd073, across.toInt(), along.toInt(), 310);
            const Fixed phase = along / Fixed::fromInt(180) + warp;
            const Fixed t = Fixed::fromRaw(floorMod(phase.raw, core::kOne.raw));
            const Fixed crest = Fixed::ratio(7, 10);
            const Fixed shape = ease(t < crest ? t / crest : (core::kOne - t) / (core::kOne - crest));
            const Fixed loose = desert * ramp(country.moisture, 100, 35) *
                                (core::kOne - crease) * (core::kOne - country.seaShare);
            detail += (shape - Fixed::ratio(1, 2)) * Fixed::fromInt(9) * loose;
        }
    }

    // Under the open sea the ground is a floor, not a hillside: the detail is
    // damped where the coarse map is all water, so the sea bed does not rise
    // through the surface in lumps.
    const Fixed damp = core::kOne - country.seaShare * Fixed::ratio(4, 5);
    const bool coherent = coarse_ && coarse_->hybridTerrain && coarse_->hybridTerrain->version>=2;
    const Fixed alpine = coherent ? core::kZero :
        alpineMetres(seed_,x.toInt(),y.toInt(),country.elevation,country.relief,country.seaShare);
    if (coarse_ && coarse_->hybridTerrain) {
        // Use the SAME geographical warp as height/climate/soil. The macro DEM
        // contribution is already in country; restore its sub-cell residual.
        // No stride input: both simulation and every render LOD see one ground.
        const auto [cx,cy,tx,ty] = coarseLookup({x,y});
        const auto side = Fixed::fromInt(generation::kMetresPerCell);
        const auto hx = (Fixed::fromInt(cx)+tx)*side, hy = (Fixed::fromInt(cy)+ty)*side;
        const auto hybrid = coarse_->hybridTerrain->sample(hx,hy);
        const auto residual = coarse_->hybridTerrain->residual(hx,hy,hybrid.delta);
        const auto land = core::kOne-core::saturate(country.seaShare);
        return {country.elevation,
                detail*damp*(core::kOne-hybrid.influence*Fixed::ratio(4,5)) +
                    alpine*(core::kOne-hybrid.influence) + residual*land*land,
                country.lakeLevel,country.lakeDeep};
    }
    return {country.elevation, detail * damp + alpine, country.lakeLevel, country.lakeDeep};
}

Fixed HeightField::sampleHeight(std::int64_t sx, std::int64_t sy,
                                std::int64_t strideMetres) const {
    auto* cache = cachedQueries();
    const auto epoch = cache ? cache->epoch : 0;
    if (cache) {
        const auto& entry = cache->heights.at(sx, sy, strideMetres);
        if (entry.matches(sx, sy, strideMetres, epoch)) return entry.value;
    }
    const Fixed worldX = Fixed::fromInt(sx * kSampleMetres);
    const Fixed worldY = Fixed::fromInt(sy * kSampleMetres);
    const Pieces ground = piecesAt(worldX, worldY, strideMetres);

    // And then the water cuts its valley into whatever that came to.
    //
    // The channel's bed is below its surface; the ground rises from the bank
    // out to the reach of the valley, where it meets the country again. Eased
    // rather than ramped, so the valley has a floor and shoulders instead of a
    // V - and because a kink here would be a crease running the length of every
    // river in the world.
    // The graph cuts it, and it cuts it the same at every spacing. It used to
    // be MacroWorld, told how narrow a valley was worth cutting at this
    // stride - so the ground itself moved when the camera pulled back, which
    // is the one thing a base surface may never do.
    const auto height = carved({worldX, worldY}, ground.country, ground.moved).floor;
    return cache ? cache->heights.at(sx, sy, strideMetres).put(sx, sy, strideMetres, epoch, height) : height;
}

MaterialWeights HeightField::sampleMaterials(std::int64_t sx, std::int64_t sy) const {
    auto* cache = cachedQueries();
    const auto epoch = cache ? cache->epoch : 0;
    if (cache) {
        const auto& entry = cache->materials.at(sx, sy, 1);
        if (entry.matches(sx, sy, 1, epoch)) return entry.value;
    }
    const auto materials = materialsGiven(sx, sy, core::kZero, core::kZero);
    return cache ? cache->materials.at(sx, sy, 1).put(sx, sy, 1, epoch, materials) : materials;
}

MaterialWeights HeightField::materialsGiven(std::int64_t sx, std::int64_t sy, Fixed height,
                                            Fixed slope, std::int64_t stride) const {
    // Material identity is a world-space property, not a property of the mesh
    // currently looking at it. The old path used the render/bake stride here,
    // which changed the climate sample, height and slope as the camera crossed
    // LODs. A coarse tile could therefore turn the same patch from grass into
    // marsh or rock. Keep the classification fixed; LOD is allowed to remove
    // detail, never to repaint the ground.
    (void)height;
    (void)slope;
    (void)stride;
    Fixed gullyWall;
    const std::int64_t worldX = sx * kSampleMetres;
    const std::int64_t worldY = sy * kSampleMetres;
    const bool foundation=coarse_ && coarse_->terrainFoundation;
    if (foundation) {
        // The same canonical H64 derivatives at ALL material LODs. Narrow wet
        // banks are a water-page shading product, not five hidden river queries.
        const auto sample=[&](std::int64_t x,std::int64_t y) {
            return coarse_->terrainFoundation->sample(Fixed::fromInt(x),Fixed::fromInt(y),
                generation::TerrainStage::Slopes);
        };
        height=sample(worldX,worldY);
        const std::int64_t span=coarse_->terrainFoundation->step;
        Fixed fallX=(sample(worldX+span,worldY)-sample(worldX-span,worldY))/Fixed::fromInt(2*span);
        Fixed fallY=(sample(worldX,worldY+span)-sample(worldX,worldY-span))/Fixed::fromInt(2*span);
        // And what the erosion cut into it, which the material pass could not
        // see at all.
        //
        // The slope was read off the sixty-four-metre foundation and nothing
        // else, so every gully this world has - all of them narrower than that
        // - was painted as the smooth hillside they were cut into. That is why
        // a mountain comes out one flat colour with a rock band across the very
        // steepest part of it and nothing anywhere else: the ground has the
        // shape, and the paint has never been told.
        //
        // This is still a world-space property and still the same at every
        // level of detail, because the filter is a function of position: LOD
        // may leave detail out, never repaint the ground.
        const auto sculpt=foundationSculpt(seed_,*coarse_->terrainFoundation,
                                           generation::TerrainStage::Slopes,
                                           Fixed::fromInt(worldX),Fixed::fromInt(worldY),height);
        fallX+=sculpt.slopeX;
        fallY+=sculpt.slopeY;
        slope=core::hypot(fallX,fallY);
        gullyWall=sculpt.wall;
    } else {
        height=sampleHeight(sx,sy);
        slope=sampleSlope(sx,sy);
    }
    const Coarse country = coarseAt(Fixed::fromInt(worldX), Fixed::fromInt(worldY),
                                    detailFor(kSampleMetres).lattice);

    // Two noises: one for where a patch of one material gives way to another,
    // one finer for the mottling inside a patch. Both are read at the sample's
    // own global coordinate, so the pattern crosses a chunk border without
    // knowing there was one.
    // A noise too fine for this spacing is read at a coarser spacing instead of
    // being turned down.
    //
    // Turning it down is what this used to do, and it was wrong in a way that
    // took a video to see. The ramps below are not linear, so shrinking the
    // range of `wet` does not merely make the ground calmer - it moves where the
    // average lands. Measured over the same country: marsh 0.56 of the ground at
    // four metres to the sample and 0.09 at sixty-four, grass 0.36 against 0.78.
    // A coarse patch beside a fine one was not blurrier, it was different
    // ground, and it showed as pale blocks with straight edges lying across the
    // country.
    //
    // Read at three samples to the wave instead, the amplitude is untouched and
    // only the pattern gets broader, so the average holds at every level and
    // nothing aliases.
    // Read at the same scales whatever the sampling stride is.
    //
    // These used to be widened when a coarse level could not resolve them, and
    // that is what made the material change under the camera: a patch at four
    // metres to the sample and the same ground at sixty-four came out as
    // different country, so every level change repainted the marshes and the
    // bare patches. The requirements are explicit that a coarse level may leave
    // detail out but must not draw *different* terrain (terrain_lod §3, §9),
    // and material that moves is worse than material that aliases.
    //
    // The finest octave here carries a fourteenth of the patch, so what a
    // coarse level cannot resolve of it averages out rather than showing as a
    // pattern. The scales stay put.
    const auto wide = [](std::int64_t scale) { return scale; };

    // Where one material gives way to another, and the mottling inside a patch
    // of it.
    //
    // Both are read through a warp, and the first is three scales rather than
    // one. A single noise is a lattice with the corners smoothed off, and what
    // it draws when it is used to divide grass from dry ground is that lattice:
    // rectangles, crosses and L-shapes with their sides due north and due east,
    // a hundred metres on a side, over the whole country. Bending the ground it
    // is read from by a slower noise and adding two more scales to it leaves
    // nothing straight for the eye to line up.
    // Gradient noise, not value noise, and for the reason written out at the top
    // of this file about the ground itself: value noise picks a number at every
    // lattice point and eases between them, so every lattice point is a flat
    // spot and the country comes out a tiling of rounded rectangles. The heights
    // were moved off it long ago; the materials were not, and it showed - not as
    // "the ground is slightly wrong" but as grass growing in evenly spaced
    // clumps, because grass is scattered where its weight crosses a threshold
    // and the threshold was being crossed on a lattice. A field looked like a
    // stencil, and no amount of wind in it was going to help.
    //
    // Gradient noise is nought at every lattice point and slopes away from it,
    // so the lattice leaves no mark. It comes out about minus one to one where
    // value noise came out nought to one, so the halves that were subtracted
    // here are gone and the amplitudes are halved to match.
    const Fixed swing = gradientNoise(seed_ ^ 0x6b2d, worldX, worldY, wide(340)) *
                        Fixed::fromInt(75);
    const Fixed sway = gradientNoise(seed_ ^ 0x91f7, worldX, worldY, wide(290)) *
                       Fixed::fromInt(75);
    const std::int64_t bentX = worldX + swing.toInt();
    const std::int64_t bentY = worldY + sway.toInt();

    const Fixed patch = (gradientNoise(seed_ ^ 0x1a5e, bentX, bentY, wide(310)) *
                                 Fixed::fromInt(4) +
                         gradientNoise(seed_ ^ 0x1a5f, bentX, bentY, wide(118)) *
                                 Fixed::fromInt(2) +
                         gradientNoise(seed_ ^ 0x1a60, bentX, bentY, wide(44))) /
                        Fixed::fromInt(14);
    const Fixed grain =
            gradientNoise(seed_ ^ 0x33c1, bentX, bentY, wide(26)) * Fixed::ratio(1, 2);

    const std::int32_t wet = std::clamp(
            country.moisture +
                    static_cast<std::int32_t>((patch * Fixed::fromInt(90)).toInt()),
            0, 255);
    const std::int32_t warmth = country.temperature;
    const WorldPos position{Fixed::fromInt(worldX), Fixed::fromInt(worldY)};
    // At the same spacing the height was taken at: what the ground is made of
    // beside a brook does not matter at a kilometre to the sample, and asking
    // costs the twenty-five channels around the point.
    const streaming::CarvedSample channel = foundation ? streaming::CarvedSample{} :
        carved(position, country.elevation, core::kZero);
    const Fixed bank = channel.reach != streaming::kInvalidRiverId
                               ? rampFixed(channel.bankDistance, Fixed::fromInt(24), core::kZero)
                               : core::kZero;
    // A dry gully is not a marsh, and most watercourses on the map are dry
    // gullies.
    //
    // The bank term was asked of whichever channel was nearest and did not care
    // whether it carried water, so in wet country - where `ramp(wet, 90, 175)`
    // is near one - every gully on the map was given marsh at twice weight. That
    // is what the dark ribbons winding across the country are: bog drawn down
    // the middle of channels that have been dry since they were cut. And since a
    // dry channel has no width, `bankDistance` is measured from its centre line,
    // so the weight peaks exactly along the thread of it.
    //
    // What collects in a dry watercourse is what the water left when it stopped
    // running: sand and gravel, requirement 6's sediment. So the wet bank keeps
    // the marsh-or-sand choice by moisture, and the dry one takes sediment
    // whatever the climate.
    //
    // A reach the graph knows is a reach that holds water: dry drainage stays
    // in the macro map by design and never becomes a body. So finding one is
    // the wet case, and there is no dry one left to find here.
    //
    // The sediment a dry watercourse collects went with MacroWorld, and it
    // should come back as a channel on the page rather than as a second carve
    // in this file - the same place the rest of requirement 6 lands.
    const Fixed wetBank = bank;
    const Fixed dryBank = core::kZero;

    MaterialWeights out{};
    // Exposed bedrock is a slope property, not an elevation band. High gentle
    // ground can retain soil/meadow/snow; cliff faces cannot retain that cover.
    // Thirty-one degrees used to be enough to call ground bare rock, and that
    // was tuned when `slope` was the sixty-four-metre foundation's gradient and
    // nothing else. It now carries the erosion's gullies as well, so the same
    // number fires on ordinary hillsides - and the whole world came out stone.
    // Thirty-nine to fifty-six: a face, not a slope.
    const auto exposure=rampFixed(slope,Fixed::ratio(8,10),Fixed::ratio(15,10));
    out.add(Material::Rock,exposure);
    // And on the walls of the gullies whatever their angle, because a wall is
    // where the soil went rather than where it is. Its edge is broken at two
    // scales at once - a wide one that decides which stretch of wall is bare
    // and a fine one that frays it - or the rock arrives as a smooth band
    // following a contour, which is the giveaway that a mask drew it and not a
    // hillside.
    if (gullyWall>core::kZero) {
        const Fixed coarseEdge=smoothNoise(seed_^0x2C0Full,worldX,worldY,140);
        const Fixed fineEdge=smoothNoise(seed_^0x2C10ull,worldX,worldY,23);
        const Fixed ragged=coarseEdge*Fixed::ratio(7,10)+fineEdge*Fixed::ratio(3,10);
        // A wall shows its bones; it is not a quarry. At full weight this put
        // stone down every gully in the world and left nothing else visible -
        // and even at a third it beat the grass on every mountainside, the
        // walls of gentle gullies included: the mountains came out solid
        // stone. Only a wall that is steep shows its rock (from about
        // nineteen degrees, all of it by thirty-nine).
        const Fixed steepWall=rampFixed(slope,Fixed::ratio(35,100),Fixed::ratio(8,10));
        out.add(Material::Rock,ease(core::saturate(gullyWall*Fixed::ratio(3,2)-ragged*
                                                   Fixed::ratio(7,10)))*Fixed::ratio(35,100)*steepWall);
        out.add(Material::Dirt,gullyWall*ragged*Fixed::ratio(1,5));
    }
    // Snow above the line, and the line is lower where the country is cold.
    const Fixed snowLine = Fixed::fromInt(400 + warmth * 6);
    out.add(Material::Snow, rampFixed(height, snowLine, snowLine + Fixed::fromInt(300)) *
                           rampFixed(slope,Fixed::ratio(5,2),Fixed::ratio(5,4)));
    // Grass wants rain and gentle ground.
    // Grass keeps off a wet bank almost entirely; a dry gully it merely thins,
    // because a gully that has not run in years grows over.
    // From a dry steppe up - the shader dries the grass where the climate is
    // dry; sand is for a desert, not for every country a little short of rain.
    out.add(Material::Grass, ramp(wet, 30, 110) * rampFixed(slope, cliffSlope(), core::kZero) *
                                     (core::kOne - wetBank * Fixed::ratio(9, 10) -
                                      dryBank * Fixed::ratio(2, 5)));
    // Sand where it is dry, and low: a desert, or a shore.
    //
    // Not every bank is a beach. A river bank is sand in stretches - on the
    // inside of a bend, below a steep reach - and grass, mud or bare earth
    // between them; laying sand down the full length of every watercourse
    // turned each river into one continuous sandy floodplain. A broad field
    // (kilometre stretches) decides which banks are sandy, and a dry climate
    // makes more of them so.
    const Fixed beach = core::saturate(
        (smoothNoise(seed_ ^ 0x5A0Dull, worldX, worldY, 520) - Fixed::ratio(2, 5)) * Fixed::fromInt(3) +
        ramp(wet, 110, 40) * Fixed::ratio(1, 2));
    out.add(Material::Sand, ramp(wet, 55, 20) + rampFixed(height, Fixed::fromInt(4), core::kZero) +
                                    wetBank * (ramp(wet, 175, 90) + Fixed::ratio(1, 2)) * beach +
                                    dryBank * Fixed::ratio(6, 5));
    // The banks that are not sand are earth under the grass.
    out.add(Material::Dirt, wetBank * (core::kOne - beach) * Fixed::ratio(3, 5));
    // Marsh where it is wet and flat and near the water - mixed into the bank
    // rather than covering it. At twice weight it beat everything else within
    // twenty-four metres of every wet channel on the map, so each river ran
    // between two black stripes of bog the same width from source to mouth. A
    // wet bank is muddy in places and sandy in others, which is what a bank
    // looks like; the shore band in the terrain shader lays sand over the top
    // of this by height above the water.
    out.add(Material::Marsh, ramp(wet, 150, 210) *
                                     rampFixed(slope, Fixed::ratio(1, 6), core::kZero) *
                                     rampFixed(height, Fixed::fromInt(25), core::kZero) +
                                     wetBank * ramp(wet, 90, 175) *
                                             rampFixed(slope, Fixed::ratio(1, 5), core::kZero));
    // A little bare earth remains between plants, but it must not be a forty per
    // cent cracked-mud veil over every meadow and mountainside. Where no climate
    // material claims the ground normalise() still makes Dirt the full fallback.
    out.add(Material::Dirt, Fixed::ratio(1, 10) + grain * Fixed::ratio(1, 20));
    if (coarse_ && coarse_->hybridTerrain) {
        const auto [cx,cy,tx,ty] = coarseLookup(position);
        const auto side = Fixed::fromInt(generation::kMetresPerCell);
        const auto barren = coarse_->hybridTerrain->sample(foundation?position.x:(Fixed::fromInt(cx)+tx)*side,
                                                          foundation?position.y:(Fixed::fromInt(cy)+ty)*side).barren;
        for (auto material : {Material::Grass,Material::Marsh,Material::Sand})
            out.weight[static_cast<std::size_t>(material)] *= core::kOne-barren;
        out.add(Material::Rock,barren*(Fixed::ratio(3,20)+exposure*Fixed::ratio(27,20)));
        out.add(Material::Dirt,barren*Fixed::ratio(9,10));
    }
    out.normalise();

    return out;
}

Normal HeightField::sampleNormal(std::int64_t sx, std::int64_t sy) const {
    // Central differences across the neighbours - which, at a chunk's edge, are
    // samples in the next chunk. Nothing special happens there: the field is
    // asked for them the same way, so the normal on a border is the same normal
    // from either side, and there is no seam to hide.
    const Fixed west = sampleHeight(sx - 1, sy), east = sampleHeight(sx + 1, sy);
    const Fixed north = sampleHeight(sx, sy - 1), south = sampleHeight(sx, sy + 1);
    const Fixed run = Fixed::fromInt(2 * kSampleMetres);
    const Fixed dzdx = (east - west) / run;
    const Fixed dzdy = (south - north) / run;

    // (-dz/dx, -dz/dy, 1), normalised.
    Fixed x = -dzdx, y = -dzdy, z = core::kOne;
    const Fixed length = core::sqrt(x * x + y * y + z * z);
    if (length.raw == 0) return {core::kZero, core::kZero, core::kOne};
    return {x / length, y / length, z / length};
}

Normal HeightField::normalAcross(std::int64_t sx, std::int64_t sy, std::int64_t stride) const {
    const Fixed west = sampleHeight(sx - stride, sy), east = sampleHeight(sx + stride, sy);
    const Fixed north = sampleHeight(sx, sy - stride), south = sampleHeight(sx, sy + stride);
    const Fixed run = Fixed::fromInt(2 * kSampleMetres * stride);
    const Fixed dzdx = (east - west) / run;
    const Fixed dzdy = (south - north) / run;
    Fixed x = -dzdx, y = -dzdy, z = core::kOne;
    const Fixed length = core::sqrt(x * x + y * y + z * z);
    if (length.raw == 0) return {core::kZero, core::kZero, core::kOne};
    return {x / length, y / length, z / length};
}

Fixed HeightField::sampleSlope(std::int64_t sx, std::int64_t sy) const {
    auto* cache = cachedQueries();
    const auto epoch = cache ? cache->epoch : 0;
    if (cache) {
        const auto& entry = cache->slopes.at(sx, sy, 1);
        if (entry.matches(sx, sy, 1, epoch)) return entry.value;
    }
    const Fixed west = sampleHeight(sx - 1, sy), east = sampleHeight(sx + 1, sy);
    const Fixed north = sampleHeight(sx, sy - 1), south = sampleHeight(sx, sy + 1);
    const Fixed run = Fixed::fromInt(2 * kSampleMetres);
    const Fixed dzdx = (east - west) / run;
    const Fixed dzdy = (south - north) / run;
    const auto slope = core::sqrt(dzdx * dzdx + dzdy * dzdy);
    return cache ? cache->slopes.at(sx, sy, 1).put(sx, sy, 1, epoch, slope) : slope;
}

bool HeightField::sampleBrokenExcept(std::int64_t sx, std::int64_t sy, std::int64_t ignoreX,
                                     std::int64_t ignoreY) const {
    const Fixed here = sampleHeight(sx, sy);
    const Fixed limit = cliffSlope() * Fixed::fromInt(kSampleMetres);
    const std::int64_t offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (const auto& off : offsets) {
        const std::int64_t nx = sx + off[0], ny = sy + off[1];
        if (nx == ignoreX && ny == ignoreY) continue;
        const Fixed there = sampleHeight(nx, ny);
        const std::int64_t drop = here.raw > there.raw ? here.raw - there.raw : there.raw - here.raw;
        if (drop >= limit.raw) return true;
    }
    return false;
}

bool HeightField::sampleBroken(std::int64_t sx, std::int64_t sy) const {
    // No neighbour ignored: a sample with any sharp drop around it at all.
    return sampleBrokenExcept(sx, sy, sx, sy);
}

namespace {
// Where a world point sits on the lattice: the sample north-west of it, and how
// far between that sample and the next, as a fraction. Floor division so the
// answer is right on both sides of the origin.
struct LatticeSpot {
    std::int64_t sx, sy;
    core::Fixed fx, fy;
};

LatticeSpot spotOf(WorldPos p) {
    const Fixed step = Fixed::fromInt(kSampleMetres);
    const std::int64_t sx = floorDiv(p.x.raw, step.raw);
    const std::int64_t sy = floorDiv(p.y.raw, step.raw);
    const Fixed fx = Fixed::fromRaw(floorMod(p.x.raw, step.raw)) / step;
    const Fixed fy = Fixed::fromRaw(floorMod(p.y.raw, step.raw)) / step;
    return {sx, sy, fx, fy};
}
} // namespace

Fixed HeightField::heightAt(WorldPos p) const {
    const LatticeSpot at = spotOf(p);
    const Fixed h00 = sampleHeight(at.sx, at.sy);
    const Fixed h10 = sampleHeight(at.sx + 1, at.sy);
    const Fixed h01 = sampleHeight(at.sx, at.sy + 1);
    const Fixed h11 = sampleHeight(at.sx + 1, at.sy + 1);
    const Fixed top = h00 + (h10 - h00) * at.fx;
    const Fixed bottom = h01 + (h11 - h01) * at.fx;
    return top + (bottom - top) * at.fy;
}

Normal HeightField::normalAt(WorldPos p) const {
    const LatticeSpot at = spotOf(p);
    const Normal n00 = sampleNormal(at.sx, at.sy);
    const Normal n10 = sampleNormal(at.sx + 1, at.sy);
    const Normal n01 = sampleNormal(at.sx, at.sy + 1);
    const Normal n11 = sampleNormal(at.sx + 1, at.sy + 1);
    const auto mix = [&](Fixed a, Fixed b, Fixed c, Fixed d) {
        const Fixed top = a + (b - a) * at.fx;
        const Fixed bottom = c + (d - c) * at.fx;
        return top + (bottom - top) * at.fy;
    };
    Fixed x = mix(n00.x, n10.x, n01.x, n11.x);
    Fixed y = mix(n00.y, n10.y, n01.y, n11.y);
    Fixed z = mix(n00.z, n10.z, n01.z, n11.z);
    const Fixed length = core::sqrt(x * x + y * y + z * z);
    if (length.raw == 0) return {core::kZero, core::kZero, core::kOne};
    return {x / length, y / length, z / length};
}

Fixed HeightField::slopeAt(WorldPos p) const {
    const LatticeSpot at = spotOf(p);
    const Fixed s00 = sampleSlope(at.sx, at.sy);
    const Fixed s10 = sampleSlope(at.sx + 1, at.sy);
    const Fixed s01 = sampleSlope(at.sx, at.sy + 1);
    const Fixed s11 = sampleSlope(at.sx + 1, at.sy + 1);
    const Fixed top = s00 + (s10 - s00) * at.fx;
    const Fixed bottom = s01 + (s11 - s01) * at.fx;
    return top + (bottom - top) * at.fy;
}

MaterialWeights HeightField::materialsAt(WorldPos p) const {
    const LatticeSpot at = spotOf(p);
    const MaterialWeights m00 = sampleMaterials(at.sx, at.sy);
    const MaterialWeights m10 = sampleMaterials(at.sx + 1, at.sy);
    const MaterialWeights m01 = sampleMaterials(at.sx, at.sy + 1);
    const MaterialWeights m11 = sampleMaterials(at.sx + 1, at.sy + 1);
    MaterialWeights out{};
    for (std::size_t i = 0; i < kMaterialCount; ++i) {
        const Fixed top = m00.weight[i] + (m10.weight[i] - m00.weight[i]) * at.fx;
        const Fixed bottom = m01.weight[i] + (m11.weight[i] - m01.weight[i]) * at.fx;
        out.weight[i] = top + (bottom - top) * at.fy;
    }
    out.normalise();
    return out;
}

namespace {
// Is this point actually in the basin, or only near one?
//
// A lake is a level surface, and that is right: still water is flat. What is
// not right is where the flat surface is allowed to reach. Basin membership was
// a threshold on lakeDeep - the metres of fill - and lakeDeep is bilinearly
// blended between coarse cells, so it decays smoothly outwards and stays over a
// metre well past the rim. Past the rim the ground is falling away, so the test
// went on saying "lake" while the bed dropped tens of metres below the surface,
// and what that draws is a flat panel of water standing in the air over a
// hillside with its edge hanging.
//
// The discriminator is that inside a real basin the two numbers stay of the
// same order: the fill is roughly how deep the water is. Outside it the water
// over the bed grows without bound while the fill keeps shrinking towards
// nothing, and it is that divergence, not the fill on its own, that says the
// point is no longer in the lake.
//
// Generously bounded rather than exactly. The fill comes off the coarse cells
// and the bed off the lattice after `carve` has cut a channel into it, so the
// water genuinely does stand deeper than the fill over a drowned watercourse,
// and a tight bound emptied nine lakes in ten. Twice the fill plus a couple of
// samples keeps those and still throws out the panels standing a hundred
// metres over a hillside.
bool inTheBasin(core::Fixed lakeLevel, core::Fixed lakeDeep, core::Fixed bed) {
    if (lakeDeep.raw <= core::kOne.raw) return false;
    const core::Fixed over = lakeLevel - bed;
    if (over.raw <= 0) return false;
    return over.raw <= (lakeDeep * core::Fixed::fromInt(2) +
                        core::Fixed::fromInt(2 * kSampleMetres)).raw;
}
} // namespace

Fixed HeightField::waterLevelAt(WorldPos p) const {
    // One question, one model. Everything this used to do - hold the surface
    // to the ground, prefer the highest reach that gets here, let a basin
    // swallow the channel crossing it - the carve already does, and does once
    // for the bed and the surface together so the two cannot disagree.
    const Pieces ground = piecesAt(p.x, p.y);
    const streaming::CarvedSample water = carved(p, ground.country, ground.moved);
    return water.wet ? water.surface : core::kZero;
}

HeightField::WaterHere HeightField::waterOver(WorldPos p, Fixed footprint, Fixed slope) const {
    const Pieces ground = piecesAt(p.x, p.y);
    const streaming::CarvedSample water = carved(p, ground.country, ground.moved);
    const Fixed span = footprint.raw > 0 ? footprint : Fixed::fromInt(kSampleMetres);
    const Fixed fall = slope * span;

    const auto clamped = [](Fixed cover) {
        if (cover.raw < 0) return core::kZero;
        if (cover.raw > core::kOne.raw) return core::kOne;
        return cover;
    };

    if (water.wet) {
        // A reach is not a body, which is what tells the three apart: the sea
        // and a lake are named, and water that is wet and nameless is a river.
        const WaterKind kind = water.body == streaming::kOceanWaterBodyId ? WaterKind::Ocean
                               : water.body != streaming::kInvalidWaterBodyId ? WaterKind::Lake
                                                                              : WaterKind::River;
        // How much of a square this wide is under it: how far the ground is
        // below the surface against how fast the ground is falling. Half a
        // square is under the line where the point sits exactly on it.
        Fixed cover = core::kOne;
        if (fall.raw > 0)
            cover = clamped(Fixed::ratio(1, 2) + (water.surface - water.floor) / fall);
        return {water.surface, cover, kind, water.flowX, water.flowY};
    }

    // Dry at the point, which does not mean dry across the square. Two edges
    // can still be under water: the bank of a reach the point stands beside,
    // and sea level where the ground is about to cross it. The bank distance
    // comes off the same carve, so the apron a shader fades surf across ends
    // exactly where the water does.
    Fixed cover = core::kZero;
    if (water.reach != streaming::kInvalidRiverId)
        cover = clamped(Fixed::ratio(1, 2) - water.bankDistance / span);
    if (water.floor.raw < 0)
        cover = core::max(cover, fall.raw > 0
                                         ? clamped(Fixed::ratio(1, 2) - water.floor / fall)
                                         : core::kOne);
    else if (fall.raw > 0)
        cover = core::max(cover, clamped(Fixed::ratio(1, 2) - water.floor / fall));
    return {core::kZero, cover};
}

bool HeightField::underWater(WorldPos p) const {
    // Asked of the carve rather than by comparing a height against a level.
    // Those were two evaluations of two models, and where they disagreed a
    // point was under a surface that the ground it stood on had never heard
    // of - which is what a lake ending in mid-air is.
    const Pieces ground = piecesAt(p.x, p.y);
    return carved(p, ground.country, ground.moved).wet;
}

Travel HeightField::travelAt(WorldPos p) const {
    // Water first, and by depth rather than by whether there is any.
    //
    // One evaluation of each, not underWater() and then the same two questions
    // again: this is asked for every corner of every cell of every chunk of the
    // navmesh, and it was already most of what building one costs.
    const Fixed level = waterLevelAt(p);
    const Fixed ground = heightAt(p);
    if (ground.raw < level.raw) {
        const Fixed deep = level - ground;
        if (deep.raw <= fordableDepth().raw) return Travel::Ford;
        if (deep.raw <= swimmableDepth().raw) return Travel::Swim;
        return Travel::None;
    }
    // Two measures, and the steeper wins. The averaged slope says what the
    // hillside is doing; the sharpest step between neighbouring samples catches
    // the bank that the average smooths away. A body meets the second.
    const LatticeSpot at = spotOf(p);
    const Fixed step = Fixed::fromInt(kSampleMetres);
    const Fixed h00 = sampleHeight(at.sx, at.sy);
    const Fixed h10 = sampleHeight(at.sx + 1, at.sy);
    const Fixed h01 = sampleHeight(at.sx, at.sy + 1);
    const Fixed h11 = sampleHeight(at.sx + 1, at.sy + 1);
    const auto drop = [](Fixed a, Fixed b) {
        return Fixed::fromRaw(a.raw > b.raw ? a.raw - b.raw : b.raw - a.raw);
    };
    Fixed sharpest = drop(h00, h10);
    for (Fixed each : {drop(h01, h11), drop(h00, h01), drop(h10, h11)})
        if (each.raw > sharpest.raw) sharpest = each;
    const Fixed steepness = sharpest / step;
    const Fixed rolling = slopeAt(p);

    // Which of the two decides what is deliberate. The sharp step between two
    // neighbouring samples is what a body actually meets - a four-metre bank is
    // a bank however gentle the hillside around it is - but it is a measure over
    // four metres, and on rolling ground it flickers either side of a threshold
    // from one step to the next. Classifying by it gave a navmesh of hundreds
    // of slivers where the country is one hillside.
    //
    // So the going is read off the hillside, which is smooth, and the sharp step
    // is kept for what it is good for: a face that has to be climbed whatever
    // the ground around it is doing, and rock that does not give at all.
    if (steepness.raw >= unclimbableSlope().raw) return Travel::None;
    if (steepness.raw >= cliffSlope().raw) return Travel::Climb;
    if (rolling.raw < walkableSlope().raw) return Travel::Walk;
    if (rolling.raw < scrambleSlope().raw) return Travel::Scramble;
    if (rolling.raw < unclimbableSlope().raw) return Travel::Climb;
    return Travel::None;
}

bool HeightField::walkable(WorldPos p) const {
    if (underWater(p)) return false;
    // Broken ground, by the same measure the cliffs are cut with: if any edge of
    // the sample cell this point stands in drops faster than a body can hold on
    // to, the point is on a cliff. Asking the averaged slope instead - the
    // central difference over eight metres - loses exactly the case that
    // matters, a four-metre bank between two flat fields, and then the navmesh
    // and the cliff geometry disagree about where the world ends.
    const LatticeSpot at = spotOf(p);
    const Fixed limit = cliffSlope() * Fixed::fromInt(kSampleMetres);
    const Fixed h00 = sampleHeight(at.sx, at.sy);
    const Fixed h10 = sampleHeight(at.sx + 1, at.sy);
    const Fixed h01 = sampleHeight(at.sx, at.sy + 1);
    const Fixed h11 = sampleHeight(at.sx + 1, at.sy + 1);
    const auto steep = [&](Fixed a, Fixed b) {
        const std::int64_t drop = a.raw > b.raw ? a.raw - b.raw : b.raw - a.raw;
        return drop >= limit.raw;
    };
    return !(steep(h00, h10) || steep(h01, h11) || steep(h00, h01) || steep(h10, h11));
}

GroundSample HeightField::groundAt(WorldPos p) const {
    GroundSample out{};
    out.height = heightAt(p);
    out.normal = normalAt(p);
    out.slope = slopeAt(p);
    out.materials = materialsAt(p);
    out.water = out.height.raw < waterLevelAt(p).raw;
    out.travel = travelAt(p);
    out.soil = soilGiven(p, out.height, out.slope);
    return out;
}

} // namespace world
