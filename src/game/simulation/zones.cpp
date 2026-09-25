#include "game/simulation/zones.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <limits>

#include "game/simulation/inventory.hpp"

namespace sim {
namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(ZoneKind::Count)> kZoneKindNames{
    "general", "settlement", "storage", "extraction", "farm", "fishing", "hunting",
    "pasture", "patrol", "fortification", "timber", "residential", "craft", "civic",
    "ritual",
};

// A stockpile tile holds a small pile. Stronghold-style visible fullness comes from
// how many of them are occupied (GDD 7).
constexpr std::int32_t kBatchesPerStorageTile = 4;

// Walks the zones covering a tile. The mask makes this cost one popcount step per
// zone actually present rather than a scan over every zone in the settlement.
template <typename Fn>
void forEachZoneAt(const World& w, TilePos at, Fn&& fn) {
    for (std::uint64_t m = w.zoneMaskAt(at); m; m &= m - 1) {
        const auto index = static_cast<std::size_t>(std::countr_zero(m));
        if (index >= w.zones().size()) continue;
        const Zone& z = w.zones()[index];
        if (z.alive) fn(z);
    }
}

} // namespace

std::string_view zoneKindName(ZoneKind k) {
    const auto i = static_cast<std::size_t>(k);
    return i < kZoneKindNames.size() ? kZoneKindNames[i] : "unknown";
}

bool workAllowedAt(const World& w, SettlementId s, WorkCategory c, TilePos at) {
    return workAllowedAt(w, s, c, core::tileCentre(at));
}

bool workAllowedAt(const World& w, SettlementId s, WorkCategory c, WorldPos at) {
    bool allowed = true;
    for (const Zone& z : w.zones()) {
        if (!allowed || z.settlement != s) continue;
        if (!z.contains(at)) continue;
        if (z.mode == ZoneMode::Forbidden && z.appliesTo(c)) allowed = false;
    }
    return allowed;
}

Fixed zoneWeightAt(const World& w, SettlementId s, WorkCategory c, TilePos at) {
    return zoneWeightAt(w, s, c, core::tileCentre(at));
}

Fixed zoneWeightAt(const World& w, SettlementId s, WorkCategory c, WorldPos at) {
    Fixed weight = core::kOne;
    bool forbidden = false;
    for (const Zone& z : w.zones()) {
        if (z.settlement != s || !z.appliesTo(c)) continue;
        if (!z.contains(at)) continue;
        switch (z.mode) {
            case ZoneMode::Forbidden:    forbidden = true; break;
            case ZoneMode::Preferred:    weight = core::max(weight, Fixed::ratio(3, 2)); break;
            case ZoneMode::HighPriority: weight = core::max(weight, Fixed::fromInt(3)); break;
            case ZoneMode::Allowed:      break;
        }
    }
    return forbidden ? core::kZero : weight;
}

bool insideSettlementZone(const World& w, SettlementId s, TilePos at) {
    return insideSettlementZone(w, s, core::tileCentre(at));
}

bool insideSettlementZone(const World& w, SettlementId s, WorldPos at) {
    bool inside = false;
    for (const Zone& z : w.zones()) {
        if (!z.contains(at)) continue;
        if (z.settlement == s && z.mode != ZoneMode::Forbidden) inside = true;
    }
    return inside;
}

bool insideZoneOfKind(const World& w, SettlementId s, ZoneKind kind, TilePos at) {
    return insideZoneOfKind(w, s, kind, core::tileCentre(at));
}

bool insideZoneOfKind(const World& w, SettlementId s, ZoneKind kind, WorldPos at) {
    bool inside = false;
    for (const Zone& z : w.zones()) {
        if (!z.contains(at)) continue;
        if (z.settlement == s && z.kind == kind) inside = true;
    }
    return inside;
}

bool insideAnyZoneOfKind(const World& w, ZoneKind kind, TilePos at) {
    return insideAnyZoneOfKind(w, kind, core::tileCentre(at));
}

bool insideAnyZoneOfKind(const World& w, ZoneKind kind, WorldPos at) {
    bool inside = false;
    for (const Zone& z : w.zones())
        if (z.contains(at) && z.kind == kind) inside = true;
    return inside;
}

bool materialsInReach(const World& w, SettlementId s, TilePos site, TilePos at) {
    if (core::distance(core::tileCentre(at), core::tileCentre(site)) <= Fixed::fromInt(kMaterialReach))
        return true;
    return insideSettlementZone(w, s, core::tileCentre(site)) &&
           insideSettlementZone(w, s, core::tileCentre(at));
}

const Zone* nearestZoneOfKind(const World& w, SettlementId s, ZoneKind kind, TilePos near) {
    return nearestZoneOfKind(w, s, kind, core::tileCentre(near));
}

const Zone* nearestZoneOfKind(const World& w, SettlementId s, ZoneKind kind, WorldPos near) {
    const Zone* best = nullptr;
    Fixed bestDist = Fixed::fromInt(1 << 20);
    for (const auto& z : w.zones()) {
        if (!z.alive || z.settlement != s || z.kind != kind || z.areas.empty()) continue;
        const Fixed d = core::distance(near, z.centreWorld());
        if (d < bestDist) { bestDist = d; best = &z; }
    }
    return best;
}

