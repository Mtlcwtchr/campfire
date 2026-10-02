#include "framework.hpp"

#include <cmath>

#include "game/world/edit_layer.hpp"
#include "game/world/terrain_brush.hpp"

namespace {
using namespace world;
using core::Fixed;

// A hill, so the brushes have something with shape in it to work on.
GroundAt hillPlus(const EditLayer& layer) {
    return [&layer](Fixed x, Fixed y) {
        const double dx = x.toDouble() - 500, dy = y.toDouble() - 500;
        const double hill = 120.0 * std::exp(-(dx * dx + dy * dy) / (2 * 90.0 * 90.0));
        return Fixed::fromDoubleForContent(hill) + layer.at(x, y);
    };
}
double heightOf(const GroundAt& ground, double x, double y) {
    return ground(Fixed::fromDoubleForContent(x), Fixed::fromDoubleForContent(y)).toDouble();
}
}

TEST(edit_layer_costs_nothing_until_something_is_edited) {
    // The one thing this must get right: a world nobody has touched is asked
    // for a height a million times a page, and it has to answer without a lock,
    // a hash or an allocation.
    EditLayer layer;
    CHECK(layer.empty());
    CHECK_EQ(layer.blocks(), std::size_t(0));
    CHECK_EQ(layer.at(Fixed::fromInt(12345), Fixed::fromInt(-678)).raw, std::int64_t(0));
    CHECK(!layer.touches({{Fixed::fromInt(-9999), Fixed::fromInt(-9999)},
                          {Fixed::fromInt(9999), Fixed::fromInt(9999)}}));

    layer.add(3, 4, Fixed::fromInt(10));
    CHECK(!layer.empty());
    CHECK_EQ(layer.blocks(), std::size_t(1));
    CHECK(layer.revision() > 0);
    // And a block holds its own neighbourhood: one sample edited does not make
    // a second block appear.
    layer.add(4, 4, Fixed::fromInt(10));
    CHECK_EQ(layer.blocks(), std::size_t(1));
    // Somewhere else does.
    layer.add(3000, 4, Fixed::fromInt(10));
    CHECK_EQ(layer.blocks(), std::size_t(2));
}

TEST(edit_layer_reads_back_what_was_written_and_blends_between_samples) {
    EditLayer layer;
    const auto step = EditLayer::kSampleMetres;
    layer.add(10, 10, Fixed::fromInt(20));
    // Exactly on the sample it was written to.
    CHECK(std::abs(layer.at(Fixed::fromInt(10 * step), Fixed::fromInt(10 * step)).toDouble() - 20) <
          0.01);
    // Halfway to an untouched neighbour is half of it - a surface, not a step.
    const auto half = layer.at(Fixed::fromDoubleForContent(10.5 * step),
                               Fixed::fromInt(10 * step))
                              .toDouble();
    CHECK(std::abs(half - 10.0) < 0.5);
    // And well away from it, nothing.
    CHECK_EQ(layer.at(Fixed::fromInt(40 * step), Fixed::fromInt(40 * step)).raw, std::int64_t(0));

    // Negative coordinates are ground too. Integer division towards zero is the
    // classic way a world west of its origin ends up sampling the wrong block.
    layer.add(-7, -9, Fixed::fromInt(5));
    CHECK(std::abs(layer.at(Fixed::fromInt(-7 * step), Fixed::fromInt(-9 * step)).toDouble() - 5) <
          0.01);
    CHECK(layer.touches({{Fixed::fromInt(-9 * step), Fixed::fromInt(-11 * step)},
                         {Fixed::fromInt(-5 * step), Fixed::fromInt(-7 * step)}}));
}

