#pragma once
// The shape of the ground, as a field rather than as a map.
//
// Height exists at every point of the world. It is defined by a lattice of
// samples in global coordinates and interpolation between them, and the lattice
// spacing is its own number: it is not the chunk size, not the navmesh, not the
// lookup grid and not the coarse world's cell. Anything that wants to know how
// high the ground is somewhere asks for that point, not for a tile.
//
// Every sample is a pure function of its own global lattice coordinate, of the
// coarse world above it and of the seed. Two chunks that share a border do not
// agree on their shared samples - they compute the same numbers, because they
// are asking the same question. That is the seam strategy for everything built
// on this: heights, normals, materials and cliff lines all come out of the
// field, so there is nothing to stitch and nothing to keep in step. A "ghost
// margin" is simply reading samples that lie outside the chunk, which costs
// nothing and needs no bookkeeping.
//
// Two layers, as GDD 4.2 asks: the coarse world - tectonics, erosion, rivers,
// climates, generation::WorldMapData - says what the country is, and the field
// details it. Anything that has to cross many chunks (a mountain system, a river
// basin, a biome) belongs to the coarse layer, which is why none of it is
// generated per chunk.
//
// No floating point: every number here can decide where a person walks.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>

#include "engine/core/fixed.hpp"
#include "engine/core/geometry.hpp"
#include "game/world/edit_layer.hpp"
#include "game/world/coords.hpp"
#include "game/world/macro.hpp"
#include "game/world/soil.hpp"
#include "game/world/terrain_streaming/graph_carve.hpp"

namespace generation { struct WorldMapData; }
namespace engine::environment { class FeatureLayer; }

namespace world {

// Shared by stationary dunes, worker wind shadows and the render scene.
// If prevailing wind changes, rebuild the terrain's cached exposure too.
inline const core::Fixed kPrevailingWindX = core::Fixed::ratio(-857, 1000);
inline const core::Fixed kPrevailingWindY = core::Fixed::ratio(516, 1000);

// Metres between height samples. Fine enough that a four-metre bank reads as a
// bank, coarse enough that a chunk is seventeen samples on a side. It divides
// the chunk exactly, and that is the only relation between the two numbers.
inline constexpr std::int32_t kSampleMetres = 4;
static_assert(kChunkMetres % kSampleMetres == 0,
              "a chunk has to be a whole number of samples across, or its border "
              "samples would not be samples at all");
inline constexpr std::int32_t kSamplesPerChunk = kChunkMetres / kSampleMetres;

// How far apart the coarse layer is evaluated, in metres. Eight samples of the
// height lattice: fine enough that nothing the coarse layer does is lost -
// nothing it does happens in eight metres - and coarse enough that it is worked
// out once for every sixty-four samples.
inline constexpr std::int64_t kCoarseLatticeMetres = 32;

// Conservative envelope for alpine detail plus the bounded hybrid DEM residual.
// Shared with quantisation and visibility bounds: neither may clip a new peak.
inline constexpr std::int64_t kAlpineHeightMarginMetres = 1024;

// How far past the last cell of the map the country takes to become open sea.
//
// The world is finite, and a finite world has to end in something. Two
// kilometres of shelving water: near enough that the edge reads as a coast
// rather than as a wall, far enough that the last mile of country is still
// country and not already surf.
inline constexpr std::int32_t kBeyondTheMapMetres = 2000;

// What is worth working out at a given spacing between samples (D135).
//
// A level of detail that only draws fewer triangles is half a level of detail.
// The other half is this: at a kilometre to the sample, a seventy-metre wave is
// not a hill, it is one number picked out of a hundred and drawn as if it stood
// for all of them; a ten-metre brook is a hundredth of the gap between two
// samples; and the coarse map, evaluated on a thirty-two metre lattice, is
// interpolated between four corners that no other sample will ever share. All
// three cost real time and none of them can be seen. Measured over one patch,
// they were four fifths of what a coarse sample cost - which is why a patch of
// a continent cost five times a patch of a hillside.
//
// So the field is asked how coarsely it is being read, and leaves out what
// cannot survive the reading. That makes a sample a function of its coordinate
// *and* of the spacing, which two levels do not share - and it is the morph
// between levels (terrain_mesh.hpp) that makes that invisible rather than a
// jump. Below thirty-two metres nothing is left out at all, so the ground the
// simulation walks on, and everything the player can get close to, is untouched.
struct Detail {
    std::int64_t lattice;      // metres between evaluations of the coarse map
    std::int64_t finestWave;   // the shortest wave still worth adding, or 0 for all
    std::int64_t narrowest;    // the narrowest valley still worth cutting
};

inline Detail detailFor(std::int64_t strideMetres) {
    if (strideMetres <= kCoarseLatticeMetres) return {kCoarseLatticeMetres, 0, 0};
    // A wave shorter than two samples is not drawn by them, it is aliased by
    // them; a valley narrower than a sample cannot be cut by one. Both of those
    // *remove* things a coarse level cannot carry, which is what a level of
    // detail is allowed to do.
    //
    // The lattice is not one of them. It is the spacing the coarse map itself
    // is read at - the base every level shares - and moving it with the stride
    // meant a coarse level did not simplify the ground, it interpolated a
    // different ground: measured over 40000 points, the same place stood 1.25 m
    // apart between four metres to the sample and sixty-four, and 6.57 m apart
    // at worst. That is what makes the terrain reshape itself under the camera
    // instead of gaining detail, and terrain_lod §3 is explicit that the base
    // must be stable and only the residual may change. So it stays put.
    return {kCoarseLatticeMetres, 2 * strideMetres, strideMetres};
}

inline bool operator==(const Detail& a, const Detail& b) {
    return a.lattice == b.lattice && a.finestWave == b.finestWave && a.narrowest == b.narrowest;
}

// What the ground is made of, at a point. Weights rather than a choice: a
// hillside is not grass or rock, it is mostly grass with rock coming through,
// and how much of each is what the renderer blends and what a walker feels
// underfoot. They sum to one.
enum class Material : std::uint8_t { Grass, Dirt, Sand, Rock, Marsh, Snow, Count };
inline constexpr std::size_t kMaterialCount = static_cast<std::size_t>(Material::Count);

struct MaterialWeights {
    std::array<core::Fixed, kMaterialCount> weight{};

