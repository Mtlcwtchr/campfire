# Fractal ridge skeleton — 2026-09-15

> Historical integration snapshot. The skeleton topology and its direct/indexed
> sampling tests below remain valid. Universal regional mountain stamping,
> the 80/20 DEM blend, uniform patch radii and cache versions below are superseded
> by [coherent massifs and downhill incisions](coherent_mountain_massifs_2026-09-15.md).
> New hybrid worlds use the positive skeleton for at most one Alpine massif;
> other uplands use coherent family bodies and subtractive erosion. Current
> page generation / hydrology versions are 8 / 10; hybrid snapshots support v1/v2.

## Method

The previous generator selected one global orientation and repeated parallel
spines. The replacement draws a finite recursive 2D ridge graph and derives
height from distance to its edges.

- Three nonparallel primary arms and five recursion levels (orders 0–4).
  Each branch has three segments: displaced anchors near one third and two
  thirds of its length, followed by a terminal taper.
- Two daughters per branch, one at each anchor on opposite sides. Their lengths
  are 60–70% of the parent's end-to-end length. Oblique forward directions
  (approximately 38–48 degrees) extend spurs rather than curling them into the
  core. The side assigned to the first anchor varies by seed.
- Anchor heights retain 93–98% and 78–86% of the branch's starting height.
  Only terminal segments ease down to zero, with zero slope at the ending.
  Descendant widths still shrink; cross-sections keep a defined crest and a
  smooth foot. This changes the structural graph, not the noise octave count.
- Junctions with degree 3 or more receive a bounded lift (5–14% of local node
  height). Node heights and branch lengths vary by seed. Plain overlaps are
  combined by maximum, not summed into unbounded artificial peaks.
- Each 8.192 km region has its own rotation, geometry and centre offset. A
  3×3 neighbouring-region stencil carries complete branches across boundaries.
  The expanded spatial index reaches 11.264 km from the nominal centre, below
  the closest omitted region's 12.288 km distance.
- Mountain patches have a 9.216–9.984 km radius, covering the local skeleton,
  widths and placement warp. The patch index follows the enlarged footprint.
  Volcano/island footprints are unchanged. The validated save limit is now a
  10.240 km radius (641×641 samples at 32 m spacing).
- Real mountain DEMs retain 80% of their filtered relative shape; the skeleton
  contributes 20%. Without a DEM, the skeleton supplies the structural form.
- The short-wavelength noise suppression from the previous revision remains.
  This changes ridge geometry, not just fragment normals or material textures.

The implementation uses deterministic Fixed arithmetic. Skeletons are held in
a 16-entry thread-local LRU with fixed-capacity arrays (320 nodes/edges each)
and a 512 m spatial index. A complete tree has 280 nodes, 279 edges and 91
junctions. Keeping two daughters per branch, rather than two at every anchor,
avoids explosive growth and capacity truncation. Graph construction happens on
cache misses, not at every terrain sample. There is no shared lock or heap
allocation in queries.

## Code and cache versions

- `src/game/generation/mountain_shape.{hpp,cpp}`
- `src/game/generation/hybrid_terrain.cpp`
- `tests/test_hybrid_terrain.cpp`
- `tools/mountain_shape_probe.cpp`

Persistent page generation version: **7**. Hydrology graph version: **9**.
The hybrid snapshot format is unchanged; existing saved DEM patches are not
silently regenerated. New worlds receive the complete updated blend.

## Validation

- Structural/hybrid tests: **18/18**.
- Save/load, determinism, canonical-water and hydrology regressions: **20/20**.
- Full terrain suite: **123/124**. The remaining failure is the pre-existing
  checked-in LOD config expectation mismatch (`detailDistanceScale` and
  `maxLookAheadMetres`), reproduced before these changes.
- Connectivity, all five orders, 60–70% daughter lengths, two separated
  attachment points, retained heights and smooth terminal taper are tested.
- Rotated support fits both the index and circular placement footprint across
  256 seeds, including ridge widths, regional jitter and placement warp margin.
- Indexed sampling equals direct all-edge sampling, including all five bitset
  words. The regional stencil matches an unclipped 5×5 union. X/Y seam tests
  check convergence from both sides, distinguishing a legitimate steep slope
  from a discontinuity. Cache eviction and worker-order independence are covered.
- The perimeter/save fixture uses a 128-cell map so it contains actual mountain
  patches; the original 64-cell map lacked them even before this revision.
  Dedicated tests cover indexing and save/load at enlarged patch extremities,
  and still accept the previous 8192 m radius.
- Client and diagnostic executables were built.

Measured against the four-order working tree at the start of this revision,
using the same standalone seed-42 skeleton:

| Metric | Before | After |
| --- | ---: | ---: |
| Nodes / edges | 91 / 90 | 280 / 279 |
| Total ridge length | 70.223 km | 96.273 km |
| Maximum centreline radius | 5.261 km | 5.700 km |

On the seed-42 / 96-cell / 8.192 km production control area sampled every 32 m:
247 local maxima above the 0.25 m prominence threshold, maximum height
2304.82 m, mean absolute discrete curvature 1.441 m. Before: 19 maxima,
2386.52 m and 0.408 m. Noise settings were not increased. These metrics describe
one diagnostic area, not a world-wide quality guarantee or a visual match to
`assets/PHX_Alphas_LITE/Details/Fractal.png`; visual similarity is not verified.

## Viewing the construction

- `build/expanded-ridges-after-skeleton.svg`: the skeleton
  beside its distance-field relief; a standalone seed-42 example.
- `build/expanded-ridges-after-world.svg`: production
  HeightField at the control area, including DEM and hydrology.
- `build/expanded-ridges-before.json` / `build/expanded-ridges-after.json`:
  comparison metrics; the corresponding `before-*.svg` files preserve the baseline.
- Test/build logs: `build/expanded-ridges-focused-tests.log`,
  `build/expanded-ridges-terrain-suite.log`, `build/expanded-ridges-regressions.log`
  and `build/expanded-ridges-final-build.log`.

Generate both views with:

```sh
./build/mountain_shape_probe \
  build/expanded-ridges-after-world.svg \
  assets/generated/terrain_references \
  build/expanded-ridges-after-skeleton.svg > build/expanded-ridges-after.json
```

These are shaded field diagnostics, not screenshots from the game renderer.
