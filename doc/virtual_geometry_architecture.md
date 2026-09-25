# Virtual Geometry Architecture for Terrain, Static Objects, Instancing, and Dynamic Characters

## 1. Goal

Implement a unified virtualized/adaptive geometry system for a custom RTS / god-sim engine.

The system must avoid the classic workflow:

```text
LOD0 mesh
LOD1 mesh
LOD2 mesh
LOD3 mesh
```

as the primary rendering model.

Instead, the renderer should operate on adaptive geometry representations selected by screen-space error and visibility.

Main requirements:

- no manual LOD authoring for most assets;
- no need to keep several full duplicate meshes for each asset;
- no hard LOD popping;
- geometry density should depend on visible geometric importance, not only distance;
- large flat/simple regions should use very little geometry;
- silhouettes, cliffs, ridges, branches, character shapes, and other important features must retain detail;
- static instanced geometry must reuse the same geometry hierarchy across all instances;
- terrain, static meshes, foliage, and dynamic/skinned meshes may use different backends while sharing the same high-level virtual-geometry architecture;
- the system must support very large worlds;
- the renderer should be GPU-driven as much as practical.

---

## 2. High-Level Architecture

Use one common abstraction:

```text
                   Virtual Geometry Layer
                            |
                    Screen-Space Error
                            |
                     Visibility / Culling
                            |
                    Cluster / Patch Selection
                            |
                      GPU Draw Submission
                            |
        ------------------------------------------------
        |                    |                         |
     Terrain              Static Mesh              Dynamic Mesh
        |                    |                         |
 Adaptive grid /       QEM cluster hierarchy     Skinned cluster /
 terrain hierarchy                               generated LOD hierarchy
```

Important principle:

> Do not force all object types through the same simplification algorithm. Share the selection, visibility, error, residency, and draw architecture, but use specialized geometry representations per asset class.

---

## 3. Common Runtime Concepts

### 3.1 Geometry Cluster

For arbitrary meshes, use a small meshlet/cluster as the basic rendering unit.

Typical target:

```text
~64-128 triangles per cluster
```

Possible structure:

```cpp
struct GeometryCluster
{
    Bounds bounds;
    float geometricError;

    uint32_t parent;
    uint32_t firstChild;
    uint16_t childCount;

    uint32_t vertexOffset;
    uint32_t indexOffset;

    uint16_t vertexCount;
    uint16_t triangleCount;

    uint16_t materialID;
    uint16_t flags;
};
```

The cluster should know:
- its bounding volume;
- its geometric approximation error;
- where its geometry lives;
- parent/children relationship;
- material/geometry flags.

### 3.2 Screen-Space Error

LOD/refinement selection should be based primarily on projected geometric error, not raw distance.

Conceptually:

```cpp
screenError =
    geometricError *
    projectionScale /
    distanceToCamera;
```

Interpretation:

```text
small projected error
    -> current representation is sufficient

large projected error
    -> refine / descend into children
```

Example thresholds:

```text
< 0.5 px     keep coarse representation
0.5-1.5 px   transition region
> 1.0 px     refine
```

Exact values must be configurable.

### 3.3 Hysteresis

Use two thresholds:

```text
refineThreshold
collapseThreshold
```

Example:

```text
refine when error > 1.2 px
collapse when error < 0.8 px
```

This prevents LOD oscillation.

### 3.4 Geomorph

Where possible, transitions should morph geometrically rather than pop.

General form:

```cpp
position =
    parentPosition +
    detailDelta * morph;
```

or:

```cpp
position =
    lerp(parentRepresentation,
         childRepresentation,
         morph);
```

At morph = 0 the child must reproduce the parent surface.
At morph = 1 the child reaches its full detailed form.

---

## 4. GPU-Driven Selection Pipeline

Target runtime flow:

```text
1. Start from resident roots
2. Frustum cull
3. Optional occlusion cull
4. Evaluate projected geometric error
5. If current cluster is sufficient:
      emit cluster
   else:
      traverse children
6. Build visible cluster/patch list
7. Submit through indirect draw
```

Prefer GPU traversal and indirect submission.

Avoid:

```text
GPU -> CPU readback -> CPU draw list -> GPU
```

Potential buffers:

