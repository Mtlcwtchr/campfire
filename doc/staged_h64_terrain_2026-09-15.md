# Generation stages and bounded H64/H16/H8 terrain — 2026-09-15

## Controls and scope

In the GPU explorer, **Shift+1…8** selects generation geometry. **M** cycles display modes independently; **0** selects natural materials; **Tab** opens settings. A compact **View / Stage / Display** readout remains visible with settings closed. Stage shortcuts use physical number-key scancodes with either Shift key; legacy worlds remain at Final. New display names accepted by the map selector: `grey`, `slope`, `stage-delta`, `erosion`, `catchment`, `geology`, `flow`; `relief` shows height with 50 m contours, `moisture` shows climate moisture.

Stage selection is renderer-owned. It does not modify the simulation world, water graph, saves, or final page identities. Legacy worlds without H64 snapshots have no reconstructed/fabricated generation history and remain at Final.

## Actual generation order

1. **Noise:** primary noise and continental drowning, calibrated into physical decimetres.
2. **Tectonics:** spatial tectonic/crustal contribution, not separately placed mountain families.
3. **Thermal erosion:** symmetric eight-neighbour Jacobi flux on the H64 lattice; all transfers read the previous pass. It conserves the height sum of this stencil.
4. **Volcanoes:** complete procedural edifices/calderas and eligible volcanic islands. New generation does not load DEMs or place Alpine, Glacial, Old, Folded, Arid, canyon or tower patches. Readers and direct legacy diagnostic functions remain available.
5. **Slope erosion:** repeated downhill receiver construction, linear-time topological accumulation and incision. Routing is recomputed after every erosion iteration. Published receivers are strictly downhill on the resulting H64 surface; adjoining drainage basins define divide edges. The solver uses integer arithmetic, including square root. Independent stencil rows use up to six workers; ordered accumulation is not six independent chunk-local rivers.
6. **Water:** standing sea/lake heads over the uncarved slope surface. Its bed is deliberately identical to stage 5. Lake/sea geometry is shown without final stream channels.
7. **Channels / hydraulic carving:** the existing shared hydrology graph owns river beds, water profiles, bank/valley carving and confluence levels. Fine pages sample this canonical geometry.
8. **Final:** same channel geometry with final materials/climate presentation. There is intentionally no extra H4 displacement pass.

`TerrainFoundation` stores five immutable H64 height arrays in signed decimetres, plus receivers, contributing-cell accumulation, detail-page evidence and basin-divide edges. Water and Channels are evaluated from the downstream hydrology products; Final does not invent a ninth geometry source. The five height arrays share the same sea-level/range calibration, derived during the macro preprocessing pass. They are not independently renormalized per displayed image. Existing macro thermal/coastal fields still establish this reference and volcano placement; they are not another runtime displacement layer.

## Stage geometry, not a colour overlay

The asynchronous planner includes both stage endpoints in its view identity and invalidates stage-dependent mesh caches. Final height pages retain their normal keys and are reused; stage geometry is sampled from immutable H64 snapshots, with final channels from the existing pages. Adaptive splitting considers both the old and new terrain endpoints and parent triangles. Shared-edge constraints solve both endpoints with identical floats.

The GPU retains the accepted cut and its page leases while the replacement meshes are incomplete. A complete replacement cut begins a common smoothstep transition; the old source endpoint, target endpoint and LOD parent/edge constraints remain independent of water height. Obsolete pending requests are replaced. A transition already on screen finishes before the latest queued selection starts, avoiding jumps from retargeting half-morphed geometry. Chunk meshes build in bounded asynchronous batches; publication/morph is synchronized across the cut, not independent unsynchronized per-chunk timers. Water is hidden while the stage geometry moves, and final foliage is disabled before Final.

Stage normals, contour height and triplanar height use the displayed geometry rather than silently sampling the final terrain underneath. Grey, slope, signed predecessor-stage delta, erosion depth, contributing area, geology and receiver-direction maps are separate display choices. Catchment and Flow explicitly show the **slope-stage H64 graph**, even when another geometry stage is selected. At coarse view distances these vertex diagnostics are interpolated/reduced with the mesh; they are not full-resolution per-pixel scientific rasters. Moisture/geology/material data are final world products, not historical climate snapshots.

## RAM preparation and residency

- New H64 worlds pre-bake/pin **only H64**, even when the old caller requests `{2,4}`.
- H16/H8 are generated on demand. H64 slope/incision evidence and a conservative river/lake halo determine where fine source pages are useful; camera distance further limits H8 demand.
- CPU fine pages live in the bounded PageStore LRU. H16 is no longer part of the unbounded pinned startup set.
- H16 GPU atlas side is bounded by `2 * h8AtlasSide` (and land coverage); only H64 has permanent atlas pins.
- Planner working-set, sibling-family, parent/halo and preload accounting covers levels 0/1/2. H4 capacity is zero in the active runtime renderer.
- Active camera-budget policies do not instantiate TerrainDetail's old local H4 or H2 feature geometry. Finest runtime mesh step is 8 m. Legacy CPU comparison fixtures remain intentionally testable.
- New-world material classification uses the same H64 height/derivatives at every LOD. The special H64 macro-material shortcut was removed because it repainted shared points. There are no repeated fine river-height queries hidden in this material pass. Fine shoreline treatment remains a water-page shading concern; canonical materials do not add a separate narrow-bank carve.
- Coverage includes land present in any saved geometry stage, not only the final land mask.

This is bounded **fine** residency, not an infinite-world implementation. H64 snapshots, H64 pins, the GPU page table and macro world still scale with total area. The current foundation refuses grids above 16,777,216 samples. Five int32 height arrays alone use 20 bytes per H64 sample; receivers, accumulation, divides, temporary solver arrays and pages are additional memory.