    core::Fixed of(Material m) const { return weight[static_cast<std::size_t>(m)]; }
    void add(Material m, core::Fixed w) { weight[static_cast<std::size_t>(m)] += w; }
    // Scales the weights so they sum to one. Everything downstream - blending,
    // lighting, walking cost - assumes that, and an unnormalised set is the kind
    // of bug that shows up as a chunk being slightly the wrong colour.
    void normalise();
    Material strongest() const;
};

// The lie of the ground at a point, as a unit vector with z up. Fixed point, so
// it is not exactly unit length; it is normalised to within a few parts in a
// billion, which is closer than anything asks for.
struct Normal {
    core::Fixed x, y, z;
};

// How a body gets over this ground.
//
// Not a flag. Between the ground anybody can cross and the ground nobody can
// there is a great deal of world, and a mountain country is made of it: a
// scramble is slow, a climb is slower and dangerous, and both beat walking
// round a range. What each costs is the mover's business; what the ground is,
// is this.
enum class Travel : std::uint8_t {
    Walk,        // level enough to carry a load over
    Scramble,    // steep: hands out of the load, slow going
    Climb,       // a face: slow, and a fall from it is a fall
    // Water is not a wall, and it was being treated as one. How deep it is
    // decides who crosses it and what it costs them, which is the difference
    // between a stream network and a set of fences: at a quarter of the
    // watercourses on this world holding water, "no water is passable" cut the
    // country into pieces - measured, three to six of them per square mile,
    // the largest holding a third of the walkable ground.
    Ford,        // shallow: everything crosses, nothing crosses quickly
    Swim,        // out of its depth: a body can, a cart or a load cannot
    None,        // too deep for anything, or rock that does not give
};

// How deep water may be and still be crossed.
//
// Knee to thigh, and a cart still goes through it, which is what a ford is for.
inline core::Fixed fordableDepth() { return core::Fixed::ratio(3, 5); }    // 0.6 m
// Out of its depth: a body swims it and arrives wet, a load does not arrive.
inline core::Fixed swimmableDepth() { return core::Fixed::ratio(5, 2); }   // 2.5 m

const char* travelName(Travel t);

// A sample of everything the field knows at one point.
struct GroundSample {
    core::Fixed height;        // metres above sea level
    Normal normal;
    core::Fixed slope;         // rise over run: 0 flat, 1 is forty-five degrees
    MaterialWeights materials;
    bool water = false;        // below the water line: sea, lake or river bed
    Travel travel = Travel::Walk;
    SoilSample soil;           // substrate, independent of material cover/weather
};

class HeightField {
public:
    // Bounded memoization for a worker's batch of nearby queries. Values keep
    // their exact lattice coordinates/stride and are invalidated by ground
    // edits or a terrain stage change. The field remains thread-local.
    class QueryCache {
    public:
        explicit QueryCache(HeightField& field);
        ~QueryCache();
        QueryCache(const QueryCache&) = delete;
        QueryCache& operator=(const QueryCache&) = delete;
    private:
        friend class HeightField;
        struct State;
        HeightField& field_;
        QueryCache* previous_;
        std::unique_ptr<State> state_;
        static thread_local QueryCache* active_;
    };
    // The coarse world may be null, in which case the field invents a country of
    // its own from the seed. The tests use that; the game always has a world.
    //
    // The macro layer - the drainage cut into channels and valleys - is built
    // here from the same coarse map, because everything the field says about
    // height depends on it and nothing may be able to see one without the
    // other.
    HeightField(const generation::WorldMapData* coarse, std::uint64_t seed);
    // The same, with the macro layer's whole-map answer already worked out
    // for this map (MacroWorld::resolved): nothing is read of the map to make it.
    HeightField(const generation::WorldMapData* coarse, std::uint64_t seed,
                std::shared_ptr<const MacroWorld::Resolved> resolved);

