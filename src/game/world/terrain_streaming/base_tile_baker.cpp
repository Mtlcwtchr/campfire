#include "game/world/terrain_streaming/base_tile_baker.hpp"

#include "engine/environment/environment.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <vector>

#include "game/generation/world_map_gen.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"

namespace world::streaming {
namespace {

using core::Fixed;

constexpr std::int64_t kQuantiseSteps = 65535;

// The widest valley the graph draws, which is how far outside a page a reach
// can still put water in it.
constexpr std::int64_t kWidestValleyMetres = 800;

// What the offshore shelf falls to past the last cell of the map, and how far
// a sea cell's bed sits below the waterline. Both are HeightField's own
// numbers; they are here because the envelope has to contain what it produces.
constexpr std::int64_t kShelfMetres = 60;
constexpr std::int64_t kSeaBedMetres = 8;

// A world with no cells in it still has to quantise something. The field
// invents a country of its own from the seed when there is no coarse map, and
// these are the bounds ringHeightBounds answers with for the same case.
constexpr std::int64_t kUnmappedLow = -256;
constexpr std::int64_t kUnmappedHigh = 512;

std::int16_t shoreOf(Fixed metres) {
    const auto scaled = (metres * Fixed::fromInt(kShoreDecimetresPerMetre)).roundToInt();
    return static_cast<std::int16_t>(
            std::clamp<std::int64_t>(scaled, std::numeric_limits<std::int16_t>::min(),
                                     std::numeric_limits<std::int16_t>::max()));
}

std::int8_t unitByte(Fixed unit) {
    const auto scaled = (unit * Fixed::fromInt(127)).roundToInt();
    return static_cast<std::int8_t>(std::clamp<std::int64_t>(scaled, -127, 127));
}

} // namespace

std::uint16_t HsimQuantisation::quantise(Fixed height) const {
    if (!valid()) return 0;
    const auto span = static_cast<Fixed::Wide>(high.raw) - static_cast<Fixed::Wide>(low.raw);
    auto offset = static_cast<Fixed::Wide>(height.raw) - static_cast<Fixed::Wide>(low.raw);
    offset = std::clamp<Fixed::Wide>(offset, 0, span);
    // Rounded, in integers, in one width that cannot overflow: the same world
    // coordinate has to give the same integer in every page that holds it and
    // on every machine that bakes it.
    return static_cast<std::uint16_t>((offset * kQuantiseSteps + span / 2) / span);
}

Fixed HsimQuantisation::dequantise(std::uint16_t stored) const {
    if (!valid()) return core::kZero;
    const auto span = static_cast<Fixed::Wide>(high.raw) - static_cast<Fixed::Wide>(low.raw);
    return Fixed::fromRaw(low.raw + static_cast<Fixed::Raw>((span * stored) / kQuantiseSteps));
}

Fixed HsimQuantisation::resolution() const {
    if (!valid()) return core::kZero;
    const auto span = static_cast<Fixed::Wide>(high.raw) - static_cast<Fixed::Wide>(low.raw);
    return Fixed::fromRaw(static_cast<Fixed::Raw>(span / (2 * kQuantiseSteps)) + 1);
}

HsimQuantisation hsimQuantisationFor(const generation::WorldMapData& world) {
    const auto cells = static_cast<std::size_t>(world.width) * world.height;
    if (world.width <= 0 || world.height <= 0 || world.cells.size() != cells)
        return {Fixed::fromInt(kUnmappedLow), Fixed::fromInt(kUnmappedHigh)};

    // The same reading of the coarse map that ringHeightBounds makes, over the
    // whole map and in integers. A filled cell stores its brim, so the bed is
    // the brim less the fill; a lake keeps its own pre-erosion head, which is
    // above the ground the spline draws.
    const std::int64_t step = generation::kMetresPerElevationStep;
    std::int64_t low = -kShelfMetres;   // a finite world ends in shelving sea
    std::int64_t high = 0;
    for (std::size_t i = 0; i < cells; ++i) {
        const generation::WorldCell& cell = world.cells[i];
        if (cell.sea) {
            low = std::min(low, -kSeaBedMetres);
            continue;
        }
        const std::int64_t fill =
                i < world.lakeDepthField.size() ? std::max(0, world.lakeDepthField[i]) : 0;
        const auto brim = static_cast<std::int64_t>(cell.elevation);
        low = std::min(low, (brim - fill) * step);
        high = std::max(high, brim * step);
        if (fill > 0 && i < world.lakeLevelField.size())
            high = std::max(high, static_cast<std::int64_t>(world.lakeLevelField[i]) * step);
    }
    // Macro elevations saturate at 255; H64 mountains are allowed to be taller.
    // Include every stage so switching geometry cannot clip an un-eroded peak
    // or change the encoding of already resident height pages.
    if (world.terrainFoundation) {
        // Held chunks only: the rest is the sea floor (cell_field.hpp).
        const auto [bottom,top]=world.terrainFoundation->heightRange();
        low=std::min(low,static_cast<std::int64_t>(bottom)/10-1);
        high=std::max(high,static_cast<std::int64_t>(top)/10+1);
    }

    // What the field adds on top of the coarse map, from ringHeightBounds:
    // Catmull-Rom overshoot is up to 0.28125 of the input range, the detail
    // octaves total under 92 m, channels deepen by under 12 m, and the rest is
    // fixed-point rounding. Generous on purpose - a height outside the
    // envelope is clamped, and a clamped mountain is a quiet loss.
    const std::int64_t margin = (high - low) * 3 / 10 + 160 + kAlpineHeightMarginMetres;
    return {Fixed::fromInt(low - margin), Fixed::fromInt(high + margin)};
}

BaseTileBaker::BaseTileBaker(const generation::WorldMapData& world, const HydrologyGraph& graph,
                             HsimQuantisation quantisation, const EditLayer* edits)
    : world_(world), graph_(graph), quantisation_(quantisation), field_(&world, world.seed) {
    field_.edits(edits);
    const auto cells = static_cast<std::size_t>(std::max(0, world.width)) *
                       static_cast<std::size_t>(std::max(0, world.height));
    if (world.cells.size() != cells) return;

    // Catchments, packed into the sixteen bits the channel has. The generator
    // labels a catchment by the cell index of its outlet, which does not fit;
    // sorting those labels gives a dense ID that does, and that does not
    // depend on the order anything was walked in. Zero means "none": the sea,
    // and anything that drains off the map.
    if (!world.basinIdField.empty() && world.basinIdField.size() == cells) {
        std::vector<std::int32_t> labels;
        labels.reserve(cells);
        for (std::size_t i = 0; i < cells; ++i)
            if (!world.cells[i].sea && world.basinIdField[i] >= 0)
                labels.push_back(world.basinIdField[i]);
        std::sort(labels.begin(), labels.end());
        labels.erase(std::unique(labels.begin(), labels.end()), labels.end());
        watershedCount_ = labels.size();
        watershedOfCell_.assign(cells, 0);
        for (std::size_t i = 0; i < cells; ++i) {
            if (world.cells[i].sea || world.basinIdField[i] < 0) continue;
            const auto at = std::lower_bound(labels.begin(), labels.end(), world.basinIdField[i]);
            const auto dense = static_cast<std::size_t>(at - labels.begin()) + 1;
            // More catchments than the channel can name is a real possibility
            // on a large map, and naming the wrong one is worse than naming
            // none: the overflow stays unlabelled rather than wrapping.
            if (dense <= std::numeric_limits<std::uint16_t>::max())
                watershedOfCell_[i] = static_cast<std::uint16_t>(dense);
        }
    }
}

BaseTile BaseTileBaker::bake(TileKey key, std::int32_t sampleMetres, std::uint16_t padding,
                             const std::function<bool()>& cancelled) const {
    return bakePage(key, sampleMetres, padding, cancelled).base;
}

void BaseTileBaker::environment(const engine::environment::Environment* environment) {
    environment_ = environment;
    field_.features(environment && !environment->features().empty() ? &environment->features() : nullptr);
}

BakedPage BaseTileBaker::bakePage(TileKey key, std::int32_t sampleMetres, std::uint16_t padding,
                                  const std::function<bool()>& cancelled, const BakedPage* parent) const {
    // Features as wide as this level can draw (HeightField::featureStride).
    field_.featureStride(sampleMetres);
    BakedPage page;
    BaseTile& tile = page.base;
    WaterTile& water = page.water;
    if (sampleMetres <= 0 || sampleMetres % kSampleMetres != 0) return page;
    const auto interior = interiorSamples(sampleMetres);
    if (interior == 0) return page;
    if (!quantisation_.valid()) return page;

    page.world = &world_;
    page.graph = &graph_;
    // A different world, graph, quantisation or lattice is not a parent, even
    // if its key happens to match. Incompatible inputs fall back to a full bake.
    if (parent && (parent->world != &world_ || parent->graph != &graph_ ||
        !parent->base.valid() || !parent->water.valid() ||
        parent->base.key.x != key.x || parent->base.key.y != key.y ||
        parent->base.sampleMetres <= sampleMetres || parent->base.sampleMetres % sampleMetres ||
        parent->base.width != interiorSamples(parent->base.sampleMetres) ||
        parent->base.height != parent->base.width ||
        parent->base.waterBodyId.size() != parent->base.heightQuantized.size() ||
        parent->base.watershedId.size() != parent->base.heightQuantized.size() ||
        parent->water.key != parent->base.key ||
        parent->water.width != parent->base.width || parent->water.height != parent->base.height ||
        parent->water.padding != parent->base.padding || parent->water.sampleMetres != parent->base.sampleMetres ||
        parent->water.elevationMin != quantisation_.low || parent->water.elevationMax != quantisation_.high ||
        parent->base.elevationMin != quantisation_.low || parent->base.elevationMax != quantisation_.high ||
        parent->refinementDepth.size() != parent->base.heightQuantized.size() ||
        parent->refinementAshore.size() != parent->base.heightQuantized.size() ||
        parent->large.deltaQuantized.size() != parent->base.heightQuantized.size())) parent = nullptr;

    tile.key = key;
    tile.width = interior;
    tile.height = interior;
    tile.padding = padding;
    tile.sampleMetres = sampleMetres;
    tile.elevationMin = quantisation_.low;
    tile.elevationMax = quantisation_.high;

    const auto side = static_cast<std::int64_t>(interior) + 2 * padding;
    const auto count = static_cast<std::size_t>(side * side);
    tile.heightQuantized.assign(count, 0);
    tile.waterBodyId.assign(count, static_cast<std::uint16_t>(kInvalidWaterBodyId));
    tile.watershedId.assign(count, 0);
    tile.walkability.assign(count, 0);

    water.key = key;
    water.width = interior;
    water.height = interior;
    water.padding = padding;
    water.sampleMetres = sampleMetres;
    water.elevationMin = quantisation_.low;
    water.elevationMax = quantisation_.high;
    water.surfaceQuantized.assign(count, 0);
    water.waterBodyId.assign(count, static_cast<std::uint16_t>(kInvalidWaterBodyId));
    water.riverId.assign(count, kInvalidRiverId);
    water.shoreDecimetres.assign(count, std::numeric_limits<std::int16_t>::max());
    water.coverage.assign(count, 0);
    water.flowX.assign(count, 0);
    water.flowY.assign(count, 0);
    water.estuary.assign(count, 0);
    // Exact depth for the dependent passes below. Parent pages retain it for
    // refinement: quantised heights cannot reproduce depth thresholds exactly.
    std::vector<Fixed> depth(count, core::kZero);
    if (sampleMetres > kSampleMetres) page.refinementAshore.resize(count);

    // H64 and H16 share the large band's value at every common lattice point.
    // This makes H64 a strict foundation of H16 instead of an independently
    // sampled shape. H8 refines H16; H4 adds the medium band.
    // And every coarser page is a strict subsample of H64, bands and all.
    const bool wantsLarge = sampleMetres <= 16 || sampleMetres >= 64;
    const bool wantsMedium = sampleMetres <= kSampleMetres;
    const auto startBand = [&](ResidualTile& band, ResidualLevel level) {
        band.key = key;
        band.level = level;
        band.width = interior;
        band.height = interior;
        band.padding = padding;
        band.sampleMetres = sampleMetres;
        band.deltaQuantized.assign(count, 0);
    };
    if (wantsLarge) startBand(page.large, ResidualLevel::Large);
    if (wantsMedium) startBand(page.medium, ResidualLevel::Medium);
    const auto storeBand = [](ResidualTile& band, std::size_t at, Fixed metres) {
        if (band.deltaQuantized.empty()) return;
        const auto scaled = (metres * Fixed::fromInt(kResidualPerMetre)).roundToInt();
        band.deltaQuantized[at] = static_cast<std::int16_t>(
                std::clamp<std::int64_t>(scaled, std::numeric_limits<std::int16_t>::min(),
                                         std::numeric_limits<std::int16_t>::max()));
    };

    // Global lattice indices, so nothing about a sample depends on which page
    // is asking. `stride` is how many lattice steps one stored sample spans:
    // a coarser page is a strict subsample of the same surface, never a
    // different one.
    const std::int64_t stride = sampleMetres / kSampleMetres;
    const std::int64_t perPage = pageMetresForSpacing(sampleMetres) / sampleMetres;
    const std::int64_t originX = static_cast<std::int64_t>(key.x) * perPage - padding;
    const std::int64_t originY = static_cast<std::int64_t>(key.y) * perPage - padding;

    // The reaches and basins that can shape this page, gathered once. What
    // the field is still asked for is the country and its detail - the
    // terrain layer, which the water has not touched yet; what cuts into it
    // is the graph.
    const std::int32_t haloMetres = static_cast<std::int32_t>(kWidestValleyMetres) +
                                    sampleMetres * (padding + 1);
    const GraphCarver carver(graph_, tileBounds(key), haloMetres);

    const std::int64_t macroMetres = generation::kMetresPerCell;
    for (std::int64_t row = 0; row < side; ++row) {
        // Between rows, not between samples: polling is cheap but a page is
        // seventeen thousand samples and the check would show up in the bake.
        if (cancelled && cancelled()) return {};
        const std::int64_t sy = (originY + row) * stride;
        const std::int64_t worldY = sy * kSampleMetres;
        const std::int64_t macroY = floorDiv(worldY, macroMetres);
        for (std::int64_t column = 0; column < side; ++column) {
            const std::int64_t sx = (originX + column) * stride;
            const std::int64_t worldX = sx * kSampleMetres;
            const auto at = static_cast<std::size_t>(row * side + column);

            if (parent) {
                const auto& p = parent->base;
                const auto dx = (column - padding) * sampleMetres;
                const auto dy = (row - padding) * sampleMetres;
                const auto px = dx / p.sampleMetres + p.padding;
                const auto py = dy / p.sampleMetres + p.padding;
                const auto parentSide = p.width + 2 * p.padding;
                if (dx % p.sampleMetres == 0 && dy % p.sampleMetres == 0 &&
                    px >= 0 && py >= 0 && px < parentSide && py < p.height + 2 * p.padding) {
                    const auto from = static_cast<std::size_t>(py * parentSide + px);
                    tile.heightQuantized[at] = p.heightQuantized[from];
                    tile.waterBodyId[at] = p.waterBodyId[from];
                    tile.watershedId[at] = p.watershedId[from];
                    water.surfaceQuantized[at] = parent->water.surfaceQuantized[from];
                    water.waterBodyId[at] = parent->water.waterBodyId[from];
                    water.riverId[at] = parent->water.riverId[from];
                    water.shoreDecimetres[at] = parent->water.shoreDecimetres[from];
                    water.flowX[at] = parent->water.flowX[from];
                    water.flowY[at] = parent->water.flowY[from];
                    if (from < parent->water.estuary.size()) water.estuary[at] = parent->water.estuary[from];
                    depth[at] = parent->refinementDepth[from];
                    const auto ashore = parent->refinementAshore[from];
                    if (!page.refinementAshore.empty()) page.refinementAshore[at] = ashore;
                    if (wantsLarge) page.large.deltaQuantized[at] = parent->large.deltaQuantized[from];
                    if (wantsMedium) {
                        if (parent->medium.deltaQuantized.size() == p.heightQuantized.size())
                            page.medium.deltaQuantized[at] = parent->medium.deltaQuantized[from];
                        else
                            storeBand(page.medium, at, field_.residualAt(
                                {Fixed::fromInt(worldX), Fixed::fromInt(worldY)}).medium * ashore);
                    }
                    ++page.reusedSamples;
                    continue;
                }
            }
            ++page.evaluatedSamples;
            // The country and how far the detail layer moved it, before any
            // water. Everything the water then does to it comes from the
            // graph, in one call, so the bed cannot end up above its own
            // surface.
            const core::WorldPos position{Fixed::fromInt(worldX), Fixed::fromInt(worldY)};
            auto pieces = field_.piecesAt(position.x, position.y);
            if (sampleMetres > 64 && landMask_) {
                const std::int64_t px = floorDiv(worldX, kPageMetres) * kPageMetres;
                const std::int64_t py = floorDiv(worldY, kPageMetres) * kPageMetres;
                constexpr std::int64_t halo = terrain::kFoundationMetres;
                if (!landMask_->anyLandInWorldRect(int(px - halo), int(py - halo), int(px + kPageMetres + halo),
                                                   int(py + kPageMetres + halo))) {
                    pieces.country = Fixed::fromInt(-60);
                    pieces.moved = core::kZero;
                }
            }
            CarvedSample carved;
            if (world_.terrainFoundation && world_.terrainStage<generation::TerrainStage::Water) {
                carved.floor=pieces.country+pieces.moved;
                carved.bankDistance=Fixed::fromInt(1 << 20);
            } else {
                carved=carver.carve(position,pieces.country,pieces.moved);
                if (world_.terrainFoundation && world_.terrainStage==generation::TerrainStage::Water) {
                    carved.floor=pieces.country+pieces.moved;
                    carved.wet=carved.wet && carved.body!=kInvalidWaterBodyId && carved.surface>carved.floor;
                }
            }
            // A bog's pools: the ground lowered a few decimetres, water standing
            // a hand below the old surface, wet where the pit is deeper than that.
            if (!carved.wet && field_.hasBogPools()) {
                const Fixed pit = field_.bogPoolDepth(position.x, position.y);
                if (pit.raw > 0) {
                    const Fixed rim = carved.floor;
                    carved.floor = rim - pit;
                    carved.surface = rim - Fixed::ratio(1, 10);
                    if (pit > Fixed::ratio(12, 100)) {
                        carved.wet = true;
                        carved.body = kBogPoolWaterBodyId;
                        carved.bankDistance = -pit;
                    }
                }
            }
            // Today's water in a feature's channel (engine/environment): a
            // lens standing on the carved bed, where no river already is.
            if (!carved.wet && environment_ && environment_->features().anyWater()) {
                const Fixed lens = environment_->features().waterDepth(position, sampleMetres);
                if (lens > Fixed::ratio(5, 100)) {
                    carved.surface = carved.floor + lens;
                    carved.wet = true;
                    carved.body = kBogPoolWaterBodyId;
                    carved.bankDistance = -lens;
                }
            }
            tile.heightQuantized[at] = quantisation_.quantise(carved.floor);
            tile.waterBodyId[at] = carved.wet
                                           ? static_cast<std::uint16_t>(carved.body)
                                           : static_cast<std::uint16_t>(kInvalidWaterBodyId);
            // The head on a dry sample is there for filtering - so the surface
            // interpolates to its own bank instead of to sea level - and it may
            // never stand above the sample's own ground.
            //
            // The renderer draws water wherever the head is over the bed, with
            // no other test, and a reach hands its head to the whole of its
            // valley. Where the valley floor lies under that head, beyond the
            // bank the water actually reaches, every dry sample of it was
            // drawn flooded at the river's level, out to the edge of the valley
            // where the head fell back to the sea's: a sheet of water standing
            // over dry country and ending in mid-air. Measured on the tiny
            // world, one river edge in four hung by more than a metre.
            water.surfaceQuantized[at] = quantisation_.quantise(
                    carved.wet ? carved.surface : core::min(carved.surface, carved.floor));
            if (carved.wet) {
                depth[at] = carved.surface - carved.floor;
                water.waterBodyId[at] = static_cast<std::uint16_t>(carved.body);
            }
            water.riverId[at] = carved.reach;
            water.shoreDecimetres[at] = shoreOf(carved.bankDistance);

            // Detail, but not in the water and not on its bank.
            //
            // A residual is a landscape a few metres deep and a water surface
            // is a level: bumps put in a channel stand out of their own river,
            // and bumps on the bank move the line the water ends at, which is
            // the one line a visual layer may not move. So it fades in from
            // the water's edge over a couple of samples and is absent inside
            // it - the same argument the detail layer's own damping makes.
            if (wantsLarge || wantsMedium) {
                const Fixed reach = Fixed::fromInt(2 * kSampleMetres);
                const Fixed ashore =
                        carved.bankDistance.raw <= 0
                                ? core::kZero
                                : (carved.bankDistance.raw >= reach.raw
                                           ? core::kOne
                                           : carved.bankDistance / reach);
                const HeightField::Residual residual = field_.residualAt(position);
                storeBand(page.large, at, residual.large * ashore);
                storeBand(page.medium, at, residual.medium * ashore);
                if (!page.refinementAshore.empty()) page.refinementAshore[at] = ashore;
            }
            water.flowX[at] = unitByte(carved.flowX);
            water.flowY[at] = unitByte(carved.flowY);
            water.estuary[at] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(
                    (carved.estuary * Fixed::fromInt(255)).roundToInt(), 0, 255));

            const std::int64_t macroX = floorDiv(worldX, macroMetres);
            const bool onMap = macroX >= 0 && macroY >= 0 && macroX < world_.width &&
                               macroY < world_.height;
            const auto cell = onMap ? static_cast<std::size_t>(macroY) * world_.width + macroX
                                    : std::size_t(0);
            tile.watershedId[at] =
                    onMap && cell < watershedOfCell_.size() ? watershedOfCell_[cell] : 0;
        }
    }
    // --- how a body gets over it ---------------------------------------
    //
    // Read off the lattice the page already holds, not by asking the field
    // again: the padded border is there precisely so a sample can see its own
    // neighbours, and a slope taken from four more field evaluations would be
    // four more chances to disagree with the ground the page stores.
    //
    // Water first, and by depth rather than by whether there is any: a stream
    // a body wades is not a wall, and treating it as one cut this country
    // into pieces. travelFrom makes that choice; the slope is worked out
    // either way because it costs four subtractions off an array.
    const Fixed run = Fixed::fromInt(sampleMetres);
    for (std::int64_t row = 1; row + 1 < side; ++row) {
        for (std::int64_t column = 1; column + 1 < side; ++column) {
            const auto at = static_cast<std::size_t>(row * side + column);
            const auto height = [&](std::int64_t r, std::int64_t c) {
                return quantisation_.dequantise(
                        tile.heightQuantized[static_cast<std::size_t>(r * side + c)]);
            };
            const Fixed here = height(row, column);
            const Fixed west = height(row, column - 1), east = height(row, column + 1);
            const Fixed north = height(row - 1, column), south = height(row + 1, column);

            // Two measures, and each is good for something different. The
            // sharpest step to a neighbour is what a body actually meets - a
            // four-metre bank is a bank however gentle the hillside around it
            // - but read alone it flickers either side of a threshold on
            // rolling ground and cuts a navmesh into slivers. So the going is
            // read off the hillside, which is smooth, and the sharp step is
            // kept for the face that has to be climbed whatever the ground
            // around it is doing.
            Fixed sharpest = core::kZero;
            for (const Fixed neighbour : {west, east, north, south})
                sharpest = core::max(sharpest, core::abs(neighbour - here));
            const Fixed steepness = sharpest / run;
            const Fixed dzdx = (east - west) / (run * 2);
            const Fixed dzdy = (south - north) / (run * 2);
            const Fixed rolling = core::sqrt(dzdx * dzdx + dzdy * dzdy);

            tile.walkability[at] = encodeTravel(travelFrom(depth[at], steepness, rolling));
        }
    }
    // --- what the ground is made of ------------------------------------
    //
    // After the heights, because the rules read the height and the slope and
    // both are on the page by now: asking the field for them again would be
    // four more evaluations a point and two more chances to disagree with the
    // ground actually stored.
    {
        const std::int32_t step = std::max(sampleMetres, 8);
        const std::int64_t every = step / sampleMetres;
        page.materialMetres = step;
        page.materialWidth = static_cast<std::uint16_t>((side + every - 1) / every);
        page.materials.assign(static_cast<std::size_t>(page.materialWidth) *
                                      page.materialWidth * kMaterialCount,
                              0);
        for (std::int64_t row = 0; row < page.materialWidth; ++row) {
            if (cancelled && cancelled()) return {};
            for (std::int64_t column = 0; column < page.materialWidth; ++column) {
                const std::int64_t r = std::min<std::int64_t>(row * every, side - 1);
                const std::int64_t c = std::min<std::int64_t>(column * every, side - 1);
                const auto from = static_cast<std::size_t>(r * side + c);
                const std::int64_t sx = (originX + c) * stride;
                const std::int64_t sy = (originY + r) * stride;
                const Fixed height = quantisation_.dequantise(tile.heightQuantized[from]);
                // The hillside as the page has it, from its own neighbours.
                const auto at = [&](std::int64_t rr, std::int64_t cc) {
                    return quantisation_.dequantise(tile.heightQuantized[static_cast<std::size_t>(
                            std::clamp<std::int64_t>(rr, 0, side - 1) * side +
                            std::clamp<std::int64_t>(cc, 0, side - 1))]);
                };
                const Fixed run = Fixed::fromInt(sampleMetres) * Fixed::fromInt(2);
                const Fixed slope = core::hypot((at(r, c + 1) - at(r, c - 1)) / run,
                                                (at(r + 1, c) - at(r - 1, c)) / run);
                // Averaged over the texel's own footprint, not point-sampled at
                // one corner of it.
                //
                // A coarse texel covers several samples of ground and used to
                // be painted with whatever the first of them happened to be. On
                // a broken hillside the odds of landing on something steep are
                // good, so a mountain read as solid stone from a distance and
                // then lost most of that stone as the camera came in and the
                // texels got small enough to tell rock from the grass beside
                // it. Nothing was repainted: one picture was a sample and the
                // other was the truth.
                //
                // Four points spread across the footprint rather than every
                // sample in it: the cost is fixed whatever the level, and four
                // is enough to stop one steep sample speaking for a hundred
                // metres of hillside.
                MaterialWeights weights = field_.materialsGiven(sx, sy, height, slope, stride);
                if (every > 1) {
                    const std::int64_t quarter = std::max<std::int64_t>(1, every / 4);
                    const std::int64_t offsets[]{quarter, every - quarter};
                    for (const std::int64_t dy : offsets)
                        for (const std::int64_t dx : offsets) {
                            const std::int64_t nr = std::min<std::int64_t>(r + dy, side - 1);
                            const std::int64_t nc = std::min<std::int64_t>(c + dx, side - 1);
                            const auto more = field_.materialsGiven(
                                    (originX + nc) * stride, (originY + nr) * stride,
                                    quantisation_.dequantise(
                                            tile.heightQuantized[static_cast<std::size_t>(nr * side + nc)]),
                                    slope, stride);
                            for (std::size_t m = 0; m < kMaterialCount; ++m)
                                weights.weight[m] += more.weight[m];
                        }
                    for (std::size_t m = 0; m < kMaterialCount; ++m)
                        weights.weight[m] = weights.weight[m] / Fixed::fromInt(5);
                }
                const auto into = static_cast<std::size_t>(row * page.materialWidth + column) *
                                  kMaterialCount;
                for (std::size_t m = 0; m < kMaterialCount; ++m)
                    page.materials[into + m] = static_cast<std::uint8_t>(std::clamp<std::int64_t>(
                            (weights.weight[m] * Fixed::fromInt(255)).roundToInt(), 0, 255));
            }
        }
    }

