#pragma once
// Brushes: what paints the layers (world_layers.hpp).
//
// Two kinds, one interface. The MANUAL brushes move a layer's value where the
// brush touches it - put a value down, raise, lower, smooth, or wipe the paint
// off so the generator's own value shows through again. The PROCEDURAL brushes
// are the generator's passes: the one that shapes the continents, the one that
// raises ranges along the plates' margins, the one that breaks the ground into
// hills. The generator runs each of them over the whole world; the brush runs
// the same code over its footprint, with dials of its own - another seed,
// another size - so "paint this the way the generator would, but here, and like
// this" is one stroke.
//
// Every brush is sized in texels of the layer it paints (brushLimits): no
// smaller than one, because a smaller brush paints nothing the layer can hold,
// and no larger than kMaxBrushTexels, so a dab costs the same on a fine layer
// and a coarse one. The radius is whole texels.
//
// A dab is soft-edged: full strength inside `hardness` of the radius, easing to
// nothing at the rim. On an override layer every tool is the same operation -
// move what the place IS towards a target by a fraction - and the target is
// what differs: a value (Paint), what is there plus a step (Raise, Lower), the
// average around it (Smooth), or the pass's own value at that place (the
// procedural brushes). Done on the premultiplied value and cover, that
// operation never needs to know what the generator made underneath; only the
// targets that start from what is there do, and they take a LayerBase.
#include <cstdint>
#include <utility>
#include <vector>

#include "game/generation/world_layers.hpp"
#include "game/generation/world_layout.hpp"

namespace generation {

enum class BrushTool : std::uint8_t {
    Paint,       // put a value down
    Raise,       // more of it
    Lower,       // less of it
    Smooth,      // the average of what is around
    Erase,       // back to what the generator makes
    Continents,  // PASS G1 as a brush: continental shape, own seed and lobe size
    Ranges,      // the plates' margin as a brush: uplift wedged along the stroke
    Hills,       // PASS G3's relief noise as a brush: own seed, size and amplitude
    Count,
};
inline constexpr std::size_t kBrushToolCount = static_cast<std::size_t>(BrushTool::Count);

struct BrushToolDef {
    BrushTool tool;
    const char* name;
    const char* label;
    bool procedural;
};
const BrushToolDef& brushToolDef(BrushTool tool);
// The tools a layer takes, its own procedural brush (if it has one) first.
std::vector<BrushTool> toolsFor(LayerId layer);

struct Brush {
    BrushTool tool = BrushTool::Paint;
    double radius = 0.0;       // metres: see legalRadius
    // How far one dab moves the layer towards its target, 0..1. For Ranges,
    // how high the range is, as a share of the most the layer holds.
    float strength = 0.5f;
    // The share of the radius at full strength; the rest eases off. 0..1.
    float hardness = 0.5f;
    float value = 0.0f;        // Paint: what goes down, in the layer's units
    // The procedural brushes' own dials.
    std::uint64_t seed = 1;
    float sizeKm = 0.0f;       // the pass's scale; nought: the generator's own
    float amount = 1.0f;       // Hills: amplitude, 1 = the generator's
};

struct BrushLimits {
    double texel = 0.0;        // metres
    double minRadius = 0.0;    // one texel
    double maxRadius = 0.0;    // kMaxBrushTexels of them, or the world if smaller
};
BrushLimits brushLimits(LayerId layer, const WorldLayout& layout);
// A radius the layer can take: whole texels, within its limits.
double legalRadius(LayerId layer, const WorldLayout& layout, double radius);
// How far apart the dabs of a stroke go: close enough that a stroke is a line
// at this layer's resolution, not a row of beads.
double dabSpacing(LayerId layer, const Brush& brush);

// The texels a dab changed, inclusive; empty if none.
struct TexelRect {
    std::int32_t x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    bool empty() const { return x1 < x0 || y1 < y0; }
    void add(const TexelRect& o);
};

// One dab of the brush at (x, y), world metres. `base` is what the generator
// makes on this layer (layerBase); Raise, Lower and Smooth on an override
// layer need it, and take the middle of the layer's range without one.
TexelRect dab(WorldLayout& layout, LayerId layer, const Brush& brush, double x, double y,
              const LayerBase* base = nullptr);

// Where the dabs go between the last one and the pointer: every `spacing`
// metres along the segment. `carry` is how far past the last dab the stroke
// already was; it is updated, so a slow drag still spaces its dabs evenly.
std::vector<std::pair<double, double>> strokeDabs(double fromX, double fromY, double toX, double toY,
                                                  double spacing, double& carry);

// What the procedural brushes put at a macro cell, before the dab blends it in:
// exactly what the generator's pass computes there, at the brush's dials.
float continentsAt(const Brush& brush, std::int32_t worldWidth, std::int32_t cellX, std::int32_t cellY);
float hillsAt(const Brush& brush, std::int32_t worldWidth, std::int32_t cellX, std::int32_t cellY);
// The size, in km, a procedural brush has when left at nought: the generator's.
float generatorSizeKm(BrushTool tool, std::int32_t worldWidth);

// What a layer is at a texel, painted over what the generator made.
float effectiveAt(const LayerMap& map, const LayerBase* base, std::int32_t tx, std::int32_t ty);

} // namespace generation