```text
GeometryHierarchyBuffer
VisibleClusterBuffer
InstanceBuffer
IndirectDrawBuffer
ResidencyPageTable
```

---

## 5. Terrain Backend

Terrain should not be implemented as arbitrary QEM mesh simplification everywhere.

Terrain has major advantages:
- mostly a heightfield;
- regular spatial organization;
- known neighborhood;
- deterministic world-space coordinates;
- efficient subdivision;
- efficient error estimation;
- easy quadtree representation.

Use a specialized adaptive terrain hierarchy.

### 5.1 Terrain Hierarchy

Example detail hierarchy:

```text
256 m
  ↓
64 m
  ↓
16 m
  ↓
4 m
  ↓
1 m optional
```

This does not mean every region receives every level.

Refinement is sparse.

Example:

```text
flat plain:
256 m

rolling terrain:
256 -> 64

hills:
256 -> 64 -> 16

mountain ridge / river / coast:
256 -> 64 -> 16 -> 4
```

### 5.2 Terrain Refinement Criterion

Each terrain node should store or derive:

```text
geometricError
heightRange
slopeRange
feature flags
```

Refinement should happen if either:

```text
A. coarse representation is geometrically insufficient
```

or:

```text
B. an important feature requires finer representation
```

Example:

```cpp
bool NeedsRefinement(const TerrainNode& n)
{
    return
        ProjectedError(n) > threshold ||
        n.hasRiver ||
        n.hasCoast ||
        n.hasCliff ||
        n.hasRidge ||
        n.hasRoad ||
        n.hasSettlement;
}
```

Feature-driven refinement is required because an apparently flat coarse node may contain a narrow river, road, cliff edge, or other gameplay-significant feature.

### 5.3 Terrain Node Structure

Possible structure:

```cpp
struct TerrainNode
{
    Bounds bounds;
    float geometricError;

    uint32_t firstChild;
    uint16_t flags;
    uint8_t level;
    uint8_t childMask;
};
```

If:

```text
firstChild == INVALID
```

the node is a leaf.

### 5.4 Terrain Data Representation

Use hierarchical residuals where useful:

```text
H64
H16 = Parent64 + D16
H4  = Parent16 + D4
H1  = Parent4  + D1
```

This naturally supports geomorph:

```cpp
H = ParentSurface + DetailDelta * morph;
```

The child at morph = 0 is geometrically identical to the parent.

### 5.5 Terrain Geometry vs Shading Detail

Use three bands:

```text
REAL GEOMETRY
-------------
major terrain silhouette
ridges
gullies
river banks
terraces
cliffs
large rocks

SHADER DISPLACEMENT / PARALLAX
------------------------------
small ruts
small stones
surface breakup
decimeter-scale detail

NORMAL / MATERIAL DETAIL
------------------------
centimeter-scale roughness
soil grain
tiny rock structure
micro erosion
```

Rule:

> If a feature materially changes the silhouette or gameplay surface, it should become geometry. If it mainly affects local surface appearance, keep it in shading.

---

## 6. Static Mesh Backend

Static meshes include:

```text
rocks
buildings
props
tree trunks/branches
large vegetation
environment structures
```

Use:

```text
source mesh
    ↓
clusterization
    ↓
QEM simplification
    ↓
hierarchy generation
    ↓
runtime screen-space selection
```

### 6.1 Import-Time Processing

On asset import:

```text
1. Read source mesh
2. Preserve important seams/features
3. Partition into meshlets/clusters
4. Calculate QEM simplification costs
5. Generate simplified parent clusters
6. Compute geometric error
7. Build hierarchy
8. Quantize/compress geometry
9. Save engine-native geometry cache
```

Artists should not need to manually author LOD0/1/2/3 unless an asset requires special art direction.

### 6.2 Feature Preservation

Simplification must protect:

```text
silhouette edges
hard normals
UV seams where required
material boundaries
attachment points
important sockets
collision-critical geometry
```

Use weighted QEM constraints such as:

```text
silhouette penalty
normal deviation penalty
UV penalty
material-boundary penalty
```

---

## 7. Instanced Static Geometry

For repeated objects such as:

```text
trees
rocks
grass clumps
props
debris
```

the geometry hierarchy must exist once per asset, not once per instance.

Example:

```text
OakTree GeometryHierarchy
    shared by 100,000 instances
```

