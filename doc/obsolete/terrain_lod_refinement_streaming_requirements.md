# Terrain LOD Refinement and Streaming Requirements

## 1. Goal

Define how terrain geometry should refine across LOD levels in a large-scale 3D RTS / colony-sim when higher LODs include materially different geometric detail such as:

- erosion;
- gullies;
- cliff breakup;
- terraces;
- river-bank detail;
- local sediment forms;
- small-scale terrain irregularity.

The system must avoid visible terrain popping, sudden topology changes, broken navigation consistency, and chunk seams.

The key principle is:

> Higher LOD should refine lower LOD, not regenerate the terrain as a different world.

---

# 2. Core Problem

In this terrain system, LOD does not only mean:

```text
same terrain shape
+
more triangles
```

Higher LOD may introduce:

```text
additional erosion
finer valleys
small gullies
cliff detail
bank breakup
terrain cuts
micro-relief
```

This means a naïve LOD swap can visibly change the terrain shape.

Potential artifacts:

```text
ground visibly rising or collapsing
river banks moving
cliffs appearing suddenly
roads intersecting newly generated cuts
building foundations floating
walkable areas becoming blocked
visible chunk-by-chunk terrain morphing
```

Therefore LOD geometry must be designed as a refinement hierarchy.

---

# 3. Stable Base + Residual Detail

Represent final terrain height as:

```text
FinalHeight(x,z) =
    BaseHeight(x,z)
  + DetailLOD2(x,z)
  + DetailLOD1(x,z)
  + DetailLOD0(x,z)
```

Each finer LOD adds residual detail to the already established coarse terrain.

Example:

```text
LOD3:
    macro mountains
    large valleys
    major river beds

LOD2:
    secondary ridges
    large erosion channels
    terraces

LOD1:
    medium gullies
    cliff breakup
    local erosion

LOD0:
    fine erosion
    small cuts
    surface irregularity
```

Higher LOD should not contradict the macro structure established by lower LOD.

---

# 4. Residual Height Representation

Prefer storing or generating delta height between LOD levels.

Example:

```text
LOD2 = Base

LOD1 = LOD2 + Delta1

LOD0 = LOD1 + Delta0
```

Or:

```text
LOD0 = Base + Delta2 + Delta1 + Delta0
```

This makes morphing explicit and controllable.

Example shader logic:

```cpp
float coarseHeight = SampleCoarseHeight(worldXZ);
float detailDelta  = SampleFineDelta(worldXZ);

float finalHeight =
    coarseHeight +
    detailDelta * lodFade;
```

Where:

```text
lodFade = 0
    -> coarse terrain

lodFade = 1
    -> full refined terrain
```

---

# 5. Do Not Run Independent Erosion Per LOD

Avoid this:

```text
LOD2:
    generate terrain
    run erosion

LOD1:
    generate terrain independently
    run erosion independently

LOD0:
    generate terrain independently
    run erosion independently
```

Independent erosion simulations will produce structurally different terrain.

This creates:

```text
different drainage
different gullies
different ridges
different river positions
different cliff boundaries
```

Instead, use hierarchical erosion refinement.

---

# 6. Hierarchical Erosion Pipeline

Recommended pipeline:

```text
Generate macro terrain
        ↓
Run large-scale erosion
        ↓
Store coarse result
        ↓
Upsample
        ↓
Add medium-scale detail
        ↓
Run constrained medium erosion
        ↓
Store residual
        ↓
Upsample
        ↓
Add fine-scale detail
        ↓
Run local fine erosion
        ↓
Store residual
```

Each level inherits the result of the previous level.

Higher-resolution erosion should refine existing drainage patterns rather than replace them.

---

# 7. Major Features Must Exist in Coarse LOD

Large gameplay-relevant terrain structures should already exist at coarse resolution.

Example: gully

Bad:

```text
LOD2:
    flat terrain

LOD0:
    suddenly deep 4 m canyon
```

Good:

```text
LOD2:
    broad depression

LOD1:
    recognizable channel

LOD0:
    broken banks
    side cuts
    fine erosion detail
```

The silhouette and gameplay meaning remain stable.

---

# 8. Separate Simulation Terrain and Visual Terrain

For a colony-sim / RTS, gameplay-critical terrain should remain stable.

Recommended split:

```text
SimulationTerrain
VisualTerrain
```

Example:

```cpp
VisualHeight =
    SimulationHeight +
    VisualResidual;
```

`SimulationTerrain` is used for:

```text
navigation
buildability
water logic
roads
settlement placement
path costs
gameplay slope
resource placement
```

