# Scale-Aware Procedural World Generation

## Goal

The world generator must support very different world sizes, from approximately `100 × 100 km` up to `10000 × 10000 km`, without making the smaller world look like an arbitrary cropped fragment of a much larger one.

The key requirement is:

> A `100 × 100 km` world should feel like a complete compact world, while a `10000 × 10000 km` world should feel like a genuinely continental-scale world.

This requires separating generation features by how they scale with world size.

## Core Principle: Scale-Aware Generation

Do not define every feature in absolute kilometers.

If major mountain belts, continental basins, climate zones, and tectonic structures always use the same physical dimensions, then a small world will look like a cropped piece of a much larger world.

Instead, terrain generation should use a mixture of:

1. **World-relative scales**
2. **Semi-relative scales**
3. **Absolute physical scales**

## 1. World-Relative Features

These features should scale strongly with the size of the generated world.

Examples:

- continental layout
- major ocean basins
- tectonic regions
- major uplift zones
- continentality
- broad climate belts
- very large mountain systems

Use normalized world coordinates:

```cpp
float2 uv = worldPos / worldSize;
```

The macro generator works primarily in this normalized coordinate space.

For example:

```text
100 × 100 km world:
    a major uplift zone may occupy 20–40 km

10000 × 10000 km world:
    a major uplift zone may occupy hundreds or thousands of km
```

The same structural logic is preserved, but its physical footprint changes with the world.

## 2. Semi-Relative Features

These features should scale with world size, but not necessarily linearly.

Examples:

- mountain systems
- major river basins
- large lakes
- regional valleys
- large plateaus
- secondary climate regions

A larger world should generally contain:

- more mountain systems
- more river basins
- more internal drainage regions
- more large lakes
- more climatic variation

A smaller world should compress these systems into a coherent composition rather than simply remove most of them.

For example:

```text
100 × 100 km:
    2–4 major mountain systems
    several significant river basins
    several broad ecological/climatic regions

10000 × 10000 km:
    dozens or hundreds of mountain systems
    major continental drainage networks
    enormous interior basins
    many climate regions
```

## 3. Absolute Physical-Scale Features

These should remain approximately the same physical size regardless of world dimensions.

Examples:

- gullies
- erosion channels
- local cliffs
- river width
- river banks
- small hills
- terraces
- talus
- individual trees
- vegetation
- rocks
- local terrain roughness

A `100 × 100 km` world should not have miniature trees, miniature erosion, or rivers reduced by a factor of 100 simply because the overall world is smaller.

These systems operate in meters, not normalized world coordinates.

Example:

```text
tree height:          meters
small river width:    meters
gully width:          meters
cliff breakup:        meters
local erosion scale:  meters
```

# Recommended Generation Model

```text
WORLD SIZE
    ↓
Normalized macro coordinates
    ↓
Tectonic / continental structure
    ↓
Major uplift and mountain belts
    ↓
Regional mountain systems / basins
    ↓
Absolute-scale terrain processes
    ↓
Local erosion / rivers / cliffs / vegetation
```

The generator therefore uses two coordinate systems simultaneously:

```text
Normalized space
    -> determines global composition

World-space meters
    -> determines physical terrain detail
```

# Example

## 100 × 100 km

The macro generator sees a complete normalized domain:

```text
0..1 × 0..1
```

It may generate:

- several uplift zones
- 2–4 significant mountain systems
- multiple drainage basins
- coastlines
- a compact climate layout

Local terrain then uses normal real-world dimensions for:

- valleys
- rivers
- erosion
- cliffs
- vegetation

The resulting map should feel like a deliberately compact world.

## 10000 × 10000 km

The macro domain is still:

```text
0..1 × 0..1
```

but its physical size is vastly larger.

The generator therefore increases:

- the number of regional structures
- hierarchy depth
- drainage complexity
- mountain-system count
- continental interiors
- climate diversity

Local features still operate in meters.

Thus the larger map gains more hierarchy instead of simply stretching every object by 100×.

# Important Rule

Do **not** scale everything linearly with `worldSize`.

Bad:

```cpp
treeSize      *= worldScale;
riverWidth    *= worldScale;
gullySize     *= worldScale;
cliffSize     *= worldScale;
```

This would make a large world physically absurd and a small world look miniature.

Instead:

```text
Macro composition:
    world-relative

Regional structures:
    semi-relative

Local terrain physics:
    absolute scale
```

# Final Architecture

```text
WORLD-RELATIVE
==============
continents
tectonic structure
major uplift
ocean basins
broad climate zones


SEMI-RELATIVE
=============
mountain systems
major rivers
large lakes
regional basins
plateaus


ABSOLUTE SCALE
==============
local hills
river width
gullies
cliffs
erosion
terrain breakup
trees
rocks
vegetation
```

This ensures that `100 × 100 km` looks like a complete compact world, while `10000 × 10000 km` looks like a much larger and more hierarchically complex world, rather than the smaller world appearing to be merely a cropped fragment of the larger one.

Алгоритмы:
Global macro relief: ridged multifractal + domain warp + uplift masks/skeletons.
Major drainage: D8/D∞ flow direction + flow accumulation на грубой сетке.
Rivers/valleys: stream-power erosion по flow accumulation и slope.
Slope stabilization: thermal erosion / talus relaxation.
Canonical 16 м erosion: grid-based hydraulic erosion, не particles.
4 м detail: ограниченная local erosion, можно уже дешёвый particle/snowball или маленький grid solver.
Cliffs / rock breakup: отдельные slope/curvature rules, а не общая erosion sim.
Sediment: отдельное deposition поле, чтобы не получить просто “выцарапанный” terrain.

Adaptive Hierarchical Terrain Refinement