TEST(brushes_raise_lower_and_leave_no_rim_outside_their_radius) {
    EditLayer layer;
    const auto ground = hillPlus(layer);
    Brush brush;
    brush.kind = BrushKind::Raise;
    brush.radiusMetres = 40;
    brush.strength = 6;

    const double was = heightOf(ground, 500, 500);
    CHECK(applyBrush(layer, ground, brush, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0) > 0);
    CHECK(heightOf(ground, 500, 500) > was + 3.0);
    // Nothing outside the brush. A stroke that changes ground it never covered
    // is a stroke nobody can aim.
    CHECK_EQ(layer.at(Fixed::fromInt(600), Fixed::fromInt(500)).raw, std::int64_t(0));
    // And the edge is soft: the last sample inside the radius moved far less
    // than the middle did, so there is no cone rim to see.
    const double atMiddle = layer.at(Fixed::fromInt(500), Fixed::fromInt(500)).toDouble();
    const double atEdge = layer.at(Fixed::fromInt(536), Fixed::fromInt(500)).toDouble();
    CHECK(atEdge < atMiddle * 0.5);

    brush.kind = BrushKind::Lower;
    const double lifted = heightOf(ground, 500, 500);
    applyBrush(layer, ground, brush, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    CHECK(heightOf(ground, 500, 500) < lifted);
}

TEST(smooth_and_flatten_take_a_hillside_towards_what_they_promise) {
    EditLayer layer;
    const auto ground = hillPlus(layer);
    // The steepest part of the hill, one sigma out.
    const double x = 590, y = 500;
    // Curvature averaged over the middle of the stroke, at the brush's own
    // spacing. Measured at one point instead, a smooth brush appears to fail:
    // it has a falloff, so it leaves its own gentle dome behind, and at a
    // single probe inside a wide brush that dome outweighs the flattening it
    // did. What Smooth promises is less curvature across the ground it covers,
    // and that is what this asks.
    const auto roughness = [&] {
        double sum = 0;
        int seen = 0;
        for (int step = -4; step <= 4; ++step) {
            const double at = x + step * 4;
            const double a = heightOf(ground, at - 4, y), b = heightOf(ground, at, y),
                         c = heightOf(ground, at + 4, y);
            sum += std::abs(a + c - 2 * b);
            ++seen;
        }
        return sum / seen;
    };

    Brush smooth;
    smooth.kind = BrushKind::Smooth;
    smooth.radiusMetres = 60;
    smooth.strength = 6;
    const double before = roughness();
    for (int stroke = 0; stroke < 12; ++stroke)
        applyBrush(layer, ground, smooth, {Fixed::fromDoubleForContent(x),
                                           Fixed::fromDoubleForContent(y)}, 1.0);
    CHECK(roughness() <= before + 1e-6);

    // Flatten takes the ground towards the height under the middle of the
    // brush, which is the one thing it is for.
    EditLayer flat;
    const auto other = hillPlus(flat);
    Brush flatten;
    flatten.kind = BrushKind::Flatten;
    flatten.radiusMetres = 60;
    flatten.strength = 6;
    const double target = heightOf(other, 500, 500);
    const double away = heightOf(other, 540, 500);
    for (int stroke = 0; stroke < 20; ++stroke)
        applyBrush(flat, other, flatten, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    const double now = heightOf(other, 540, 500);
    CHECK(std::abs(now - target) < std::abs(away - target));
}

TEST(the_thermal_brush_only_ever_removes_and_stops_at_its_angle) {
    // The same statement the generator's talus cut makes, because it is the
    // same operator: a person laying back a slope by hand and the generator
    // laying it back at build time must leave the same kind of slope.
    EditLayer layer;
    // A cliff: forty metres of drop in one sample.
    const GroundAt cliff = [&layer](Fixed x, Fixed y) {
        return Fixed::fromDoubleForContent(x.toDouble() < 500 ? 40.0 : 0.0) + layer.at(x, y);
    };
    Brush brush;
    brush.kind = BrushKind::Thermal;
    brush.radiusMetres = 60;
    brush.strength = 8;

    const double high = heightOf(cliff, 480, 500);
    for (int stroke = 0; stroke < 30; ++stroke)
        applyBrush(layer, cliff, brush, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    // It cut the lip down...
    CHECK(heightOf(cliff, 480, 500) < high);
    // ...and it never lifted anything. A talus cut that adds is a talus cut
    // that invents mass, which is how a "smoothing" tool grows hills.
    for (std::int64_t sy = 110; sy < 140; ++sy)
        for (std::int64_t sx = 110; sx < 140; ++sx)
            CHECK(layer.at(Fixed::fromInt(sx * EditLayer::kSampleMetres),
                           Fixed::fromInt(sy * EditLayer::kSampleMetres))
                          .raw <= 0);
}

TEST(the_carve_brush_cuts_a_channel_and_the_noise_brush_makes_ground) {
    EditLayer layer;
    const auto ground = hillPlus(layer);
    Brush carve;
    carve.kind = BrushKind::Carve;
    carve.radiusMetres = 50;
    carve.scaleMetres = 40;   // a forty-metre channel
    carve.strength = 5;
    applyBrush(layer, ground, carve, {Fixed::fromInt(500), Fixed::fromInt(500)}, 2.0);
    // Deepest in the middle, shallower at the bank, nothing past it.
    const double middle = layer.at(Fixed::fromInt(500), Fixed::fromInt(500)).toDouble();
    const double bank = layer.at(Fixed::fromInt(516), Fixed::fromInt(500)).toDouble();
    CHECK(middle < -1.0);
    CHECK(bank > middle);
    CHECK(bank <= 0.0);
    CHECK_EQ(layer.at(Fixed::fromInt(560), Fixed::fromInt(500)).raw, std::int64_t(0));

    // Noise has to vary across its own footprint, or it is a bump with extra
    // steps. Two points a wavelength apart must differ.
    EditLayer rough;
    const auto plain = hillPlus(rough);
    Brush noise;
    noise.kind = BrushKind::Noise;
    noise.radiusMetres = 200;
    noise.scaleMetres = 40;
    noise.strength = 10;
    noise.seed = 7;
    applyBrush(rough, plain, noise, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    double lowest = 1e9, highest = -1e9;
    for (int step = -20; step <= 20; ++step) {
        const double v = rough.at(Fixed::fromInt(500 + step * 4), Fixed::fromInt(500)).toDouble();
        lowest = std::min(lowest, v);
        highest = std::max(highest, v);
    }
    CHECK(highest - lowest > 1.0);
}

TEST(a_wide_smooth_takes_the_shape_of_a_hill_down_not_only_its_ripple) {
    // What a person sees a smooth do: a peak under a wide brush comes down
    // and its flanks fill. At four metres to the neighbour a sixty-metre
    // smooth moved the top of this hill by millimetres; at the brush's own
    // scale it has to move it by metres.
    EditLayer layer;
    const auto ground = hillPlus(layer);
    Brush smooth;
    smooth.kind = BrushKind::Smooth;
    smooth.radiusMetres = 160;
    smooth.strength = 6;
    const double top = heightOf(ground, 500, 500);
    for (int stroke = 0; stroke < 10; ++stroke)
        applyBrush(layer, ground, smooth, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    CHECK(heightOf(ground, 500, 500) < top - 2.0);
}

TEST(restore_gives_hand_edits_back_to_the_generated_ground) {
    // Hybrid, in one brush: a hand-raised bump, taken back by Restore, is the
    // ground the generator made there - and Restore never goes past it.
    EditLayer layer;
    const auto ground = hillPlus(layer);
    Brush raise;
    raise.radiusMetres = 40;
    raise.strength = 10;
    const double generated = heightOf(ground, 500, 500);
    applyBrush(layer, ground, raise, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    CHECK(heightOf(ground, 500, 500) > generated + 5.0);
    Brush restore;
    restore.kind = BrushKind::Restore;
    restore.radiusMetres = 60;
    restore.strength = 8;
    restore.softness = 0.2;
    for (int stroke = 0; stroke < 40; ++stroke) {
        // Resolved on a layer of its own and then added, as the tools do.
        EditLayer scratch;
        applyBrush(scratch, ground, restore, {Fixed::fromInt(500), Fixed::fromInt(500)}, 0.5, &layer);
        for (std::int64_t sy = 110; sy <= 140; ++sy)
            for (std::int64_t sx = 110; sx <= 140; ++sx)
                if (const auto add = scratch.sample(sx, sy); add.raw != 0) layer.add(sx, sy, add);
    }
    CHECK(std::abs(heightOf(ground, 500, 500) - generated) < 0.1);
    CHECK(heightOf(ground, 500, 500) >= generated - 0.01);
    // Where nothing was edited it does nothing at all.
    EditLayer untouched;
    EditLayer nothing;
    CHECK_EQ(applyBrush(nothing, hillPlus(untouched), restore, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0,
                        &untouched), std::size_t(0));
}

TEST(erode_cuts_down_the_slope_and_leaves_the_crest) {
    EditLayer layer;
    const auto ground = hillPlus(layer);
    Brush erode;
    erode.kind = BrushKind::Hydraulic;
    erode.radiusMetres = 200;
    erode.strength = 8;
    const double crest = heightOf(ground, 500, 500), flank = heightOf(ground, 600, 500);
    for (int stroke = 0; stroke < 10; ++stroke)
        applyBrush(layer, ground, erode, {Fixed::fromInt(500), Fixed::fromInt(500)}, 1.0);
    // The flank, where the water from the crest runs, lost ground; the crest,
    // which nothing drains through, hardly any.
    const double cutFlank = flank - heightOf(ground, 600, 500);
    const double cutCrest = crest - heightOf(ground, 500, 500);
    CHECK(cutFlank > 0.5);
    CHECK(cutFlank > cutCrest);
}

TEST(a_wide_brush_reads_the_ground_at_its_own_scale) {
    // The ground is dear to ask for - a real one carves rivers into every
    // answer - so a brush the size of a valley must not ask for it at every
    // four metres of the valley.
    EditLayer layer;
    std::size_t asked = 0;
    const auto hill = hillPlus(layer);
    const GroundAt counted = [&](Fixed x, Fixed y) { ++asked; return hill(x, y); };
    Brush smooth;
    smooth.kind = BrushKind::Smooth;
    smooth.radiusMetres = 400;
    applyBrush(layer, counted, smooth, {Fixed::fromInt(500), Fixed::fromInt(500)}, 0.1);
    CHECK(asked < 2000);
    // And a raise does not read it at all.
    asked = 0;
    Brush raise;
    raise.radiusMetres = 400;
    applyBrush(layer, counted, raise, {Fixed::fromInt(500), Fixed::fromInt(500)}, 0.1);
    CHECK_EQ(asked, std::size_t(0));
}
