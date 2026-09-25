#pragma once
// The world's large structures, at play scale.
//
// The coarse map knows where the water runs - it computed the drainage with
// flow accumulation and left every cell pointing downhill at its neighbour -
// but it knows it as flags on cells half a kilometre across. That is a river in
// the sense that a map has a blue line on it, and it is why the field's first
// attempt flooded two hundred metres of valley floor: a cell was either river
// or not, so the water was as wide as the cell.
//
// This turns the flags into geometry. The drainage becomes a network of
// channels in world metres, each with a surface height that falls downstream, a
// width and a depth taken from how much water it carries, and a valley hung
// either side of it. Then the field asks it, at every sample, what water is
// near - and cuts the ground down to the valley floor where there is any.
//
// What that buys is the difference between a heightfield and a landscape:
//
//   channels   a brook is ten metres across and a trunk river a hundred and
//              fifty, instead of everything being the width of a map cell;
//   valleys    the ground falls towards the water it drains into, which is what
//              makes country read as country rather than as noise;
//   passes     a valley cut from either side of a range meets at the col
//              between them, and the wall has a way through it. Nobody places a
//              pass: it is what is left when two rivers have finished cutting.
//
// This is the coarse half of the two-layer scheme (GDD 4.2, D113): it is built
// once from the world map, it is the same for everybody, and it is asked about
// by global coordinate - so a chunk generated on its own gets the same valley
// as a chunk generated with its neighbours.

#include <cstdint>
#include <optional>
#include <vector>

#include "engine/core/geometry.hpp"
#include "game/world/coords.hpp"

namespace generation { struct WorldMapData; struct WorldCell; }

namespace world {

// A run of water between two points of the drainage network, with what the
// ground around it should look like.
struct Channel {
    core::WorldPos from, to;
    // The reaches either side of this one, as the outer control points of the
    // curve the water actually follows. Without them a river is a chain of
    // straight segments between cell centres, pinched at every node and turning
    // in eight directions - which is the coarse map's lattice, drawn in water.
    core::WorldPos before, after;
    core::Fixed surfaceFrom, surfaceTo;   // water level at each end, metres
    core::Fixed halfWidth;                // half the wet channel, where it begins
    core::Fixed depth;                    // how far the bed sits below the surface
    core::Fixed valleyReach;              // how far out the ground is drawn down
    // The same three where the reach ends, taken from the cell it flows into.
    core::Fixed halfWidthEnd;
    core::Fixed depthEnd;
    core::Fixed valleyReachEnd;
    std::uint8_t size = 0;                // the coarse map's own doublings of flow
    bool wet = false;                     // whether water still stands in it
    // Whether the water starts here rather than arriving from somewhere. A
    // reach that nothing drains into used to begin at its cell's full width and
    // depth, and since distance to a course is radial past its end, that drew
    // every stream's source as a round pit the width of its valley sitting on
    // an open hillside. A headwater grows out of nothing along its first reach
    // instead.
    bool head = false;
    // How this reach wanders between its two ends, and how far it may. A
    // channel that runs straight from one cell centre to the next draws the
    // map's own lattice across the country.
    std::uint64_t wander = 0;
    core::Fixed wanderReach;
};

// What the water does to the ground at a point.
struct WaterNearby {
    bool found = false;       // any channel near enough to shape the ground
    core::Fixed floor;        // the ground, cut down to the valleys around it
    bool wet = false;         // inside a channel's own width: this is water
    core::Fixed surface;      // and its level, when it is
    core::Fixed distance;     // to the middle of the nearest channel
    // Signed distance to its bank: negative in the channel, zero at the water's
    // edge, positive across the flood plain. Meaningful when found is true.
    core::Fixed bankDistance;
    // Whether the nearest channel carries water at all, as against being a dry
    // gully. Meaningful whether or not the point is inside it, which `wet` is
    // not - and the difference matters, because how much of a footprint is under
    // water is worked out from the distance to a bank, and a dry gully has no
    // bank to be near.
    bool nearestWet = false;
    // The highest water standing here, over every channel that reaches the
    // point rather than only the nearest of them.
    //
    // `surface` is the nearest wet channel's level, and answering with it alone
    // is what made the water stop being one surface. A point between two
    // reaches took whichever was closer and ignored the other, so two crossing
    // channels each kept their own level with a step where they met; a dry
    // gully between two wet ones answered "no water" and rode as a ridge across
    // the flood; and a valley cut below the waterline stayed dry because its
    // nearest channel was the narrow one at the bottom rather than the wide one
    // that had drowned it.
    //
    // Water finds its own level, and the level is the highest one that reaches.
    // Below it the ground is under water whatever the nearest channel says.
    core::Fixed highest;
    bool anyWet = false;      // whether `highest` means anything
    core::Fixed flowX, flowY; // unit tangent of the wet channel, downstream
    core::Fixed channelWidth; // full width of that same wet channel, metres
};

class MacroWorld {
public:
    // Attached to the coarse map rather than built from it.
    //
    // The drainage is every cell that sheds water downhill, which on a
    // continent is a million and a half lines - too many to hold, and pointless
    // to hold, because each one is four numbers away from the cell it belongs
    // to. So nothing is stored: a query reads the cells around the point and
    // works out their channels there and then. That keeps it a pure function of
    // world coordinates, which is what the whole architecture rests on, and it
    // costs no memory at all however much of the world is asked about.
    // Attaching also decides what counts as a stream here, which is a question
    // about the whole map and so cannot be answered per query. See attach().
    void attach(const generation::WorldMapData* coarse);
    bool ready() const { return coarse_ != nullptr; }