    // The environment's masks, on the materials' own grid.
    if (environment_ && environment_->writesMasks()) {
        const auto masks = environment_->masks(double(originX * stride * kSampleMetres),
                                               double(originY * stride * kSampleMetres),
                                               double(page.materialMetres), page.materialWidth);
        page.envMasks = masks.channels;
        page.envZones = masks.zones;
    }

    // What share of a sample's own footprint is under water, read off the
    // lattice the page already holds rather than by taking more samples of a
    // field: water is a yes or a no at a point, which is a lie once the
    // points are further apart than the stream between them.
    for (std::int64_t row = 0; row < side; ++row)
        for (std::int64_t column = 0; column < side; ++column) {
            const auto at = static_cast<std::size_t>(row * side + column);
            int wet = 0, seen = 0;
            for (const auto& step : {std::pair<std::int64_t, std::int64_t>{0, 0},
                                     {0, 1}, {0, -1}, {1, 0}, {-1, 0}}) {
                const std::int64_t r = row + step.first, c = column + step.second;
                ++seen;
                if (r < 0 || c < 0 || r >= side || c >= side) continue;
                if (depth[static_cast<std::size_t>(r * side + c)].raw > 0) ++wet;
            }
            int share = wet * 255 / seen;

            // And what the stencil cannot see at all.
            //
            // A brook ten metres across, sampled on a sixty-four metre lattice,
            // misses every one of the five points - so its coverage is nought
            // and it is not drawn. That is a river vanishing as the camera
            // pulls back, and the comment above says why without doing anything
            // about it.
            //
            // The graph knows. Every sample already carries which reach is
            // nearest and how far its bank is, so the width of that reach
            // against the width of a texel is the share of the texel the
            // channel really occupies - a tenth of a texel for that brook,
            // which is a thin line rather than nothing. Held to the texel it
            // touches, so a course cannot smear across ground it does not run
            // through.
            const auto reach = water.riverId[at];
            if (const auto* segment = riverSegmentOf(graph_,reach)) {
                const double bank = water.shoreDecimetres[at] / 10.0;
                const double texel = double(sampleMetres);
                if (bank < texel * 0.5) {
                    const double width = segment->width.toDouble();
                    const double filled = std::clamp(width / texel, 0.0, 1.0);
                    // Eased out over the half texel, so a course does not end
                    // on a texel boundary the way the stencil did.
                    const double near = 1.0 - bank / (texel * 0.5);
                    share = std::max(share, int(filled * near * 255.0));
                }
            }
            water.coverage[at] = static_cast<std::uint8_t>(std::clamp(share, 0, 255));
        }
    if (sampleMetres == 16 && padding >= 1) {
        constexpr int cells = kPageMetres / 16;
        page.featureCells.resize(cells*cells);
        const auto height = [&](std::int64_t i) {
            return quantisation_.dequantise(tile.heightQuantized[i]).toDouble() +
                (page.large.deltaQuantized.empty() ? 0 : page.large.deltaQuantized[i]*0.01);
        };
        for (int y=0;y<cells;++y) for (int x=0;x<cells;++x) {
            const auto i=std::int64_t(y+padding)*side+x+padding;
            std::uint8_t flags=0;
            int wet=0;
            for (const auto d : {std::int64_t(0),std::int64_t(1),side,side+1}) {
                wet += depth[i+d].raw > 0;
                // GraphCarver sets reach only inside its valley influence.
                // Protect the whole floodplain, not just +/-16 m at the bank.
                if (water.riverId[i+d] != kInvalidRiverId) {
                    flags|=1;
                    if (std::abs(int(water.shoreDecimetres[i+d]))<=160) flags|=4;
                }
                const double h=height(i+d);
                if (std::abs(height(i+d-1)-2*h+height(i+d+1))>4.0 ||
                    std::abs(height(i+d-side)-2*h+height(i+d+side))>4.0) flags|=2;
            }
            if (wet>0 && wet<4) flags|=4;
            page.featureCells[y*cells+x]=flags;
        }
    }
    if (sampleMetres > kSampleMetres) page.refinementDepth = std::move(depth);
    return page;
}

