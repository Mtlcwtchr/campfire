# Procedural Foliage Placement for a 3D RTS Terrain

Просто правило вида:

```cpp
if (terrain == Grass)
    ScatterFoliage();
```

почти гарантированно даст искусственный результат: равномерный ковёр случайно разбросанных объектов.

Для полноценного 3D terrain лучше рассматривать foliage как отдельную процедурную систему, которая работает поверх климатических, геологических и локальных terrain-полей.

---

## 1. Environmental fields вместо одного типа террейна

Для каждой world-position полезно иметь набор полей:

```text
height
slope
normal
moisture
temperature
soil_type
distance_to_water
water_table
sun_exposure
wind_exposure
rockiness
fertility
disturbance
```

Для каждого вида или группы растений считается пригодность участка:

```cpp
suitability =
    biomeFactor
  * moistureFactor
  * temperatureFactor
  * soilFactor
  * slopeFactor
  * heightFactor
  * sunlightFactor
  * disturbanceFactor;
```

Пример для высокой травы:

```text
moisture      0.4–0.8
slope         < 25°
rockiness     < 0.4
fertility     > 0.5
```

Пример для кустарника:

```text
moisture      0.3–0.7
slope         < 35°
rockiness     < 0.65
```

Пример для камыша:

```text
distanceWater < 4 m
moisture      > 0.85
```

Тип terrain material при этом остаётся одним из факторов, но не единственным источником истины.

---

## 2. Foliage должен расти пятнами

Нельзя независимо бросать каждую травинку.

Сначала лучше получить low-frequency density field:

```cpp
density =
    suitability *
    fbm(worldXZ * 0.01);
```

Визуально нужна структура вроде:

```text
████████
██████░░
███░░░░░
░░░░  ░░
      ░░
```

а не равномерный шум:

```text
. . . . . .
 . . . . .
. . . . . .
```

Для разных типов foliage удобно использовать разные масштабы паттернов:

```text
grass       3–15 m patches
flowers     1–5 m patches
shrubs      8–30 m patches
trees       20–100 m patches
```

Лучше смешивать 2–3 частоты noise, чем использовать один Perlin.

---

## 3. Blue noise / Poisson вместо обычного random

После density map нужно получить реальные позиции.

Для крупных объектов:

```text
trees
shrubs
large rocks
```

лучше использовать:

```text
Poisson disk
или
blue-noise sampling
```

Затем:

```cpp
if (random(seed, point) < density(point))
    spawn();
```

Это предотвращает неестественное слипание объектов.

Для мелкой травы можно использовать более дешёвый:

```text
grid + jitter
```

или GPU placement.

---

## 4. Vegetation communities вместо независимых видов

Лучше сначала определить растительное сообщество, а уже потом выбирать конкретные species.

Например:

```text
Dry Meadow
  short_grass      0.60
  tall_grass       0.15
  flowers          0.10
  shrub            0.03
```

```text
Wet Meadow
  tall_grass       0.45
  sedge            0.30
  flowers          0.08
  willow_shrub     0.05
```

Это сильно уменьшает визуальный шум и делает распределение растений связным.

---

## 5. Ecotones — границы биомов

Самые интересные зоны обычно находятся на переходах.

Например:

```text
forest -> meadow
```

На границе можно:

```text
уменьшать плотность деревьев
увеличивать кустарники
увеличивать высокую траву
увеличивать цветы
добавлять fallen branches
```

Пример edge-factor:

```cpp
float forestEdge =
    1.0 - abs(forestWeight - 0.5) * 2.0;
```

Далее:

```cpp
shrubDensity += forestEdge * 0.5;
tallGrassDensity += forestEdge * 0.3;
```

Для:

```text
grass -> rock
```

можно делать:

```text
трава реже
больше сухой травы
больше мелких камней
мох в сырых местах
кустарники в трещинах
```

---

## 6. Привязка к микрорельефу

Полезно учитывать:

```cpp
slope = 1.0 - normal.y;
curvature = terrainCurvature(xz);
```

На выпуклых участках:

```text
dry grass
low vegetation
rocks
```

В вогнутых местах:

```text
denser grass
flowers
wet plants
```

Простой модификатор:

```cpp
moisture += concavity * 0.2;
```

Это дешёво, но сильно улучшает естественность.

---

## 7. Вода как отдельный фактор

Для рек и озёр имеет смысл заранее считать:

```cpp
d = distanceToWater(worldXZ);
```

И применять зоны:

```text
0–1 m    reeds / mud vegetation
1–4 m    lush grass / bushes
4–15 m   moisture bonus
15 m+    no effect
```

Для текущей воды можно дополнительно учитывать floodplain.

Так естественно появляются зелёные полосы вдоль рек.

---

## 8. Disturbance mask

Нужно учитывать области, где растительность подавляется:

```text
roads
settlements
fields
grazing
logging
fires
foot traffic
```

Например:

```cpp
vegetationDensity *= 1.0 - disturbance;
```

Для дорог лучше делать не бинарное вырезание, а профиль:

```text
road center       foliage = 0
road shoulder     short grass
farther           tall grass
```

---

## 9. Леса лучше генерировать в два этапа

Не стоит одним проходом расставлять весь лес.

Сначала строится:

```cpp
forestCoverage(world)
```

Потом внутри него отдельными проходами:

```text
canopy trees
understory
shrubs
ground cover
deadwood
```

Большие деревья могут создавать локальную область влияния:

```cpp
shade += treeCanopyMask;
```

А последующие foliage-pass учитывают shade.

Например, под большим деревом:

```text
меньше высокой травы
больше мха
больше папоротников
```

---

## 10. Общая архитектура генерации

```text
Climate simulation
       ↓
Terrain fields
       ↓
moisture / soil / temp / slope / waterDistance
       ↓
Vegetation community map
       ↓
Species suitability maps
       ↓
Low-frequency patch noise
       ↓
Blue-noise / Poisson placement
       ↓
Local ecological modifiers
       ↓
Instances
```

---

## 11. Детерминированная генерация по чанкам

Не нужно хранить позиции каждой травинки.

Можно хранить:

```cpp
struct ChunkVegetation
{
    uint64_t seed;
    ClimateData climate;
    SoilData soil;
    DisturbanceMap disturbance;
};
```

А затем детерминированно восстанавливать foliage при загрузке chunk.

---

## 12. Разделение foliage на 3 класса

### A. Simulation foliage

Это объекты, которые участвуют в gameplay:

```text
trees
berry bushes
reeds
harvestable plants
```

Они существуют в ECS / simulation layer.

### B. Structural visual foliage

Крупные визуальные элементы:

```text
shrubs
large grass clumps
flowers
dead branches
```

Их можно детерминированно генерировать per chunk.

### C. Micro foliage

Мелкая визуальная растительность:

```text
individual grass blades
tiny flowers
ground clutter
```

Её лучше генерировать на GPU вокруг камеры.

Так можно иметь визуально миллионы травинок, не превращая их в миллионы simulation entities.

---

## 13. Correlation masks

Для каждого biome/community полезно иметь несколько независимых, но связанных полей:

```text
wet_patches
shrub_islands
exposed_ground
rocky_pockets
flower_clusters
```

Тогда виды не распределяются независимо друг от друга.

Например:

```cpp
grassDensity *= 1.0 - exposedGround;
flowersDensity *= wetPatches;
shrubsDensity *= shrubIslands;
rocksDensity += rockyPockets;
```

Именно такие корреляции часто создают ощущение настоящего природного ландшафта.

---

## Итог

Хороший foliage placement для 3D RTS лучше строить не как:

```text
terrain type -> scatter objects
```

а как:

```text
terrain + climate + soil + water + slope + disturbance
                ↓
        ecological suitability
                ↓
          large-scale patches
                ↓
       blue-noise distribution
                ↓
        local relationships
                ↓
             foliage
```

Главный визуальный принцип: растения должны образовывать сообщества, пятна, переходные зоны и локальные зависимости, а не выглядеть как независимо разбросанные случайные объекты.