    // The drainage, in doublings of flow, at or above which a channel holds
    // water the year round. Below it the valley is there and the water is not.
    //
    // The map-wide reference, and then what it is in a particular cell: the same
    // catchment is a stream in wet country and a dry wadi in a desert, and the
    // difference between those two is most of what a drainage network looks
    // like. Measured on seed 11, moisture runs the full nought-to-255 range with
    // about a sixth of the land in each band of it, and the wettest sixth
    // carries a quarter of all the channels on the map.
    std::uint8_t streamFlow() const { return streamFlow_; }
    std::uint8_t streamFlowIn(const generation::WorldCell& cell) const;

    // The largest catchment anywhere on this map, in the same doublings. The
    // trunk of the whole drainage, and the other end of the scale every other
    // channel is sized against - see halfWidthFor.
    std::uint8_t largestFlow() const { return largestFlow_; }

    // Whether water stands in this cell's course the year round, as attach()
    // resolved it over the whole map: the coarse map's own blue lines, plus
    // every catchment that clears the local threshold, plus everything
    // downstream of those.
    //
    // Public because it is the map's answer, not a query's. Anything that has
    // to agree with the water the player sees - the hydrology graph extracted
    // for streaming, a lint pass, a minimap - has to read this rather than
    // re-derive it from WorldCell::river, which is a threshold that scales
    // with the map and on a continent marks only the trunks.
    //
    // Note it is the *course* leaving the cell: a sea cell a river arrives at
    // is not itself a wet course, and callers that mean "water here" have to
    // exclude the sea themselves.
    bool wetAt(core::TilePos cell) const;

    // The level the water stands at in this cell, in metres, as attach()
    // resolved it in drainage order over the whole map: a node has one head
    // shared by every tributary and by the reach leaving it, and a head is
    // never lower than the one below it.
    //
    // Public for the same reason wetAt is. A water surface that each query
    // decides for itself is how one lake ended up with two levels; anything
    // that has to agree with the water the player sees reads this.
    core::Fixed surfaceAt(core::TilePos cell) const;

    // The channel one cell sheds into, or nothing when it sheds nowhere.
    std::optional<Channel> channelOf(core::TilePos cell) const;

    // A point on that channel's course, at a fraction along it. Public because
    // anything that wants to follow a river - a tool looking for artefacts, a
    // renderer drawing a bank - has to follow the curve the water actually
    // takes rather than the line between the cells it joins.
    static core::WorldPos pointOn(const Channel& channel, core::Fixed along);

    // How wide and how deep the water is at that fraction, which is not
    // constant along a reach: a river grows downstream, and holding it constant
    // within a cell and stepping at the boundary puts a crease across every
    // valley in the world at exactly the spacing of the coarse map.
    static core::Fixed halfWidthAt(const Channel& channel, core::Fixed along);
    static core::Fixed depthAt(const Channel& channel, core::Fixed along);
    static core::Fixed reachAt(const Channel& channel, core::Fixed along);