    const MacroWorld& macro() const { return macro_; }

    // The lattice. Coordinates are global sample indices, so sample (0,0) is at
    // world (0,0) and sample (-1,-1) is four metres north-west of it. Cheap
    // enough to recompute rather than cache - caching is the lookup grid's job,
    // and a cache here would be one more thing that can disagree with itself
    // across a border.
    // `strideMetres` is how far apart the samples being asked for are, which
    // decides how much of the world is worth working out at all (see Detail
    // above). The default is the lattice itself: the whole truth, which is what
    // the simulation and everything close to the eye get.
    core::Fixed sampleHeight(std::int64_t sx, std::int64_t sy,
                             std::int64_t strideMetres = kSampleMetres) const;
    // The two pieces the ground is made of before the water cuts it: what the
    // coarse map says the country stands at, and how far the detail layer has
    // moved it. Kept apart because the macro layer treats them differently -
    // the country is the level the water is measured against, the detail is a
    // landscape that has to give way near a channel.
    struct Pieces { core::Fixed country, moved, lakeLevel, lakeDeep; };

    // The two bands of visual detail that H_sim does not carry.
    //
    // The detail layer stops at seventy metres, which is the shortest octave
    // the generator has. Below that the world is exactly smooth: between one
    // four-metre sample and the next there is nothing at all, and a hillside
    // seen from a few metres up is a curved plane. That is not a level of
    // detail failing to arrive, it is detail that was never generated.
    //
    // These are the missing octaves, kept out of H_sim rather than added to
    // it. H_sim is what the navmesh, the buildability and the simulation read,
    // and the whole point of a residual is that adding it cannot move a river,
    // a cliff or a travel class: it is stored beside the page's heights, never
    // folded into them, so the freeze holds by construction instead of by a
    // check that has to be believed.
    //
    // `large` is sixty-four and thirty-two metres, which a page can carry down
    // to sixteen metres to the sample; `medium` is sixteen and eight, which
    // needs the four-metre page. Anything shorter belongs to the shader.
    struct Residual { core::Fixed large, medium; };
    [[nodiscard]] Residual residualAt(core::WorldPos p) const;
    // Shallow pools of the bogs (moor marsh, peat plain, river fen): how many
    // metres the ground is lowered at this point, 0 where there is no pool. Pools
    // are a pattern of noise a few tens of metres across, fading in over the
    // marsh categories' borders; the baker makes the water that stands in them.
    // Only a world with a category map has any.
    [[nodiscard]] bool hasBogPools() const;
    [[nodiscard]] core::Fixed bogPoolDepth(core::Fixed x, core::Fixed y) const;