TerrainWorkerRuntime::BuildFunction makeBaseTileBuildFunction(
        const generation::WorldMapData& world, const HydrologyGraph& graph,
        HsimQuantisation quantisation) {
    return [&world, &graph, quantisation](
                   const TerrainStreamRequest& request,
                   const TerrainCancellation& cancellation) -> std::optional<TerrainBuildResult> {
        if (!containsProduct(request.key.products, TileProduct::Base)) return std::nullopt;

        // One baker to a worker, kept between tiles. Constructing one resolves
        // the macro map's hydrology, and a HeightField keeps a drainage
        // neighbourhood cache in itself that only stays correct while one
        // thread uses it.
        //
        // Keyed on the graph as well as the world. Keyed on the world alone,
        // a second build function over the same world with a different graph
        // was handed a baker still pointing at a graph that had gone - which
        // is a use-after-free, and it showed up as one.
        thread_local std::unique_ptr<BaseTileBaker> baker;
        thread_local const generation::WorldMapData* builtForWorld = nullptr;
        thread_local const HydrologyGraph* builtForGraph = nullptr;
        if (builtForWorld != &world || builtForGraph != &graph || baker == nullptr) {
            baker = std::make_unique<BaseTileBaker>(world, graph, quantisation);
            builtForWorld = &world;
            builtForGraph = &graph;
        }

        const auto cancelled = [&cancellation] { return cancellation.cancelled(); };
        BaseTile tile = baker->bake(request.key.tile, kSampleMetres, kDefaultPaddingSamples,
                                    cancelled);
        if (!tile.valid()) return std::nullopt;   // abandoned, or asked for nonsense

        TerrainBuildResult result;
        result.key = request.key;
        result.base = std::make_shared<const BaseTile>(std::move(tile));
        return result;
    };
}

} // namespace world::streaming
