# Climate, Weather, Biomes and Ecological State Requirements

## Goal

Define how long-term climate, biome generation, runtime weather, ecological state, and slow biome transitions interact in a large-scale 3D RTS / colony-sim.

Core principle:

```text
Long-term climate determines biome.
Runtime weather modifies ecological state.
Biome changes only slowly, if at all.
```

## 1. System layers

Use four separate layers:

```text
1. Long-Term Climate
2. Climate Biome
3. Ecological State
4. Current Weather
```

These operate at different time scales and should not be conflated.

## 2. Long-Term Climate

Long-term climate is mostly precomputed during world generation.

Suggested fields:

```text
meanTemperature
meanPrecipitation
seasonality
aridity
prevailingWind
meanWindSpeed
meanHumidity
meanPressure
continentality
elevation influence
ocean influence
```

Example:

```cpp
struct ClimateCell
{
    float meanTemperature;
    float meanPrecipitation;
    float seasonality;
    float aridity;

    float2 prevailingWind;
    float meanWindSpeed;

    float meanHumidity;
    float meanPressure;
};
```

## 3. Climate Biome

Biome is derived from long-term climate, soil, elevation and hydrology.

```text
Biome = function(
    long_term_temperature,
    long_term_precipitation,
    seasonality,
    soil,
    elevation,
    hydrology
)
```

Possible biome types:

```text
temperate forest
dry forest
woodland
steppe
savanna
desert
semi-desert
mediterranean scrub
wetland
tundra
mountain biome
river floodplain
```

Biome influences:

```text
possible vegetation communities
baseline soil moisture
baseline fertility
animal distributions
resource expectations
settlement suitability
cultural starting conditions
visual identity
```

## 4. Weather must not directly switch biomes

Temporary weather anomalies should alter ecological state, not biome classification.

Bad:

```text
forest + one dry year = desert
```

Correct:

```text
forest + one dry year = drier ecological state
```

## 5. Ecological State

Ecological state represents what the biome currently looks like.

Example:

```cpp
struct EcologyCell
{
    float treeCover;
    float grassCover;
    float shrubCover;

    float soilMoisture;
    float biomass;
    float disturbance;

    float burnSeverity;
    float grazingPressure;
    float regrowth;
};
```

A forest biome may currently be:

```text
dense forest
open woodland
young forest
burnt forest
dry woodland
overgrazed forest
logged woodland
flooded woodland
regrowing scrub
```

Biome defines long-term ecological potential.

Ecological state defines the current realized condition.

## 6. Current Weather

Weather is fully dynamic.

Example:

```cpp
struct WeatherCell
{
    float pressure;
    float temperature;
    float humidity;

    float precipitation;
    float cloudWater;

    float2 wind;
    float gustiness;
};
```

Includes:

```text
rain
storms
wind
heat waves
cold snaps
fog
snowfall
dry spells
pressure systems
fronts
```

## 7. Time scales

Recommended update scales:

```text
Weather:
    seconds / minutes

Soil moisture:
    minutes / hours / days

Ecology:
    days / seasons

Climate averages:
    years

Biome transitions:
    years / decades
```

## 8. Dependency chain

Primary dependency:

```text
Long-Term Climate
        ↓
Climate Biome
        ↓
Ecological State
        ↓
Current Weather Effects
```

Slow feedback:

```text
Current Weather
        ↓
Ecological State
        ↓
Long-Term Running Averages
        ↓
Possible Very Slow Biome Transition
```

## 9. Weather effects

Heavy rain:

```text
soil moisture ↑
river flow ↑
mud ↑
plant growth ↑
fire risk ↓
```

Drought:

```text
soil moisture ↓
biomass growth ↓
grass dries
fire risk ↑
tree stress ↑
```

Storm:

```text
tree damage
temporary flooding
erosion increase
local vegetation damage
```

Cold snap:

```text
growth slowdown
snow cover
water freezing
temporary crop damage
```

## 10. Long-term climate drift

If climate drift is supported, maintain slow running averages.

Example:

```cpp
climateTemperatureAverage =
    lerp(
        climateTemperatureAverage,
        yearlyTemperature,
        climateAdaptationRate
    );
```

Do the same for precipitation, aridity, humidity and seasonality.

## 11. Biome transition hysteresis

Biome changes should require sustained climate change.

Example:

Forest -> Steppe:

```text
mean precipitation below threshold
for at least 20 years
```

Steppe -> Forest:

```text
mean precipitation above a higher threshold
for at least 30 years
```

Use different enter/leave thresholds to prevent oscillation.

## 12. Gradual biome transition chains

Prefer:

```text
Forest
    ↓
Dry Forest
    ↓
Woodland
    ↓
Steppe
    ↓
Semi-Desert
    ↓
Desert
```

Avoid abrupt jumps such as:

```text
Forest -> Desert
```

except for exceptional scripted catastrophes.

## 13. Vegetation lag

Climate change should not instantly replace vegetation.

Example:

```text
climate becomes drier
        ↓
young tree recruitment decreases
        ↓
existing trees survive for years
        ↓
old trees die
        ↓
grass expands
        ↓
woodland replaces forest
```

