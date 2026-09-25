#include "game/content/defs.hpp"

#include <array>

namespace content {
namespace {

// One table per enum, indexed by the enum value, so a name and its value can
// never drift apart the way two parallel switch statements can.
constexpr std::array<std::string_view, kWorkCategoryCount> kWorkCategoryNames{
    "hauling", "construction", "woodcutting", "mining", "foraging", "farming",
    "hunting", "fishing", "herding", "crafting", "cooking", "water_carrying",
    "cleaning", "medical", "childcare", "ritual", "patrol", "scouting", "teaching",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(ItemCategory::Count)> kItemCategoryNames{
    "food", "liquid", "raw", "tool", "weapon", "clothing", "building_material", "component", "waste",
};

constexpr std::array<std::string_view, kFoodGroupCount> kFoodGroupNames{
    "grain", "vegetable", "fruit", "meat", "fish", "dairy", "fat",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(ToolClass::Count)> kToolClassNames{
    "none", "axe", "pick", "knife", "hoe", "hammer", "spear", "bow", "needle", "quern", "vessel",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(ResourceKind::Count)> kResourceKindNames{
    "tree", "rock", "bush", "wild_plant", "water_source", "game",
};

constexpr std::array<std::string_view, static_cast<std::size_t>(BuildingKind::Count)> kBuildingKindNames{
    "housing", "storage", "workshop", "hearth", "fortification", "other",
};

template <typename Enum, std::size_t N>
bool parseFrom(const std::array<std::string_view, N>& names, std::string_view s, Enum& out) {
    for (std::size_t i = 0; i < N; ++i) {
        if (names[i] == s) { out = static_cast<Enum>(i); return true; }
    }
    return false;
}

template <typename Enum, std::size_t N>
std::string_view nameFrom(const std::array<std::string_view, N>& names, Enum e) {
    const auto i = static_cast<std::size_t>(e);
    return i < N ? names[i] : "unknown";
}

} // namespace

std::string_view workCategoryName(WorkCategory c) { return nameFrom(kWorkCategoryNames, c); }
bool parseWorkCategory(std::string_view s, WorkCategory& out) { return parseFrom(kWorkCategoryNames, s, out); }

std::string_view itemCategoryName(ItemCategory c) { return nameFrom(kItemCategoryNames, c); }
bool parseItemCategory(std::string_view s, ItemCategory& out) { return parseFrom(kItemCategoryNames, s, out); }

std::string_view foodGroupName(FoodGroup g) { return nameFrom(kFoodGroupNames, g); }
bool parseFoodGroup(std::string_view s, FoodGroup& out) { return parseFrom(kFoodGroupNames, s, out); }

std::string_view toolClassName(ToolClass c) { return nameFrom(kToolClassNames, c); }
bool parseToolClass(std::string_view s, ToolClass& out) { return parseFrom(kToolClassNames, s, out); }

std::string_view resourceKindName(ResourceKind k) { return nameFrom(kResourceKindNames, k); }
bool parseResourceKind(std::string_view s, ResourceKind& out) { return parseFrom(kResourceKindNames, s, out); }

std::string_view buildingKindName(BuildingKind k) { return nameFrom(kBuildingKindNames, k); }
bool parseBuildingKind(std::string_view s, BuildingKind& out) { return parseFrom(kBuildingKindNames, s, out); }

} // namespace content