Each instance stores only instance data:

```cpp
struct GeometryInstance
{
    Transform transform;
    uint32_t geometryID;
    uint32_t variationID;
    uint32_t flags;
};
```

Runtime:

```text
instance culling
↓
cluster selection for visible instances
↓
indirect draw
```

Different instances of the same mesh may select different geometry resolutions depending on projected size.

---

## 8. Trees and Foliage

Do not treat all vegetation geometry identically.

Recommended split:

```text
WOOD
----
trunks
large branches

-> cluster hierarchy / QEM


LEAF MASSES
-----------
leaf cards
small leaf clusters

-> simplified cluster representations


GRASS
-----
instanced blades / clumps
distance-based density reduction
optional cards / impostors far away
```

Do not run arbitrary topology refinement per individual grass blade.

---

## 9. Dynamic / Skinned Mesh Backend

Characters and animated objects are harder because geometry moves every frame.

Do not rebuild topology every frame.

Instead:

```text
source skinned mesh
↓
generate simplification hierarchy in bind pose
↓
preserve skinning attributes
↓
runtime select geometry
↓
skin only selected vertices/clusters
```

The simplification representation must preserve:

```text
bone indices
bone weights
UVs
normals/tangents
material assignment
```

### 9.1 Initial Dynamic-Mesh Scope

Do not start with fully view-dependent continuous topology refinement inside every character.

First implementation:

```text
automatic generated mesh hierarchy
+
meshlet/cluster selection
+
GPU skinning
```

Later, if profiling justifies it:

```text
local cluster refinement
continuous progressive topology
```

---

## 10. Streaming and Residency

Separate:

```text
logical geometry hierarchy
```

from:

```text
resident geometry pages
```

A hierarchy node may exist even if its highest-detail children are not resident.

Runtime rule:

```text
desired child resident?
    YES -> refine
    NO  -> render parent and request child page
```

This guarantees a valid fallback at all times.

### 10.1 Residency Model

Possible page organization:

```text
GeometryPage
├ cluster metadata
├ quantized vertices
├ local indices
└ optional material data
```

Keep coarse/root geometry resident.
Stream fine pages on demand.

---

## 11. Occlusion and Culling

At minimum support:

```text
frustum culling
cluster bounds culling
instance culling
```

Later add:

```text
Hi-Z occlusion culling
backface/cone culling for meshlets
```

For very large RTS scenes, culling is essential.

---

## 12. Triangle Density Target

Avoid rendering large quantities of subpixel triangles.

Practical rough target:

```text
~4-16 pixels per triangle
```

depending on surface complexity.

Accept:

```text
2-8 px/triangle
```

for important silhouettes and detailed geometry.

Allow much coarser geometry:

```text
20-50+ px/triangle
```

for flat/smooth surfaces with very low geometric error.

Do not use triangle density alone.

Final refinement must be driven by projected geometric error.

---

## 13. Difference from Full Unreal Nanite

Do not attempt to reproduce every Nanite feature immediately.

Do not initially require:

```text
software rasterization
full generalized DAG compression
all-material support
every arbitrary mesh topology
extreme page compression
all Nanite visibility-buffer features
```

Target the valuable core:

```text
automatic hierarchy generation
meshlet/cluster representation
screen-space error
GPU culling
adaptive geometry
streamed pages
indirect rendering
geomorph where needed
```

---

## 14. Recommended Implementation Order

### Phase 1 - Common Infrastructure

Implement:

```text
GeometryID
GeometryInstance
GeometryCluster
Bounds
geometricError
GPU cluster buffers
VisibleClusterBuffer
indirect draw path
```

No fancy streaming yet.

### Phase 2 - Static Mesh Prototype

Implement:

```text
mesh import
QEM simplification
clusterization
hierarchy generation
screen-space selection
```

Test using:

```text
rocks
simple buildings
tree trunks
```

### Phase 3 - Terrain Backend

Implement:

```text
adaptive terrain quadtree
geometric error calculation
feature-driven refinement
nested terrain patches
geomorph
neighbor stitching
```

Terrain must use its own specialized hierarchy rather than generic QEM clusters.

### Phase 4 - Instancing

Implement:

```text
instance culling
shared geometry hierarchy
per-instance projected error
indirect batching
```

