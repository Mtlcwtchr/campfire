#include "game/ui/info_panel.hpp"

#include <algorithm>
#include <cstdio>
#include <sstream>
#include <string_view>

#include "game/simulation/farming.hpp"
#include "game/simulation/inventory.hpp"
#include "game/work/planner.hpp"
#include "game/simulation/zones.hpp"

namespace ui {
namespace {

using client::Selection;
using client::SelectionKind;
using core::Fixed;

std::string num(Fixed v, int decimals = 2) { return core::toString(v, decimals); }

std::string pct(Fixed v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", static_cast<int>((v * 100).roundToInt()));
    return buf;
}

// A small text bar, so needs read at a glance rather than as four decimals.
std::string bar(Fixed v, int width = 10) {
    const int filled = std::clamp(static_cast<int>((v * width).roundToInt()), 0, width);
    return std::string(filled, '=') + std::string(width - filled, '.');
}

std::string joinCounts(const sim::World& w, const std::vector<content::IngredientSpec>& list) {
    std::string out;
    for (const auto& i : list) {
        if (!out.empty()) out += ", ";
        out += w.db().item(i.item).label + " x" + std::to_string(i.count);
    }
    return out.empty() ? "nothing" : out;
}

const char* whereName(sim::StackWhere where) {
    switch (where) {
        case sim::StackWhere::Ground:      return "on the ground";
        case sim::StackWhere::Carried:     return "carried by";
        case sim::StackWhere::InBuilding:  return "stored in";
        case sim::StackWhere::Equipped:    return "held by";
    }
    return "?";
}

// "a axe" reads as a bug even when it is not one.
std::string withArticle(std::string_view noun) {
    const bool vowel = !noun.empty() && std::string_view("aeiou").find(noun.front()) != std::string_view::npos;
    return (vowel ? "an " : "a ") + std::string(noun);
}

const char* stageName(sim::LifeStage s) {
    switch (s) {
        case sim::LifeStage::Child: return "child";
        case sim::LifeStage::Adult: return "adult";
        case sim::LifeStage::Elder: return "elder";
    }
    return "?";
}

// Demand is O(items x batches) and the panel redraws every frame, so it is
// computed at most once per simulation tick and reused until the world moves on.
const sim::work::Demand& cachedDemand(const sim::World& w) {
    static sim::work::Demand demand;
    static std::int64_t computedAtTick = -1;
    static const sim::World* computedFor = nullptr;
    if (computedAtTick != w.tickCount() || computedFor != &w) {
        demand = w.settlements().empty() ? sim::work::Demand{} : sim::work::computeDemand(w, w.settlements().front().id);
        computedAtTick = w.tickCount();
        computedFor = &w;
    }
    return demand;
}

void addDemandLine(const sim::World& w, core::DefId item, std::vector<std::string>& lines) {
    const auto& d = cachedDemand(w);
    const Fixed value = d.forItem(item);
    const Fixed wanted = item.value < d.wanted.size() ? d.wanted[item.value] : core::kZero;
    if (value <= core::kZero) {
        lines.push_back("the community wants no more of this right now");
    } else {
        lines.push_back("wanted: " + num(wanted, 0) + " units, worth " + num(value) + " each");
    }
}

// ---------------------------------------------------------------------------

void describePerson(const sim::World& w, core::PersonId id, InfoText& out) {
    const auto& p = w.person(id);
    out.title = p.name;
    out.subtitle = std::to_string(p.ageYears) + "y " + stageName(p.stage) + ", " +
                   (p.sex == sim::Sex::Female ? "female" : "male");

    out.lines.push_back("#body");
    out.lines.push_back("satiety  " + bar(p.satiety) + " " + pct(p.satiety));
    out.lines.push_back("water    " + bar(p.hydration) + " " + pct(p.hydration));
    out.lines.push_back("rest     " + bar(p.rest) + " " + pct(p.rest));
    out.lines.push_back("health   " + bar(p.health) + " " + pct(p.health));
    out.lines.push_back("body temperature " + num(p.bodyTempOffset, 1) + " C from comfortable");
    if (p.asleep) out.lines.push_back("asleep");
    out.lines.push_back("strength " + num(p.strength, 2) + "  endurance " + num(p.endurance, 2) +
                        "  dexterity " + num(p.dexterity, 2));

    out.lines.push_back("");
    out.lines.push_back("#doing");
    if (p.job.valid()) {
        out.lines.push_back(std::string(sim::jobKindName(p.job.kind)) + " (" +
                            std::string(content::workCategoryName(p.job.category)) + ")");
        if (p.job.workRequired > core::kZero)
            out.lines.push_back("progress " + num(p.job.workDone, 0) + " / " +
                                num(p.job.workRequired, 0));
        out.lines.push_back("target tile " + std::to_string(p.job.target.x) + ", " +
                            std::to_string(p.job.target.y));
        if (p.job.recipe.valid()) out.lines.push_back("recipe: " + w.db().recipe(p.job.recipe).label);
    } else {
        // GDD 9: the player must be able to see why nothing is happening.
        out.lines.push_back("idle - " + std::string(sim::idleReasonName(p.idleReason)));
    }
    out.lines.push_back("main profession: " + std::string(content::workCategoryName(p.profession)));

    out.lines.push_back("");
    out.lines.push_back("#carrying");
    if (p.carrying.valid() && w.stack(p.carrying).alive) {
        const auto& s = w.stack(p.carrying);
        out.lines.push_back(w.db().item(s.def).label + " x" + std::to_string(s.count));
    } else {
        out.lines.push_back("empty handed");
    }
    if (p.equippedTool.valid() && w.stack(p.equippedTool).alive) {
        const auto& s = w.stack(p.equippedTool);
        const auto& def = w.db().item(s.def);
        std::string line = "tool: " + def.label;
        if (def.durability > 0)
            line += " (" + std::to_string(s.durabilityLeft) + "/" + std::to_string(def.durability) + ")";
        out.lines.push_back(line);
    } else {
        out.lines.push_back("tool: none");
    }

    std::string skills;
    for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) {
        if (p.skills[i].level <= 0) continue;
        if (!skills.empty()) skills += ", ";
        skills += std::string(content::workCategoryName(static_cast<content::WorkCategory>(i))) + " " +
                  std::to_string(p.skills[i].level);
    }
    out.lines.push_back("");
    out.lines.push_back("#skills");
    out.lines.push_back(skills.empty() ? "nothing above zero yet" : skills);

