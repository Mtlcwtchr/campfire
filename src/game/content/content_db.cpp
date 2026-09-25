#include "game/content/content_db.hpp"

#include <algorithm>
#include <fstream>
#include <algorithm>
#include <set>

#include "nlohmann/json.hpp"

namespace content {
namespace {

using json = nlohmann::json;

// Content authors write plain decimals; the simulation needs Fixed. This is the
// one and only place a double crosses into simulation values, and it happens at
// load time, identically on every machine, before the first tick.
Fixed jsonFixed(const json& j, const char* key, Fixed fallback) {
    if (!j.contains(key)) return fallback;
    const auto& v = j.at(key);
    if (v.is_number_integer()) return Fixed::fromInt(v.get<std::int64_t>());
    if (v.is_number_float()) return Fixed::fromDoubleForContent(v.get<double>());
    return fallback;
}

std::int32_t jsonInt(const json& j, const char* key, std::int32_t fallback) {
    return j.contains(key) ? j.at(key).get<std::int32_t>() : fallback;
}

bool jsonBool(const json& j, const char* key, bool fallback) {
    return j.contains(key) ? j.at(key).get<bool>() : fallback;
}

std::string jsonStr(const json& j, const char* key, std::string fallback = {}) {
    return j.contains(key) ? j.at(key).get<std::string>() : std::move(fallback);
}

std::vector<std::string> jsonStrArray(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key)) for (const auto& e : j.at(key)) out.push_back(e.get<std::string>());
    return out;
}