Test with:

```text
10k
50k
100k
```

trees/rocks.

### Phase 5 - Streaming

Implement:

```text
geometry page allocator
resident-page table
request queue
parent fallback
background loading
GPU upload
eviction policy
```

### Phase 6 - Dynamic Meshes

Implement:

```text
skinned cluster hierarchy
automatic simplified representations
GPU skinning of selected geometry
```

Start conservative.

### Phase 7 - Advanced Optimization

Only after profiling:

```text
Hi-Z occlusion
meshlet cone culling
better geometry compression
GPU hierarchy traversal optimization
persistent visibility
page prediction
specialized rasterization
```

---

## 15. Terrain-Specific Requirements

Terrain must support:

```text
adaptive geometry based on actual surface error
feature-aware refinement
no uniform maximum-detail grid
smooth geomorph
stable gameplay topology
river/coast/cliff preservation
local runtime refinement
```

Do not refine a region simply because it is close.

Example:

```text
near flat plain
    -> may remain coarse

far sharp ridge
    -> may require finer representation
```

Distance influences projected error, but does not define LOD by itself.

---

## 16. Static-Object Requirements

Static object import must automatically produce virtual geometry data.

Source:

```text
one high-detail mesh
```

Engine output:

```text
cluster hierarchy
simplification metadata
compressed geometry pages
error metrics
```

Manual LOD assets should be optional.

---

## 17. Dynamic-Object Requirements

Dynamic objects must reuse the common virtual geometry interface where practical.

However:

```text
topology hierarchy is generated offline/import-time
animation happens at runtime
```

Do not run full QEM retopology every frame.

---

## 18. Core Design Rule

The renderer should not think:

```text
which LOD mesh do I draw?
```

It should think:

```text
what is the cheapest geometric representation
that keeps projected error below the current threshold?
```

That rule should apply across:

```text
terrain
rocks
trees
buildings
props
characters
```

with specialized geometry backends under the common virtual-geometry layer.

---

## 19. Final Target Architecture

```text
                       VIRTUAL GEOMETRY
                              |
                   projected geometric error
                              |
                       visibility tests
                              |
                    adaptive representation
                              |
                         GPU draw list
                              |
       -------------------------------------------------
       |                    |                          |
    Terrain              Static                    Dynamic
       |                    |                          |
  adaptive quadtree      QEM meshlets            skinned clusters
  residual heights      cluster hierarchy        generated hierarchy
  geomorph              shared instancing        GPU skinning
       |                    |                          |
       -------------------------------------------------
                              |
                        Indirect Rendering
```

The goal is not to reproduce Unreal Nanite feature-for-feature.

The goal is to build a specialized virtual geometry renderer that provides the benefits relevant to this engine:

- automatic detail reduction;
- no manual LOD burden;
- low triangle waste;
- smooth detail transitions;
- high-detail terrain without uniform tessellation;
- efficient instancing;
- scalable large-world rendering;
- a common architecture for static and dynamic geometry.

---

## 20. Состояние в этом коде (2026-09-16)

Документ выше — спецификация. Ниже — что из неё уже есть, что нет и в каком
порядке это делать. Числа взяты замерами, а не по памяти.

### Уже реализовано

**Терраса — фактически §5 целиком.** Это важно понимать, чтобы не переписывать
работающее: адаптивный quadtree с бисекцией по длинному ребру и общими решениями
по ромбам (`world/terrain_adaptive.*`), консервативная накопленная оценка ошибки,
критерий по нормали для защиты мелких гребней, feature-driven refinement по
рекам/озёрам (`terrain_plan.cpp: dataLevel`), геоморф между родителем и ребёнком,
сшивка рёбер с разрешением дядических ограничений (`terrain_seams.cpp`),
иерархические остатки (`ResidualTile` Large/Medium), страничный стриминг с
резидентностью, parent fallback и pin/lease (`terrain_streaming/`).

**Статические меши — §6 частично.** `tools/mesh_lod.py` даёт QEM-схлопывание
рёбер с сохранением границ материалов, квотой на компоненту и **измеренной**
геометрической ошибкой в метрах модели. Но это **дискретная цепочка уровней**
на весь меш, а не кластерный DAG: уровень переключается целиком и мгновенно.