    if (!p.traits.empty()) {
        std::string traits;
        for (core::DefId t : p.traits) {
            if (!traits.empty()) traits += ", ";
            traits += w.db().trait(t).label;
        }
        out.lines.push_back("");
        out.lines.push_back("#traits");
        out.lines.push_back(traits);
    }

    out.lines.push_back("");
    out.lines.push_back("#knowledge");
    std::string methods;
    for (core::DefId k : p.knownMethods) {
        if (!methods.empty()) methods += ", ";
        methods += w.db().knowledge(k).label;
    }
    out.lines.push_back(methods.empty() ? "none" : methods);

    out.lines.push_back("");
    out.lines.push_back("#family");
    const core::BuildingId home = sim::work::homeForPerson(w, p);
    if (home.valid()) {
        const auto& building = w.building(home);
        out.lines.push_back("home: " + w.db().building(building.def).label + " at " +
                            std::to_string(building.origin.x) + ", " + std::to_string(building.origin.y));
    } else {
        out.lines.push_back("home: no completed house");
    }
    if (p.spouse.valid() && w.person(p.spouse).alive)
        out.lines.push_back("married to " + w.person(p.spouse).name);
    if (p.mother.valid()) out.lines.push_back("mother " + w.person(p.mother).name);
    if (p.father.valid()) out.lines.push_back("father " + w.person(p.father).name);
    std::string children;
    for (const auto& c : w.people()) {
        if (!c.alive || (c.mother != p.id && c.father != p.id)) continue;
        if (!children.empty()) children += ", ";
        children += c.name;
    }
    if (!children.empty()) out.lines.push_back("children: " + children);
    if (!p.spouse.valid() && !p.mother.valid() && !p.father.valid() && children.empty())
        out.lines.push_back("no living kin here");
}

void describeAnimal(const sim::World& w, core::AnimalId id, InfoText& out) {
    const auto& a = w.animal(id);
    const auto& def = w.db().animal(a.def);
    const auto& time = w.db().time();
    out.title = def.label;
    out.subtitle = std::string(a.adult(w.db()) ? "grown" : "young") + ", " +
                   (a.sex == sim::Sex::Female ? "ewe" : "ram") + ", " +
                   std::to_string(a.ageDays) + " days old";

    out.lines.push_back("#condition");
    out.lines.push_back("fed  " + bar(a.condition) + " " + pct(a.condition));
    if (a.condition < core::Fixed::ratio(1, 2))
        out.lines.push_back("losing condition - nothing to graze where it stands");

    auto whenReady = [&](std::int64_t tick, std::int32_t interval, const char* what) {
        if (interval <= 0) return;
        if (!a.adult(w.db())) { out.lines.push_back(std::string(what) + ": not until it is grown"); return; }
        if (w.tickCount() >= tick) { out.lines.push_back(std::string(what) + ": ready now"); return; }
        const std::int64_t days = (tick - w.tickCount()) / time.ticksPerDay();
        out.lines.push_back(std::string(what) + ": in about " + std::to_string(days + 1) + " days");
    };

    out.lines.push_back("");
    out.lines.push_back("#gives");
    if (!def.shearYields.empty()) {
        out.lines.push_back("shearing: " + joinCounts(w, def.shearYields) + " every " +
                            std::to_string(def.shearIntervalDays) + " days");
        whenReady(a.nextShearTick, def.shearIntervalDays, "next fleece");
    }
    if (!def.milkYields.empty() && a.sex == sim::Sex::Female) {
        out.lines.push_back("milking: " + joinCounts(w, def.milkYields) + " every " +
                            std::to_string(def.milkIntervalDays) + " days");
        whenReady(a.nextMilkTick, def.milkIntervalDays, "next milking");
    }
    if (!def.slaughterYields.empty())
        out.lines.push_back("slaughter would give " + joinCounts(w, def.slaughterYields));

    out.lines.push_back("");
    out.lines.push_back("#the flock");
    std::int32_t total = 0, grown = 0;
    for (const auto& other : w.animals()) {
        if (!other.alive || other.def != a.def) continue;
        ++total;
        if (other.adult(w.db())) ++grown;
    }
    out.lines.push_back(std::to_string(total) + " head, " + std::to_string(grown) + " grown");
    out.lines.push_back("a flock is capital before it is meat: it is only cut into");
    out.lines.push_back("when there is a surplus, or nothing else to eat");
}

void describeBuilding(const sim::World& w, core::BuildingId id, InfoText& out) {
    const auto& b = w.building(id);
    const auto& def = w.db().building(b.def);
    out.title = def.label;
    const std::size_t tiles = b.footprintTiles(w.db()).size();
    out.subtitle = std::string(content::buildingKindName(def.kind)) + ", " +
                   std::to_string(tiles) + (tiles == 1 ? " tile" : " tiles");

    out.lines.push_back("#state");
    switch (b.state) {
        case sim::BuildState::Blueprint: out.lines.push_back("planned, no work started"); break;
        case sim::BuildState::Building:  out.lines.push_back("under construction"); break;
        case sim::BuildState::Complete:  out.lines.push_back("finished and in use"); break;
    }
    if (b.state != sim::BuildState::Complete) {
        out.lines.push_back("work " + num(b.workDone, 0) + " / " + num(def.workAmount, 0));
        out.lines.push_back("");
        out.lines.push_back("#materials delivered");
        for (std::size_t i = 0; i < def.materials.size(); ++i) {
            const std::int32_t got = i < b.delivered.size() ? b.delivered[i] : 0;
            out.lines.push_back(w.db().item(def.materials[i].item).label + "  " + std::to_string(got) +
                                " / " + std::to_string(def.materials[i].count));
        }
        if (def.requiredTool != content::ToolClass::None)
            out.lines.push_back("needs " + withArticle(content::toolClassName(def.requiredTool)));
        if (!def.requiredKnowledge.empty())
            out.lines.push_back("needs the method: " + def.requiredKnowledge);
    }

    out.lines.push_back("");
    out.lines.push_back("#properties");
    out.lines.push_back(def.blocksMovement ? "blocks movement" : "can be walked through");
    if (def.sheltered) out.lines.push_back("sheltered from the weather");
    if (def.providesFire) out.lines.push_back("fire or oven: cooking and boiling happen here");
    if (def.providesHeat) out.lines.push_back("warms anyone within three tiles");
    if (def.warmthBonus > core::kZero) out.lines.push_back("warmth +" + num(def.warmthBonus, 1) + " C");
    if (def.comfortBonus > core::kZero)
        out.lines.push_back("sleep recovery +" + pct(def.comfortBonus));
    if (def.sleepingSlots > 0) out.lines.push_back("sleeping places: " + std::to_string(def.sleepingSlots));

    if (def.storageSlots > 0) {
        out.lines.push_back("");
        out.lines.push_back("#stored");
        const std::int32_t used = sim::stacksInBuilding(w, id);
        out.lines.push_back(std::to_string(used) + " of " + std::to_string(def.storageSlots) +
                            " batches, spoilage x" + num(def.spoilRateMultiplier, 2));
        for (const auto& s : w.stacks()) {
            if (!s.alive || s.count <= 0) continue;
            if (s.where != sim::StackWhere::InBuilding || s.building != id) continue;
            std::string line = w.db().item(s.def).label + " x" + std::to_string(s.count);
            if (w.db().item(s.def).spoilDays > 0) line += "  fresh " + pct(s.freshness);
            out.lines.push_back(line);
        }
    }

    // Which recipes this building is the workplace for: the reason it exists.
    std::string uses;
    for (const auto& r : w.db().recipes()) {
        if (std::find(r.workplaceDefs.begin(), r.workplaceDefs.end(), b.def) == r.workplaceDefs.end())
            continue;
        if (!uses.empty()) uses += ", ";
        uses += r.label;
    }
    if (!uses.empty()) {
        out.lines.push_back("");
        out.lines.push_back("#work done here");
        out.lines.push_back(uses);
    }
}

void describeResource(const sim::World& w, core::ResourceNodeId id, InfoText& out) {
    const auto& n = w.node(id);
    const auto& def = w.db().resourceNode(n.def);
    const auto& h = def.harvest;
    out.title = def.label;
    out.subtitle = std::string(content::resourceKindName(def.kind)) + ", natural";

    out.lines.push_back("#state");
    if (n.depleted) {
        if (n.regrowAtTick > w.tickCount()) {
            const std::int64_t days = (n.regrowAtTick - w.tickCount()) / w.db().time().ticksPerDay();
            out.lines.push_back("harvested, back in about " + std::to_string(days + 1) + " days");
        } else {
            out.lines.push_back("harvested, recovering");
        }
    } else {
        out.lines.push_back("ready to harvest");
    }
    if (n.workDone > core::kZero)
        out.lines.push_back("work put in so far " + num(n.workDone, 0));

    out.lines.push_back("");
    out.lines.push_back("#harvest");
    out.lines.push_back("yields " + joinCounts(w, h.yields));
    out.lines.push_back("work " + num(h.workAmount, 0) + " as " +
                        std::string(content::workCategoryName(h.category)));
    if (h.requiredTool != content::ToolClass::None)
        out.lines.push_back("requires " + withArticle(content::toolClassName(h.requiredTool)) +
                            " - bare hands will not do");
    else if (h.preferredTool != content::ToolClass::None)
        out.lines.push_back("bare hands work at " + pct(h.bareHandPenalty) + " speed; " +
                            withArticle(content::toolClassName(h.preferredTool)) + " is faster");
    else
        out.lines.push_back("bare hands are enough");
    if (!h.requiredKnowledge.empty()) out.lines.push_back("needs the method: " + h.requiredKnowledge);

    if (!def.seasons.empty()) {
        std::string seasons;
        for (const auto& s : def.seasons) {
            if (!seasons.empty()) seasons += ", ";
            seasons += s;
        }
        out.lines.push_back("only in " + seasons);
    } else {
        out.lines.push_back("available all year");
    }
    out.lines.push_back(def.consumedOnHarvest ? "taking it uses it up" : "regrows in place");

    if (!h.yields.empty()) {
        out.lines.push_back("");
        out.lines.push_back("#why anyone would");
        addDemandLine(w, h.yields.front().item, out.lines);
    }
}

void describeStack(const sim::World& w, const Selection& sel, InfoText& out) {
    const auto& s = w.stack(sel.stack);
    const auto& def = w.db().item(s.def);
    out.title = def.label;
    out.subtitle = "x" + std::to_string(s.count) + " of " + std::to_string(def.stackLimit) +
                   ", " + std::string(content::itemCategoryName(def.category));

    out.lines.push_back("#where");
    std::string where = whereName(s.where);
    if (s.where == sim::StackWhere::InBuilding && s.building.valid())
        where += " " + w.db().building(w.building(s.building).def).label;
    else if ((s.where == sim::StackWhere::Carried || s.where == sim::StackWhere::Equipped) &&
             s.holder.valid())
        where += " " + w.person(s.holder).name;
    else
        where += " at " + std::to_string(s.tile.x) + ", " + std::to_string(s.tile.y);
    out.lines.push_back(where);
    out.lines.push_back("mass " + num(def.massPerUnit * std::int64_t(s.count), 1) + " kg total");

    if (def.category == content::ItemCategory::Food && def.nutrition > core::kZero) {
        out.lines.push_back("");
        out.lines.push_back("#as food");
        out.lines.push_back("feeds " + num(def.nutrition, 2) + " per unit, " +
                            num(def.nutrition * std::int64_t(s.count), 1) + " in this batch");
        std::string groups;
        for (auto g : def.foodGroups) {
            if (!groups.empty()) groups += ", ";
            groups += content::foodGroupName(g);
        }
        if (!groups.empty()) out.lines.push_back("food groups: " + groups);
        if (!def.edibleRaw) out.lines.push_back("must be cooked before anyone will eat it");
        if (def.rawUnsafe) out.lines.push_back("risky raw - only a starving person will touch it");
        if (def.spoilDays > 0) {
            const Fixed daysLeft = s.freshness * def.spoilDays;
            out.lines.push_back("freshness " + bar(s.freshness) + " " + pct(s.freshness));
            out.lines.push_back("keeps " + std::to_string(def.spoilDays) + " days, about " +
                                num(daysLeft, 1) + " left");
        } else {
            out.lines.push_back("does not spoil");
        }
    }

    if (def.toolClass != content::ToolClass::None) {
        out.lines.push_back("");
        out.lines.push_back("#as a tool");
        out.lines.push_back("counts as " + withArticle(content::toolClassName(def.toolClass)));
        out.lines.push_back("work rate x" + num(def.toolEfficiency, 2));
        if (def.durability > 0)
            out.lines.push_back("durability " + std::to_string(s.durabilityLeft) + " / " +
                                std::to_string(def.durability));
    }
    if (def.insulation > core::kZero) {
        out.lines.push_back("");
        out.lines.push_back("#as clothing");
        out.lines.push_back("insulation +" + num(def.insulation, 1) + " C");
    }
    if (def.containerCapacity > core::kZero)
        out.lines.push_back("holds " + num(def.containerCapacity, 1) + " litres");

    out.lines.push_back("");
    out.lines.push_back("#demand");
    addDemandLine(w, s.def, out.lines);
    out.lines.push_back("in the settlement: " + std::to_string(sim::countAvailable(w, s.def)) +
                        " available");

    // What this feeds into, so a pile of flint explains itself.
    std::string usedBy;
    for (const auto& r : w.db().recipes())
        for (const auto& in : r.inputs)
            if (in.item == s.def) {
                if (!usedBy.empty()) usedBy += ", ";
                usedBy += r.label;
            }
    for (const auto& b : w.db().buildings())
        for (const auto& m : b.materials)
            if (m.item == s.def) {
                if (!usedBy.empty()) usedBy += ", ";
                usedBy += b.label;
            }
    if (!usedBy.empty()) {
        out.lines.push_back("");
        out.lines.push_back("#used for");
        out.lines.push_back(usedBy);
    }
}

void describeTile(const sim::World& w, core::TilePos tile, InfoText& out) {
    const auto& t = w.map().at(tile);
    out.title = std::string(sim::terrainName(t.terrain));
    out.subtitle = "tile " + std::to_string(tile.x) + ", " + std::to_string(tile.y);

    out.lines.push_back("#ground");
    out.lines.push_back(w.map().blocked(tile) ? "blocked" : "walkable");
    out.lines.push_back("travel cost x" + num(sim::tileMoveCost(t), 2));
    if (t.traffic >= sim::kPathVisible)
        out.lines.push_back("a path worn in by use - quicker to cross than the ground it was");
    const core::Fixed grows = sim::effectiveFertility(w, tile);
    out.lines.push_back("fertility " + bar(grows) + " " + pct(grows));
    if (t.irrigated && grows > t.fertility)
        out.lines.push_back("watered by a channel - the ground itself gives " + pct(t.fertility));
    if (t.pollution > core::kZero)
        out.lines.push_back("filth " + bar(t.pollution) + " " + pct(t.pollution) +
                            " - raises the risk of illness");
    else
        out.lines.push_back("clean");

    if (t.tilled || t.crop.valid()) {
        out.lines.push_back("");
        out.lines.push_back("#field");
        if (!t.crop.valid()) {
            out.lines.push_back("broken ground, nothing sown yet");
        } else {
            const auto& crop = w.db().crop(t.crop);
            out.lines.push_back("sown with " + crop.label);
            out.lines.push_back("ripeness " + bar(t.cropGrowth) + " " + pct(t.cropGrowth));
            if (t.cropGrowth >= core::kOne) {
                // What this tile will give, not what the crop gives in general:
                // the ground decides the harvest as much as the seed does.
                const core::Fixed share = core::Fixed::ratio(1, 2) + grows / 2;
                const std::int32_t yield = std::max<std::int32_t>(
                        1, (core::Fixed::fromInt(crop.harvestCount) * share).roundToInt());
                out.lines.push_back("ready to reap: " + w.db().item(crop.harvestItem).label + " x" +
                                    std::to_string(yield));
            }
            else
                out.lines.push_back("frost stops it growing; poor ground slows it");
        }
    }

    out.lines.push_back("");
    out.lines.push_back("#zones");
    bool any = false;
    for (const auto& z : w.zones()) {
        if (!z.alive) continue;
        if (!z.covers(tile)) continue;
        any = true;
        const char* mode = "allowed";
        switch (z.mode) {
            case sim::ZoneMode::Forbidden:    mode = "FORBIDDEN"; break;
            case sim::ZoneMode::Preferred:    mode = "preferred"; break;
            case sim::ZoneMode::HighPriority: mode = "high priority"; break;
            case sim::ZoneMode::Allowed:      break;
        }
        std::string line = z.label + " (" + std::string(sim::zoneKindName(z.kind)) + ", " + mode + ")";
        if (z.playerDrawn) line += " [yours]";
        if (!z.categories.empty()) {
            line += " for ";
            for (std::size_t i = 0; i < z.categories.size(); ++i) {
                if (i) line += ", ";
                line += content::workCategoryName(z.categories[i]);
            }
        }
        out.lines.push_back(line);
    }
    if (!any) out.lines.push_back("no zone covers this tile");
}

} // namespace