    Pieces piecesAt(core::Fixed x, core::Fixed y,
                    std::int64_t strideMetres = kSampleMetres) const;
    MaterialWeights sampleMaterials(std::int64_t sx, std::int64_t sy) const;
    // The same, told the height and the slope rather than working them out
    // again. A mesh builder already knows both - it has just sampled the whole
    // lattice - and re-deriving them costs eight more height samples a vertex,
    // each of which is a walk through the coarse map and the drainage.
    // `stride` is how many lattice steps apart the samples being asked for are,
    // which a coarse mesh knows and a fine one leaves at one. The mottling in
    // the ground is noise a few tens of metres across, and noise read at steps
    // wider than itself is not mottling, it is a scatter of squares the size of
    // the samples - which is what the far view of grassland used to be. Each
    // scale is faded out before its samples get that far apart, so a coarse
    // mesh shows the broad of the same field rather than an aliased copy of it.
    MaterialWeights materialsGiven(std::int64_t sx, std::int64_t sy, core::Fixed height,
                                   core::Fixed slope, std::int64_t stride = 1) const;
    // Presentation communities: steppe, boreal, temperate, tropical. Sampled
    // once per mesh vertex, never per frame/blade. Sum may be < 1 on barren land.
    std::array<core::Fixed, 4> foliageAt(core::WorldPos p) const;
    struct SurfaceClimate {
        std::array<core::Fixed, 4> foliage{};
        core::Fixed desert; // Actual Desert climate coverage, NOT any sandy material.
        // Temperature/fertility/moisture in 0..1, wind velocity in units of
        // the reference breeze, drainage in 0..1. Static, independent of LOD.
        std::array<core::Fixed, 6> environment{};
        core::Fixed woodland = core::kZero; // Tree habitat, NOT the plant colour palette.
    };
    SurfaceClimate surfaceClimateAt(core::WorldPos p) const;
    // Regional vector attenuated by upwind terrain. A fixed 64 m field,
    // independent of render LOD/weather; no buildings or fluid simulation.
    std::array<core::Fixed, 2> windAt(core::WorldPos p) const;
    // Three upwind terrain probes; called only for near sandy mesh vertices.
    // Terrain obstacles only: separate buildings/props need an obstruction mask.
    core::Fixed sandWindExposureAt(core::WorldPos p, core::Fixed height) const;
    Normal sampleNormal(std::int64_t sx, std::int64_t sy) const;
    // The same, measured over a stride of lattice steps rather than one. What a
    // coarse mesh wants: a normal taken from its own neighbours rather than
    // from detail it is not drawing, so a distant hillside is lit by its shape
    // and not by the roughness it is too far away to show.
    Normal normalAcross(std::int64_t sx, std::int64_t sy, std::int64_t stride) const;
    core::Fixed sampleSlope(std::int64_t sx, std::int64_t sy) const;
    // Broken ground: the ground at this sample drops away faster than a body
    // can hold on to, measured against its nearest neighbours rather than
    // averaged over them. Averaging is what loses a four-metre bank between two
    // flat fields - it comes out as a gentle slope over eight metres - and the
    // bank is exactly the thing a cliff is.
    bool sampleBroken(std::int64_t sx, std::int64_t sy) const;
    // The same question with one neighbour left out. What the cliff cutter asks
    // about the ground at the foot of a drop: the drop itself is what it is
    // standing under, so counting it would make every bank its own overhang and
    // no cliff would ever be found.
    bool sampleBrokenExcept(std::int64_t sx, std::int64_t sy, std::int64_t ignoreX,
                            std::int64_t ignoreY) const;

    // Anywhere at all, by interpolation between the four samples around it.
    core::Fixed heightAt(core::WorldPos p) const;
    Normal normalAt(core::WorldPos p) const;
    core::Fixed slopeAt(core::WorldPos p) const;
    MaterialWeights materialsAt(core::WorldPos p) const;
    SoilSample soilAt(core::WorldPos p) const;
    // Use full-resolution geometry here, not render-LOD geometry. groundAt
    // already has both numbers, so it avoids resampling height and slope.
    SoilSample soilGiven(core::WorldPos p, core::Fixed height, core::Fixed slope) const;
    GroundSample groundAt(core::WorldPos p) const;

    // Where the water is. The coarse world's seas and rivers, as a height: land
    // below this is under water.
    core::Fixed waterLevelAt(core::WorldPos p) const;

    // Where the water stands here, and how much of a patch this size is under
    // it.
    //
    // A vertex is one point and water is a yes or a no at it, which is fine
    // while the points are four metres apart and a lie once they are sixty-four.
    // At that spacing a vertex that lands in a river floods everything out to
    // the next vertex, so pulling the camera back turned a valley with a stream
    // in it into a lake, and a coastline into open sea. The geometry was not
    // wrong; the sampling was.
    //
    // `cover` is the share of a square `footprint` metres across that is water,
    // worked out from how far the point is from the bank rather than by taking
    // more samples: the channel already knows its own width, so this costs
    // nothing over asking for the level alone.
    enum class WaterKind : std::uint8_t { Ocean, River, Lake };
    struct WaterHere {
        core::Fixed level;   // where the surface stands, when there is one
        core::Fixed cover;   // nought to one
        WaterKind kind=WaterKind::Ocean;
        core::Fixed flowX, flowY;
    };
    //
    // `slope` is how fast the ground is falling here. It is handed in rather
    // than worked out because whoever is asking has just built a mesh and has
    // the vertex normal in its hand, and deriving it again is four more height
    // samples per vertex - which, measured, was most of the cost of building a
    // patch.
    WaterHere waterOver(core::WorldPos p, core::Fixed footprint, core::Fixed slope) const;
    bool underWater(core::WorldPos p) const;

