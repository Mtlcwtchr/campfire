#pragma once
// Loads every JSON file under content/ into immutable tables and resolves the
// name references between them into DefIds.

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "game/content/defs.hpp"
#include "engine/core/time.hpp"

namespace content {

struct SimConfig {
    // Needs. Rates are per game hour; the simulation scales them by ticksPerHour.
    Fixed hungerPerHour = Fixed::ratio(1, 16);       // full to starving in ~16h of neglect
    Fixed thirstPerHour = Fixed::ratio(1, 25);
    Fixed fatiguePerHour = Fixed::ratio(1, 18);
    Fixed sleepRecoveryPerHour = Fixed::ratio(1, 6);
    Fixed hungerEatThreshold = Fixed::ratio(13, 20); // leaves time for a physical trip to food/home
    Fixed thirstDrinkThreshold = Fixed::ratio(13, 20);
    Fixed fatigueSleepThreshold = Fixed::ratio(1, 4);

    // Movement, in tiles per game hour.
    Fixed walkSpeedTilesPerHour = Fixed::fromInt(55);
    Fixed carryPenaltyPerKg = Fixed::ratio(1, 200);

    // Work. One pawn at skill 0 contributes this many work units per game hour.
    Fixed baseWorkPerHour = Fixed::fromInt(110);
    Fixed skillWorkBonusPerLevel = Fixed::ratio(1, 10);   // +10% per level
    std::int32_t maxSkillLevel = 20;
    Fixed skillGainPerWorkUnit = Fixed::ratio(1, 400);

    // Carrying.
    Fixed carryCapacityKg = Fixed::fromInt(25);

    // Age stages, in years.
    std::int32_t adultAge = 16;
    std::int32_t elderAge = 50;
    std::int32_t maxAge = 80;

    // Health / temperature.
    Fixed comfortableTempC = Fixed::fromInt(20);
    Fixed hypothermiaTempC = Fixed::fromInt(5);

    // What watered ground gains a day, as a fraction of full fertility. Water
    // led onto poor soil does not only help this year's crop: it lays silt, it
    // washes the salt down, and the ground itself becomes better. This is what
    // makes a canal worth its plot - the improvement stays.
    Fixed irrigatedSoilGainPerDay = Fixed::ratio(1, 200);

    // The climate the map sits in: the middle of each season in degrees, and how
    // far the day swings above it and the night below. A river valley in the
    // Fertile Crescent is not the Anatolian plateau, and while it was - winter
    // averaging one degree against a hypothermia line of five - the cold ran the
    // whole season with no let-up, health could never recover, and a settlement
    // lost a third of its people every winter (D82).
    std::array<Fixed, 4> seasonMidC{Fixed::fromInt(18), Fixed::fromInt(32), Fixed::fromInt(21),
                                    Fixed::fromInt(9)};
    Fixed dayWarmthC = Fixed::fromInt(4);
    Fixed nightChillC = Fixed::fromInt(5);
};

class ContentDb {
public:
    bool load(const std::filesystem::path& root);

    const std::vector<std::string>& errors() const { return errors_; }
    const std::vector<std::string>& warnings() const { return warnings_; }

    const core::TimeConfig& time() const { return time_; }
    const SimConfig& sim() const { return sim_; }

    const std::vector<ItemDef>& items() const { return items_; }
    const std::vector<RecipeDef>& recipes() const { return recipes_; }
    const std::vector<ResourceNodeDef>& resourceNodes() const { return resourceNodes_; }
    const std::vector<BuildingDef>& buildings() const { return buildings_; }
    const std::vector<KnowledgeDef>& knowledge() const { return knowledge_; }
    const std::vector<EthnosDef>& ethnoi() const { return ethnoi_; }
    const std::vector<TraitDef>& traits() const { return traits_; }
    const std::vector<CropDef>& crops() const { return crops_; }
    const std::vector<AnimalDef>& animals() const { return animals_; }

