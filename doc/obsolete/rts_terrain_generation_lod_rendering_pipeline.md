# RTS Terrain Generation, LOD Refinement and Rendering Pipeline

## Goal

Define the complete terrain pipeline for a large-scale RTS / colony-sim so that:

- terrain transitions between LOD levels are smooth;
- the overall world topology remains stable;
- higher-detail terrain refines rather than replaces lower-detail terrain;
- erosion and local terrain detail do not cause major shape changes during streaming;
- navigation, buildability, rivers, cliffs and settlements remain consistent;
- material transitions, water, vegetation and clutter are layered on top deterministically.

Core principle:

```text
Stable world topology
+
Progressively revealed visual detail
```

Higher LOD must not mean "generate a different terrain".

## 1. Pipeline Overview

```text
A. World Generation / Stable Topology
B. Chunk Refinement / Streaming
C. Per-frame Rendering
```

Everything that determines gameplay topology must be resolved before visual LOD refinement.

Visual LOD may add surface detail, but should not redefine the world.

## 2. Phase A - World Generation / Stable Topology

Recommended order:

```text
WORLD SEED
   ↓
1. Macro Height
   ↓
2. Global Hydrology / Drainage
   ↓
3. Large-scale Erosion
   ↓
4. Simulation Heightfield
   ↓
5. Stable Topology Fields
   ↓
6. Soil
   ↓
7. Climate / Biome
   ↓
8. Buildability / Navigation
   ↓
9. Permanent Macro Feature Maps
```

## 3. Macro Height

Generate only the large-scale terrain shape:

```text
continents / region shape
mountains
large hills
valleys
plateaus
basins
coastline
```

Canonical representation:

```cpp
H_macro(x, z)
```

## 4. Global Hydrology

Resolve hydrology before detailed local erosion.

Compute globally or on large macro-regions:

```text
flow direction
flow accumulation
watersheds
river centerlines
major lakes
wetland basins
groundwater tendency
major drainage corridors
```

Large waterways and drainage structures must not be regenerated independently per chunk.

## 5. Large-scale Erosion

Apply erosion that affects major terrain structure:

```text
large river valleys
major gullies
main cliff lines
large terraces
erosion basins
major ridges
large sediment zones
```

The result becomes:

```cpp
H_sim(x, z)
```

This is the canonical gameplay terrain.

## 6. Freeze Gameplay Topology

Derive stable gameplay fields from `H_sim`:

```text
SimulationNormal
SimulationSlope
CliffMask
Walkability
Buildability
MajorRiverMask
CrossingMask
RoadCandidateField
SettlementCandidateField
```

Visual LOD must not change:

```text
walkable -> unwalkable
buildable -> unbuildable
river crossing -> no crossing
major cliff position
major road topology
settlement viability
major shoreline topology
```

Fundamental contract:

```cpp
VisualLOD != GameplayTopology;
```

## 7. Soil

Generate soil after terrain, hydrology and major erosion.

Possible fields:

```text
sand
clay
silt
loam
rock
peat
alluvium
salinity
fertility
rockiness
organic matter
```

Inputs may include:

```text
hydrology
erosion
sediment
slope
geology
water table
```

## 8. Climate and Biome

Generate long-term climate and biome state after hydrology and soil are known.

Possible fields:

```text
temperature
mean precipitation
moisture baseline
aridity
seasonality
biome weights
vegetation community weights
```

Biome belongs to the stable world-generation layer.

## 9. Visual LOD Hierarchy

Higher LOD is cumulative residual detail:

```text
H_LOD3 = H_sim

H_LOD2 = H_sim
       + D_large

H_LOD1 = H_sim
       + D_large
       + D_medium

H_LOD0 = H_sim
       + D_large
       + D_medium
       + D_fine
```

Conceptually:

```text
Simulation Height
        ↓
Large Visual Residual
        ↓
Medium Visual Residual
        ↓
Fine Visual Residual
```

Each level inherits all lower-frequency geometry.

## 10. Large Visual Residual

`D_large` may include:

```text
secondary erosion channels
terrain terraces
secondary cliff breakup
broad bank detail
large local surface deformation
```

It must not move major rivers, create new major cliffs, or invalidate navigation.

## 11. Medium Visual Residual

`D_medium` may include:

```text
small gullies
cliff lip breakup
bank irregularity
rocky surface breakup
small sediment shapes
secondary terrain cuts
```

## 12. Fine Visual Residual

`D_fine` may include:

```text
minor grooves
small erosion cuts
tiny terrain displacement
surface irregularity
micro terraces
```

A large part of this level should preferably be shading rather than geometry.

## 13. Geometry vs Shader Detail

Use real geometry for:

