# World Generation & GPU Terrain Pipeline

## Цель

Система должна генерировать большой бесшовный мир, в котором:

- глобальная география определяется один раз при world generation;
- чанки материализуются по мере необходимости;
- тяжёлые регулярные вычисления terrain выполняются на GPU;
- промежуточные GPU-данные не возвращаются на CPU без необходимости;
- CPU остаётся авторитетным для gameplay-логики, навигации и симуляции;
- visual terrain может иметь более высокое разрешение и более сложные вычисления, чем gameplay representation;
- рельеф, климат, гидрология и биомы должны быть взаимосвязаны.

Основной принцип:

```text
CPU = topology + semantics + authoritative simulation
GPU = dense fields + visual terrain + local refinement
```

## 1. Общая архитектура

```text
WORLD GENERATION
│
├── Global coarse world data
│   ├── land / ocean
│   ├── tectonics / mountain systems
│   ├── macro height
│   ├── geology
│   ├── wind / circulation
│   ├── ocean currents
│   ├── temperature
│   ├── precipitation
│   ├── hydrology
│   ├── river graph
│   ├── lakes
│   ├── floodplains
│   ├── soils
│   └── biome suitability
│
├── Serialized global fields
│
└── CHUNK MATERIALIZATION
    │
    ├── GPU compute
    │   ├── local height refinement
    │   ├── river / erosion detail
    │   ├── normal / slope
    │   ├── material control
    │   ├── wetness / snow
    │   ├── cliff visual fields
    │   └── vegetation suitability
    │
    ├── GPU rendering
    │   ├── terrain
    │   ├── cliffs
    │   ├── water
    │   └── cosmetic vegetation
    │
    └── CPU gameplay layer
        ├── walkability
        ├── navmesh
        ├── A*
        ├── buildings / blockers
        ├── simulation
        └── spatial structures
```

## 2. Разрешения мира

Не привязывать физический размер мира к одному resolution.

Пример для мира около `100 × 100 km`:

```text
Macro world maps: 1024²–2048²
Climate: 512²–2048²
Hydrology: 2048²–4096²
Regional detail: 4096² or sparse/chunked
Chunk visual fields: 256² / 512² / 1024² depending on LOD
```

Разные системы должны иметь независимое разрешение.

## 3. Global generation flow

### PASS G0 — Seed / deterministic world constants

CPU.

Создать:

```text
worldSeed
worldSize
seaLevel
global temperature scale
global moisture scale
planet rotation sign
season parameters
```

Все procedural функции должны быть воспроизводимыми по seed и global coordinates.

### PASS G1 — Continental mask

Цель:

- форма материков;
- моря;
- крупные острова;
- шельф.

Методы:

- low-frequency fBm;
- domain warp;
- optional Voronoi / region control;
- custom continent masks.

Output:

```text
continentalField
distanceToCoast
initialLandMask
```

### PASS G2 — Pseudo-tectonics

Генерировать условные плиты или крупные geological regions.

Для каждой plate:

```text
id
velocity vector
crust type
elevation bias
geology family
```

Boundary types:

```text
convergent
divergent
transform
passive
```

Эффекты:

```text
convergent continental -> mountain uplift
convergent oceanic     -> trench / coastal mountains
divergent              -> rift / depression / ridge
transform              -> fault zones
```

Output:

```text
upliftField
riftField
faultField
geologyRegion
```

### PASS G3 — Base macro height

Комбинировать:

```text
continentalField
+ upliftField
+ riftField
+ ridged multifractal
+ low-frequency fBm
+ domain warp
```

Output:

```text
macroHeight
```

Горные системы должны быть вытянутыми, а низины — связными.

### PASS G4 — Geology

Сформировать geological regions:

```text
hard rock
soft sediment
limestone-like
volcanic
clay-rich
sandstone-like
alluvial
```

Output:

```text
rockType
erosionResistance
soilParentMaterial
permeability
```

### PASS G5 — Thermal erosion

Input:

```text
macroHeight
erosionResistance
```

Цель:

- убрать невозможные сверхкрутые slopes;
- сформировать осыпи;
- стабилизировать mountain profiles.

Output:

```text
thermallyRelaxedHeight
```

### PASS G6 — Hydraulic / fluvial erosion

