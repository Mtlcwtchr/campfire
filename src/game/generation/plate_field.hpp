#pragma once
// Plates as a function of where you are, not as a map of the world.
//
// The old model is not wrong about tectonics - jittered seeds, a warped Voronoi
// so margins are torn rather than straight, drift vectors, continental against
// oceanic crust, convergence and shear worked out per boundary. What is wrong
// is that it is an ARRAY: uplift, rift, fault and plate ownership are four
// world-sized fields, and the stress is spread inland by a flood fill over
// macro cells. A world that has no size cannot have those.
//
// So the same physics, asked pointwise:
//
//   1. Seeds on a jittered lattice. The lattice has a spacing in metres, and
//      that spacing IS the number of plates: a region of side S contains about
//      (S / spacing)^2 of them. Six to eight over a world means a spacing of
//      about a third of it.
//   2. The query position is warped by two scales of noise before the nearest
//      seed is looked up, which is what turns a straight bisector into a torn
//      margin. A coastline that runs dead straight for eighty kilometres is a
//      diagram, not a coastline.
//   3. Nearest and second nearest give both the owning plate and the DISTANCE
//      TO THE BOUNDARY between them, which the array version never had - it had
//      a flood fill whose reach was counted in cells, so the width of the Andes
//      depended on the resolution of the macro map.
//   4. Relative drift projected onto the line between the two seeds gives
//      convergence; the perpendicular component gives shear. What that does to
//      the ground depends on which crust meets which.
//
// Everything is a pure function of the seed and the position. Two machines, two
// chunk sizes and two levels of detail get the same answer, and nothing is
// stored.
#include <cstdint>

namespace generation {

// What happens where two plates meet, which is all that is interesting.
enum class Margin : std::uint8_t {
    None,        // deep inside a plate
    Collision,   // continent into continent: the highest ground there is
    Cordillera,  // ocean floor under a continent: a range along the edge
    Trench,      // the other side of the same thing
    Arc,         // ocean into ocean: an island arc
    Rift,        // opening: a valley on land, a ridge on the floor
    Transform,   // sliding past: no relief of its own, but it offsets the rest
};

struct PlateSample {
    // Which plates these are. Stable identities, so a caller can tell "the same
    // plate" from "another plate that happens to be here".
    std::uint64_t plate = 0;
    std::uint64_t neighbour = 0;
    bool oceanic = false;
    bool neighbourOceanic = false;

    // Metres from the boundary between those two. Zero on it.
    double boundaryMetres = 0;
    // How the two move relative to each other, in the same units the drift is
    // given in: positive closing is convergence.
    double closing = 0;
    double shear = 0;
    Margin margin = Margin::None;

    // What the margin delivers here, 0 to 1, after falling off inland. This is
    // the stress that a ridge skeleton is built from - it says how much, not
    // what shape.
    double stress = 0;
    // The crust's own contribution: continental crust stands high wherever it
    // is, and the step at its edge is softened over a shelf rather than being a
    // cliff along the whole margin.
    double crust = 0;
};

struct PlateFieldSettings {
    // The distance between plate seeds. A world of side S has about
    // (S / plateMetres)^2 plates in it.
    double plateMetres = 140000;
    // How far a margin carries inland before it has delivered everything. A
    // physical distance, not a number of cells: the Andes are about this wide
    // whatever resolution anybody looks at them with.
    //
    // Held below four tenths of the plate spacing, because the furthest any
    // point can be from a boundary is about half of it - a margin wider than
    // that reaches everywhere, there is no plate interior left, and "stress
    // falls off inland" stops meaning anything.
    double marginMetres = 90000;
    // How wide the crust step is smeared, which is the continental shelf.
    //
    // Held well below the margin, and for a reason the numbers found: the step
    // is blended towards the NEIGHBOURING plate's crust, and which plate is the
    // neighbour changes abruptly along the medial axis between three of them.
    // At a shelf of a third of the plate spacing the crust jumped by a quarter
    // at every such line - a cliff in the middle of open water. The blend has
    // to have reached zero before the neighbour can change.
    double shelfMetres = 16000;
    // How much of the crust is oceanic, 0 to 1.
    double oceanShare = 0.58;
    // How far the lookup is lied to, in metres, and at what scales. Without
    // this every margin is a straight bisector.
    double warpMetres = 26000;
    double warpBroadMetres = 300000;
    double warpFineMetres = 70000;
};

class PlateField {
public:
    PlateField(std::uint64_t seed, PlateFieldSettings settings = {});

    [[nodiscard]] PlateSample at(double x, double y) const;
    [[nodiscard]] const PlateFieldSettings& settings() const { return settings_; }

    // Where the nearest seed of the plate owning this point sits. For a caller
    // that wants a plate's own frame - a drift direction to lay a range along,
    // say - rather than only what the margin does.
    void drift(std::uint64_t plate, double& vx, double& vy) const;

private:
    std::uint64_t seed_ = 0;
    PlateFieldSettings settings_;
};

} // namespace generation
