#include "game/simulation/farming.hpp"

#include <algorithm>

#include "game/simulation/inventory.hpp"
#include "game/simulation/zones.hpp"

namespace sim {

Fixed effectiveFertility(const World& w, TilePos at) {
    if (!w.map().inBounds(at)) return core::kZero;
    const Tile& t = w.map().at(at);
    if (!t.irrigated) return t.fertility;
    // Whatever a channel adds, the ground cannot be better than the best silt.
    // Asked of the content rather than cached in a static: a static would answer
    // for whichever world happened to run first, and the tests run many.
    Fixed bonus = core::kZero;
    for (const auto& def : w.db().buildings())
        if (def.irrigationRadius > 0) bonus = core::max(bonus, def.irrigationBonus);
    return core::min(core::kOne, t.fertility + bonus);
}

DefId chooseCropToSow(const World& w, SettlementId sid) {
    const auto& st = w.settlement(sid);
    if (!st.ethnos.valid()) return {};
    const auto season = core::seasonName(w.now().season);

    for (const auto& name : w.db().ethnos(st.ethnos).crops) {
        const DefId id = w.db().cropByName(name);
        if (!id.valid()) continue;
        const auto& crop = w.db().crop(id);
        if (!crop.sowSeasons.empty() &&
            std::find(crop.sowSeasons.begin(), crop.sowSeasons.end(), std::string(season)) ==
                    crop.sowSeasons.end())
            continue;
        // Seed is food the community is choosing not to eat. It only sows what it
        // can spare.
        if (!crop.seedItem.valid()) continue;
        if (countAvailable(w, crop.seedItem) < crop.seedCount * 4) continue;
        return id;
    }
    return {};
}

JobKind nextFieldAction(const World& w, SettlementId sid, TilePos tile, DefId& outCrop) {
    outCrop = DefId{};
    if (!w.map().inBounds(tile)) return JobKind::None;
    if (!insideZoneOfKind(w, sid, ZoneKind::Farm, tile)) return JobKind::None;

    const auto& t = w.map().at(tile);
    if (t.building.valid()) return JobKind::None;
    if (!terrainPassable(t.terrain)) return JobKind::None;
    // A tree standing where the field is wanted is work, not a refusal. Treating
    // it as a refusal meant the community laid its fields around every thicket
    // and never cleared an inch of ground in ten years.
    if (t.node.valid() && w.node(t.node).alive) return JobKind::Clear;

    if (t.crop.valid()) {
        outCrop = t.crop;
        return t.cropGrowth >= core::kOne ? JobKind::ReapCrop : JobKind::None;
    }
    if (!t.tilled) return JobKind::Till;

    const DefId crop = chooseCropToSow(w, sid);
    if (!crop.valid()) return JobKind::None;
    outCrop = crop;
    return JobKind::Sow;
}

void tickFarming(World& w) {
    const auto& time = w.db().time();
    if (w.tickCount() % time.ticksPerDay() != 0) return;

    const bool freezing = w.outdoorTemperature() < core::kZero;

    // --- water on the ground ----------------------------------------------
    // Irrigation is the one technology that changes what the land is rather than
    // what the community does with it, so it is resolved before anything grows.
    for (std::int32_t y = 0; y < w.map().height(); ++y)
        for (std::int32_t x = 0; x < w.map().width(); ++x) w.map().at(TilePos{x, y}).irrigated = false;
    for (const auto& b : w.buildings()) {
        if (!b.alive || b.state != BuildState::Complete) continue;
        const auto& def = w.db().building(b.def);
        if (def.irrigationRadius <= 0) continue;
        for (TilePos t : core::tilesWithin(b.origin, def.irrigationRadius))
            if (w.map().inBounds(t)) w.map().at(t).irrigated = true;
    }

    // What the fields will ask for next. Only ground already broken and waiting
    // for seed is reserved: a field is sown a stretch at a time, not all at once.
    // Reserving seed for every plot the community works held back nine hundred
    // units of grain out of sixteen hundred - the whole stock, locked away from
    // the quern - and a village starved with a full granary.
    std::vector<std::int32_t> idleGround(w.settlements().size(), 0);
    for (const auto& z : w.zones()) {
        if (!z.alive || z.kind != ZoneKind::Farm || !z.settlement.valid()) continue;
        for (TilePos t : z.tiles) {
            if (!w.map().inBounds(t)) continue;
            const Tile& tile = w.map().at(t);
            if (tile.tilled && !tile.crop.valid()) idleGround[z.settlement.value] += 1;
        }
    }
    for (auto& st : w.settlements()) {
        st.brokenUnsown = idleGround[st.id.value];
        st.seedReserve.assign(w.db().items().size(), 0);
        if (!st.alive || !st.ethnos.valid()) continue;
        std::int32_t population = 0;
        for (PersonId id : st.members)
            if (w.person(id).alive) ++population;
        // A little ahead of the plough, so the sowers are never waiting on the
        // threshing floor.
        const std::int32_t waiting = st.brokenUnsown + population;
        for (const auto& name : w.db().ethnos(st.ethnos).crops) {
            const DefId id = w.db().cropByName(name);
            if (!id.valid()) continue;
            const auto& crop = w.db().crop(id);
            if (!crop.seedItem.valid()) continue;
            // Crops sharing a seed share the reserve rather than stacking it:
            // a plot sown with emmer is not also sown with einkorn.
            std::int32_t& reserve = st.seedReserve[crop.seedItem.value];
            reserve = std::max(reserve, waiting * crop.seedCount);
        }
    }

    // Watered ground gets better. Not for the season - for good: the water lays
    // silt and carries the salt down, and that is what a canal buys. Before
    // this, irrigation was a modifier that vanished the moment the channel
    // silted up, so the whole works paid for one harvest at a time.
    const Fixed soilGain = w.db().sim().irrigatedSoilGainPerDay;
    if (soilGain > core::kZero) {
        for (auto& z : w.zones()) {
            if (!z.alive || z.kind != ZoneKind::Farm) continue;
            for (TilePos p : z.tiles) {
                if (!w.map().inBounds(p)) continue;
                Tile& t = w.map().at(p);
                if (!t.irrigated) continue;
                t.fertility = core::min(core::kOne, t.fertility + soilGain);
            }
        }
    }

    for (auto& z : w.zones()) {
        if (!z.alive || z.kind != ZoneKind::Farm) continue;
        for (TilePos p : z.tiles) {
            if (!w.map().inBounds(p)) continue;
            Tile& t = w.map().at(p);
            if (!t.crop.valid()) continue;

            const auto& crop = w.db().crop(t.crop);
            // Fertility decides how fast it comes on; frost stops it dead. Nothing
            // here is a timer: a field on poor ground genuinely yields later.
            Fixed perDay = core::kOne / std::max(1, crop.growDays);
            perDay = perDay * (Fixed::ratio(1, 2) + effectiveFertility(w, p) / 2);
            if (freezing) perDay = core::kZero;

            if (t.cropGrowth < core::kOne) {
                t.cropGrowth = core::min(core::kOne, t.cropGrowth + perDay);
                continue;
            }
            // Past ripeness the crop keeps ageing and is eventually lost. Grain
            // left standing is grain nobody will eat, and the pressure to reap it
            // has to come from somewhere.
            t.cropGrowth += Fixed::ratio(1, kDaysRipeBeforeLost);
            if (t.cropGrowth >= kOverripe) {
                t.crop = DefId{};
                t.cropGrowth = core::kZero;
                w.report().cropsLost++;
            }
        }
    }
}

} // namespace sim