**Инстансинг — §7 частично.** Общая геометрия на ассет, per-instance выбор
уровня по спроецированной ошибке (`world::decor::meshLevel`), но culling и выбор
уровня считаются **на CPU каждый кадр** для всех объектов.

**Плотность треугольников — §12.** После ретопологии (шаг 5) 11 кадров клиента
дают 1 288 241 треугольник вместо 5 576 091. Дерево высотой 60 px больше не
рисуется шестью тысячами треугольников.

**VG1 — фундамент GPU-submission.** Compute-пайплайны, storage-буферы, indirect
draw и чтение буфера обратно в движке (`Device::makeCompute/makeBuffer/readBuffer`,
`RenderPipeline::dispatch/runDispatches`, `DrawItem::indirect`). Отбор кластеров —
`engine::ClusterCuller` + `assets/shaders/cluster_{geometry.hlsli,reset,cull}.hlsl`.

### Чего нет

- Полного GPU-отбора расстановки: CPU каждый кадр обходит объекты и выбирает LOD
  деревьев. Трава после VG1 делает один draw; первый подшаг VG2 уже отсеивает её
  вне кадра и стабильно уплотняет корни на GPU для indirect draw. Проверки
  материала/покрытия травы пока остаются в вершинном/фрагментном шейдерах.
- Кластерного DAG со схлопыванием группами и запертыми границами (§6). Сейчас
  дискретные уровни, поэтому переход между ними — мгновенная подмена.
- Hi-Z occlusion и cone culling (§11).
- Стриминга геометрических страниц для статических мешей (§10) — для террасы это
  есть, для мешей нет.
- Скиннинга (§9) — в проекте пока нет скиннед-мешей.

### Замеры, от которых считать выигрыш

`./build/asr_client --explore --seed 42 --world 64 --at steppe --camera orbit --zoom 0.6 --measure`,
1728×960: **63–68 fps, 15,7 мс на кадр, 529 draw calls, 2,4–2,5 мс CPU на запись
команд**. Лесной ближний вид: 75–120 fps, 176–308 draws, 1,4–2,3 мс.

---

## 21. Этапы и критерии приёмки

Нумерация **VG**, отдельная от `gen_rework_step_*`: это параллельная дорожка,
она не блокирует генератор и им не блокируется. Отчёты — `doc/reports/vg_step_N/`.

### VG1 — фундамент GPU-submission · **сделано**, отчёт `doc/reports/vg_step_1/`

Compute-пайплайны, storage-буферы, indirect draw, чтение обратно. Ядро отбора:
один кластер рисуется, когда **его** спроецированная ошибка укладывается в допуск,
а ошибка **родителя** — нет. Это разрез, а не порог: вдоль каждого пути иерархии
выбирается ровно один уровень, без дыр и без двойного покрытия. Плоский список
инстансов — тот же разрез с бесконечной ошибкой родителя, поэтому иерархия и
расстановка не могут разойтись в понимании «достаточной детализации».

**Gate (выполнен):** 4 GPU-теста сверяют выбор с CPU-эталоном того же правила при
трёх допусках; проверяются разрез по иерархии из 5 уровней, зажим переполнения
ведра без записи за пределы среза и сброс счётчиков каждый кадр. Два бага,
найденные этими тестами, стоит помнить: очередь диспатчей не очищалась вне
`run()`, и — коварнее — **индекс привязки следует тому, что точка входа реально
использует**, а не тому, что объявлено в файле, поэтому сброс обнулял не тот
буфер. Оба давали неправильную картинку без единой ошибки где-либо.

Вместе с VG1 трава сведена в **один draw** (341 в широком плане → 1, 1283 по
одиннадцати сценариям → 11, картинка попиксельно та же): четыре числа, которые
действительно различаются между блоками земли, упакованы в один дополнительный
float на кандидата вместо собственного uniform'а каждого draw'а.

Записано отдельно, потому что стоило дня: storage-буферы **вершинного** этапа
адресуются после текстур, которые этот этап РЕАЛЬНО использует, а не по номерам
регистров из включаемых файлов; неверный номер читается нулями без единой ошибки.
Поэтому поддержка таких буферов в `DrawItem` откачена до появления GPU-теста на
offscreen-цель — непроверенный API в движке и был причиной тупика.