```text
silhouette
large shadows
river banks
cliff lips
terraces
major gullies
large erosion cuts
```

Use shaders for:

```text
small cracks
tiny erosion channels
centimeter-scale grooves
fine scree
surface roughness
minor sand cuts
micro erosion
```

Useful techniques:

```text
normal maps
parallax
height blending
material masks
decals
procedural detail
```

## 14. Phase B - Runtime Chunk Streaming

The existing ring-based RTS streaming system chooses the desired LOD.

Recommended sequence:

```text
Ring Manager
    ↓
Determine desired LOD
    ↓
Load stable simulation data
    ↓
Load / generate required residual layers
    ↓
Prepare terrain render data
    ↓
GPU upload
    ↓
Wait for safe transition
    ↓
Morph toward target LOD
```

Do not independently regenerate each LOD.

Correct model:

```text
H_sim
+
required residual layers
```

## 15. Residual Caching

Cache refinement separately:

```text
BaseHeight
DeltaLOD2
DeltaLOD1
DeltaLOD0
```

Benefits:

```text
load only required detail
reuse generated erosion detail
avoid rerunning expensive erosion
stream detail independently
```

## 16. Generation Padding

Every terrain chunk should be generated with border padding.

Example:

```text
visible chunk:
128 x 128

actual generation area:
136 x 136
```

Padding is needed for:

```text
erosion
normal generation
curvature
flow accumulation
material blending
cliff detection
foliage suitability
```

Only the center becomes visible.

## 17. World-space Determinism

Avoid:

```cpp
seed = Hash(chunkId);
```

Prefer:

```cpp
seed = Hash(globalSeed, macroRegion, worldCoordinate);
```

A world position should produce the same detail regardless of chunk ownership.

## 18. Derived Visual Fields

After visual height for the current LOD is resolved:

```text
VisualHeight
    ↓
VisualNormal
    ↓
VisualSlope
    ↓
VisualCurvature
```

Keep simulation and visual fields separate:

```text
SimulationSlope -> gameplay
VisualSlope     -> materials / shading / cliff decoration
```

## 19. Material Weights

Inputs:

```text
height
visual slope
soil
moisture
rockiness
erosion
biome
water distance
sediment
```

Outputs may include:

```text
grass
dirt
rock
sand
mud
scree
peat
snow
```

## 20. Material Transition Masks

```text
material weights
        +
edge breakup masks
        +
world-space noise
        ↓
final material blend weights
```

Transition masks must be world-space and deterministic.

## 21. Cliff Visual Pass

Gameplay cliff structure comes from simulation terrain:

```text
Simulation Cliff
       ↓
Visual Cliff Refinement
```

Visual refinement may add:

```text
broken cliff lip
small ledges
erosion streaks
surface irregularity
scree
local cracks
```

It must not create or remove gameplay barriers.

## 22. Water and Shoreline

```text
stable water body
        ↓
stable river centerline
        ↓
stable major shoreline
        ↓
fine shoreline visual refinement
```

Higher detail may add:

```text
small sandbars
bank breakup
minor erosion cuts
wet border
foam
shallow-water transitions
local sediment
```

It must not significantly move rivers or crossings.

## 23. Persistent World Deformation

Roads, settlements and terrain modification belong to gameplay state rather than LOD.

Examples:

```text
roads
paths
building pads
flattened terrain
farmland
ditches
mud
trampling
logging disturbance
```

Recommended equation:

```cpp
H_render =
    H_sim
  + PersistentWorldDeformation
  + VisualResidual;
```

Persistent deformation survives all LOD changes.

## 24. Height Data Separation

```cpp
struct TerrainHeightState
{
    HeightField macro;
    HeightField simulation;

    HeightDelta visualLarge;
    HeightDelta visualMedium;
    HeightDelta visualFine;

    HeightDelta persistentDeformation;
};
```

Rendering:

```cpp
float GetRenderedHeight(float2 p, LodBlend lod)
{
    return Sample(simulation, p)
         + Sample(persistentDeformation, p)
         + Sample(visualLarge, p)  * lod.large
         + Sample(visualMedium, p) * lod.medium
         + Sample(visualFine, p)   * lod.fine;
}
```

Gameplay:

```cpp
float GetGameplayHeight(float2 p)
{
    return Sample(simulation, p)
         + Sample(persistentDeformation, p);
}
```

## 25. LOD Morphing

Never hard-swap terrain.

If:

```cpp
H_current = H_sim + D_large;
H_target  = H_sim + D_large + D_medium;
```

morph only the incoming residual:

```cpp
float t =
    SmoothStep(morphStart, morphEnd, time);

height =
    H_sim
  + D_large
  + D_medium * t;
```