Для world-scale предпочтительнее:

```text
stream-power approximation
+ flow-directed erosion
+ limited hydraulic iterations
```

Input:

```text
height
provisional rainfall/moisture
erosionResistance
```

Output:

```text
erodedHeight
sedimentPotential
erosionField
```

## 4. Climate generation

### PASS C0 — Base temperature

Температура зависит минимум от:

```text
latitude
altitude
distance to ocean
season
```

База:

```text
T = latitudeTemperature(latitude)
    - altitude * lapseRate
```

Дополнительно:

```text
ocean moderation
continentality
```

Output:

```text
baseTemperature
```

### PASS C1 — Global prevailing winds / wind rose

Не генерировать wind direction случайным шумом.

Использовать упрощённую крупномасштабную циркуляцию:

```text
equatorial / tropical circulation
mid-latitude westerlies
polar easterlies
```

Для каждого coarse climate sample хранить:

```text
windDirection.xy
windStrength
windVariability
```

Для региональной розы ветров можно хранить 8 направлений:

```text
N NE E SE S SW W NW
```

и для каждого:

```text
probability
meanSpeed
```

В runtime terrain обычно достаточно prevailing wind vector + variability, а роза ветров может быть производной.

Output:

```text
prevailingWindField
windStrengthField
windVariability
```

### PASS C2 — Atmospheric moisture transport / orographic precipitation

Океан — главный moisture source.

Старт:

```text
airMoisture = function(ocean proximity, temperature)
```

Дальше moisture переносится по wind field.

При подъёме воздушной массы:

```text
cooling ↑
precipitation ↑
air moisture ↓
```

На подветренной стороне:

```text
rain shadow
```

Output:

```text
precipitation
airMoisture
rainShadow
```

### PASS C3 — Ocean currents

Полноценная fluid simulation не обязательна.

Input:

```text
ocean mask
latitude
prevailing wind
coast geometry
Coriolis-like bias / planet rotation
```

Сформировать:

```text
surfaceCurrent.xy
currentTemperatureBias
```

Тёплые течения:

```text
coastal temperature ↑
air moisture ↑
winter moderation ↑
```

Холодные течения:

```text
coastal temperature ↓
evaporation ↓
aridity potential ↑
```

Output:

```text
oceanCurrentField
seaSurfaceTemperatureBias
coastalClimateBias
```

### PASS C4 — Final temperature

```text
finalTemperature =
    latitude
  + altitude
  + continentality
  + ocean current influence
  + coastal moderation
```

Output:

```text
temperatureField
```

### PASS C5 — Final rainfall / humidity / seasonality

Использовать:

```text
prevailing winds
ocean evaporation
current temperature
terrain elevation
rain shadow
continentality
```

Output:

```text
annualRainfall
humidity
seasonality
winterRain
summerRain
drySeasonStrength
```

## 5. Hydrology

### PASS H0 — Depression handling

Перед построением рек:

- найти numerical pits;
- различить их с настоящими lake basins;
- использовать depression fill / priority flood;
- определить spill points.

Output:

```text
hydrologicallyCorrectedHeight
basinIDs
spillPoints
```

### PASS H1 — Flow direction

Для каждого sample определить downhill direction.

Варианты:

```text
D8
D-infinity
continuous gradient routing
```

Output:

```text
flowDirection
```

### PASS H2 — Flow accumulation

Считать upstream contributing area.

```text
flowAccumulation[p] =
    local water contribution
    + upstream contribution
```

Local contribution желательно учитывать через:

```text
rainfall
snowmelt
soil permeability
evaporation
```

Output:

```text
flowAccumulation
dischargePotential
```

### PASS H3 — River extraction

Река появляется, когда:

```text
flowAccumulation > threshold
```

Threshold может зависеть от:

```text
climate
permeability
slope
```

Получить CPU river graph:

```text
source
tributary
junction
segment
mouth
```

Output:

```text
RiverGraph
riverCenterlines
riverDischarge
```

### PASS H4 — River sources

Поддержать source types:

```text
converging headwater gullies
spring
lake outlet
snowfield melt
wetland source
karst / cliff spring
```

Перед постоянным руслом формировать:

```text
wet depression
small gullies
seasonal drainage
```