const Zone* nearestStorageZone(const World& w, SettlementId s, TilePos near) {
    return nearestZoneOfKind(w, s, ZoneKind::Storage, near);
}

const Zone* nearestStorageZone(const World& w, SettlementId s, WorldPos near) {
    return nearestZoneOfKind(w, s, ZoneKind::Storage, near);
}

bool findStorageSpot(const World& w, DefId item, TilePos near, TilePos& outTile, BuildingId& outBuilding) {
    outBuilding = findStorageFor(w, item, near);
    if (outBuilding.valid()) {
        outTile = w.building(outBuilding).origin;
        return true;
    }

    // No store built yet: fall back to a ground stockpile.
    std::int32_t bestDist = std::numeric_limits<std::int32_t>::max();
    bool found = false;
    for (const auto& z : w.zones()) {
        if (!z.alive || z.kind != ZoneKind::Storage || z.mode == ZoneMode::Forbidden) continue;
        for (TilePos p : z.tiles) {
            if (!w.map().inBounds(p) || w.map().blocked(p)) continue;
            std::int32_t batches = 0;
            bool roomInPile = false;
            for (const auto& s : w.stacks()) {
                if (!s.alive || s.count <= 0 || s.where != StackWhere::Ground) continue;
                if (!(s.tile == p)) continue;
                if (s.def == item && s.count < w.db().item(item).stackLimit) { roomInPile = true; break; }
                ++batches;
            }
            if (roomInPile) { outTile = p; outBuilding = BuildingId{}; return true; }
            if (batches >= kBatchesPerStorageTile) continue;
            if (w.isReserved(World::tileKey(p))) continue;
            const std::int32_t d = core::tileDistance(near, p);
            if (d < bestDist) { bestDist = d; outTile = p; found = true; }
        }
    }
    outBuilding = BuildingId{};
    return found;
}

// ---------------------------------------------------------------------------
// Laying out a settlement
// ---------------------------------------------------------------------------
namespace {

// How the community sizes its own areas, in tiles of radius from a centre.
constexpr std::int32_t kSettlementRadius = 12;
// The walled core: never smaller than this, never larger, and in between it is
// whatever encloses the buildings that matter.
constexpr std::int32_t kCitadelRadius = 6;
constexpr std::int32_t kCitadelRadiusMax = 13;
// How much has to stand inside before a wall is worth building at all.
constexpr std::int32_t kWallWorthDefending = 5;
constexpr std::int32_t kStorageRadius = 4;
// The common ground around the fire: the hearth itself, the granaries, whatever
// the whole community keeps rather than one family. Small on purpose - it is a
// square, not a district.
constexpr std::int32_t kCivicRadius = 4;
// Where the community keeps its rites. Beside the common ground rather than on
// it, and small: nothing in the content builds here yet.
constexpr std::int32_t kRitualRadius = 2;
// The craftsmen's quarter: benches, kilns, the mill. Sited by the clay and the
// fire both, because that is what the work needs to hand.
constexpr std::int32_t kCraftRadius = 5;
// How far a family's yard reaches around its seat. Two quarters of the same
// trade run together into one; different trades stay apart.
constexpr std::int32_t kQuarterRadius = 3;
constexpr std::int32_t kWorkAreaRadius = 6;
constexpr std::int32_t kHuntAreaRadius = 9;
constexpr std::int32_t kFishAreaRadius = 5;
// What a family gives up by putting its yard on ground that could be sown.
constexpr std::int32_t kSeatOnFieldCost = 90;
// Mean grass left in a pasture below which the community looks for new grazing.
constexpr std::int32_t kPastureWornOut = 60;
constexpr std::int32_t kFarmRadius = 10;
// One row in four is left free for an irrigation channel. A Mesopotamian field
// is plots divided by ditches, not one unbroken block.
constexpr std::int32_t kChannelSpacing = 4;
constexpr std::int32_t kPastureRadius = 7;
// How far from home the community is willing to put a working area. Beyond this
// the walk costs more than the work is worth.

bool usableGround(const World& w, TilePos p) {
    return w.map().inBounds(p) && terrainPassable(w.map().at(p).terrain);
}

// Scores every candidate centre in a coarse scan and returns the best one, or an
// invalid answer when nothing scores at all. Coarse on purpose: neighbouring
// centres produce nearly the same area, so testing every tile would cost far more
// for no better result.
bool bestCentre(const World& w, TilePos hearth, std::int32_t minDist, std::int32_t maxDist,
                std::int32_t sampleRadius, TilePos& out,
                const std::function<std::int64_t(TilePos)>& scoreHex) {
    std::int64_t bestScore = 0;
    bool found = false;
    for (std::int32_t radius = minDist; radius <= maxDist; radius += 3) {
        for (TilePos candidate : core::tileRing(hearth, radius)) {
            if (!usableGround(w, candidate)) continue;
            std::int64_t score = 0;
            std::int32_t usable = 0;
            for (TilePos p : core::tilesWithin(candidate, sampleRadius)) {
                if (!w.map().inBounds(p)) continue;
                ++usable;
                score += scoreHex(p);
            }
            if (usable < 8) continue;
            // Closer is better, all else equal: a wood two rings nearer is worth
            // more than one slightly denser.
            score = score * 100 / (100 + radius * 3);
            if (score > bestScore) { bestScore = score; out = candidate; found = true; }
        }
    }
    return found;
}

void paintDisc(World& w, ZoneId zone, TilePos centre, std::int32_t radius,
               const std::function<bool(TilePos)>& accept) {
    // Autonomous areas are now authored in world-space. The tile loop remains the
    // filter that decides which cache cells should advertise the area to the rest
    // of the simulation.
    const WorldPos worldCentre = core::tileCentre(centre);
    const Fixed worldRadius = Fixed::fromInt(radius) + Fixed::ratio(1, 2);
    w.addDiscToZone(zone, worldCentre, worldRadius);
    if (!accept) return;

    std::vector<TilePos> rejected;
    for (TilePos p : core::tilesWithin(centre, radius + 1)) {
        if (!w.map().inBounds(p)) continue;
        if (!w.zones()[zone.value].covers(p)) continue;
        if (accept(p)) continue;
        rejected.push_back(p);
    }
    if (!rejected.empty()) w.removeTilesFromZone(zone, rejected);
}

} // namespace