std::string jobPhrase(const sim::World& w, const sim::Person& p) {
    if (!p.job.valid()) return std::string("idle: ") + std::string(sim::idleReasonName(p.idleReason));
    std::string what = std::string(sim::jobKindName(p.job.kind));
    if (p.job.kind == sim::JobKind::Craft && p.job.recipe.valid())
        what += " " + w.db().recipe(p.job.recipe).label;
    if (p.job.workRequired > core::kOne) {
        const auto done = (p.job.workDone * 100 / p.job.workRequired).roundToInt();
        what += " " + std::to_string(std::clamp<std::int64_t>(done, 0, 100)) + "%";
    }
    return what;
}

InfoText describeTrades(const sim::World& w, sim::SettlementId settlement, std::uint32_t expanded) {
    InfoText out;
    out.title = "Who does what";
    if (!settlement.valid() || settlement.value >= w.settlements().size()) return out;
    const sim::Settlement& st = w.settlement(settlement);

    std::int32_t adults = 0;
    std::int32_t children = 0;
    for (sim::PersonId id : st.members) {
        const sim::Person& q = w.person(id);
        if (!q.alive) continue;
        if (q.stage == sim::LifeStage::Child) ++children;
        else ++adults;
    }
    out.subtitle = std::to_string(adults) + " grown, " + std::to_string(children) + " children";

    for (std::size_t i = 0; i < content::kWorkCategoryCount; ++i) {
        const auto category = static_cast<content::WorkCategory>(i);
        std::vector<sim::PersonId> living;
        for (sim::PersonId id : st.members) {
            const sim::Person& q = w.person(id);
            if (q.alive && q.profession == category) living.push_back(id);
        }
        if (living.empty()) continue;

        const bool open = (expanded & (1u << i)) != 0;
        out.lines.push_back(std::string(open ? "- " : "+ ") +
                            std::string(content::workCategoryName(category)) + ": " +
                            std::to_string(living.size()));
        if (!open) continue;
        for (sim::PersonId id : living) {
            const sim::Person& q = w.person(id);
            out.lines.push_back("    " + q.name + " - " + jobPhrase(w, q));
        }
    }
    if (out.lines.empty()) out.lines.push_back("nobody left");
    return out;
}