    const ItemDef& item(DefId id) const { return items_[id.value]; }
    const RecipeDef& recipe(DefId id) const { return recipes_[id.value]; }
    const ResourceNodeDef& resourceNode(DefId id) const { return resourceNodes_[id.value]; }
    const BuildingDef& building(DefId id) const { return buildings_[id.value]; }
    const KnowledgeDef& knowledge(DefId id) const { return knowledge_[id.value]; }
    const EthnosDef& ethnos(DefId id) const { return ethnoi_[id.value]; }
    const TraitDef& trait(DefId id) const { return traits_[id.value]; }
    const CropDef& crop(DefId id) const { return crops_[id.value]; }
    const AnimalDef& animal(DefId id) const { return animals_[id.value]; }

    DefId itemByName(std::string_view n) const { return lookup(itemsByName_, n); }
    DefId recipeByName(std::string_view n) const { return lookup(recipesByName_, n); }
    DefId resourceNodeByName(std::string_view n) const { return lookup(resourceNodesByName_, n); }
    DefId buildingByName(std::string_view n) const { return lookup(buildingsByName_, n); }
    DefId knowledgeByName(std::string_view n) const { return lookup(knowledgeByName_, n); }
    DefId ethnosByName(std::string_view n) const { return lookup(ethnoiByName_, n); }
    DefId traitByName(std::string_view n) const { return lookup(traitsByName_, n); }
    DefId cropByName(std::string_view n) const { return lookup(cropsByName_, n); }
    DefId animalByName(std::string_view n) const { return lookup(animalsByName_, n); }

    // What a community standing on this ground, knowing these methods and holding
    // nothing at all, can eventually get to (GDD 7, the zero production chain).
    struct Reachable {
        std::vector<bool> item;         // indexed by item DefId
        std::vector<bool> building;
        std::vector<std::string> problems;   // named things that do not exist

        bool has(DefId id) const { return id.valid() && id.value < item.size() && item[id.value]; }
        bool canBuild(DefId id) const {
            return id.valid() && id.value < building.size() && building[id.value];
        }
    };
    Reachable reachableFrom(const std::vector<DefId>& startingResourceNodes,
                            const std::vector<std::string>& startingKnowledge) const;

    // Can this culture, on the ground its biome offers, get to the things a
    // community cannot live without: something to eat, somewhere to sleep,
    // somewhere to keep food, a fire, and an edge? Cultures differ in what they
    // build - a marsh people never touches an oak - so this asks about the
    // essentials rather than about every definition in the tree.
    bool validateEthnos(DefId ethnos, const std::vector<DefId>& biomeResourceNodes,
                        std::vector<std::string>& problems) const;

private:
    static DefId lookup(const std::unordered_map<std::string, DefId>& m, std::string_view n) {
        auto it = m.find(std::string(n));
        return it == m.end() ? DefId{} : it->second;
    }

    void loadConfig(const std::filesystem::path& root);
    void loadItems(const std::filesystem::path& dir);
    void loadKnowledge(const std::filesystem::path& dir);
    void loadRecipes(const std::filesystem::path& dir);
    void loadResourceNodes(const std::filesystem::path& dir);
    void loadBuildings(const std::filesystem::path& dir);
    void loadEthnoi(const std::filesystem::path& dir);
    void loadTraits(const std::filesystem::path& dir);
    void loadCrops(const std::filesystem::path& dir);
    void loadAnimals(const std::filesystem::path& dir);
    void resolveReferences();

    core::TimeConfig time_;
    SimConfig sim_;

    std::vector<ItemDef> items_;
    std::vector<RecipeDef> recipes_;
    std::vector<ResourceNodeDef> resourceNodes_;
    std::vector<BuildingDef> buildings_;
    std::vector<KnowledgeDef> knowledge_;
    std::vector<EthnosDef> ethnoi_;
    std::vector<TraitDef> traits_;
    std::vector<CropDef> crops_;
    std::vector<AnimalDef> animals_;

    std::unordered_map<std::string, DefId> itemsByName_;
    std::unordered_map<std::string, DefId> recipesByName_;
    std::unordered_map<std::string, DefId> resourceNodesByName_;
    std::unordered_map<std::string, DefId> buildingsByName_;
    std::unordered_map<std::string, DefId> knowledgeByName_;
    std::unordered_map<std::string, DefId> ethnoiByName_;
    std::unordered_map<std::string, DefId> traitsByName_;
    std::unordered_map<std::string, DefId> cropsByName_;
    std::unordered_map<std::string, DefId> animalsByName_;

    std::vector<std::string> errors_;
    std::vector<std::string> warnings_;
};

} // namespace content