## 14. Vegetation succession

Example after wildfire:

```text
Mature Forest
    ↓
Burnt Ground
    ↓
Grass / Pioneer Plants
    ↓
Shrub Stage
    ↓
Young Woodland
    ↓
Mature Forest
```

Other disturbances:

```text
logging
grazing
flooding
drought
agriculture
abandonment
```

can move ecology between states without changing the climate biome.

## 15. Human impact

Examples:

```text
logging
    -> tree cover decreases

grazing
    -> grass height decreases
    -> erosion risk increases

farming
    -> natural vegetation suppressed
    -> soil state changes

roads
    -> vegetation disturbance

fires
    -> succession reset
```

## 16. Ethnos and population placement

Starting cultures should use climate biome / long-term environmental regions.

Example:

```text
Forest culture
    starts in forest / woodland climate regions
```

If the climate later dries:

```text
the people remain
the environment changes
resources shift
economy adapts
migration may occur
```

Do not dynamically relocate or reclassify populations simply because the biome changes.

## 17. Settlement continuity

A settlement founded in forest may later experience:

```text
declining timber
increasing grassland
water scarcity
crop changes
livestock advantages
migration pressure
```

This is desirable emergent gameplay.

Avoid instant biome replacement under settlements.

## 18. Visual biome state

Visual terrain should use both biome and ecology.

Example:

```text
Climate Biome:
    Temperate Forest

Ecological State:
    Dry Open Woodland

Visual Result:
    sparse trees
    dry grass
    exposed soil
    reduced undergrowth
```

## 19. Seasons

Season is separate from biome.

Example for forest biome:

```text
spring:
    wet soil
    bright grass
    flowers

summer:
    dense foliage
    dry patches

autumn:
    leaf litter
    color shift

winter:
    dormant vegetation
    snow patches
```

## 20. Soil state

Useful fields:

```text
soilMoisture
soilFertility
soilCompaction
erosionExposure
salinity
organicMatter
```

Weather modifies soil.

Ecology reacts to soil.

## 21. Hydrology interaction

Long-term hydrology can affect biome classification:

```text
river presence
groundwater
floodplain
wetland tendency
```

Runtime hydrology affects ecology:

```text
flood event
river level
temporary wetness
```

## 22. World-generation pipeline

Recommended pre-runtime order:

```text
Macro Terrain
        ↓
Hydrology
        ↓
Long-Term Climate
        ↓
Soil
        ↓
Climate Biomes
        ↓
Initial Ecology State
        ↓
Initial Vegetation
        ↓
Starting Populations / Ethnoses
```

## 23. Runtime pipeline

```text
Dynamic Weather
        ↓
Soil Moisture / Water
        ↓
Ecological State
        ↓
Vegetation Growth / Death
        ↓
Long-Term Climate Running Average
        ↓
Rare Biome Transition Check
```

Biome transition should be the slowest stage.

## 24. Example transition rule

```cpp
if (
    climate.meanPrecipitation < forestToSteppeThreshold &&
    climate.meanTemperature > minimumForestTemperature &&
    yearsBelowThreshold >= 20
)
{
    BeginBiomeTransition(
        Forest,
        DryForest
    );
}
```

Do not do:

```cpp
if (thisYearWasDry)
    biome = Desert;
```

## 25. Transition state

Biome change itself should be gradual.

Example:

```text
BiomeTransition
{
    from = Forest
    to = DryForest
    progress = 0..1
}
```

Systems can gradually blend:

```text
species availability
vegetation density
soil expectations
visual materials
resource probabilities
```

## 26. Example data model

```cpp
struct ClimateCell
{
    float meanTemperature;
    float meanPrecipitation;
    float seasonality;
    float aridity;

    float2 prevailingWind;
};

struct BiomeCell
{
    BiomeType biome;

    BiomeType transitionTarget;
    float transitionProgress;
};

struct EcologyCell
{
    float treeCover;
    float shrubCover;
    float grassCover;

    float soilMoisture;
    float biomass;
    float disturbance;
};

struct WeatherCell
{
    float pressure;
    float temperature;
    float humidity;
    float precipitation;

    float2 wind;
};
```

## 27. Acceptance criteria

The system is acceptable if:

- biomes are generated before runtime from long-term climate;
- temporary weather does not immediately change biome;
- runtime weather changes soil and ecology;
- vegetation responds gradually;
- fires, logging, grazing and drought alter ecological state;
- biome transitions, if enabled, take many years;
- hysteresis prevents biome oscillation;
- major environmental transitions are visually gradual;
- population placement remains logically consistent;
- settlements are not invalidated by sudden biome swaps;
- climate drift creates long-term gameplay rather than visual randomness.

## 28. Final principle

```text
Climate
    determines what can exist

Biome
    defines long-term ecological identity

Ecology
    defines what currently exists

Weather
    defines what is happening now
```

In short:

```text
Climate = decades
Biome = long-term category
Ecology = seasons / years
Weather = minutes / days
```

This keeps the world dynamic without allowing short-term weather to turn a forest settlement into a desert overnight.