InfoText describeStores(const sim::World& w, sim::SettlementId settlement) {
    InfoText out;
    out.title = "What we have";
    if (!settlement.valid() || settlement.value >= w.settlements().size()) return out;

    struct Where { std::int32_t stores = 0, workshops = 0, homes = 0, ground = 0; };
    std::vector<Where> byItem(w.db().items().size());
    std::int32_t loose = 0;

    for (const auto& s : w.stacks()) {
        if (!s.alive || s.count <= 0) continue;
        if (s.owner != settlement) continue;
        Where& row = byItem[s.def.value];
        if (s.where == sim::StackWhere::InBuilding && s.building.valid()) {
            switch (w.db().building(w.building(s.building).def).kind) {
                case content::BuildingKind::Housing:  row.homes += s.count; break;
                case content::BuildingKind::Workshop: row.workshops += s.count; break;
                default:                              row.stores += s.count; break;
            }
        } else if (s.where == sim::StackWhere::Ground) {
            row.ground += s.count;
            loose += s.count;
        }
    }

    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < byItem.size(); ++i) {
        const Where& row = byItem[i];
        if (row.stores + row.workshops + row.homes + row.ground > 0) order.push_back(i);
    }
    const auto total = [&](std::size_t i) {
        const Where& r = byItem[i];
        return r.stores + r.workshops + r.homes + r.ground;
    };
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return total(a) != total(b) ? total(a) > total(b) : a < b;
    });

    out.subtitle = std::to_string(order.size()) + " kinds, " + std::to_string(loose) +
                   " still lying out";
    out.lines.push_back("#store / shop / home / out");
    for (std::size_t i : order) {
        const Where& row = byItem[i];
        out.lines.push_back(w.db().items()[i].label + ": " + std::to_string(row.stores) + " / " +
                            std::to_string(row.workshops) + " / " + std::to_string(row.homes) +
                            " / " + std::to_string(row.ground));
    }
    if (order.empty()) out.lines.push_back("nothing at all");
    return out;
}