// Files are visited in sorted order so a content directory always loads in the
// same sequence and DefIds are stable across machines.
std::vector<std::filesystem::path> jsonFilesIn(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// "requires_skill": { "crafting": 12 }. Absent from everything ordinary, which is
// the point: a level is a condition on the exceptional, never on the everyday.
SkillRequirement parseSkillRequirement(const json& j, const std::string& owner,
                                       std::vector<std::string>& errors) {
    SkillRequirement out;
    if (!j.contains("requires_skill")) return out;
    for (auto it = j.at("requires_skill").begin(); it != j.at("requires_skill").end(); ++it) {
        WorkCategory c;
        if (!parseWorkCategory(it.key(), c)) {
            errors.push_back(owner + ": unknown work category " + it.key());
            continue;
        }
        out.category = c;
        out.level = it.value().get<std::int32_t>();
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------

bool ContentDb::load(const std::filesystem::path& root) {
    errors_.clear();
    warnings_.clear();

    if (!std::filesystem::exists(root)) {
        errors_.push_back("content root not found: " + root.string());
        return false;
    }

    loadConfig(root / "config");
    loadKnowledge(root / "knowledge");
    loadItems(root / "items");
    loadRecipes(root / "recipes");
    loadResourceNodes(root / "resources");
    loadBuildings(root / "buildings");
    loadTraits(root / "traits");
    loadCrops(root / "crops");
    loadAnimals(root / "animals");
    loadEthnoi(root / "ethnos");

    resolveReferences();
    return errors_.empty();
}

void ContentDb::loadConfig(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        if (j.contains("time")) {
            const auto& t = j.at("time");
            time_.ticksPerHour = jsonInt(t, "ticks_per_hour", time_.ticksPerHour);
            time_.hoursPerDay = jsonInt(t, "hours_per_day", time_.hoursPerDay);
            time_.daysPerSeason = jsonInt(t, "days_per_season", time_.daysPerSeason);
            time_.seasonsPerYear = jsonInt(t, "seasons_per_year", time_.seasonsPerYear);
        }
        if (j.contains("needs")) {
            const auto& n = j.at("needs");
            sim_.hungerPerHour = jsonFixed(n, "hunger_per_hour", sim_.hungerPerHour);
            sim_.thirstPerHour = jsonFixed(n, "thirst_per_hour", sim_.thirstPerHour);
            sim_.fatiguePerHour = jsonFixed(n, "fatigue_per_hour", sim_.fatiguePerHour);
            sim_.sleepRecoveryPerHour = jsonFixed(n, "sleep_recovery_per_hour", sim_.sleepRecoveryPerHour);
            sim_.hungerEatThreshold = jsonFixed(n, "eat_threshold", sim_.hungerEatThreshold);
            sim_.thirstDrinkThreshold = jsonFixed(n, "drink_threshold", sim_.thirstDrinkThreshold);
            sim_.fatigueSleepThreshold = jsonFixed(n, "sleep_threshold", sim_.fatigueSleepThreshold);
        }
        if (j.contains("work")) {
            const auto& w = j.at("work");
            sim_.baseWorkPerHour = jsonFixed(w, "base_work_per_hour", sim_.baseWorkPerHour);
            sim_.skillWorkBonusPerLevel = jsonFixed(w, "skill_bonus_per_level", sim_.skillWorkBonusPerLevel);
            sim_.maxSkillLevel = jsonInt(w, "max_skill_level", sim_.maxSkillLevel);
            sim_.skillGainPerWorkUnit = jsonFixed(w, "skill_gain_per_work_unit", sim_.skillGainPerWorkUnit);
        }
        if (j.contains("body")) {
            const auto& b = j.at("body");
            sim_.walkSpeedTilesPerHour = jsonFixed(b, "walk_tiles_per_hour", sim_.walkSpeedTilesPerHour);
            sim_.carryCapacityKg = jsonFixed(b, "carry_capacity_kg", sim_.carryCapacityKg);
            sim_.carryPenaltyPerKg = jsonFixed(b, "carry_penalty_per_kg", sim_.carryPenaltyPerKg);
            sim_.adultAge = jsonInt(b, "adult_age", sim_.adultAge);
            sim_.elderAge = jsonInt(b, "elder_age", sim_.elderAge);
            sim_.maxAge = jsonInt(b, "max_age", sim_.maxAge);
            sim_.comfortableTempC = jsonFixed(b, "comfortable_temp_c", sim_.comfortableTempC);
            sim_.hypothermiaTempC = jsonFixed(b, "hypothermia_temp_c", sim_.hypothermiaTempC);
        }
        if (j.contains("farming")) {
            const auto& f = j.at("farming");
            sim_.irrigatedSoilGainPerDay =
                    jsonFixed(f, "irrigated_soil_gain_per_day", sim_.irrigatedSoilGainPerDay);
        }
        if (j.contains("climate")) {
            const auto& c = j.at("climate");
            if (c.contains("season_mid_c") && c.at("season_mid_c").is_array()) {
                const auto& mids = c.at("season_mid_c");
                for (std::size_t i = 0; i < sim_.seasonMidC.size() && i < mids.size(); ++i)
                    sim_.seasonMidC[i] = Fixed::fromDoubleForContent(mids[i].get<double>());
            }
            sim_.dayWarmthC = jsonFixed(c, "day_warmth_c", sim_.dayWarmthC);
            sim_.nightChillC = jsonFixed(c, "night_chill_c", sim_.nightChillC);
        }
    }
}

void ContentDb::loadItems(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            ItemDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": item with no name"); continue; }
            if (itemsByName_.count(d.name)) { errors_.push_back("duplicate item: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);

            if (!parseItemCategory(jsonStr(e, "category", "raw"), d.category))
                errors_.push_back(d.name + ": unknown category " + jsonStr(e, "category"));

            d.massPerUnit = jsonFixed(e, "mass_kg", d.massPerUnit);
            d.stackLimit = jsonInt(e, "stack_limit", d.stackLimit);
            d.nutrition = jsonFixed(e, "nutrition", d.nutrition);
            d.edibleRaw = jsonBool(e, "edible_raw", d.edibleRaw);
            d.rawUnsafe = jsonBool(e, "raw_unsafe", d.rawUnsafe);
            d.spoilDays = jsonInt(e, "spoil_days", d.spoilDays);
            d.cookedSpoilMultiplier = jsonFixed(e, "spoil_multiplier", d.cookedSpoilMultiplier);
            d.toolEfficiency = jsonFixed(e, "tool_efficiency", d.toolEfficiency);
            d.durability = jsonInt(e, "durability", d.durability);
            d.insulation = jsonFixed(e, "insulation", d.insulation);
            d.healsWound = jsonFixed(e, "heals_wound", d.healsWound);
            d.healsSickness = jsonFixed(e, "heals_sickness", d.healsSickness);
            d.containerCapacity = jsonFixed(e, "container_litres", d.containerCapacity);

            for (const auto& g : jsonStrArray(e, "food_groups")) {
                FoodGroup fg;
                if (parseFoodGroup(g, fg)) d.foodGroups.push_back(fg);
                else errors_.push_back(d.name + ": unknown food group " + g);
            }
            const std::string tc = jsonStr(e, "tool_class", "none");
            if (!parseToolClass(tc, d.toolClass)) errors_.push_back(d.name + ": unknown tool class " + tc);

            d.id = DefId{static_cast<std::uint32_t>(items_.size())};
            itemsByName_[d.name] = d.id;
            items_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadKnowledge(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            KnowledgeDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": knowledge with no name"); continue; }
            if (knowledgeByName_.count(d.name)) { errors_.push_back("duplicate knowledge: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);
            const std::string from = jsonStr(e, "discovered_from");
            if (!from.empty() && !parseWorkCategory(from, d.discoveredFrom))
                errors_.push_back(d.name + ": unknown work category " + from);
            d.discoveryChanceDenominator = jsonInt(e, "discovery_one_in", 0);
            d.requiresKnowledge = jsonStrArray(e, "requires");

            d.id = DefId{static_cast<std::uint32_t>(knowledge_.size())};
            knowledgeByName_[d.name] = d.id;
            knowledge_.push_back(std::move(d));
        }
    }
}

namespace {
std::vector<IngredientSpec> parseIngredients(const json& j, const char* key,
                                             const std::unordered_map<std::string, DefId>& items,
                                             const std::string& owner,
                                             std::vector<std::string>& errors) {
    std::vector<IngredientSpec> out;
    if (!j.contains(key)) return out;
    for (const auto& e : j.at(key)) {
        const std::string item = e.at("item").get<std::string>();
        auto it = items.find(item);
        if (it == items.end()) { errors.push_back(owner + ": unknown item " + item); continue; }
        out.push_back({it->second, e.contains("count") ? e.at("count").get<std::int32_t>() : 1});
    }
    return out;
}
} // namespace

void ContentDb::loadRecipes(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            RecipeDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": recipe with no name"); continue; }
            if (recipesByName_.count(d.name)) { errors_.push_back("duplicate recipe: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);

            const std::string cat = jsonStr(e, "category", "crafting");
            if (!parseWorkCategory(cat, d.category)) errors_.push_back(d.name + ": unknown category " + cat);

            d.inputs = parseIngredients(e, "inputs", itemsByName_, d.name, errors_);
            d.outputs = parseIngredients(e, "outputs", itemsByName_, d.name, errors_);
            if (d.outputs.empty()) errors_.push_back(d.name + ": recipe produces nothing");

            d.workAmount = jsonFixed(e, "work", d.workAmount);
            const std::string tool = jsonStr(e, "tool", "none");
            if (!parseToolClass(tool, d.requiredTool)) errors_.push_back(d.name + ": unknown tool " + tool);
            d.requiredWorkplace = jsonStr(e, "workplace");
            d.requiredKnowledge = jsonStr(e, "knowledge");
            d.requiredSkill = parseSkillRequirement(e, d.name, errors_);
            d.waterLitres = jsonFixed(e, "water_litres", d.waterLitres);

            d.id = DefId{static_cast<std::uint32_t>(recipes_.size())};
            recipesByName_[d.name] = d.id;
            recipes_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadResourceNodes(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            ResourceNodeDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": resource with no name"); continue; }
            if (resourceNodesByName_.count(d.name)) { errors_.push_back("duplicate resource: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);

            const std::string kind = jsonStr(e, "kind", "bush");
            if (!parseResourceKind(kind, d.kind)) errors_.push_back(d.name + ": unknown kind " + kind);

            d.consumedOnHarvest = jsonBool(e, "consumed_on_harvest", d.consumedOnHarvest);
            d.clearWork = jsonFixed(e, "clear_work", d.clearWork);
            d.spreadOneIn = jsonInt(e, "spread_one_in", d.spreadOneIn);
            d.keepStandingPercent = jsonInt(e, "keep_standing_percent", d.keepStandingPercent);
            d.regrowDays = jsonInt(e, "regrow_days", d.regrowDays);
            d.seasons = jsonStrArray(e, "seasons");
            d.blocksMovement = jsonBool(e, "blocks_movement", d.blocksMovement);

            const auto readSpec = [&](const json& h, HarvestSpec& spec, const char* defaultCategory) {
                spec.yields = parseIngredients(h, "yields", itemsByName_, d.name, errors_);
                spec.workAmount = jsonFixed(h, "work", spec.workAmount);
                const std::string req = jsonStr(h, "tool", "none");
                if (!parseToolClass(req, spec.requiredTool)) errors_.push_back(d.name + ": unknown tool " + req);
                const std::string pref = jsonStr(h, "preferred_tool", "none");
                if (!parseToolClass(pref, spec.preferredTool)) errors_.push_back(d.name + ": unknown tool " + pref);
                spec.bareHandPenalty = jsonFixed(h, "bare_hand_penalty", spec.bareHandPenalty);
                const std::string cat = jsonStr(h, "category", defaultCategory);
                if (!parseWorkCategory(cat, spec.category)) errors_.push_back(d.name + ": unknown category " + cat);
                spec.requiredKnowledge = jsonStr(h, "knowledge");
            };
            if (e.contains("harvest")) readSpec(e.at("harvest"), d.harvest, "foraging");
            if (e.contains("fell")) readSpec(e.at("fell"), d.fell, "woodcutting");

            d.id = DefId{static_cast<std::uint32_t>(resourceNodes_.size())};
            resourceNodesByName_[d.name] = d.id;
            resourceNodes_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadBuildings(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            BuildingDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": building with no name"); continue; }
            if (buildingsByName_.count(d.name)) { errors_.push_back("duplicate building: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);

            const std::string kind = jsonStr(e, "kind", "other");
            if (!parseBuildingKind(kind, d.kind)) errors_.push_back(d.name + ": unknown kind " + kind);

            // "footprint": [w, d]. "radius" is still read for anything that has
            // not been converted: radius r covered 2r+1 tiles each way.
            if (e.contains("footprint") && e["footprint"].is_array() && e["footprint"].size() == 2) {
                d.footprintWidth = std::max(1, e["footprint"][0].get<std::int32_t>());
                d.footprintDepth = std::max(1, e["footprint"][1].get<std::int32_t>());
            } else {
                const std::int32_t radius = jsonInt(e, "radius", 0);
                d.footprintWidth = d.footprintDepth = 2 * radius + 1;
            }
            d.blocksMovement = jsonBool(e, "blocks_movement", d.blocksMovement);
            d.sheltered = jsonBool(e, "sheltered", d.sheltered);
            d.materials = parseIngredients(e, "materials", itemsByName_, d.name, errors_);
            d.workAmount = jsonFixed(e, "work", d.workAmount);
            const std::string tool = jsonStr(e, "tool", "none");
            if (!parseToolClass(tool, d.requiredTool)) errors_.push_back(d.name + ": unknown tool " + tool);
            d.requiredKnowledge = jsonStr(e, "knowledge");
            d.requiredSkill = parseSkillRequirement(e, d.name, errors_);

            d.sleepingSlots = jsonInt(e, "sleeping_slots", d.sleepingSlots);
            d.warmthBonus = jsonFixed(e, "warmth_bonus", d.warmthBonus);
            d.comfortBonus = jsonFixed(e, "comfort_bonus", d.comfortBonus);
            d.firmGround = jsonBool(e, "firm_ground", d.firmGround);
            d.maxPerSettlement = jsonInt(e, "max_per_settlement", d.maxPerSettlement);
            d.workerSlots = std::max(1, jsonInt(e, "worker_slots", d.workerSlots));
            d.familySlots = std::max(1, jsonInt(e, "family_slots", d.familySlots));
            d.salvageShare = jsonFixed(e, "salvage", d.salvageShare);
            d.replaces = jsonStr(e, "replaces");
            d.animalSlots = jsonInt(e, "animal_slots", d.animalSlots);
            d.storageSlots = jsonInt(e, "storage_slots", d.storageSlots);
            d.irrigationRadius = jsonInt(e, "irrigates", d.irrigationRadius);
            d.irrigationBonus = jsonFixed(e, "irrigation_bonus", Fixed::ratio(2, 5));
            d.spoilRateMultiplier = jsonFixed(e, "spoil_multiplier", d.spoilRateMultiplier);
            d.providesHeat = jsonBool(e, "provides_heat", d.providesHeat);
            d.providesFire = jsonBool(e, "provides_fire", d.providesFire);
            d.function = jsonStr(e, "function");
            d.nearArea = jsonStr(e, "near");

            for (const auto& c : jsonStrArray(e, "accepts")) {
                ItemCategory ic;
                if (parseItemCategory(c, ic)) d.acceptsCategories.push_back(ic);
                else errors_.push_back(d.name + ": unknown item category " + c);
            }

            d.id = DefId{static_cast<std::uint32_t>(buildings_.size())};
            buildingsByName_[d.name] = d.id;
            buildings_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadTraits(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            TraitDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": trait with no name"); continue; }
            if (traitsByName_.count(d.name)) { errors_.push_back("duplicate trait: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);
            d.heritable = jsonBool(e, "heritable", d.heritable);
            d.innateChanceDenominator = jsonInt(e, "innate_one_in", 0);
            d.heritableChanceNumerator = jsonInt(e, "heritable_numerator", 1);
            d.heritableChanceDenominator = jsonInt(e, "heritable_denominator", 2);
            d.excludes = jsonStrArray(e, "excludes");
            d.strengthMod = jsonFixed(e, "strength", d.strengthMod);
            d.enduranceMod = jsonFixed(e, "endurance", d.enduranceMod);
            d.dexterityMod = jsonFixed(e, "dexterity", d.dexterityMod);
            d.learnRateMod = jsonFixed(e, "learn_rate", d.learnRateMod);
            d.discoveryMod = jsonFixed(e, "discovery", d.discoveryMod);
            if (e.contains("work_affinity")) {
                for (auto it = e.at("work_affinity").begin(); it != e.at("work_affinity").end(); ++it) {
                    WorkCategory c;
                    if (!parseWorkCategory(it.key(), c)) { errors_.push_back(d.name + ": unknown category " + it.key()); continue; }
                    const auto& v = it.value();
                    Fixed f = v.is_number_integer() ? Fixed::fromInt(v.get<std::int64_t>())
                                                    : Fixed::fromDoubleForContent(v.get<double>());
                    d.workAffinity.emplace_back(c, f);
                }
            }

            d.id = DefId{static_cast<std::uint32_t>(traits_.size())};
            traitsByName_[d.name] = d.id;
            traits_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadCrops(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            CropDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": crop with no name"); continue; }
            if (cropsByName_.count(d.name)) { errors_.push_back("duplicate crop: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);
            d.seed = jsonStr(e, "seed");
            d.harvest = jsonStr(e, "harvest");
            d.harvestCount = jsonInt(e, "harvest_count", d.harvestCount);
            d.seedCount = jsonInt(e, "seed_count", d.seedCount);
            d.growDays = jsonInt(e, "grow_days", d.growDays);
            d.sowSeasons = jsonStrArray(e, "sow_seasons");
            d.tillWork = jsonFixed(e, "till_work", d.tillWork);
            d.sowWork = jsonFixed(e, "sow_work", d.sowWork);
            d.harvestWork = jsonFixed(e, "harvest_work", d.harvestWork);
            d.bareHandPenalty = jsonFixed(e, "bare_hand_penalty", d.bareHandPenalty);
            const std::string tool = jsonStr(e, "till_tool", "none");
            if (!parseToolClass(tool, d.tillTool)) errors_.push_back(d.name + ": unknown tool " + tool);
            d.requiredKnowledge = jsonStr(e, "knowledge");

            d.id = DefId{static_cast<std::uint32_t>(crops_.size())};
            cropsByName_[d.name] = d.id;
            crops_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadAnimals(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            AnimalDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": animal with no name"); continue; }
            if (animalsByName_.count(d.name)) { errors_.push_back("duplicate animal: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);
            d.adultAgeDays = jsonInt(e, "adult_age_days", d.adultAgeDays);
            d.maxAgeDays = jsonInt(e, "max_age_days", d.maxAgeDays);
            d.shearIntervalDays = jsonInt(e, "shear_interval_days", d.shearIntervalDays);
            d.milkIntervalDays = jsonInt(e, "milk_interval_days", d.milkIntervalDays);
            d.breedIntervalDays = jsonInt(e, "breed_interval_days", d.breedIntervalDays);
            d.breedingStock = jsonInt(e, "breeding_stock", d.breedingStock);
            d.wild = jsonBool(e, "wild", d.wild);
            d.predator = jsonBool(e, "predator", d.predator);
            d.tamesInto = jsonStr(e, "tames_into");
            d.guardsFlock = jsonBool(e, "guards_flock", d.guardsFlock);
            d.wildCarryingCapacity = jsonInt(e, "wild_capacity", d.wildCarryingCapacity);
            d.sizeMetres = jsonFixed(e, "size_metres", d.sizeMetres);
            d.huntWork = jsonFixed(e, "hunt_work", d.huntWork);
            if (e.contains("hunt_tool")) parseToolClass(jsonStr(e, "hunt_tool"), d.huntTool);
            d.huntYields = parseIngredients(e, "hunt", itemsByName_, d.name, errors_);
            d.twinOneIn = jsonInt(e, "twin_one_in", d.twinOneIn);
            d.shearYields = parseIngredients(e, "shear", itemsByName_, d.name, errors_);
            d.milkYields = parseIngredients(e, "milk", itemsByName_, d.name, errors_);
            d.slaughterYields = parseIngredients(e, "slaughter", itemsByName_, d.name, errors_);
            d.shearWork = jsonFixed(e, "shear_work", d.shearWork);
            d.milkWork = jsonFixed(e, "milk_work", d.milkWork);
            d.slaughterWork = jsonFixed(e, "slaughter_work", d.slaughterWork);
            d.requiredKnowledge = jsonStr(e, "knowledge");

            d.id = DefId{static_cast<std::uint32_t>(animals_.size())};
            animalsByName_[d.name] = d.id;
            animals_.push_back(std::move(d));
        }
    }
}

void ContentDb::loadEthnoi(const std::filesystem::path& dir) {
    for (const auto& path : jsonFilesIn(dir)) {
        std::ifstream in(path);
        json j;
        try { in >> j; } catch (const std::exception& e) {
            errors_.push_back(path.string() + ": " + e.what());
            continue;
        }
        for (const auto& e : j) {
            EthnosDef d;
            d.name = jsonStr(e, "name");
            if (d.name.empty()) { errors_.push_back(path.string() + ": ethnos with no name"); continue; }
            if (ethnoiByName_.count(d.name)) { errors_.push_back("duplicate ethnos: " + d.name); continue; }
            d.label = jsonStr(e, "label", d.name);
            d.language = jsonStr(e, "language", d.name);
            d.commonKnowledge = jsonStrArray(e, "common_knowledge");
            d.familyKnowledgePool = jsonStrArray(e, "family_knowledge_pool");
            d.familyKnowledgeDraws = jsonInt(e, "family_knowledge_draws", d.familyKnowledgeDraws);
            d.preferredFoods = jsonStrArray(e, "preferred_foods");
            d.architectureSet = jsonStr(e, "architecture", "default");
            d.biome = jsonStr(e, "biome", d.biome);
            d.baseSkill = jsonInt(e, "base_skill", d.baseSkill);
            d.specialistSkill = jsonInt(e, "specialist_skill", d.specialistSkill);
            d.trades = jsonStrArray(e, "trades");
            d.crops = jsonStrArray(e, "crops");
            if (e.contains("starting_goods"))
                for (auto it = e.at("starting_goods").begin(); it != e.at("starting_goods").end(); ++it)
                    d.startingGoods.emplace_back(it.key(), it.value().get<std::int32_t>());
            if (e.contains("starting_livestock"))
                for (auto it = e.at("starting_livestock").begin(); it != e.at("starting_livestock").end(); ++it)
                    d.startingLivestock.emplace_back(it.key(), it.value().get<std::int32_t>());

            d.id = DefId{static_cast<std::uint32_t>(ethnoi_.size())};
            ethnoiByName_[d.name] = d.id;
            ethnoi_.push_back(std::move(d));
        }
    }
}

void ContentDb::resolveReferences() {
    auto resolveKnowledge = [&](const std::string& n, const std::string& owner) -> DefId {
        if (n.empty()) return DefId{};
        auto it = knowledgeByName_.find(n);
        if (it == knowledgeByName_.end()) { errors_.push_back(owner + ": unknown knowledge " + n); return DefId{}; }
        return it->second;
    };

    for (auto& r : recipes_) {
        r.knowledgeDef = resolveKnowledge(r.requiredKnowledge, r.name);
        if (!r.requiredWorkplace.empty()) {
            for (const auto& b : buildings_)
                if (b.name == r.requiredWorkplace || b.function == r.requiredWorkplace)
                    r.workplaceDefs.push_back(b.id);
            if (r.workplaceDefs.empty())
                errors_.push_back(r.name + ": no building serves as " + r.requiredWorkplace);
        }
    }
    for (auto& b : buildings_) b.knowledgeDef = resolveKnowledge(b.requiredKnowledge, b.name);
    for (auto& b : buildings_)
        if (!b.replaces.empty()) b.replacesDef = buildingByName(b.replaces);

    for (auto& c : crops_) {
        c.knowledgeDef = resolveKnowledge(c.requiredKnowledge, c.name);
        auto seed = itemsByName_.find(c.seed);
        if (seed == itemsByName_.end()) errors_.push_back(c.name + ": unknown seed item " + c.seed);
        else c.seedItem = seed->second;
        auto harvest = itemsByName_.find(c.harvest);
        if (harvest == itemsByName_.end()) errors_.push_back(c.name + ": unknown harvest item " + c.harvest);
        else c.harvestItem = harvest->second;
    }
    for (auto& a : animals_) a.knowledgeDef = resolveKnowledge(a.requiredKnowledge, a.name);
    for (auto& n : resourceNodes_) n.harvest.knowledgeDef = resolveKnowledge(n.harvest.requiredKnowledge, n.name);

    for (const auto& k : knowledge_)
        for (const auto& req : k.requiresKnowledge)
            if (!knowledgeByName_.count(req)) errors_.push_back(k.name + ": unknown prerequisite " + req);

    for (const auto& t : traits_)
        for (const auto& x : t.excludes)
            if (!traitsByName_.count(x)) errors_.push_back(t.name + ": excludes unknown trait " + x);

    for (const auto& eth : ethnoi_) {
        for (const auto& k : eth.commonKnowledge)
            if (!knowledgeByName_.count(k)) errors_.push_back(eth.name + ": unknown common knowledge " + k);
        for (const auto& k : eth.familyKnowledgePool)
            if (!knowledgeByName_.count(k)) errors_.push_back(eth.name + ": unknown family knowledge " + k);
        for (const auto& f : eth.preferredFoods)
            if (!itemsByName_.count(f)) warnings_.push_back(eth.name + ": preferred food " + f + " does not exist");
        for (const auto& t : eth.trades) {
            WorkCategory c;
            if (!parseWorkCategory(t, c)) errors_.push_back(eth.name + ": unknown trade " + t);
        }
        for (const auto& c : eth.crops)
            if (!cropsByName_.count(c)) errors_.push_back(eth.name + ": unknown crop " + c);
        for (const auto& [a, n] : eth.startingLivestock) {
            if (!animalsByName_.count(a)) errors_.push_back(eth.name + ": unknown animal " + a);
            if (n <= 0) errors_.push_back(eth.name + ": non-positive livestock count for " + a);
        }
    }
}

// ---------------------------------------------------------------------------
// GDD 7: there must be at least one acyclic path from bare hands to a stable
// life. This is a closure: start from what the terrain offers and what the
// culture already knows, then keep applying every recipe and building whose
// prerequisites are already satisfied until nothing new becomes reachable.
// ---------------------------------------------------------------------------
ContentDb::Reachable ContentDb::reachableFrom(const std::vector<DefId>& startingResourceNodes,
                                              const std::vector<std::string>& startingKnowledge) const {
    Reachable out;
    out.item.assign(items_.size(), false);
    out.building.assign(buildings_.size(), false);

    std::set<std::uint32_t> haveKnowledge;
    std::set<std::uint32_t> haveToolClass;
    haveToolClass.insert(static_cast<std::uint32_t>(ToolClass::None));

    for (const auto& kn : startingKnowledge) {
        DefId id = knowledgeByName(kn);
        if (id.valid()) haveKnowledge.insert(id.value);
        else out.problems.push_back("starting knowledge does not exist: " + kn);
    }

    auto knowledgeOk = [&](DefId k) { return !k.valid() || haveKnowledge.count(k.value) > 0; };
    auto toolOk = [&](ToolClass t) { return haveToolClass.count(static_cast<std::uint32_t>(t)) > 0; };

    // Anything a new item is: a tool class the community can now wield, and a
    // reason to revisit every harvest and recipe that was blocked on it.
    auto gainItem = [&](DefId item, bool& changed) {
        if (!item.valid() || item.value >= out.item.size() || out.item[item.value]) return;
        out.item[item.value] = true;
        changed = true;
        const auto& def = items_[item.value];
        if (def.toolClass != ToolClass::None) haveToolClass.insert(static_cast<std::uint32_t>(def.toolClass));
    };

    bool changed = true;
    while (changed) {
        changed = false;

        // Harvesting is re-checked every round, not only at the start: making the
        // first axe is exactly what turns a standing tree into reachable timber.
        for (DefId nodeId : startingResourceNodes) {
            if (!nodeId.valid() || nodeId.value >= resourceNodes_.size()) continue;
            const auto& node = resourceNodes_[nodeId.value];
            if (!knowledgeOk(node.harvest.knowledgeDef)) continue;
            if (!toolOk(node.harvest.requiredTool)) continue;
            for (const auto& y : node.harvest.yields) gainItem(y.item, changed);
        }

        // A discoverable method becomes reachable once its prerequisites are.
        for (const auto& k : knowledge_) {
            if (haveKnowledge.count(k.id.value)) continue;
            if (k.discoveredFrom == WorkCategory::Count) continue;
            bool prereqs = true;
            for (const auto& req : k.requiresKnowledge) {
                DefId r = knowledgeByName(req);
                if (!r.valid() || !haveKnowledge.count(r.value)) { prereqs = false; break; }
            }
            if (prereqs) { haveKnowledge.insert(k.id.value); changed = true; }
        }

        // A sown crop is a source of items just as a wild stand is, and so is a
        // flock: without this the validator calls bread unreachable in a culture
        // that has grown grain for a thousand years.
        for (const auto& c : crops_) {
            if (!knowledgeOk(c.knowledgeDef)) continue;
            if (!c.seedItem.valid() || !out.item[c.seedItem.value]) continue;
            gainItem(c.harvestItem, changed);
        }
        for (const auto& a : animals_) {
            if (!knowledgeOk(a.knowledgeDef)) continue;
            for (const auto& y : a.shearYields) gainItem(y.item, changed);
            for (const auto& y : a.milkYields) gainItem(y.item, changed);
            for (const auto& y : a.slaughterYields) gainItem(y.item, changed);
        }

        for (const auto& b : buildings_) {
            if (out.building[b.id.value]) continue;
            if (!knowledgeOk(b.knowledgeDef) || !toolOk(b.requiredTool)) continue;
            bool ok = true;
            for (const auto& m : b.materials) if (!out.item[m.item.value]) { ok = false; break; }
            if (!ok) continue;
            out.building[b.id.value] = true;
            changed = true;
        }

        for (const auto& r : recipes_) {
            if (!knowledgeOk(r.knowledgeDef) || !toolOk(r.requiredTool)) continue;
            if (!r.workplaceDefs.empty()) {
                bool haveOne = false;
                for (DefId b : r.workplaceDefs) if (out.building[b.value]) { haveOne = true; break; }
                if (!haveOne) continue;
            }
            bool ok = true;
            for (const auto& in : r.inputs) if (!out.item[in.item.value]) { ok = false; break; }
            if (!ok) continue;
            for (const auto& o : r.outputs) gainItem(o.item, changed);
        }
    }
    return out;
}

bool ContentDb::validateEthnos(DefId ethnosId, const std::vector<DefId>& biomeResourceNodes,
                               std::vector<std::string>& problems) const {
    if (!ethnosId.valid() || ethnosId.value >= ethnoi_.size()) return false;
    const auto& eth = ethnoi_[ethnosId.value];

    std::vector<std::string> knowledge = eth.commonKnowledge;
    // The family pool is drawn from at world creation, so for "can this culture
    // get there at all" it counts as available.
    for (const auto& k : eth.familyKnowledgePool) knowledge.push_back(k);

    const Reachable r = reachableFrom(biomeResourceNodes, knowledge);
    const std::size_t before = problems.size();
    for (const auto& p : r.problems) problems.push_back(p);

    // Something edible without a tool or a building: the first day has to be
    // survivable.
    bool immediateFood = false;
    for (DefId nodeId : biomeResourceNodes) {
        if (!nodeId.valid() || nodeId.value >= resourceNodes_.size()) continue;
        const auto& node = resourceNodes_[nodeId.value];
        if (node.harvest.requiredTool != ToolClass::None) continue;
        if (node.harvest.knowledgeDef.valid()) {
            const auto& name = knowledge_[node.harvest.knowledgeDef.value].name;
            if (std::find(knowledge.begin(), knowledge.end(), name) == knowledge.end()) continue;
        }
        for (const auto& y : node.harvest.yields) {
            const auto& it = items_[y.item.value];
            if (it.category == ItemCategory::Food && it.edibleRaw && !it.rawUnsafe) immediateFood = true;
        }
    }
    if (!immediateFood) problems.push_back(eth.name + ": nothing edible on day one without a tool");

    auto needsBuilding = [&](BuildingKind kind, const char* what) {
        for (const auto& b : buildings_)
            if (b.kind == kind && r.canBuild(b.id)) return;
        problems.push_back(eth.name + ": cannot ever build " + what);
    };
    needsBuilding(BuildingKind::Housing, "anywhere to sleep");
    needsBuilding(BuildingKind::Storage, "anywhere to keep food");
    needsBuilding(BuildingKind::Hearth, "a fire");

    // An edge of some kind. Everything downstream of it depends on it.
    bool anyTool = false;
    for (const auto& it : items_)
        if (it.toolClass != ToolClass::None && r.has(it.id)) anyTool = true;
    if (!anyTool) problems.push_back(eth.name + ": cannot make a single tool");

    // Whatever this culture sows, it must be able to sow and to use.
    for (const auto& cropName : eth.crops) {
        const DefId id = cropByName(cropName);
        if (!id.valid()) continue;
        const auto& crop = crops_[id.value];
        if (!r.has(crop.seedItem))
            problems.push_back(eth.name + ": no way to get seed for " + crop.name);
        if (!r.has(crop.harvestItem))
            problems.push_back(eth.name + ": nothing comes of sowing " + crop.name);
    }
    // And whatever clothing exists must be reachable, or winter is fatal.
    bool anyClothing = false;
    for (const auto& it : items_)
        if (it.category == ItemCategory::Clothing && r.has(it.id)) anyClothing = true;
    if (!anyClothing) problems.push_back(eth.name + ": cannot clothe anybody");

    return problems.size() == before;
}

} // namespace content