void layOutSettlementZones(World& w, SettlementId sid) {
    const TilePos hearth = w.settlement(sid).hearth;

    // How much ground the community lays out depends on how many mouths it has.
    // Laid out once at founding and never again, a settlement of twenty worked
    // the field a settlement of ten had drawn, and never had a reason to lead
    // water anywhere: the silt it started on was already the best ground on the
    // map. Growing the field is what pushes the fields onto dry land, and dry
    // land is what a canal is for.
    std::int32_t population = 0;
    for (PersonId id : w.settlement(sid).members)
        if (w.person(id).alive) ++population;
    std::int32_t livestock = 0;
    for (const auto& a : w.animals())
        if (a.alive && a.owner == sid) ++livestock;
    const std::int32_t farmRadius = std::clamp(kFarmRadius + population / 4, kFarmRadius, 16);
    // The settled ground grows too. A village of twenty needs room for its
    // quarters, its granaries and its workshops; laid out once for ten it packs
    // them all against the fire and stops.
    const std::int32_t settlementRadius =
            std::clamp(kSettlementRadius + population / 3, kSettlementRadius, 22);
    bool knowsIrrigation = false;
    {
        const DefId irrigation = w.db().knowledgeByName("irrigation");
        for (PersonId id : w.settlement(sid).members)
            if (w.person(id).alive && w.person(id).knows(irrigation)) knowsIrrigation = true;
    }
    const std::int32_t pastureRadius = std::clamp(kPastureRadius + livestock / 6, kPastureRadius, 14);
    w.settlement(sid).zonedForPopulation = population;

    // Replace what the community decided last time; never touch what the player
    // drew (GDD 9 keeps the two loops apart, and so does this).
    std::vector<ZoneId> stale;
    for (const auto& z : w.zones())
        if (z.alive && z.settlement == sid && !z.playerDrawn) stale.push_back(z.id);
    for (ZoneId id : stale) w.destroyZone(id);

    // --- where they live -------------------------------------------------
    const ZoneId settlement =
            w.createZone("settlement", ZoneKind::Settlement, ZoneMode::Preferred, sid, false);
    paintDisc(w, settlement, hearth, settlementRadius,
              [&](TilePos p) { return usableGround(w, p); });

    const ZoneId storage = w.createZone("stockpile", ZoneKind::Storage, ZoneMode::Allowed, sid, false);
    paintDisc(w, storage, hearth, kStorageRadius, [&](TilePos p) { return usableGround(w, p); });

    // --- the quarters ------------------------------------------------------
    // The areas the settlement is actually made of. Each one is a driver: the
    // planner sites a building in the area that building belongs to (D79), so
    // what a quarter contains follows from what the community decides to build
    // there, not from a list written here.
    const ZoneId civic = w.createZone("common ground", ZoneKind::Civic, ZoneMode::Allowed, sid, false);
    paintDisc(w, civic, hearth, kCivicRadius, [&](TilePos p) { return usableGround(w, p); });

    // The rites are kept beside the common ground, not on it. Which side is
    // decided by the ground: the first quarter of the compass that offers dry
    // walkable tiles, so the choice is the map's and not a constant.
    for (int dir : core::kCardinalDirections) {
        const TilePos step = core::neighbour(TilePos{0, 0}, dir);
        const std::int32_t reach = kCivicRadius + kRitualRadius;
        const TilePos centre{hearth.x + step.x * reach, hearth.y + step.y * reach};
        if (!usableGround(w, centre)) continue;
        const ZoneId ritual =
                w.createZone("ritual ground", ZoneKind::Ritual, ZoneMode::Allowed, sid, false);
        if (ritual.valid()) {
            w.zones()[ritual.value].categories = {WorkCategory::Ritual};
            paintDisc(w, ritual, centre, kRitualRadius, [&](TilePos p) { return usableGround(w, p); });
        }
        break;
    }

    // The craftsmen's quarter is where the benches are: every workshop that is
    // not tied to an area of its own by the content - a dairy belongs by the
    // flock, a kiln by the clay - with the ground round it.
    //
    // Two earlier versions both failed, and for the same reason. Sited on the
    // clay bank, it dragged the mill and the bakery out to the edge of the
    // settled ground, away from the granary and the people they feed: a
    // community that had been losing four to hunger in ten years lost ten.
    // Sited beside the common ground and turned towards the nearest rock, it
    // wandered - the bank was worked out, the nearest rock became another one,
    // the quarter jumped across the village and left every workshop standing
    // outside it. An area that follows what has been built stays put.
    {
        std::vector<TilePos> benches;
        for (const auto& b : w.buildings()) {
            if (!b.alive || b.settlement != sid) continue;
            const auto& def = w.db().building(b.def);
            if (def.kind != content::BuildingKind::Workshop) continue;
            if (!def.nearArea.empty()) continue;
            benches.push_back(b.origin);
        }

        const ZoneId craft = w.createZone("craft quarter", ZoneKind::Craft, ZoneMode::Allowed, sid, false);
        if (craft.valid()) {
            const auto paint = [&](TilePos centre) {
                paintDisc(w, craft, centre, kCraftRadius, [&](TilePos p) {
                    if (!usableGround(w, p)) return false;
                    const Tile& t = w.map().at(p);
                    return !t.tilled && !t.crop.valid();
                });
            };
            if (benches.empty()) {
                // Nothing built yet: beside the common ground, on the side the
                // craftsmen's material lies on.
                std::int32_t nearest = 1 << 20;
                TilePos towards{hearth.x, hearth.y + 1};
                for (TilePos p : core::tilesWithin(hearth, settlementRadius)) {
                    if (!w.map().inBounds(p)) continue;
                    const Tile& t = w.map().at(p);
                    if (!t.node.valid() || !w.node(t.node).alive) continue;
                    if (w.db().resourceNode(w.node(t.node).def).kind != content::ResourceKind::Rock)
                        continue;
                    const std::int32_t d = core::tileDistance(hearth, p);
                    if (d > kCivicRadius && d < nearest) { nearest = d; towards = p; }
                }
                const std::int32_t dx = towards.x - hearth.x;
                const std::int32_t dy = towards.y - hearth.y;
                const std::int32_t span = std::max(1, std::max(std::abs(dx), std::abs(dy)));
                const std::int32_t reach = kCivicRadius + kCraftRadius - 2;
                paint(TilePos{hearth.x + dx * reach / span, hearth.y + dy * reach / span});
            } else {
                for (TilePos at : benches) paint(at);
            }
        }
    }

    // The wall rings what there is to defend - the fire, the stores, the
    // workshops, the houses of the people who keep them - and nothing else. Its
    // size follows those buildings rather than being a number: a wall drawn
    // round all the settled ground encloses the fields and the pasture with it,
    // which is not a fortification but a fence around a county. And a settlement
    // with nothing inside worth walling gets no line at all.
    std::int32_t worthWalling = 0;
    std::int32_t reach = 0;
    for (const auto& b : w.buildings()) {
        if (!b.alive || b.settlement != sid) continue;
        const auto& def = w.db().building(b.def);
        if (def.kind == content::BuildingKind::Fortification) continue;
        if (def.kind == content::BuildingKind::Other) continue;
        ++worthWalling;
        reach = std::max(reach, core::tileDistance(hearth, b.origin) + 2);
    }
    if (worthWalling >= kWallWorthDefending) {
        const std::int32_t citadel = std::clamp(reach, kCitadelRadius, kCitadelRadiusMax);
        const ZoneId wall = w.createZone("fortification line", ZoneKind::Fortification,
                                         ZoneMode::Allowed, sid, false);
        if (wall.valid()) {
            // A gateway. The wall grows out of whatever already stands, so left
            // to itself the line closes, and a closed line is a wall with the
            // whole community inside it and the fields, the pasture and the
            // water outside: everyone starves behind their own fortification.
            // One tile of the line is set aside for the gate, which is a
            // building people walk through - a hole in the wall was the first
            // answer and it looked like one. It goes where the traffic is: the
            // way out towards the work.
            TilePos gate = hearth;
            bool haveGate = false;
            std::int32_t nearest = 0;
            for (const auto& z : w.zones()) {
                if (!z.alive || z.settlement != sid || z.tiles.empty()) continue;
                if (z.kind != ZoneKind::Farm && z.kind != ZoneKind::Pasture &&
                    z.kind != ZoneKind::Extraction)
                    continue;
                const TilePos c = z.centre();
                const std::int32_t d = core::tileDistance(hearth, c);
                if (!haveGate || d < nearest) {
                    nearest = d;
                    gate = c;
                    haveGate = true;
                }
            }

            std::vector<TilePos> ring;
            for (TilePos p : core::tileRing(hearth, citadel)) {
                if (!usableGround(w, p)) continue;
                ring.push_back(p);
            }
            w.addTilesToZone(wall, ring);

            // The way out does not move while it is still on the line and still
            // open. Choosing it afresh each time the community reconsidered its
            // areas would walk the gateway round the ring, and the wall would
            // close over the tile it had just left.
            auto& settlement = w.settlement(sid);
            // Two different questions. Whether the way out still works: only a
            // building that stops a person closes it, and the gate itself does
            // not - reading the gate as an obstruction sent the community off to
            // choose another gateway and build another gate, three to a wall.
            // And whether a tile could take the gate at all: nothing built
            // there, dry firm ground, no crossing, no sown ground. Setting aside
            // a tile the gate could not occupy left no way out but the gaps the
            // ring happened to have.
            const auto stillOpen = [&](TilePos p) {
                const BuildingId b = w.map().at(p).building;
                return !(b.valid() && w.buildings()[b.value].alive &&
                         w.db().building(w.buildings()[b.value].def).blocksMovement);
            };
            const auto couldTakeTheGate = [&](TilePos p) {
                const Tile& t = w.map().at(p);
                if (t.building.valid()) return false;
                if (t.ford || t.terrain == Terrain::Marsh) return false;
                return !(t.tilled || t.crop.valid());
            };
            bool keep = false;
            if (settlement.hasGateway && stillOpen(settlement.gateway))
                for (TilePos p : ring)
                    if (p.x == settlement.gateway.x && p.y == settlement.gateway.y) { keep = true; break; }

            if (!keep) {
                TilePos gateway = hearth;
                bool foundGateway = false;
                std::int32_t best = 0;
                for (TilePos p : ring) {
                    if (!couldTakeTheGate(p)) continue;
                    const std::int32_t d = haveGate ? core::tileDistance(gate, p) : 0;
                    if (!foundGateway || d < best) {
                        best = d;
                        gateway = p;
                        foundGateway = true;
                    }
                }
                settlement.hasGateway = foundGateway;
                if (foundGateway) settlement.gateway = gateway;
            }
        }
    }

    // --- where the families live ------------------------------------------
    // One residential quarter per trade, painted round the yards the families of
    // that trade have taken. It follows their seats rather than deciding them:
    // the seats are chosen by the work (D48), so the quarter ends up on the side
    // of the settlement its trade works on, and the planner then keeps the
    // family's own house and store inside it.
    for (std::size_t trade = 0; trade < content::kWorkCategoryCount; ++trade) {
        const auto category = static_cast<WorkCategory>(trade);
        std::vector<TilePos> seats;
        for (const auto& house : w.households())
            if (house.alive && house.seated && house.settlement == sid && house.trade == category)
                seats.push_back(house.seat);
        // And the roofs themselves. A quarter is where the houses are, not only
        // where families have said they mean to build: counted from the seats
        // alone, the area left behind whenever a family died out and its house
        // stayed standing, and two thirds of the dwellings ended up outside any
        // quarter at all.
        for (const auto& b : w.buildings()) {
            if (!b.alive || b.settlement != sid) continue;
            if (w.db().building(b.def).kind != content::BuildingKind::Housing) continue;
            if (!b.household.valid()) continue;
            const auto& house = w.household(b.household);
            if (house.trade != category) continue;
            seats.push_back(b.origin);
        }
        if (seats.empty()) continue;
        const ZoneId quarter =
                w.createZone(std::string(content::workCategoryName(category)) + " quarter",
                             ZoneKind::Residential, ZoneMode::Allowed, sid, false);
        if (!quarter.valid()) continue;
        for (TilePos seat : seats)
            paintDisc(w, quarter, seat, kQuarterRadius, [&](TilePos p) {
                if (!usableGround(w, p)) return false;
                const Tile& t = w.map().at(p);
                return !t.tilled && !t.crop.valid();
            });
    }

    // A dwelling whose family has died out, or whose trade no longer has a
    // quarter, is still part of where the settlement lives.
    {
        std::vector<TilePos> orphans;
        for (const auto& b : w.buildings()) {
            if (!b.alive || b.settlement != sid) continue;
            if (w.db().building(b.def).kind != content::BuildingKind::Housing) continue;
            if (insideZoneOfKind(w, sid, ZoneKind::Residential, b.origin)) continue;
            orphans.push_back(b.origin);
        }
        if (!orphans.empty()) {
            const ZoneId quarter =
                    w.createZone("older houses", ZoneKind::Residential, ZoneMode::Allowed, sid, false);
            if (quarter.valid())
                for (TilePos at : orphans)
                    paintDisc(w, quarter, at, kQuarterRadius, [&](TilePos p) {
                        if (!usableGround(w, p)) return false;
                        const Tile& t = w.map().at(p);
                        return !t.tilled && !t.crop.valid();
                    });
        }
    }

    // --- what the ground offers -------------------------------------------
    auto countKind = [&](TilePos p, content::ResourceKind kind) -> std::int64_t {
        const auto& t = w.map().at(p);
        if (!t.node.valid() || !w.node(t.node).alive) return 0;
        return w.db().resourceNode(w.node(t.node).def).kind == kind ? 1 : 0;
    };

    TilePos centre;
    if (bestCentre(w, hearth, kSettlementRadius - 3, kMaxWorkDistance, kWorkAreaRadius, centre,
                   [&](TilePos p) { return countKind(p, content::ResourceKind::Tree) * 3; })) {
        const ZoneId z = w.createZone("timber", ZoneKind::Timber, ZoneMode::Preferred, sid, false);
        if (z.valid()) {
            w.zones()[z.value].categories = {WorkCategory::Woodcutting, WorkCategory::Foraging};
            paintDisc(w, z, centre, kWorkAreaRadius, [&](TilePos p) { return w.map().inBounds(p); });
        }
    }

    if (bestCentre(w, hearth, kSettlementRadius - 3, kMaxWorkDistance, kWorkAreaRadius, centre,
                   [&](TilePos p) { return countKind(p, content::ResourceKind::Rock) * 3; })) {
        const ZoneId z = w.createZone("quarry", ZoneKind::Extraction, ZoneMode::Preferred, sid, false);
        if (z.valid()) {
            w.zones()[z.value].categories = {WorkCategory::Mining};
            paintDisc(w, z, centre, kWorkAreaRadius, [&](TilePos p) { return w.map().inBounds(p); });
        }
    }

    if (bestCentre(w, hearth, settlementRadius + 4, kMaxWorkDistance + 12, kHuntAreaRadius, centre,
                   [&](TilePos p) { return countKind(p, content::ResourceKind::Game) * 10; })) {
        const ZoneId z = w.createZone("hunting ground", ZoneKind::Hunting, ZoneMode::Preferred, sid, false);
        if (z.valid()) {
            w.zones()[z.value].categories = {WorkCategory::Hunting};
            paintDisc(w, z, centre, kHuntAreaRadius, [&](TilePos p) { return w.map().inBounds(p); });
        }
    }

    // --- the fishing ground ------------------------------------------------
    // Where the shoals run, on the bank the community can reach. Without an
    // area of its own a fishing family had nowhere to live by its work, so it
    // was seated by the fire like a potter.
    if (bestCentre(w, hearth, 3, kMaxWorkDistance + 12, kFishAreaRadius, centre, [&](TilePos p) {
            if (!w.map().inBounds(p)) return std::int64_t(0);
            const Tile& t = w.map().at(p);
            if (!t.node.valid() || !w.node(t.node).alive) return std::int64_t(0);
            return w.db().resourceNode(w.node(t.node).def).harvest.category ==
                                   content::WorkCategory::Fishing
                           ? std::int64_t(10)
                           : std::int64_t(0);
        })) {
        const ZoneId z = w.createZone("fishing water", ZoneKind::Fishing, ZoneMode::Preferred, sid, false);
        if (z.valid()) {
            w.zones()[z.value].categories = {WorkCategory::Fishing};
            paintDisc(w, z, centre, kFishAreaRadius, [&](TilePos p) { return w.map().inBounds(p); });
        }
    }

    // --- the fields --------------------------------------------------------
    // Open, fertile, workable ground, close in: a field far from home is a field
    // nobody weeds.
    auto farmScore = [&](TilePos p) -> std::int64_t {
        if (!usableGround(w, p)) return 0;
        const auto& t = w.map().at(p);
        if (t.building.valid()) return 0;
        // Dry ground is field too, once the community knows how to lead water to
        // it. This is the other half of irrigation and without it the technology
        // buys nothing: the silt a river people starts on is already the best
        // ground on the map, so watering it changes almost nothing, and the
        // steppe behind it is what a canal is actually for.
        const bool dryGroundCounts = knowsIrrigation;
        if (t.terrain != Terrain::Grass && t.terrain != Terrain::Dirt &&
            !(dryGroundCounts && t.terrain == Terrain::Sand))
            return 0;
        // A tile the water can reach is worth as much as good silt; the score is
        // what the ground will be, not what it is.
        // Fertility decides, and it decides steeply: the difference between good
        // silt and passable ground is the difference between a farm that feeds
        // ten and one that feeds six, so it is squared rather than counted. The
        // field is chosen this way and the farmers' houses follow the field.
        const std::int64_t fertile = t.irrigated ? 100 : (t.fertility * 100).toInt();
        std::int64_t worth = fertile * fertile / 100;
        worth = std::max<std::int64_t>(worth, dryGroundCounts ? 4 : 0);
        // Ground with something standing on it is field too - it just has to be
        // cleared first, and that is work the community can see the point of.
        // Refusing such ground outright had it lay its fields around every
        // thicket and never fell a tree to sow in ten years.
        if (t.node.valid() && w.node(t.node).alive) worth = worth * 2 / 3;
        return worth;
    };
    if (bestCentre(w, hearth, 4, settlementRadius + 8, farmRadius, centre, farmScore)) {
        const ZoneId z = w.createZone("fields", ZoneKind::Farm, ZoneMode::HighPriority, sid, false);
        if (z.valid()) {
            w.zones()[z.value].categories = {WorkCategory::Farming};
            // Every fourth row is left out of the field: that is where a channel
            // can go. Without the gaps the plots close ranks around the field and
            // a canal can never get in - a community dug a hundred and six of
            // them along the bank and watered not one furrow, because every tile
            // nearer the field than three was standing under a crop.
            const std::int32_t centreRow = centre.y;
            paintDisc(w, z, centre, farmRadius, [&](TilePos p) {
                if (std::abs(p.y - centreRow) % kChannelSpacing == 0) return false;
                return farmScore(p) > 0;
            });
        }
    }

    // --- the pasture -------------------------------------------------------
    // Grass, not the fields, not the stockpile: sheep and barley do not share.
    auto pastureScore = [&](TilePos p) -> std::int64_t {
        if (!usableGround(w, p)) return 0;
        if (insideZoneOfKind(w, sid, ZoneKind::Farm, p)) return 0;
        if (core::tileDistance(p, hearth) <= kStorageRadius + 1) return 0;
        const auto& t = w.map().at(p);
        if (t.node.valid() && w.node(t.node).alive) return 0;
        // What is standing on it now, not what kind of ground it is. A pasture
        // grazed bare should stop being the pasture.
        return t.grass;
    };
    if (bestCentre(w, hearth, settlementRadius - 2, settlementRadius + 12, pastureRadius, centre, pastureScore)) {
        const ZoneId z = w.createZone("pasture", ZoneKind::Pasture, ZoneMode::Preferred, sid, false);
        if (z.valid()) {
            w.zones()[z.value].categories = {WorkCategory::Herding};
            paintDisc(w, z, centre, pastureRadius, [&](TilePos p) { return pastureScore(p) > 0; });
        }
    }
}