InfoText describe(const sim::World& w, const Selection& sel) {
    InfoText out;
    switch (sel.kind) {
        case SelectionKind::Person:   describePerson(w, sel.person, out); break;
        case SelectionKind::Animal:   describeAnimal(w, sel.animal, out); break;
        case SelectionKind::Building: describeBuilding(w, sel.building, out); break;
        case SelectionKind::Resource: describeResource(w, sel.node, out); break;
        case SelectionKind::Stack:    describeStack(w, sel, out); break;
        case SelectionKind::Tile:     describeTile(w, sel.tile, out); break;
        case SelectionKind::None:     break;
    }

    // What it looks like, for an inspector that shows the thing rather than only
    // writing about it. Named the way the sprite library names things, which is
    // by the content definition's own name.
    switch (sel.kind) {
        case SelectionKind::Building:
            if (sel.building.valid() && sel.building.value < w.buildings().size())
                out.picture = "buildings/" + w.db().building(w.buildings()[sel.building.value].def).name;
            break;
        case SelectionKind::Resource:
            if (sel.node.valid() && sel.node.value < w.nodes().size())
                out.picture = "nodes/" + w.db().resourceNode(w.nodes()[sel.node.value].def).name;
            break;
        case SelectionKind::Animal:
            if (sel.animal.valid() && sel.animal.value < w.animals().size())
                out.picture = "animals/" + w.db().animal(w.animals()[sel.animal.value].def).name;
            break;
        case SelectionKind::Stack:
            if (sel.stack.valid() && sel.stack.value < w.stacks().size())
                out.picture = "items/" + w.db().item(w.stacks()[sel.stack.value].def).name;
            break;
        default: break;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
namespace {

// SDL's built-in debug font is a fixed 8x8 grid, which is what makes wrapping a
// matter of counting characters rather than measuring glyphs.
constexpr float kGlyph = 8.0f;
constexpr float kLineHeight = 12.0f;
constexpr float kPad = 10.0f;

void wrapInto(const std::string& text, std::size_t width, std::vector<std::string>& out) {
    if (text.size() <= width) { out.push_back(text); return; }

    std::istringstream words(text);
    std::string word;
    std::string line;
    // Continuation lines are indented so a wrapped entry does not read as two.
    const std::string indent = "  ";
    while (words >> word) {
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            out.push_back(line);
            line = indent;
        }
        if (!line.empty() && line != indent) line += ' ';
        line += word;
        // A single word longer than the panel is cut rather than pushing the
        // panel wider.
        while (line.size() > width) {
            out.push_back(line.substr(0, width));
            line = indent + line.substr(width);
        }
    }
    if (!line.empty()) out.push_back(line);
}

} // namespace

float drawInfoPanel(SDL_Renderer* r, const sim::World& w, const Selection& sel) {
    if (!sel.valid()) return 0.0f;

    int outW = 0, outH = 0;
    SDL_GetCurrentRenderOutputSize(r, &outW, &outH);
    const float panelW = std::clamp(outW * 0.30f, 380.0f, 520.0f);
    const float x = outW - panelW - 8.0f;
    const float y = 8.0f;
    const float h = outH - 16.0f;
    const auto wrapWidth = static_cast<std::size_t>((panelW - kPad * 2) / kGlyph);

    const InfoText info = describe(w, sel);

    SDL_SetRenderDrawColor(r, 16, 18, 22, 225);
    SDL_FRect box{x, y, panelW, h};
    SDL_RenderFillRect(r, &box);
    SDL_SetRenderDrawColor(r, 96, 102, 112, 230);
    SDL_RenderRect(r, &box);

    float cursor = y + kPad;
    SDL_SetRenderDrawColor(r, 255, 246, 214, 255);
    SDL_RenderDebugText(r, x + kPad, cursor, info.title.c_str());
    cursor += kLineHeight * 1.4f;
    if (!info.subtitle.empty()) {
        SDL_SetRenderDrawColor(r, 176, 182, 190, 255);
        SDL_RenderDebugText(r, x + kPad, cursor, info.subtitle.c_str());
        cursor += kLineHeight * 1.6f;
    }

    std::vector<std::string> wrapped;
    for (const auto& raw : info.lines) {
        if (raw.empty()) { wrapped.emplace_back(); continue; }
        if (raw[0] == '#') { wrapped.push_back(raw); continue; }   // headings never wrap
        wrapInto(raw, wrapWidth, wrapped);
    }

    for (const auto& line : wrapped) {
        if (cursor > y + h - kPad - kLineHeight) {
            SDL_SetRenderDrawColor(r, 150, 150, 150, 200);
            SDL_RenderDebugText(r, x + kPad, cursor, "...");
            break;
        }
        if (line.empty()) { cursor += kLineHeight * 0.6f; continue; }
        if (line[0] == '#') {
            cursor += kLineHeight * 0.3f;
            SDL_SetRenderDrawColor(r, 214, 198, 138, 255);
            SDL_RenderDebugText(r, x + kPad, cursor, line.c_str() + 1);
            cursor += kLineHeight;
            SDL_SetRenderDrawColor(r, 70, 74, 82, 220);
            SDL_RenderLine(r, x + kPad, cursor - 2, x + panelW - kPad, cursor - 2);
            cursor += 2;
            continue;
        }
        SDL_SetRenderDrawColor(r, 224, 224, 220, 255);
        SDL_RenderDebugText(r, x + kPad, cursor, line.c_str());
        cursor += kLineHeight;
    }
    return panelW;
}

} // namespace ui