`VisualTerrain` adds:

```text
small erosion
surface breakup
minor cuts
micro terraces
visual roughness
```

This prevents LOD refinement from changing gameplay rules.

---

# 9. Gameplay Topology Must Not Change with Visual LOD

LOD-dependent detail may modify terrain by small visual amounts.

Acceptable:

```text
±10–30 cm surface variation
small erosion grooves
small banks
rock breakup
minor slope irregularity
```

Avoid changing:

```text
walkable -> unwalkable
buildable -> unbuildable
river crossing -> no crossing
major road route
major cliff position
settlement viability
```

Gameplay topology should be determined by the stable simulation terrain.

---

# 10. Frequency Separation

Terrain detail should be separated by spatial frequency.

Example:

```text
0–1 octave:
    macro shape

2–3 octaves:
    large erosion

4–5 octaves:
    medium terrain breakup

6+ octaves:
    fine detail
```

Then define LODs as cumulative frequency bands.

Example:

```text
LOD3 =
    macro

LOD2 =
    macro
    + large erosion

LOD1 =
    macro
    + large erosion
    + medium breakup

LOD0 =
    macro
    + large erosion
    + medium breakup
    + fine detail
```

This naturally supports progressive refinement.

---

# 11. Global Drainage Before Local Erosion

Large drainage structures should be determined at a global or macro-region level.

Examples:

```text
major rivers
watersheds
large gullies
ridge lines
large valley floors
```

Then local chunk refinement adds:

```text
bank breakup
small tributaries
surface cuts
sediment detail
```

Recommended architecture:

```text
Macro flow map
        ↓
Major river / valley structure
        ↓
Chunk-level refinement
        ↓
Fine erosion detail
```

This improves cross-chunk continuity.

---

# 12. Deterministic World-Space Generation

Do not seed erosion detail purely from chunk IDs.

Avoid:

```cpp
seed = Hash(chunkId);
```

Prefer:

```cpp
seed =
    Hash(
        globalWorldSeed,
        macroRegionId,
        worldCoordinate
    );
```

The same world position must produce the same terrain detail regardless of which chunk currently owns it.

This prevents visible boundaries.

---

# 13. Generation Padding

Terrain processing must include border padding.

Example:

```text
visible chunk:
128 x 128

generation area:
136 x 136
```

or larger.

Padding is required for:

```text
normal generation
erosion
curvature
flow accumulation
material blending
cliff detection
foliage suitability
```

Only the central region becomes the visible chunk.

---

# 14. Chunk Boundary Continuity

Fine erosion must not be simulated independently with no neighbor context.

Possible approaches:

```text
generation padding
shared macro drainage data
world-space deterministic noise
neighbor-aware erosion
macro-region erosion passes
```

The final terrain surface must remain continuous across chunk boundaries.

---

# 15. Geomorph Instead of Hard LOD Swap

Do not immediately replace coarse geometry when fine geometry becomes available.

Use gradual morphing.

Example:

```cpp
float lodFade =
    smoothstep(0.0, 1.0, transitionTime);

height =
    coarseHeight +
    fineResidual * lodFade;
```

Recommended transition duration:

```text
~0.3–1.0 seconds
```

Exact values depend on camera speed and visual scale.

---

# 16. Regional Morph Instead of Per-Chunk Popping

Avoid visible square-by-square refinement.

Bad:

```text
chunk A morphs
then chunk B
then chunk C
```

This makes the world appear to pulse in squares.

Prefer:

```text
regional transition
ring transition
distance-band transition
batched chunk morph
```

The user should perceive a continuous terrain refinement rather than individual chunk swaps.

---

# 17. Morph Bands

A transition band can exist between coarse and fine terrain.

Example:

```text
inner zone:
    100% fine

transition band:
    coarse -> fine

outer zone:
    100% coarse
```

The morph band should span enough distance that terrain changes are not obvious near the camera.

For RTS use, this can align with existing terrain streaming rings.

---

# 18. LOD Ring Integration

Because terrain is streamed in rings, use the ring system to control refinement.

Example:

```text
Ring 0:
    full visual detail

Ring 1:
    full geometry, reduced visual residual

Ring 2:
    coarse refinement

Outer fallback:
    macro terrain only
```

Important:

The same gameplay terrain remains underneath all rings.

Only visual residual detail changes.

---

# 19. Major Geometry vs Shader Detail

Not every terrain feature should exist as geometry.

Use real geometry for details that materially affect:

```text
silhouette
large shadows
river banks
cliff lips
large erosion cuts
terraces
major gullies
```

