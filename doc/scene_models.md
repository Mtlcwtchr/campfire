# Nature models: import, instancing and LOD

The explorer draws a small selected set from `assets/models/Stylized Nature MegaKit[Standard].zip`:
CommonTree_1, Pine_1, Bush_Common, Rock_Medium_1 and Mushroom_Common.
These are Quaternius models, CC0-1.0; source: <https://quaternius.com>.
The archive's original `License_Standard.txt` is copied beside generated assets.
Other archives in `assets/models` are not imported or assigned this licence.

## Build

C++ has no Python runtime dependency. Offline preparation needs Python **3.12+**,
numpy and Pillow, pinned in `tools/scene_models_requirements.txt`.

Use the existing `.cache/terrain-reference-tools` environment, or create one:

```sh
python3.12 -m venv .cache/scene-model-tools
.cache/scene-model-tools/bin/python -m pip install -r tools/scene_models_requirements.txt
cmake -S . -B build -DASR_SCENE_PYTHON="$PWD/.cache/scene-model-tools/bin/python"
cmake --build build --target asr_client -j 6
```

When the existing terrain-reference environment is present CMake selects it by
 default unless `ASR_SCENE_PYTHON` is set. `ASR_PREPARE_SCENE_MODELS=ON` is the
client-build default. The archive, importer and requirements are dependencies of
`asr_scene_models`; the client depends on that target. A missing dependency fails
the preparation rather than silently claiming that objects are available.
Headless builds do not need Python. `-DASR_PREPARE_SCENE_MODELS=OFF` deliberately
skips preparation; already prepared assets can still be rendered.

The standalone command is:

```sh
.cache/terrain-reference-tools/bin/python tools/prepare_scene_models.py
```

Output: `assets/generated/scene_models/`, intentionally ignored by Git. To rebuild
all outputs after a partial deletion remove the generated manifest, then rebuild
the client. Original archives remain untouched. Missing generated content at runtime
is explicitly reported; the rest of the explorer remains usable.

## Rendering contract

- glTF metres/Y-up are converted to Z-up; pivots are placed at the base. Per-species
  size is fixed in physical metres, not scaled with world dimensions.
- A shared vertex/index mesh per model; material index is a vertex attribute into
  texture arrays. All instances use the engine's frame arena and instanced draws.
- **Level chain per model** (`SCM2`, manifest version 2): the imported mesh plus
  three simplified levels at roughly a quarter, a tenth and a twenty-fifth of its
  triangles. All levels index one vertex buffer and differ only by index range,
  so a level change costs two numbers, not a binding. `tools/mesh_lod.py` builds
  them offline: quadric-error half-edge collapse for solid geometry, with a
  per-component quota so no branch is ground down while its neighbours keep every
  triangle, and whole-card thinning with area compensation for alpha-cut leaves,
  which a collapse would turn into half-leaves. Grown cards are clamped to the
  imported silhouette so the shared billboard frame does not widen.
- A level is chosen by **projected shape error**, not by distance or a triangle
  ratio: each level carries the 95th-percentile distance from the imported
  surface in model metres, and the coarsest level whose error stays under
  `kLevelPixelError` (3 px) wins. The instance scale cancels, so a sapling and a
  giant that reach the same height on screen get the same triangles. The choice
  depends on the model and the screen alone - never on how crowded the frame is.
  Mesh budget pressure still moves the mesh/impostor crossover, never the level.
- For exact nearby objects, below 22 projected pixels: eight-azimuth impostor. Above 38: full mesh. Between
  them: complementary screen-door transition, no unsorted alpha blending. Under
  dense load this interval shifts upward to keep mesh submissions within **1,500,000
  triangles**, prioritizing larger screen footprints. Pixel-size ties are excluded
  together at the cutoff; camera movement never changes the scatter population.
  Objects denied a mesh remain impostors, not holes in the forest.
- The impostor contains baked albedo and object-space normals, not fixed sunlight;
  current landscape lighting, root occlusion, transmission and haze are applied
  at runtime. Geometry also uses authored normal maps. Both arrays have mip chains.
- Exact objects fade below 0.8–2 pixels. Their 768 m source window is a residency
  limit, not the visibility limit of the forest. View-wide grove impostors cover
  the displayed terrain; ready exact data replaces them over 360–560 m with a
  time fade. Cold far views do not generate precise tree positions (14/20 px
  request hysteresis). A focus jump retains a coarse fallback.
- Perspective residency demand uses projection and resident terrain depth, not
  zoom alone (`GpuTerrain::vegetationPixelsPerMetre`). Raising a free camera can
  therefore switch to coarse groves/grass without changing zoom or scatter identity.
- Trees/vegetation share the wind field. The displacement formula is shared with
  CPU regression tests. Root height zero, wind zero and non-vegetation all produce
  zero displacement. Rocks and mushrooms are static.