### VG2 — GPU-отбор инстансов и indirect-батчинг

#### Разделение владельцев перед продолжением VG2 (2026-09-16)

- `world::WorldSystem` — headless-владелец `WorldBuilder` и отдельных
  `WorldPreparation` для независимых потребителей. Удаление renderer не является
  удалением игрового мира; новые публикации не переиспользуют старые CPU-кэши.
- `engine::Publication<T>` публикует `shared_ptr<const T>` с монотонной версией.
  Билеты привязаны к источнику; устаревший, отменённый или уже опубликованный
  результат не может заменить текущий. Чтение/публикация синхронизированы,
  освобождение старых данных происходит вне mutex. Исключение при подготовке
  сохраняет предыдущую публикацию.
- `SurfaceReconstruction` теперь владеет `TerrainPlanner`, корневыми областями,
  jobs и CPU-кэшами адаптивной поверхности. Запрос содержит значения
  `TerrainView` и immutable residency, а не ссылки на Camera/SDL/atlas.
- `ScenePlacement` владеет фоновым scatter job и immutable результатом
  с версиями мира/запроса. Сохраняются seed/position-derived Object IDs.
  Новая область отменяет право старой работы на публикацию, без callbacks
  в renderer. Отменённая работа может закончить вычисление; это не preemption.
- `WorldRenderer` удерживает lease подготовки, читает планы/расстановку и
  владеет GPU residency, загрузками, LOD/compaction и общим render pipeline.
  `ExploreView` больше не наследует renderer: это невладеющий адаптер камеры
  и меню. `SceneModelsPass` больше не содержит future и не запускает scatter.
- `ASR_ENABLE_DIAGNOSTICS=OFF` отключает FPS, pipeline/frame timing,
  terrain timing/profiling panel, vegetation reports, traces и диагностический
  grass readback. Прежний `ASR_ENABLE_PROFILING` задаёт только начальное значение
  новой CMake-опции; явное значение новой опции имеет приоритет.
  `ASR_DIAGNOSTIC(...)` при OFF не вычисляет аргументы. Лимиты кандидатов, байтовые
  бюджеты, время анимации, polling и обработка ошибок не отключаются.

**Граница этого этапа, не закрытый полный gate:** генерация macro-карты ещё
синхронная; `WorldBuilder::request/complete/cancel` задают безопасный протокол
для её будущего scheduler. Кэши grass/grove roots пока остаются в draw-pass,
а часть старой телеметрии legacy `Explorer` и cache/job accounting ещё требует
отдельной классификации и переноса под флаг. Поэтому полное исключение
всей диагностики репозитория и завершение VG2 пока **не заявляются**.

Проверки этапа: сборка клиента с диагностикой ON/OFF; `asr_world_system_tests`
(публикация, конкурирующие читатели, время жизни, отказ неполной карты,
отмена устаревшей области, отсутствие вычисления diagnostic arguments);
два коротких terrain-plan теста на стабильный запрос и latest-only публикацию.
Без измерений FPS, графических прогонов и серий захватов. Генератор,
адаптивный алгоритм и shader/culling правила этим этапом не переписываются.

**Проверено:** `asr_client`, `asr_world_system_tests`, `asr_terrain_tests`
собраны при OFF (`build`) и ON (`build-client`). В каждой конфигурации прошли
7/7 тестов подсистем мира и 2/2 выбранных terrain-plan теста. В том числе
проверено, что повторное использование адреса источника не оживляет старые
билеты публикации. Журналы: `build/world-system-build-{off,on}-final.log`,
`build/world-system-tests-{off,on}.log`, `build/world-system-terrain-{off,on}.log`.

#### Текущий GPU-этап

**Частично реализован**, отчёт [vg_step_2](reports/vg_step_2/README.md).
`game::GrassCuller` — первый runtime-потребитель `engine::ClusterCuller`:
GPU строит bounds из четырёх морф-высот, учитывает ширину карточки и ветер,
отсеивает вне кадра, сохраняет исходный порядок через ограниченный prefix-scan
и записывает обычный instance vertex buffer. Его рисует один indirect draw.
Vertex-storage bindings для этого не возвращались. Instance-арена читается compute
после загрузки с поздним разрешением ссылки, включая увеличение буфера.