## Saves and caches

World JSON version 3 embeds the complete H64 snapshot. Savegame world-parameter section version 3 restores its snapshot before downstream climate/hydrology reconstruction; old sections 1/2 explicitly keep the legacy path. Snapshot readers validate dimensions, array lengths, height ranges, receiver adjacency/strict descent, divide endpoints and a content fingerprint. Diagnostic timing values are not part of saved identity. Old hybrid v1/v2 payloads remain readable.

Page-generation version: **9**. Hydrology-graph version: **11**. Cache fingerprints include the H64 foundation, not just macro cells or volcano patches. No old user cache was deleted.

## Measurements

Headless optimized (`-O2`) benchmark, seed 11, six workers, disk cache disabled. Numbers below are observed runs, not universal guarantees or a full client startup measurement. Climate texture generation/upload, mesh building, GPU upload and drawing are **not** included in the RAM column.

| World cells | H64 grid samples | Whole generator | Hydrology graph | Cold RAM H64 | Pinned pages | Pinned page payload |
|---|---:|---:|---:|---:|---:|---:|
| 128×128 | 1,168,561 | 0.090 s | 0.084 s | 0.452 s | 6,364 | 45.130 MiB |
| 192×192 | 2,627,641 | 0.228 s | 0.182 s | 0.818 s | 12,834 | 91.013 MiB |

Both runs published finest pinned level 4 (H64), zero failed pages and zero reused parent samples. H16 was not globally prepared. The slope/incision evidence flagged 4,664/18,225 and 6,925/41,209 total geographic pages respectively; actual fine demand also accounts for water and the view, so these are **not** measured resident H16 counts.

Artifacts: `build/staged-terrain-benchmark.cpp`, `build/staged-terrain-benchmark-128.log`, `build/staged-terrain-benchmark-192.log`. These are local ignored benchmark files, not a shipped executable.

## Verification

The initial H64 implementation was verified before the high-peak/readout follow-up below:

- Client, CPU terrain tests and Metal GPU tests build successfully.
- 30/30 focused hybrid/mountain/H64/water tests.
- 18/18 selected save/determinism/hydrology regressions.
- 8/8 targeted stage geometry, H16 admission and no-H4 regressions.
- 20/20 Metal tests, including pixel readback of both stage endpoints, concurrent LOD morph, shared edges, water head and skirts.
- Full terrain suite: **132/133**. The remaining existing config test expects detail-distance 2 / lookahead 512, while the user's checked-in config has 8 / 256. The config was not changed to satisfy that unrelated expectation.

Logs are `build/staged-terrain-*.log`. Previous irrigation and legacy-water baseline failures were not claimed fixed by this work.

## High-peak and readout follow-up

- Shared height calibration now includes the un-eroded primary/tectonic maxima, rather than clipping summits to the lower post-thermal maximum. A smooth quadratic highland expansion above 300 m raises the reference peak from 2295 m to 4290 m without raising low plains. H64 physical heights are not limited by the 8-bit macro summary.
- The pre-erosion peak regression checks summit/shoulder separation, unchanged low plains, zero-pass identity, thermal mass conservation, slope-stage erosion, and quantisation/culling bounds for every stored stage.
- The direct legacy mountain DEM sampler has a low-pass regression: planar slopes remain unchanged while isolated small peaks are attenuated. New staged generation still does **not** load DEMs.
- Validation found signed volcanic flank noise lowering the bed at the outer foot. Only the added volcanic relief is now bounded below by zero; absolute terrain heights are not capped or flattened. Existing tests retain hillside-independent deltas, a caldera below its rim, irregular deposits and v2/v3 snapshot round trips.
- Menu tests now traverse all diagnostic displays rather than expecting Wind to wrap directly to Natural. They cover independent stage/display selection, both Shift keys, ignored repeat/unmodified/out-of-range keys and legacy-world stage rejection.

Follow-up verification (logs `build/staged-peaks-*.log`):

- Client and all five requested test/client build targets compile successfully.
- **134/135** full terrain tests; only the same checked-in LOD config expectation fails. No user configuration was changed.
- **5/5** menu tests; **18/18** selected save/determinism/hydrology tests; **20/20** Metal GPU tests.
- A bounded Metal explorer smoke run (`--explore --seed 42 --world 64 --camera orbit --at mountains --zoom 0.12 --shot-frame 180`) exited successfully and wrote `build/staged-peaks-client-smoke.png`. The screenshot request also supplied that path with `--shot`. H64 preparation reported 1686 pages and zero errors; the frame drew 123 patches / 103414 triangles. This was a fixed-frame smoke run, not a claim that all LODs had settled or that landscape appearance was visually approved.

The timings in Measurements above precede this follow-up and were not re-benchmarked.

## Known limitations and visual review

This is a first inspectable H64 erosion pipeline, **not** a validated natural-landscape simulation. D8 routing can retain directional bias and closed sinks. The slope solver's terminal deposition is capped; it is not a mass-conserving sediment-transport model across all passes. H16/H8 currently add canonical channel geometry, not a separate high-resolution regional hillslope erosion simulation. The divide graph is derived from the H64 drainage forest; the large river network still uses the macro priority-flood outlet topology. A full H64 watershed-constrained rerouting of that river network is **not implemented**.

Numeric tests and shader readback establish contracts, not artistic quality. Mountain silhouettes, valley branching, river/ridge relationships and stage-transition appearance still need in-game visual review. The previously running user Debug client was not stopped or replaced; it does not acquire these changes without a rebuild/relaunch.