Для генерации больших миров не нужно строить terrain с одинаковым шагом по всей карте. Вместо uniform grid используется адаптивная иерархия, где каждый участок уточняется только до того уровня детализации, который ему реально нужен.

Базовая идея:

256 м
↓
проверить участок
↓
если он простой — остановиться
если есть важная структура — subdivide

64 м
↓
проверить детей
↓
простые оставить
интересные subdivide

16 м
↓
то же самое

4 м
↓
то же самое

То есть участок равнины может навсегда остаться на 256 м, а горный склон, берег или речная долина пройти всю цепочку:

256 → 64 → 16 → 4

В результате подробная геометрия и дорогие расчёты выполняются только там, где они действительно нужны.

Критерий refinement

Для каждого узла считается NeedsRefinement().

Он должен учитывать две группы факторов:

1. Geometry-driven
2. Feature-driven

Geometry-driven критерии:

height range
slope variation
curvature
roughness
approximation error

Feature-driven критерии:

river intersection
coastline
cliff
ridge
fault
road
settlement
biome/material boundary
other gameplay-significant feature

Пример:

bool NeedsRefinement(const TerrainNode& node)
{
return
node.approximationError > errorThreshold ||
node.heightRange > heightThreshold ||
node.slopeVariance > slopeThreshold ||
node.hasRiver ||
node.hasCoast ||
node.hasCliff ||
node.hasRidge ||
node.hasRoad ||
node.hasSettlement ||
node.hasMaterialBoundary;
}

Нельзя опираться только на variation высоты. Плоская coarse-cell может содержать узкую реку, дорогу или берег, которые на текущем разрешении почти не влияют на высоту, но всё равно требуют subdivision.

Лучше всего использовать approximation error

Наиболее универсальный критерий:

насколько coarse representation отличается от более точного источника

Например:

float error = MaxDeviation(coarseSurface, referenceSurface);

if (error < threshold)
keepAsLeaf();
else
subdivide();

Это позволяет автоматически оставлять плоские и простые участки грубыми, а сложные раскрывать глубже.

Threshold может зависеть от уровня:

256 м -> большой допустимый error
64 м  -> меньше
16 м  -> ещё меньше
4 м   -> финальный локальный уровень

Или от gameplay importance:

обычная равнина       -> высокий threshold
река                   -> низкий threshold
город                   -> очень низкий threshold
cliff / проход / берег  -> низкий threshold
Структура данных

Под это хорошо подходит quadtree или аналогичное sparse tree:

struct TerrainNode
{
Bounds bounds;

    uint8_t level;
    uint8_t flags;

    float approximationError;

    uint32_t firstChild; // INVALID если это leaf
};

Если:

firstChild == INVALID

значит subdivision остановлен и этот node является финальным представлением данного участка.

Пример дерева:

256m root
├── 64m leaf
├── 64m
│   ├── 16m leaf
│   ├── 16m
│   │   ├── 4m leaf
│   │   ├── 4m leaf
│   │   └── ...
│   └── ...
├── 64m leaf
└── 64m leaf

Таким образом подробные уровни вообще не существуют там, где они не нужны.

Генерация level-by-level

Удобно генерировать не рекурсивно, а списками кандидатов.

Например:

currentCandidates = all 256m cells

Для каждого:

if NeedsRefinement(cell)
push children into nextCandidates
else
mark as leaf

После прохода:

currentCandidates = nextCandidates

и идём на следующий уровень.

Получается:

256m candidates
↓
64m candidates
↓
16m candidates
↓
4m candidates

Такую систему легко параллелить через jobs или compute shader.

Какие уровни использовать

Один из практичных вариантов:

256 м
macro structure

64 м
regional terrain

16 м
canonical gameplay terrain

4 м
local refinement

При необходимости:

1 м
very-near local detail

Но этот уровень должен появляться только на небольшой площади.

Важный принцип

Refinement должен быть sparse, а не uniform.

Плохой вариант:

весь мир:
256
↓
64
↓
16
↓
4

Хороший:

равнина:
256

пологая местность:
256 → 64

холмы:
256 → 64 → 16

горы / реки / берег:
256 → 64 → 16 → 4

Это означает, что размер данных и время генерации зависят не от:

worldArea × finestResolution

а скорее от:

количества реально сложных участков
Масштабирование мира

Для 100×100 км система может раскрыть значительную часть terrain до 16 м.

Для 10000×10000 км большая часть мира может оставаться на 256 м или 64 м, а detailed levels будут существовать только вокруг:

mountain systems
river networks
coasts
settlements
important gameplay regions

Поэтому увеличение размера мира не требует увеличивать весь terrain dataset пропорционально площади на максимальной детализации.

Связь с глобальными features

Глобальные структуры должны существовать независимо от refinement tree:

river graph
ridge graph
coastline
roads
faults
settlements

При проверке node:

если feature пересекает node
→ node обязан refinement

Это предотвращает потерю узких, но важных структур.

Например:

256м cell почти плоская

но через неё проходит:

20м river

Тогда:

height variance low
BUT
hasRiver = true
→ subdivide
Итоговая архитектура
GLOBAL FEATURES
===============
ridge graph
river graph
coastline
faults
roads
settlements

        ↓

ADAPTIVE TERRAIN TREE
=====================

256m
↓
NeedsRefinement?

    NO  -> leaf

    YES
     ↓

64m
↓
NeedsRefinement?

    NO  -> leaf

    YES
     ↓

16m
↓
NeedsRefinement?

    NO  -> leaf

    YES
     ↓

4m
↓
final local terrain

Главная идея:

Не генерировать максимальную детализацию там, где coarse representation уже достаточно хорошо описывает поверхность.

Это позволяет одновременно поддерживать маленькие детальные миры и огромные sandbox-миры без необходимости хранить или генерировать весь мир на минимальном шаге.