    // How this ground is crossed, and the shorthand for "at all". The navmesh
    // takes these as its raw material and then subtracts what has been built;
    // both are asked of geometry, never of a tile.
    Travel travelAt(core::WorldPos p) const;
    bool walkable(core::WorldPos p) const;
    bool passable(core::WorldPos p) const { return travelAt(p) != Travel::None; }

    std::uint64_t seed() const { return seed_; }
    const generation::WorldMapData* coarse() const { return coarse_; }

    // World metres of one lattice step, as a Fixed, for interpolation.
    static core::Fixed sampleStep() { return core::Fixed::fromInt(kSampleMetres); }

    // The drainage network this field's water is carved from, which is the one
    // authority on where a river is. Public because a tool that checks the
    // rivers has to ask the model that draws them: checking MacroWorld's
    // description of a reach instead reports faults in the difference between
    // two models rather than in either of them.
    //
    // Taken on the first question about water, not in the constructor. The
    // graph is built from the country this field describes - the profile of a
    // reach is read off the ground it runs through - so a field made while a
    // graph is being built must be able to answer about the ground without
    // asking for the graph that is not there yet.
    [[nodiscard]] const streaming::HydrologyGraph* graph() const;

    // What a person changed about this ground, or nothing.
    //
    // Set once by whoever owns the world; every field built from that world
    // reads the same layer, so a page baked on a worker and a query answered on
    // the main thread agree about where the valley was dug. Null means an
    // unedited world, which is the case that must cost nothing - see
    // edit_layer.hpp for what "nothing" is.
    void edits(const EditLayer* layer) { edits_ = layer; }
    [[nodiscard]] const EditLayer* edits() const { return edits_; }

    // The procedural environment's features (engine/environment/feature_layer.hpp):
    // gullies, cliff bands, outcrops. Added to the detail, so the water still
    // carves through them and a channel's banks still give way. Null, or a
    // layer that moves no ground, costs one branch.
    void features(const engine::environment::FeatureLayer* layer) { features_ = layer; }
    [[nodiscard]] const engine::environment::FeatureLayer* features() const { return features_; }

private:
    QueryCache::State* cachedQueries() const;
    struct CoarseLookup {
        std::int64_t cx, cy;
        core::Fixed tx, ty;
    };
    // Shared geographical warp for terrain, climate and soil composition.
    CoarseLookup coarseLookup(core::WorldPos p) const;
    // The coarse world's own numbers at a world point, interpolated between its
    // cells so the field does not step at every cell boundary.
    struct Coarse {
        core::Fixed elevation;    // metres
        std::int32_t moisture;    // 0..255
        std::int32_t temperature;
        // How much of the country around this point is sea, and how much is
        // river, as fractions rather than as flags: a flag is a cell boundary,
        // and a cell boundary in the water's edge is a hundred and eighty
        // metres of straight coastline.
        core::Fixed seaShare;
        core::Fixed riverShare;
        core::Fixed relief;       // how broken the country is around here, metres
        // Where a filled basin's water stands, in metres, and how much of the
        // country here is under it. The elevation above is the basin's floor -
        // the fill is taken back out of it - so the two together are a lake.
        core::Fixed lakeLevel;
        core::Fixed lakeDeep;     // metres of fill, blended: nought off a basin
    };
    // What the coarse world says at a point, and what it says at the corners of
    // its own lattice.
    //
    // The coarse layer changes over hundreds of metres - it is sixteen cells
    // interpolated by a curve, with a domain warp on top - so working it out
    // afresh for every four-metre sample is asking a slow question a hundred
    // times and getting the same answer. It is evaluated on a lattice of its
    // own instead, in global coordinates so that everybody agrees on it, and
    // interpolated between. That is still a pure function of position, which is
    // what the seams rest on.
    Coarse coarseAt(core::Fixed worldX, core::Fixed worldY, std::int64_t lattice) const;
    Coarse coarseExact(core::Fixed worldX, core::Fixed worldY) const;
    const Coarse& coarseCorner(std::int64_t lx, std::int64_t ly, std::int64_t lattice) const;