## 26. Shared LOD Detail Factor

Geometry and material refinement should transition together.

```cpp
float lodDetailFactor;
```

Apply it to:

```text
geometry residual
normal refinement
erosion accents
cliff breakup
micro texture detail
ground clutter density
```

Example:

```cpp
height += D_medium * lodDetailFactor;

normal =
    normalize(
        lerp(
            coarseNormal,
            fineNormal,
            lodDetailFactor
        )
    );

erosionAccent *= lodDetailFactor;
microNormalStrength *= lodDetailFactor;
```

## 27. Regional Morphing

Avoid independent chunk morphing.

Prefer:

```text
regional transition
ring transition
distance-band transition
batched chunk morph
```

A shared world-space field can drive it:

```cpp
lodBlend =
    GetRingMorph(worldXZ);
```

## 28. Ring Integration

For the existing RTS ring system:

```text
Visible Area
    = target LOD

Same-LOD Preload Rings
    = target LOD prepared ahead of camera

Outer Backup Ring
    = one coarser LOD
```

At the boundary:

```text
fine residual contribution
1.0
↓
transition band
↓
0.0
```

The simulation terrain remains identical.

## 29. Immutable Data During Visual LOD Morph

Do not morph or regenerate these with visual LOD:

```text
river centerlines
major shorelines
main cliff boundaries
major valley topology
major ridge topology

buildability
walkability connectivity
major roads

building terrain deformation
settlement pads
bridges
crossings
```

## 30. Vegetation Placement Stage

After terrain, materials and persistent disturbance are resolved:

```text
biome/community
soil
moisture
slope
water distance
disturbance
rockiness
```

feed:

```text
tree placement
shrubs
grass
reeds
ground clutter
rocks
```

Vegetation should form communities and patches rather than uniform scatter.

## 31. Logical Per-frame Rendering Order

```text
1. Resolve streamed terrain state
2. Resolve LOD morph factors
3. Resolve render height
4. Terrain geometry / depth
5. Terrain base materials
6. Cliff material / projection
7. Shore / sediment / wetness
8. Terrain stylization
9. Water
10. Large rocks
11. Trees
12. Shrubs
13. Grass / reeds
14. Ground clutter
15. Roads / settlement overlays / decals
16. Dynamic weather overlays
17. Lighting / shadows
18. Final compositing
```

These are logical stages, not necessarily separate GPU passes.

## 32. Complete Pipeline Diagram

```text
                    WORLD GENERATION
                          |
                          v
                     Macro Height
                          |
                          v
                Global Hydrology
                          |
                          v
               Large-scale Erosion
                          |
                          v
                Simulation Height
                          |
          +---------------+---------------+
          |                               |
          v                               v
   GAMEPLAY TOPOLOGY               VISUAL REFINEMENT
   slope / cliffs                  D_large
   rivers / crossings              D_medium
   buildability                    D_fine
   navigation                        |
          |                           |
          |                     LOD streaming
          |                           |
          +---------------+-----------+
                          |
                          v
               Persistent deformation
               roads/buildings/fields
                          |
                          v
                    Render Height
                          |
                          v
             Normal / Slope / Curvature
                          |
                          v
              Material Weight Fields
                          |
                          v
             Transition / Breakup Masks
                          |
             +------------+------------+
             |                         |
             v                         v
           Cliffs                 Water / Shore
             |                         |
             +------------+------------+
                          |
                          v
                  Terrain Shader
                          |
                          v
                   Stylization
                          |
                          v
                 Terrain Surface
                          |
      +-------------------+-------------------+
      |                   |                   |
      v                   v                   v
    Trees              Foliage             Clutter
      |                   |                   |
      +-------------------+-------------------+
                          |
                          v
                Weather / Wetness
                          |
                          v
                    FINAL FRAME
```

## 33. Acceptance Criteria

The pipeline is correct if:

- high LOD makes terrain richer, not fundamentally different;
- major rivers do not move during LOD transitions;
- main cliffs do not jump or appear from nowhere;
- navigation topology remains unchanged by visual refinement;
- buildings do not float or become buried after refinement;
- chunk seams are not visible;
- erosion is continuous across chunk borders;
- material transitions align with refined geometry;
- LOD transition is gradual rather than a hard swap;
- refinement appears regional rather than square-by-square;
- gameplay deformation survives all LOD changes;
- small visual detail is delegated to shading where possible.

## 34. Final Principle

The user should perceive:

```text
same terrain
becoming richer,
sharper,
more eroded,
more detailed
```

not:

```text
terrain transforming
into a different shape
```

The system should always preserve:

```text
Stable World
+
Persistent Gameplay State
+
Progressive Visual Refinement
```