void reconsiderZones(World& w) {
    // Once a season is often enough: nothing here changes between days, and a
    // community that redrew its fields every morning would abandon half-tilled
    // ground for the sake of a better centre.
    const auto& time = w.db().time();
    if (w.tickCount() == 0 || w.tickCount() % (time.ticksPerDay() * time.daysPerSeason) != 0) return;

    for (const auto& st : w.settlements()) {
        if (!st.alive) continue;
        std::int32_t population = 0;
        for (PersonId id : st.members)
            if (w.person(id).alive) ++population;
        // Two reasons to look at the land again: four more mouths, or building
        // having reached the edge of the ground the community laid out. The
        // second is what makes a settlement spread as it is built rather than
        // packing everything inside a boundary drawn once.
        // Grazed out? Then the pasture is somewhere else now. A herding people
        // moves its flock rather than standing it on bare ground.
        bool grazedOut = false;
        for (const auto& z : w.zones()) {
            if (!z.alive || z.kind != ZoneKind::Pasture || z.settlement != st.id) continue;
            if (z.tiles.empty()) continue;
            std::int64_t total = 0;
            for (TilePos t : z.tiles)
                if (w.map().inBounds(t)) total += w.map().at(t).grass;
            if (total / static_cast<std::int64_t>(z.tiles.size()) < kPastureWornOut)
                grazedOut = true;
        }

        bool atTheEdge = false;
        for (const auto& b : w.buildings()) {
            if (!b.alive || b.settlement != st.id) continue;
            for (TilePos t : b.footprintTiles(w.db())) {
                for (int dir : core::kCardinalDirections) {
                    const TilePos n = core::neighbour(t, dir);
                    if (!w.map().inBounds(n)) continue;
                    if (!insideSettlementZone(w, st.id, n)) atTheEdge = true;
                }
            }
            if (atTheEdge) break;
        }
        if (!grazedOut && !atTheEdge && population < st.zonedForPopulation + 4) continue;
        layOutSettlementZones(w, st.id);

        // The areas moved, so the families move with them. A place outside the
        // settled ground cannot reach the settlement's stores, and a place
        // further from its own work than the fire is is not by that work at all -
        // which is what happened every time a hunting ground or a fishing water
        // was laid somewhere new.
        for (auto& house : w.households()) {
            if (!house.alive || house.settlement != st.id || !house.seated) continue;
            if (!insideSettlementZone(w, st.id, house.seat)) { house.seated = false; continue; }
            const ZoneKind kind = zoneOfTrade(house.trade);
            if (kind == ZoneKind::Settlement) continue;
            const Zone* work = nearestZoneOfKind(w, st.id, kind, st.hearth);
            if (!work || work->tiles.empty()) continue;
            if (core::tileDistance(house.seat, work->centre()) >
                core::tileDistance(st.hearth, work->centre()) + kSeatSlack)
                house.seated = false;
        }
    }
}

