#include "game/world/soil.hpp"
#include "game/world/height_field.hpp"
#include "game/generation/world_map_gen.hpp"

#include <algorithm>

namespace world {
namespace {
using core::Fixed;
Fixed unit(Fixed value) { return std::clamp(value, core::kZero, core::kOne); }
Fixed ramp(Fixed value, Fixed low, Fixed high) { return unit((value-low)/(high-low)); }

// Relative defaults: fertility, permeability, digging resistance, bearing,
// erodibility, mud potential. These are model tuning, not geotechnical data.
constexpr std::array<std::array<int, 6>, kSoilCount> kProperties{{
    {15, 90, 15, 40, 80, 10}, // sand
    {60, 10, 65, 55, 35, 95}, // clay
    {80, 35, 25, 35, 95, 85}, // silt
    {85, 55, 35, 60, 50, 55}, // loam
    {10, 40, 95, 90, 10,  5}, // rocky soil
    {45, 20, 15,  5, 70, 95}, // peat
    {90, 60, 25, 45, 80, 70}, // alluvium
    { 5, 25, 50, 35, 55, 65}, // saline soil
}};

SoilWeights parentProfile(generation::RockType rock) {
    // Sand, clay, silt, loam, rocky, peat, alluvium, saline.
    std::array<int, kSoilCount> values{};
    switch (rock) {
    case generation::RockType::HardRock: values = {5,0,0,15,80,0,0,0}; break;
    case generation::RockType::SoftSediment: values = {0,15,50,35,0,0,0,0}; break;
    case generation::RockType::Limestone: values = {0,35,0,25,40,0,0,0}; break;
    case generation::RockType::Volcanic: values = {0,10,0,45,45,0,0,0}; break;
    case generation::RockType::ClayRich: values = {0,75,15,10,0,0,0,0}; break;
    case generation::RockType::Sandstone: values = {75,0,5,20,0,0,0,0}; break;
    case generation::RockType::Alluvial: values = {0,0,30,10,0,0,60,0}; break;
    case generation::RockType::Count: values[static_cast<std::size_t>(Soil::Loam)] = 100; break;
    }
    SoilWeights result;
    for (std::size_t i = 0; i < kSoilCount; ++i) result.weight[i] = Fixed::ratio(values[i],100);
    result.normalise();
    return result;
}
}

const char* soilName(Soil soil) {
    constexpr std::array names{"sand", "clay", "silt", "loam", "rocky soil", "peat", "alluvium", "saline soil"};
    const auto i = static_cast<std::size_t>(soil);
    return i < names.size() ? names[i] : "unknown";
}

void SoilWeights::normalise() {
    Fixed total;
    for (auto& value : weight) { value = std::max(core::kZero,value); total += value; }
    if (total <= core::kZero) {
        weight = {};
        weight[static_cast<std::size_t>(Soil::Loam)] = core::kOne;
        return;
    }
    Fixed sum;
    for (auto& value : weight) { value = value/total; sum += value; }
    weight[static_cast<std::size_t>(strongest())] += core::kOne-sum;
}

Soil SoilWeights::strongest() const {
    return static_cast<Soil>(std::max_element(weight.begin(),weight.end())-weight.begin());
}

SoilProperties SoilWeights::properties() const {
    auto normalised = *this;
    normalised.normalise();
    std::array<Fixed,6> values{};
    for (std::size_t i = 0; i < kSoilCount; ++i)
        for (std::size_t j = 0; j < values.size(); ++j)
            values[j] += normalised.weight[i] * Fixed::ratio(kProperties[i][j],100);
    return {values[0],values[1],values[2],values[3],values[4],values[5]};
}

SoilSample HeightField::soilAt(core::WorldPos p) const {
    return soilGiven(p,heightAt(p),slopeAt(p));
}

SoilSample HeightField::soilGiven(core::WorldPos p, Fixed height, Fixed slope) const {
    SoilWeights weights;
    Fixed moisture = Fixed::ratio(1,2), drainage = moisture, organic = moisture;
    Fixed permeability = moisture, fertility = moisture, flood, delta, coast;
    if (coarse_ && coarse_->width > 0 && coarse_->height > 0 &&
        coarse_->cells.size() >= static_cast<std::size_t>(coarse_->width)*coarse_->height) {
        const auto [cx,cy,tx,ty] = coarseLookup(p);
        std::array<Fixed,8> environment{};
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 2; ++x) {
            const auto ix = std::clamp<std::int64_t>(cx+x,0,coarse_->width-1);
            const auto iy = std::clamp<std::int64_t>(cy+y,0,coarse_->height-1);
            const auto index = static_cast<std::size_t>(iy)*coarse_->width+ix;
            const auto& cell = coarse_->cells[index];
            const Fixed share = (x ? tx : core::kOne-tx)*(y ? ty : core::kOne-ty);
            const auto scalar = [&](const auto& field, Fixed fallback) {
                return index < field.size() ? Fixed::ratio(std::clamp<int>(field[index],0,255),255) : fallback;
            };
            auto rock = generation::RockType::SoftSediment;
            if (index < coarse_->rockTypeField.size()) rock = coarse_->rockTypeField[index];
            else if (index < coarse_->soilParentMaterialField.size())
                rock = static_cast<generation::RockType>(coarse_->soilParentMaterialField[index]);
            if (rock >= generation::RockType::Count) rock = generation::RockType::SoftSediment;
            auto local = parentProfile(rock);
            if (index < coarse_->soilTypeField.size()) {
                Soil primary = Soil::Loam;
                switch (coarse_->soilTypeField[index]) {
                case generation::SoilType::Bedrock: primary = Soil::Rocky; break;
                case generation::SoilType::Alluvial: primary = Soil::Alluvium; break;
                case generation::SoilType::Clay: primary = Soil::Clay; break;
                case generation::SoilType::Sandy: primary = Soil::Sand; break;
                case generation::SoilType::Peat: primary = Soil::Peat; break;
                default: break;
                }
                for (auto& value : local.weight) value *= Fixed::ratio(1,2);
                local.add(primary,Fixed::ratio(1,2));
            }
            for (std::size_t i = 0; i < kSoilCount; ++i) weights.weight[i] += local.weight[i]*share;
            const auto wet = scalar(coarse_->humidityField,Fixed::ratio(cell.moisture,255));
            const auto permeable = scalar(coarse_->permeabilityField,Fixed::ratio(1,2));
            const auto drain = scalar(coarse_->soilDrainageField,permeable);
            const auto coastal = index < coarse_->distanceToCoast.size()
                ? core::kOne-Fixed::ratio(std::clamp(coarse_->distanceToCoast[index],0,3),3)
                : (cell.sea ? core::kOne : core::kZero);
            const std::array localEnvironment{wet,drain,
                scalar(coarse_->soilOrganicPotentialField,wet*(core::kOne-drain)),permeable,
                scalar(coarse_->soilFertilityField,Fixed::ratio(1,2)),
                scalar(coarse_->floodplainPotentialField,core::kZero),
                scalar(coarse_->deltaPotentialField,core::kZero),coastal};
            for (std::size_t i = 0; i < environment.size(); ++i) environment[i] += localEnvironment[i]*share;
        }
        moisture = unit(environment[0]); drainage = unit(environment[1]); organic = unit(environment[2]);
        permeability = unit(environment[3]); fertility = unit(environment[4]);
        flood = unit(environment[5]); delta = unit(environment[6]); coast = unit(environment[7]);
    } else {
        weights.add(Soil::Loam,core::kOne);
    }
    weights.normalise();
    const Fixed flat = core::kOne-ramp(slope,Fixed::ratio(1,10),Fixed::ratio(9,10));
    const auto pieces = piecesAt(p.x,p.y);
    const auto channel = macro_.carve(p,pieces.country,pieces.moved);
    const Fixed bank = channel.nearestWet && channel.surface > core::kZero
        ? core::kOne-ramp(channel.bankDistance,core::kZero,Fixed::fromInt(80)) : core::kZero;
    const auto deposit = [&](Soil kind, Fixed share) {
        share = unit(share);
        for (auto& value : weights.weight) value *= core::kOne-share;
        weights.add(kind,share);
    };
    deposit(Soil::Alluvium,std::max(bank,flood)*flat*Fixed::ratio(3,4));
    deposit(Soil::Silt,delta*flat*Fixed::ratio(1,2));
    deposit(Soil::Peat,moisture*(core::kOne-drainage)*organic*flat*Fixed::ratio(4,5));
    // Salinity is a dry, low coastal-land potential, not an ocean-bottom cover.
    const Fixed lowCoast = coast*(core::kOne-ramp(height,Fixed::fromInt(2),Fixed::fromInt(35)))*
                           ramp(height,Fixed::fromInt(-1),core::kOne);
    deposit(Soil::Saline,lowCoast*(core::kOne-moisture)*flat*Fixed::ratio(4,5));
    deposit(Soil::Rocky,ramp(slope,Fixed::ratio(2,5),Fixed::ratio(3,2)));
    weights.normalise();
    auto properties = weights.properties();
    properties.fertility *= Fixed::ratio(1,2)+fertility*Fixed::ratio(1,2);
    properties.permeability = (properties.permeability+permeability)*Fixed::ratio(1,2);
    return {weights,properties};
}

} // namespace world