- Scatter is world-coordinate/seed based, independent of render LOD. Conservative
  512 m land admission precedes site sampling; final height/water/slope/climate
  checks reject unsuitable candidates. A single bounded asynchronous task owns its
  own HeightField. World replacement drains it before destroying borrowed data.

The source window contains at most **192×192 candidate sites**, one jittered candidate
per **8 m** cell. A continuous seeded density field combines 512/192 m forest masses,
80 m clearings and 32 m grove variation. Tree frequency follows that field and the
local forest habitat, slope and treeline, rather than independent uniform thinning.
Bushes and rocks use separate 40/112 m fields; mushrooms favor wooded sites. Bush
frequency peaks in the intermediate-density edge belt (0.2–0.45), falling off in
both empty clearings and dense cores, rather than increasing with empty space.
Rock outcrops also form in sites with zero forest habitat. These physical scales
do not grow with world size. Tests require over 100 trees/ha in
forest cores and at least six times the tree frequency of clearings on the fixture.
Four-seed regressions check edge-biased bush frequency and clustered rock counts;
species changes, both focus axes and world resizing preserve placement in overlaps.

A settled model frame has at most one mesh batch per model and level plus one
shared impostor batch, including approximate nine-tree grove cards. There is no
collision geometry, planet-wide exact vegetation residency or dynamic object-edit
persistence system. There is no full shadow-map pass in this step. Level
transitions are an instant swap: there is no dithered cross-fade between two
levels of one model, only between a mesh and its impostor.

## Validation and screenshots

### Ground cover and adaptive grass

Terrain now uses the same biome palette as grass at every distance, retaining
substrate texture contrast rather than repainting only when cards disappear.
Fragment-stage world-climate sampling keeps biome boundaries independent of the
triangle grid. A climate channel holds woodland habitat × seeded forest density
at 64 m. Woodland is independent of the grass palette: pure steppe stays treeless,
while river valleys can retain a green palette with sparse trees. Stronger zero
regions in the forest field preserve open meadows. This intentionally changes
placement relative to steps 1–3, not its stability within the current version.

The active adaptive cut supplies two grass tiers: up to 65,536 precise roots on
a stable 2 m grid and up to 65,536 coarse group candidates over all displayed
blocks. Both use source/parent/prior triangle heights including stitched edges.
The 144–192 m fade is only the near/far handoff, not a limit on all vegetation.
Each distant block uses an appropriate coarser grid; unresolved grass is represented
by persistent terrain cover. Forest soil supports understory; water, rock, sand,
snow and unsuitable slopes are excluded. Grass costs are separate from the 1.5M
model mesh budget. Submitted roots include GPU-rejected candidates; compaction
and indirect batching are not implemented, so view-wide grass adds draw calls.

`tools/capture_vegetation_cover.py` uses the existing numpy/Pillow environment to
capture seven scenes and an isolated grass A/B. `ASR_GRASS_DISABLED=1` disables
only the grass pass in that diagnostic child process, never the terrain palette.
The current [step 4 report](reports/gen_rework_step_4/README.md) covers view-wide
groves/grass, cold far views, open biomes and the continuous pan/zoom trace.
Steps 1–3 preserve historical measurements; their population counts are not a
contract for the current woodland policy.

```sh
./build/asr_terrain_tests scene_
python3 tools/capture_vegetation_lods.py
```

The LOD capture tool launches eleven bounded client cases, including a free-camera
pair at height offsets 80/2000 m with the same zoom 12. It retries incomplete terrain,
checks cold far residency, actual readiness, budgets and instance-copy bytes, and
writes PNGs plus `.scene.json` metadata and `metrics.json` to the step 4 directory.
The four distant orbital cases retain minimums of 100 groves in 10 blocks. The upper
free-camera case requires grove presence instead, while preserving the >768 m reach,
far-grass coverage, no-exact-residency and budget checks; its current 82 groves in
7 blocks are measured results, not an orbital-density requirement.
`--trace` logs the same vegetation counters during actual movement;
`ASR_VEGETATION_TRACE_SETTLE=1` adds a held-camera tail, validated by
`tools/verify_vegetation_trace.py`. Runtime logs remain ignored.
The CPU budget regression covers all 24 orderings of mixed-cost demands, including
equal pixel sizes, and verifies that denied meshes retain full impostor coverage.
Metadata reports submitted LOD instance counts, not unique visible objects:
transition objects can have both a mesh and an impostor submission.
Terrain GPU/RAM figures do not include every model texture or driver allocation;
shared model geometry and instance-upload bytes are reported separately.

See [step 1 report](reports/gen_rework_step_1/README.md) for measured results and limits.
The [step 2 report](reports/gen_rework_step_2/README.md) covers coherent forest density,
the bounded mesh budget and the separate P1a physical-domain/scale-policy work.