ZoneKind zoneOfTrade(content::WorkCategory trade) {
    switch (trade) {
        case content::WorkCategory::Farming:     return ZoneKind::Farm;
        case content::WorkCategory::Herding:     return ZoneKind::Pasture;
        case content::WorkCategory::Woodcutting: return ZoneKind::Timber;
        case content::WorkCategory::Mining:      return ZoneKind::Extraction;
        case content::WorkCategory::Hunting:     return ZoneKind::Hunting;
        case content::WorkCategory::Fishing:     return ZoneKind::Fishing;
        // A ritual is kept where the community keeps them.
        case content::WorkCategory::Ritual:      return ZoneKind::Ritual;
        // A potter, a baker, a builder, a hauler have no ground of their own -
        // their work is wherever the settlement needs it, and the craftsmen's
        // quarter follows their benches rather than the other way round - so
        // their families belong by the fire. Seating them by the craft quarter
        // instead closed the loop the wrong way: the quarter followed the
        // benches, the benches followed the seats, and the seats followed the
        // quarter, which drifted as a whole across the village.
        default:                                  return ZoneKind::Settlement;
    }
}

void siteHouseholds(World& w) {
    const auto& time = w.db().time();
    if (w.tickCount() % time.ticksPerDay() != 0) return;

    // A house whose family has died out stands empty, and the next family that
    // needs a roof moves into it. Leaving it tagged to the dead meant every new
    // family built its own from scratch: nine families, and sixty huts raised
    // over a decade with most of them standing empty.
    for (auto& house : w.households()) {
        if (!house.alive) continue;
        bool anyone = false;
        auto people = w.ecs().view<const ecs::Identity, ecs::Person>();
        for (const entt::entity entity : people) {
            const auto& q = w.person(PersonId{people.get<const ecs::Identity>(entity).legacyIndex});
            if (q.household == house.id) anyone = true;
        }
        if (anyone) continue;
        house.alive = false;
        for (auto& b : w.buildings())
            if (b.alive && b.household == house.id) b.household = HouseholdId{};
    }

    // A place is taken once and kept - but a family that has not yet raised
    // anything on it is not living there, and the quarters move while the
    // settlement is young: a farming family seated before there were any fields
    // ended up a quarter of the map from the one that was eventually broken.
    // So an empty place is given up and chosen again, and a family that has
    // started building stays where it started.
    for (auto& house : w.households()) {
        if (!house.alive || !house.seated) continue;
        bool building = false;
        for (const auto& b : w.buildings())
            if (b.alive && b.household == house.id) building = true;
        if (!building) house.seated = false;
    }

    for (auto& house : w.households()) {
        if (!house.alive || house.seated || !house.settlement.valid()) continue;
        const Settlement& st = w.settlement(house.settlement);
        if (!st.alive) continue;

        // Somebody has to still be living in it.
        bool anyone = false;
        for (PersonId id : st.members)
            if (w.person(id).alive && w.person(id).household == house.id) anyone = true;
        if (!anyone) continue;

        const ZoneKind kind = zoneOfTrade(house.trade);
        const Zone* work = nearestZoneOfKind(w, st.id, kind, st.hearth);
        const TilePos towards =
                work && !work->tiles.empty() ? work->centre() : st.hearth;

        // Close to the work, but still part of the settlement: a farmstead an
        // hour's walk from the fire is a different village, not a quarter of
        // this one. And never on the field or the stockpile itself.
        std::int64_t best = -1;
        TilePos seat = st.hearth;
        for (TilePos p : core::tilesWithin(st.hearth, kSettlementRadius + 12)) {
            if (!w.map().inBounds(p)) continue;
            // Inside the settled ground: work draws on the settlement's stores,
            // and a quarter outside them cannot see them.
            if (!insideSettlementZone(w, st.id, p)) continue;
            const Tile& t = w.map().at(p);
            if (!terrainPassable(t.terrain) || t.building.valid()) continue;
            if (t.tilled || t.crop.valid()) continue;
            if (insideAnyZoneOfKind(w, ZoneKind::Storage, p)) continue;
            if (!workAllowedAt(w, st.id, WorkCategory::Construction, p)) continue;
            // Unbroken ground inside the field is allowed, and costs something:
            // a farmstead stands on the edge of its own fields. Forbidding it
            // outright sent farming families to the far side of the village -
            // every tile on the field's side of the settlement was field - and
            // they ended up living in the shepherds' quarter.

            // Distance to the work dominates; distance to the fire only breaks
            // ties, so quarters form on the side the work is on.
            std::int64_t score = 1000 - core::tileDistance(p, towards) * 8 -
                                 core::tileDistance(p, st.hearth);
            if (insideAnyZoneOfKind(w, ZoneKind::Farm, p)) score -= kSeatOnFieldCost;
            // Families of the same trade cluster, but not on one another's roof.
            std::int64_t crowding = 0;
            for (const auto& other : w.households()) {
                if (!other.alive || !other.seated || other.id == house.id) continue;
                const std::int32_t apart = core::tileDistance(p, other.seat);
                if (apart < 3) crowding += 400;
                else if (other.trade == house.trade && apart < 8) crowding -= 40;
            }
            if (score - crowding > best) { best = score - crowding; seat = p; }
        }
        if (best < 0) continue;
        house.seat = seat;
        house.seated = true;
    }
}

} // namespace sim