    // The water, from the graph the pages are baked from.
    //
    // This is the one answer. MacroWorld used to cut the valleys and place the
    // water here, and GraphCarver used to do it for a page, and they were two
    // models of one river: measured across two shores, the graph ended a lake
    // where its floor rose to meet the surface - nought point one of a metre
    // out - and this file ended the same lakes nine metres up in the air, and
    // on a third shore did not end one at all. Two answers about where the
    // water is are not a tolerance, they are a bug with a rendering of its own.
    //
    // The window is kept because a carver is built from the graph's page index
    // over an area, and building one is a microsecond while a query is a walk
    // over what it found. Whoever asks walks - a mesh, a probe, a cursor - so
    // the next question is nearly always inside the last answer's area.
    [[nodiscard]] streaming::CarvedSample carved(core::WorldPos at, core::Fixed country,
                                                 core::Fixed detail) const;
    const EditLayer* edits_ = nullptr;
    const engine::environment::FeatureLayer* features_ = nullptr;
    // piecesAt before the features: what the features themselves stand on.
    Pieces piecesWithout(core::Fixed x, core::Fixed y, std::int64_t strideMetres) const;
    const generation::WorldMapData* coarse_ = nullptr;
    std::uint64_t seed_ = 0;
    MacroWorld macro_;
    mutable std::shared_ptr<const streaming::HydrologyGraph> hydrology_;
    mutable bool askedForGraph_ = false;
    mutable std::optional<streaming::GraphCarver> carver_;
    mutable core::WorldRect carverArea_{};

    // What the coarse map says around one of its cells, worked out once.
    //
    // Every sample interpolates a four-by-four block of cells, and the relief -
    // how broken the country is - is itself the spread of the nine cells around
    // each of those sixteen. That is a hundred and forty-four cell reads for
    // one height, and a patch of ground asks for hundreds of heights that all
    // fall in the same cell.
    //
    // So the block is remembered, keyed on which cell the point is in. A patch
    // walks its vertices in order, so the memo is hit almost every time. It
    // makes the field no longer thread-safe, which is why a thread that builds
    // ground gets a HeightField of its own - the class is a pointer, a seed and
    // this, and copying it costs nothing.
    struct CellBlock {
        bool valid = false;
        std::int64_t cx = 0, cy = 0;
        core::Fixed elevation[4][4];
        core::Fixed lakeLevel[4][4];
        core::Fixed lakeDeep[4][4];
        core::Fixed moisture[4][4];
        core::Fixed temperature[4][4];
        core::Fixed sea[4][4];
        core::Fixed river[4][4];
        core::Fixed relief[4][4];
    };
    mutable CellBlock block_;

    // Corners of the coarse lattice, kept because a patch of ground walks its
    // samples in order and asks for the same ones over and over.
    //
    // Big enough to hold the whole span a patch reaches across, and addressed
    // by a hash of the corner's coordinate rather than searched: what a coarse
    // patch wants is not the last few corners but a band of them two rows deep,
    // and a ring of sixteen searched linearly held neither.
    struct Corner {
        bool valid = false;
        std::int64_t lx = 0, ly = 0, lattice = 0;
        Coarse value;
    };
    static constexpr std::size_t kCornersKept = 512;   // a power of two: masked, not divided
    mutable Corner corners_[kCornersKept];
    struct WindCorner {
        bool valid = false;
        std::int64_t x = 0, y = 0;
        std::array<core::Fixed, 2> velocity{};
    };
    mutable std::array<WindCorner, 512> windCorners_{};
};

// The three angles the ground is read at (D117).
//
// Level enough to walk with a load; steep enough that it is a scramble; a face
// that has to be climbed. Past the last of them the rock does not give at all.
// A cliff - the thing that gets its own geometry and its own drawing - is the
// third: the line where the ground stops being ground.
inline core::Fixed walkableSlope() { return core::Fixed::ratio(1, 2); }     // 27 degrees
inline core::Fixed scrambleSlope() { return core::Fixed::ratio(9, 10); }    // 42 degrees
inline core::Fixed cliffSlope() { return core::Fixed::ratio(3, 2); }        // 56 degrees
inline core::Fixed unclimbableSlope() { return core::Fixed::fromInt(3); }   // 72 degrees

} // namespace world