    // What the water leaves the ground at, given what the ground would be
    // without it. Pure: the same answer wherever it is asked from.
    //
    // Every channel within reach has its say, not merely the closest one. The
    // closest one alone is a Voronoi diagram: two channels running at different
    // heights cut their valleys to different depths, and where their territories
    // meet the ground steps from one to the other. The country came out as a
    // tiling of flat-bottomed cells with walls between them. Taking the lowest
    // floor of all of them is a minimum of continuous functions, which is
    // continuous - valleys that meet simply join.
    //
    // The ground it is cutting into is given in two pieces - what the coarse
    // map says the country stands at, and how far the detail layer has moved it
    // - because the second is not welcome everywhere. A river's surface is a
    // level, and the detail layer is a landscape of its own tens of metres
    // deep: where the two meet without arbitration the water fills whatever
    // hollow the noise happened to put beside the channel, and a trunk river
    // ends up with forty-seven-metre pools along its bank that no bank leads
    // down to. So the detail is faded out towards the channel, which is also
    // what a flood plain is.
    //
    // `narrowest` is the narrowest valley the caller can still draw, in metres:
    // the spacing it is sampling at. Nought asks for all of them, which is what
    // the simulation and the close view get. Wider, and the brooks - which at
    // that spacing are a hundredth of the gap between two samples - are left
    // uncut, and past the size of a map cell the drainage is dropped whole,
    // before the neighbourhood around the point is even gathered.
    WaterNearby carve(core::WorldPos p, core::Fixed country, core::Fixed detail,
                      std::int64_t narrowest = 0) const;

private:
    // How far out the cells around a point have to be read. A valley may reach
    // further than a cell is wide, so its channel can be two cells away and
    // still shape the ground here.
    // 800 m valley + one 540 m downstream segment + spline/meander excursion.
    // A two-cell window loses incoming courses at some cell boundaries.
    static constexpr std::int32_t kCellsAround = 3;

    // The channels around one cell, with their courses already walked.
    //
    // Asking the drainage where the water is was ninety-three per cent of the
    // cost of the whole terrain - the coarse map and every octave of detail
    // noise together came to 0.29 microseconds a sample, and this came to 6.39.
    // All of it went on rebuilding the same twenty-five channels and walking the
    // same two hundred and twenty-five points of curve, for every sample, twice
    // over: once for the ground and once for the water.
    //
    // None of that depends on the point being asked about, only on which cell it
    // falls in, and samples are four metres apart while a cell is five hundred
    // and forty. So the whole neighbourhood is worked out when the cell changes
    // and reused until it changes again, which on a patch of ground is once.
    struct Course {
        Channel channel;
        bool exists = false;
        core::WorldPos points[33];  // same polyline returned by pointOn
        // What the early rejection needs, so that a channel too far away to
        // matter costs four comparisons: the box the curve actually occupies,
        // and how far outside it this channel can still shape the ground.
        //
        // Against the box rather than against the midpoint. A midpoint has to
        // allow a whole cell for the length of the reach, in both directions,
        // and a reach is long one way and thin the other - so across the course
        // that cell was pure slack. It did not matter while valleys were
        // clamped to seventy metres; once they reach as far as their relief
        // needs, the slack stopped rejecting anything at all and every sample
        // walked all twenty-five courses of its neighbourhood.
        core::Fixed lowX, lowY, highX, highY, pad;
        // Centre of that box, for measuring how far the next watercourse runs.
        core::Fixed midX, midY;
        // How wide the ground this channel shapes is, across the water. What a
        // coarse level compares against its own spacing: a valley narrower than
        // the gap between two samples cannot be cut by them.
        core::Fixed span;
    };
    static constexpr int kCurveSteps = 32;
    static constexpr int kAround = 2 * kCellsAround + 1;
    struct Neighbourhood {
        bool valid = false;
        std::int64_t cellX = 0, cellY = 0;
        // Which channels were worth gathering: a neighbourhood put together for
        // a coarse level has left the brooks out of it, and handing that to a
        // fine one would be a river missing from the ground the player walks on.
        std::int64_t narrowest = -1;
        Course courses[kAround * kAround];
    };
    // Mutable because it is a cache and nothing else: every answer with it is
    // the answer without it. Each worker thread has its own HeightField and so
    // its own MacroWorld, which is what makes one entry enough and locking
    // unnecessary.
    mutable Neighbourhood block_;
    // Nothing is a stream until a map is attached and the question is answered.
    std::uint8_t streamFlow_ = 255;
    // And nothing has a size until the map says what the biggest river on it is.
    std::uint8_t largestFlow_ = 0;

    // Resolved once in drainage order, independent of the query window/LOD.
    // A node has ONE head shared by every tributary and the outgoing reach.
    // Wet runoff is inherited downstream even when the local climate is dry.
    std::vector<core::Fixed> surface_;
    std::vector<std::uint8_t> wet_;

    // How wide the water is and how far out the ground is drawn down to it,
    // both as a share of this map's own range of flows rather than as absolute
    // doublings. Members, not free functions, because the answer depends on the
    // map: see the note over the definitions.
    core::Fixed halfWidthFor(std::uint8_t flowLog) const;
    core::Fixed reachFor(std::uint8_t flowLog) const;

    const Neighbourhood& around(std::int64_t cellX, std::int64_t cellY,
                                std::int64_t narrowest) const;

    const generation::WorldMapData* coarse_ = nullptr;
};

} // namespace world