### PASS H5 — Valley + channel carving

Не вырезать одну канаву постоянной ширины.

Минимум три масштаба:

```text
valley
floodplain
channel
```

Upper course:

```text
narrow
steep
V-shaped
rocky
```

Middle course:

```text
wider valley
larger channel
more bends
```

Lower course:

```text
wide floodplain
low gradient
sediment deposition
meanders
```

Width/depth depend on:

```text
discharge
slope
geology
```

### PASS H6 — Lakes

Для basin:

```text
fill to spill point
```

Определить:

```text
water level
lake polygon
outflow point
```

Output:

```text
lakeRegions
lakeLevels
```

### PASS H7 — Waterfalls / rapids

Считать longitudinal river gradient.

Waterfall candidate:

```text
large local height drop
+ short horizontal distance
+ sufficient discharge
```

Особенно вероятно:

```text
plateau edges
hard/soft geology boundary
hanging valleys
lake outlet cliffs
```

Representation:

```text
upper river
waterfall segment
plunge pool
lower river
```

Generate:

```text
waterfall strip geometry
foam field
mist zone
wet cliff zone
plunge pool deformation
```

### PASS H8 — Sediment transport / floodplains

Условная river energy:

```text
energy ~ discharge * slope
```

High energy:

```text
erosion
rock transport
narrow channels
```

Low energy:

```text
deposition
silt
wide floodplain
delta
```

Output:

```text
sedimentField
alluvialSoil
floodplainPotential
deltaPotential
```

## 6. Soil and biome generation

### PASS B0 — Soil

Input:

```text
geology
sediment
rainfall
temperature
slope
drainage
flooding
```

Output:

```text
soilType
fertility
drainage
organicPotential
```

### PASS B1 — Biome suitability

Biome определяется из:

```text
temperature
rainfall
seasonality
soil
altitude
drainage
flooding
```

Примеры:

```text
tundra
taiga
temperate forest
grassland
steppe
savanna
mediterranean
semi-desert
desert
wetland
alpine
mesa/badlands
```

Хранить:

```text
primary biome
secondary suitability
transition information
```

### PASS B2 — Terrain material suitability

Из biome + local physical fields получить material preferences:

```text
grass
dry grass
soil
fertile soil
mud
sand
gravel
rock
forest floor
snow
red soil
etc.
```

Material control должен отражать физическую поверхность, а не только название biome.

## 7. Что сериализовать после worldgen

Сохранить глобально:

```text
macroHeight
geology
temperature
rainfall
wind
ocean currents
flow direction
flow accumulation
basins
river graph
lakes
floodplain potential
sediment
soil
biome suitability
```

Это не пересчитывается при обычной загрузке chunk.

## 8. Runtime chunk materialization

CPU передаёт GPU:

```text
chunk origin
chunk size
LOD / resolution
references to global maps
dynamic terrain deformation
river segments intersecting chunk
water levels
runtime season state
```

### GPU PASS R0 — Local height

Compute shader.

Считать в одном kernel:

```text
global macro height sample
+ regional detail
+ local fBm
+ ridged detail where appropriate
+ domain warp
+ river valley/channel carve
+ runtime deformation
```

Output:

```text
HeightField
```

Не делать отдельный dispatch для каждого noise contribution.

### GPU PASS R1 — Neighbour-dependent derived fields

После GPU barrier.

Compute shader читает `HeightField`.

Считает:

```text
normal
slope
curvature if needed
local drainage approximation
visual cliff classification
```

Output:

```text
NormalField
SlopeField optional
CliffField
```

Если slope нужен только визуально и normal уже существует, отдельный SlopeField не хранить.

### GPU PASS R2 — Surface / climate refinement

Input:

```text
height
normal
global climate
soil
river/flood fields
season
```

Считать:

```text
local moisture
wetness
snow cover
material suitability
vegetation suitability
```

Output:

```text
WetnessField
SnowField
VegetationField
```

Не хранить поле, если дешевле вычислить его прямо в render shader.

### GPU PASS R3 — Material control

Не интерполировать material weights через terrain vertices.

Material topology должна быть независима от mesh triangulation.

Recommended control representation:

```text
MaterialIds:
R = material A
G = material B

BorderField:
signed distance to A/B boundary
```

Material IDs:

```text
integer / point sampled
```

Border distance:

```text
float / linearly sampled
```

CPU или compute заранее определяет:

```text
top material
competing material
signed boundary
```

Shader не должен каждый fragment обходить 5×5 / 7×7 logical tiles.

## 9. GPU data lifetime

Compute outputs остаются на GPU:

```text
GPU compute writes:
HeightField
MaterialControl
NormalField
WetnessField
...

GPU render reads:
same resources
```

Не делать:

```text
GPU -> CPU readback -> CPU conversion -> GPU upload
```

если CPU gameplay не требует этих данных.

## 10. Compute pass fusion

15 logical world-generation stages не равны 15 runtime dispatch.

Объединять операции, если они:

- используют одинаковые inputs;
- независимы для одного sample;
- не требуют результатов других thread groups.

Пример:

```text
R0:
height
+ noise
+ ridge
+ river carve
+ deformation
```

в одном kernel.

R1:

```text
normal
+ slope
+ cliff
```

в одном kernel.

R2:

```text
wetness
+ snow
+ material suitability
+ vegetation suitability
```

в одном kernel, если зависимости позволяют.

Цель:

```text
1–3 compute dispatch
+ render
```

на новый/dirty chunk.

## 11. GPU barriers

Отдельный pass нужен, когда стадия B должна читать результаты стадии A, созданные произвольными thread groups.

Например:

```text
height
↓
normal from neighbouring height samples
```

Это два dispatch с GPU barrier.

Не делать CPU wait.

Команды:

```text
Dispatch R0
Barrier
Dispatch R1
Barrier
Dispatch R2
Barrier
Draw terrain
```

CPU после submit продолжает simulation.

## 12. Terrain rendering

### Vertex stage

Terrain mesh topology может быть generic grid / adaptive mesh.

Vertex shader:

```text
gets chunk-local X/Y
samples HeightField
constructs world position
```

CPU не обязан генерировать detailed vertex positions.

Для LOD можно иметь reusable topology/index buffers.

### Fragment stage

Fragment shader получает:

```text
worldXY
normal
material control
wetness
snow
etc.
```

Base material UV:

```text
materialUV = worldXY / materialWorldScale
```

Не использовать per-quad `0..1 UV`.

## 13. Terrain material blending

At pixel:

```text
material A
material B
signed border distance
```

Затем:

```text
signedDistance
+ low-frequency edge distortion
+ small edge breakup
+ height-aware modification near border
```

Final:

```text
smoothstep(-blendWidth, blendWidth, signedDistance)
```

Важно:

```text
blendWidth     = softness of the actual edge
noiseAmplitude = how far the edge wanders spatially
```

Не путать эти параметры.

## 14. Texture anti-tiling

Base:

```text
world-space material texture
```

Добавить:

```text
macro variation
dual-scale sampling
large-region hashed offset
optional rotation/mirroring
```

Не возвращаться к per-tile UV.

## 15. Cliffs

Gameplay cliff:

```text
CPU / authoritative field
```

Visual cliff:

```text
GPU derived from slope + height + geology
```

Render separately if needed:

```text
top edge
face
base debris/shadow
```

Cliff face:

```text
local boundary UV
or
triplanar world-space projection
```

## 16. Water

Water — отдельный render pipeline, а не terrain material.

GPU inputs:

```text
water level
depth
flow direction
shore distance
sediment/turbidity
```

Shader:

```text
two scrolling normals
flow-oriented river UV
depth color
shore transparency
foam
```

## 17. Dynamic flooding

Не заменять terrain на flood tiles.

```text
water level rises
↓
terrain remains the same
↓
water depth changes
↓
shoreline moves automatically
```

Terrain wetness:

```text
dry
→ damp
→ mud
→ recovering soil
→ vegetation
```

## 18. Vegetation

GPU может генерировать visual density/suitability:

```text
biome
moisture
soil
slope
clustering noise
```

Placement:

```text
blue noise / Poisson candidates
```

Gameplay vegetation остаётся CPU-authoritative, если влияет на симуляцию.

## 19. CPU gameplay terrain

CPU должен иметь deterministic/coarse terrain information для:

```text
walkability
building placement
navmesh
collision
gameplay cliff
water blocking
```

GPU-only micro detail не влияет на authoritative gameplay.

## 20. Navmesh

CPU.

Input:

```text
authoritative terrain height
walkability
water
cliffs
buildings
walls
```

Generate:

```text
walkable polygons
adjacency
cross-chunk portals
```

Pathfinding:

```text
hierarchical region route
↓
A* on nav polygons
↓
polygon corridor
↓
funnel / string pulling
↓
continuous path
```

## 21. Spatial acceleration

Static geometry:

```text
BVH / AABB tree / KD-tree
```

Выбирать по query profile.

Dynamic entities:

```text
spatial hash / uniform grid
```

Не использовать static tree для часто двигающихся units.

## 22. Chunk boundaries

Chunk — техническая единица, не world boundary.

Требования:

```text
shared deterministic global coordinates
ghost/read margins
cross-chunk nav portals
continuous control fields
continuous world-space texture mapping
```

Normals и neighbour-dependent compute должны читать margin за пределами видимой части chunk.

## 23. Dirty updates

Runtime changes:

```text
digging
dam
flood
road
fire
snow
vegetation change
```

Инвалидировать только:

```text
dirty region
+ required neighbour margin
```

## 24. GPU/CPU synchronization policy

Avoid blocking readback.

GPU -> CPU readback только для данных, которые действительно нужны CPU.

Потенциально допустимо:

```text
small coarse walkability field
specific generated statistics
debug data
```

Не читать обратно:

```text
full high-resolution height
normal
material textures
wetness
visual fields
```

без необходимости.

## 25. Suggested runtime chunk flow

```text
CPU
│
├── determine requested/dirty chunks
├── schedule generation
├── simulation / AI / pathfinding
│
└── submit GPU commands
       │
       ▼
GPU R0
local height
       │
       ▼
barrier
       │
       ▼
GPU R1
normals / slope / cliff
       │
       ▼
barrier
       │
       ▼
GPU R2
materials / moisture / snow / vegetation fields
       │
       ▼
barrier
       │
       ▼
terrain draw
       │
       ├── terrain materials
       ├── cliffs
       ├── water
       └── cosmetic vegetation
```

## 26. Complete world generation order

```text
SEED
↓
continental mask
↓
pseudo-tectonics
↓
macro geological structures
↓
macro height
↓
geology
↓
thermal erosion
↓
coarse hydraulic/fluvial erosion
↓
base temperature
↓
global wind circulation
↓
wind rose / regional wind statistics
↓
ocean currents
↓
ocean temperature influence
↓
atmospheric moisture transport
↓
orographic precipitation
↓
rain shadows
↓
final temperature
↓
final rainfall / seasonality
↓
hydrological depression handling
↓
flow direction
↓
flow accumulation
↓
drainage basins
↓
river graph
↓
river sources
↓
lake fill / spill points
↓
river valley carving
↓
waterfall / rapid detection
↓
sediment transport
↓
floodplain / delta generation
↓
soil generation
↓
biome suitability
↓
terrain material suitability
↓
serialize global fields
↓
chunk-local GPU refinement
↓
terrain rendering
↓
CPU navmesh/gameplay overlay
```

## 27. Things that should NOT happen

Не делать:

```text
use mesh triangulation to define biome boundaries
use per-quad 0..1 UV for base terrain
recompute tectonics/climate every chunk load
run full hydraulic erosion on entire world every runtime load
read GPU visual terrain back to CPU every frame
make visual micro terrain authoritative
create rivers as constant-width carved splines
create biome transitions as multiple parallel offset bands
make 15 logical generation stages equal 15 runtime GPU passes
```

## 28. Core design rule

У мира три разных слоя истины:

```text
GLOBAL PHYSICAL WORLD
climate
geology
hydrology
macro terrain

AUTHORITATIVE GAMEPLAY WORLD
walkability
navmesh
objects
simulation
collisions

VISUAL GPU WORLD
micro terrain
materials
normals
wetness details
snow breakup
visual erosion
water surface details
cosmetic vegetation
```

Они используют одни глобальные координаты и общие deterministic source data, но не обязаны иметь одинаковое resolution или одинаковый pipeline.