Use shader detail for:

```text
small cracks
tiny erosion channels
centimeter-scale grooves
fine scree
surface roughness
minor sand cuts
```

Possible shader techniques:

```text
normal maps
parallax
height blending
material masks
decals
procedural detail
```

This reduces geometric differences between LODs.

---

# 20. Cliff Requirements

Cliffs are especially sensitive to LOD refinement.

Large cliff structure must exist in coarse terrain.

Higher LOD may add:

```text
broken lip
small ledges
erosion cuts
surface irregularity
scree
local overhang impression
```

Higher LOD must not:

```text
move cliff several meters
create new major cliff where none existed
remove an existing traversal route
change settlement topology
```

---

# 21. River and Shoreline Requirements

Major river channels and shoreline shape must be stable across LODs.

Higher LOD can refine:

```text
bank irregularity
small sand bars
small erosion cuts
shoreline breakup
local sediment
```

Avoid:

```text
moving river centerline
changing crossing availability
changing major water depth
moving shore by several meters
```

---

# 22. Buildability Requirements

Building gameplay must use stable terrain data.

Building validation should not depend on fine visual LOD.

Recommended:

```text
SimulationHeight
SimulationSlope
BuildabilityMask
```

Visual terrain may deviate slightly from the simulation terrain, but foundations should visually compensate.

Possible techniques:

```text
foundation skirt
local flattening
building-ground blend
small visual terrain displacement suppression under structures
```

---

# 23. Navigation Requirements

Navigation must use stable gameplay terrain.

Do not regenerate nav topology because a visual LOD becomes more detailed.

Navigation can still use local high-resolution detail for:

```text
movement cost
visual foot placement
minor avoidance
```

but major connectivity must remain stable.

---

# 24. Runtime Streaming Sequence

Recommended terrain refinement pipeline:

```text
coarse terrain available
        ↓
show coarse terrain
        ↓
request finer residual
        ↓
worker generates / loads residual
        ↓
GPU upload
        ↓
wait for local transition conditions
        ↓
morph residual into terrain
        ↓
full visual terrain
```

The user should never see:

```text
coarse terrain
-> instant different terrain
```

---

# 25. Cancellation

If the camera leaves the area before refinement completes:

```text
cancel fine erosion / refinement jobs
```

or:

```text
deprioritize them heavily
```

Do not waste worker time refining terrain that is no longer relevant.

Coarse fallback remains sufficient.

---

# 26. Cache Strategy

Fine residual terrain data can be cached separately from base terrain.

Example:

```text
BaseHeight
DeltaLOD2
DeltaLOD1
DeltaLOD0
```

This allows:

```text
load only required detail level
reuse already generated residuals
avoid rerunning erosion
```

---

# 27. Suggested Data Model

Example:

```cpp
struct TerrainChunkData
{
    HeightField baseHeight;

    HeightDelta lod2Delta;
    HeightDelta lod1Delta;
    HeightDelta lod0Delta;

    MaterialControlData materialData;
    ErosionData erosionData;

    SimulationTerrainData simulation;
};
```

Or residuals may live in GPU textures/buffers rather than CPU memory.

---

# 28. Suggested Terrain Pipeline

```text
World Macro Height
        ↓
Global Drainage
        ↓
Large-Scale Erosion
        ↓
Simulation Heightfield
        ↓
LOD2 Residual
        ↓
LOD1 Residual
        ↓
LOD0 Residual
        ↓
Visual Terrain
```

Runtime:

```text
Simulation Terrain
        +
Loaded Visual Residuals
        =
Displayed Terrain
```

---

# 29. Acceptance Criteria

The system is acceptable if:

- higher LOD visibly refines terrain instead of replacing it;
- major terrain features remain spatially stable;
- no major river, cliff, or route moves during LOD transitions;
- buildings do not float or become buried after LOD refinement;
- navigation topology remains stable;
- chunk boundaries are not visible;
- erosion detail is continuous across chunks;
- LOD transitions are gradual;
- fine detail can be canceled when the camera moves away;
- coarse terrain is sufficient for immediate RTS rendering;
- small terrain details are delegated to shaders where possible.

---

# 30. Final Principle

The terrain system should behave as:

```text
stable world
+
progressively revealed detail
```

not as:

```text
different regenerated worlds
for each LOD
```

The correct mental model is:

```text
Base World
    +
Large Detail
    +
Medium Detail
    +
Fine Detail
```

with each layer added deterministically and progressively.

This preserves visual continuity, gameplay consistency, and fast streaming while still allowing high-detail erosion and terrain refinement.