Контроль изображения — GPU/direct/GPU в **одном** процессе с фиксированными камерой
и shader time; повторный GPU-кадр обязан совпадать побитово. Независимые процессы
меняют панель статистики и не являются чистым A/B способа подачи геометрии.
GPU-счётчик читается обратно только при явной съёмке, не в обычном кадре/trace.
`submitted_candidates` остаётся числом загруженных корней; `drawn_candidates` —
число из indirect arguments после frustum-culling, **не число видимых травинок**.

Полный gate ниже **остаётся открытым**: CPU всё ещё загружает корни каждый кадр,
деревья не переведены на GPU LOD/indirect multi-draw, material compaction не сделана.
Добавились 7 compute dispatch на непустой кадр и около 9 MiB ограниченных рабочих
буферов; сокращение поданных в vertex stage кандидатов не доказывает рост FPS.

Расстановка и трава переходят на `ClusterCuller`: расстановка загружается на
карту при изменении, а не каждый кадр; выжившие компактятся на GPU; деревья
сводятся в один indirect multi-draw вместо двенадцати.

**Gate:** те же 11 сценариев захвата дают ту же картинку (попиксельное сравнение,
как в шаге 5); draw calls травы остаются 1–2 (341 было до VG1); CPU в `collect()` перестаёт зависеть от
числа объектов расстановки; счётчик «кандидатов» становится числом **нарисованных**
экземпляров, а не отправленных. Бюджет мешей остаётся детерминированным: порог
меш/импостор считается из гистограммы фиксированного размера, а не гонкой атомиков.

### VG3 — кластерный DAG для твёрдой геометрии

Оффлайн: кластеризация ~128 треугольников, группировка, QEM **группой целиком с
запертыми границами группы**, разбиение результата на новые кластеры, повтор.
Именно запертые границы делают разрез бесшовным по построению. Рантайм: выбор
кластеров уже есть (VG1).

Применяется к стволам, камням, постройкам. **Не применяется** к альфа-карточкам
листвы: 960 отдельных двухтреугольных квадов на дерево — худший случай для DAG,
и прореживание с компенсацией площади (шаг 5) для них лучше. Это §8 документа.

**Gate:** отсутствие щелей на границах кластеров при любом разрезе; треугольники
при равной измеренной ошибке не хуже дискретной цепочки; переход между уровнями
перестаёт быть мгновенной подменой.

### VG4 — Hi-Z occlusion и cone culling

Двухпроходный отбор по пирамиде глубины предыдущего кадра, затем доотбор
проявившихся. Cone culling кластеров по нормали.

**Gate:** в виде, где большая часть сцены закрыта рельефом, число отправленных
кластеров падает кратно, а картинка не меняется попиксельно.

### VG5 — стриминг геометрических страниц

Резидентность кластеров статических мешей по образцу террасы: parent fallback,
бюджет в байтах, вытеснение.

### VG6 — скиннед-меши

Только когда в проекте появятся скиннед-меши. Иерархия строится в bind pose,
скиннинг на GPU только выбранных кластеров.

---

## 22. Чего из Nanite здесь намеренно не будет

Не «пока не успели», а решение с причиной. Пересмотреть — по замеру, не по моде.

**Программный растеризатор и 64-битный visibility buffer.** Nanite нужен софтовый
растеризатор потому, что он **целится** в треугольник на пиксель: аппаратный
растеризатор платит за треугольник целым квадом 2×2, и на субпиксельной геометрии
это катастрофа. Здесь цель другая — 4–16 пикселей на треугольник (§12), и при
работающем разрезе субпиксельных треугольников просто не возникает. Плюс 64-битные
атомики через HLSL → SPIR-V → MSL сейчас ненадёжны. Если замер покажет, что
субпиксельные треугольники доминируют, — вернуться к этому с числами.

**Visibility buffer с отложенным вычислением материалов.** Окупается, когда
материалов много и перекрытие тяжёлое. Здесь терраса — одна материальная система
с общим пайплайном, объекты сцены — один пайплайн с текстурным массивом. Высокая
цена, низкая отдача.

**Nanite для листвы.** Альфа-вырезанная листва — исторически худший случай для
Nanite, и здесь она составляет большую часть геометрии дерева. Применять к ней
кластерный DAG было бы **хуже**, чем то, что уже сделано.
