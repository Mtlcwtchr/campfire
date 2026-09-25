Нужно реализовать систему незаметной прогрузки terrain chunks для большого бесшовного RTS-мира.

Главная цель:
игрок не должен видеть бинарное состояние "чанк отсутствует / чанк появился". Мир должен визуально существовать заранее в упрощённом виде и постепенно уточняться без popping, квадратных границ и резких подмен.

Архитектура chunk readiness должна быть разделена на состояния:

1. PROXY
2. TERRAIN_READY
3. DRESSING_READY
4. GAMEPLAY_READY

Важно:
VisualReady и GameplayReady — НЕ одно и то же.

Chunk может уже отображаться полностью визуально, пока CPU ещё строит navmesh / gameplay structures.

--------------------------------------------------
1. PROXY LAYER
--------------------------------------------------

Для дальних чанков всегда должен существовать дешёвый proxy.

Proxy должен содержать минимум:

- coarse height;
- macro material / biome appearance;
- coarse water;
- optional distant vegetation representation.

Нельзя оставлять дальние чанки пустыми.

Рекомендуемая схема вокруг камеры:

center:
FULL DETAIL

inner ring:
TERRAIN_READY / DRESSING_READY

middle rings:
PROXY

outside streaming range:
UNLOADED

Streaming radius должен быть больше effective visible radius.

--------------------------------------------------
2. PREDICTIVE STREAMING
--------------------------------------------------

Нужно учитывать скорость и направление камеры.

Вместо загрузки чанков только по distance использовать predicted camera position:

predictedCamera =
cameraPosition
+ cameraVelocity * lookAheadTime

Приоритет чанка должен учитывать:

- distance to current camera;
- distance to predicted camera;
- направление движения камеры;
- текущую скорость камеры.

При высокой скорости камеры preload radius должен увеличиваться.

Пример:

preloadRadius =
baseRadius
+ cameraSpeed * lookAheadTime

Чанки впереди направления движения должны иметь повышенный priority.

--------------------------------------------------
3. GEOMETRY MORPHING
--------------------------------------------------

Нельзя делать резкую замену coarse mesh -> fine mesh.

Нужно использовать geomorph.

Например:

far terrain:
32x32

near terrain:
256x256

Fine mesh при появлении должен сначала совпадать с coarse surface.

Высота:

height =
lerp(
coarseHeight,
fineHeight,
terrainMorph
)

Normal:

normal =
normalize(
lerp(
coarseNormal,
fineNormal,
terrainMorph
)
)

terrainMorph плавно меняется 0 -> 1.

LOD switch должен быть визуально незаметным.

--------------------------------------------------
4. MATERIAL REFINEMENT
--------------------------------------------------

Материалы также нельзя переключать мгновенно.

Proxy может использовать:

- macro material;
- coarse biome/material field.

После compute генерации detailed MaterialControl нужно делать постепенный переход:

finalColor =
lerp(
proxyMaterialColor,
detailedMaterialColor,
materialReveal
)

Рекомендуемый transition duration:
примерно 0.2–0.7 s,
но сделать configurable.

ВАЖНО:
не использовать обычный прямоугольный chunk fade.

--------------------------------------------------
5. IRREGULAR WORLD-SPACE REVEAL MASK
--------------------------------------------------

Любой detail reveal должен скрывать квадратную форму чанка.

Использовать world-space noise / dither / irregular reveal.

Например:

reveal =
smoothstep(
threshold - width,
threshold + width,
worldNoise(worldXY) + progress
)

Эта маска должна применяться к:

- detailed terrain materials;
- wetness;
- snow;
- grass;
- small rocks;
- cosmetic vegetation;
- optional cliff detail.

Не делать:

wholeChunkAlpha = progress

Иначе будет видна квадратная граница чанка.

--------------------------------------------------
6. VEGETATION STREAMING
--------------------------------------------------

Растительность нельзя спавнить мгновенно.

Плохой вариант:

empty
-> instant
-> 500 trees

Нужно использовать:

- dithered fade;
- optional scale 0.7 -> 1.0;
- gradual instance activation.

Для opaque vegetation предпочтительно dither / clip reveal, а не transparent alpha blending.

Например:

clip(loadProgress - dither(screenPos))

Дальний лес может быть purely GPU/procedural representation.

Near/gameplay forest:
CPU creates actual entities.

Но обе версии должны использовать один deterministic source:

- same world seed;
- same candidate positions;
- same blue-noise / Poisson candidate set.

То есть дальний лес не должен визуально превращаться в другой лес после активации.

--------------------------------------------------
7. ATMOSPHERIC MASKING
--------------------------------------------------

Использовать естественную атмосферную перспективу как дополнительное скрытие streaming boundary.

Поддержать:

- distance haze;
- humidity haze;
- fog;
- sea mist;
- desert dust;
- optional biome-specific atmospheric attenuation.

Это НЕ должно быть единственным способом скрытия чанков.

Streaming radius должен всё равно быть больше зоны, где игрок отчётливо видит terrain detail.

--------------------------------------------------
8. GPU TERRAIN GENERATION
--------------------------------------------------

Chunk terrain generation должна идти GPU-driven.

Рекомендуемый flow:

CPU:
- определяет requested/dirty chunks;
- сортирует priority;
- передаёт chunk generation commands.

GPU:

PASS R0:
- local height;
- fine detail;
- river carve;
- runtime deformation.

Barrier.

PASS R1:
- normals;
- slope;
- cliff visual data.

Barrier.

PASS R2:
- material control;
- moisture;
- snow;
- vegetation suitability.

Barrier.

Render:
- terrain;
- cliffs;
- water;
- cosmetic vegetation.

Промежуточные данные не должны возвращаться на CPU.

Compute outputs должны напрямую использоваться rendering pipeline.

--------------------------------------------------
9. VISUAL READY != GAMEPLAY READY
--------------------------------------------------

Gameplay preparation выполняется отдельно на CPU.

GAMEPLAY_READY включает:

- navmesh;
- walkability;
- gameplay collision;
- resource entities;
- AI activation;
- buildings / blockers;
- spatial structures.

Chunk может быть:

TERRAIN_READY = true
DRESSING_READY = true
GAMEPLAY_READY = false

и уже полностью отображаться.

Gameplay readiness не должна блокировать terrain rendering.

--------------------------------------------------
10. GAMEPLAY ACTIVATION
--------------------------------------------------

Когда gameplay structures готовы:

- подключить chunk к cross-chunk nav portals;
- активировать local simulation;
- spawn authoritative resources/entities;
- активировать AI relevance.

Это должно происходить без визуального эффекта.

--------------------------------------------------
11. CHUNK BOUNDARY HIDING
--------------------------------------------------

Ни одна система не должна визуально выдавать chunk rectangle.

Проверить отдельно:

- terrain height;
- normals;
- materials;
- wetness;
- snow;
- vegetation;
- water;
- cliffs.

Neighbour-dependent data должны использовать ghost/read margins.

Например normal calculation должен иметь доступ к height samples соседнего chunk.

World-space material UV должны оставаться continuous.

--------------------------------------------------
12. STREAMING PRIORITY
--------------------------------------------------

Сделать central chunk streaming scheduler.

Priority учитывать примерно так:

priority =
distanceWeight
+ predictedDistanceWeight
+ forwardMotionBias
+ visibilityWeight
+ gameplayNeedWeight

Повышенный priority:

- chunk в направлении движения камеры;
- chunk около видимого края;
- chunk, куда скоро попадут units;
- chunk, где требуется gameplay activation.

Пониженный:

- далеко за камерой;
- закрыт;
- не виден;
- не нужен simulation.

--------------------------------------------------
13. CHUNK STATES
--------------------------------------------------

Реализовать явную state machine:

UNLOADED

-> PROXY_REQUESTED
-> PROXY_READY

-> TERRAIN_REQUESTED
-> TERRAIN_READY

-> DRESSING_REQUESTED
-> DRESSING_READY

-> GAMEPLAY_REQUESTED
-> GAMEPLAY_READY

При удалении от камеры возможна обратная деградация:

GAMEPLAY_READY
-> DRESSING_READY
-> TERRAIN_READY
-> PROXY_READY
-> UNLOADED

Не обязательно физически уничтожать данные сразу:
использовать cache / eviction policy.

--------------------------------------------------
14. TRANSITIONS
--------------------------------------------------

Хранить отдельно:

terrainMorph
materialReveal
vegetationReveal

Не связывать всё одним progress value.

Например:

terrain готов:
terrainMorph starts

material compute finished:
materialReveal starts

vegetation instances ready:
vegetationReveal starts

Это позволит pipeline работать асинхронно.

--------------------------------------------------
15. POPPING TESTS
--------------------------------------------------

Добавить debug mode:

A. Draw chunk boundaries.
B. Draw chunk state.
C. Freeze streaming.
D. Artificially delay terrain generation.
E. Artificially delay dressing.
F. Artificially delay gameplay.
G. High-speed camera test.
H. Teleport camera test.

Критерий:
даже с artificial delay игрок не должен видеть пустой квадрат terrain.

--------------------------------------------------
16. CAMERA TELEPORT
--------------------------------------------------

Отдельно обработать teleport / minimap jump / debug jump.

Если camera мгновенно переместилась далеко:

- сразу показать coarse proxy;
- временно разрешить более сильный atmospheric masking;
- резко повысить priority ближайших chunks;
- fine terrain догружать потом;
- не блокировать main thread ожиданием full world generation.

--------------------------------------------------
17. GPU MEMORY / CACHE
--------------------------------------------------

Не пересоздавать GPU resources без необходимости.

Использовать pool для:

- HeightField;
- MaterialControl;
- NormalField;
- WetnessField;
- vegetation instance buffers.

При unload chunk resources должны переходить в pool/cache.

Нужен budget-based eviction.

--------------------------------------------------
18. DESIRED FINAL FLOW
--------------------------------------------------

Expected visual flow:

Camera starts moving
↓
Predict future position
↓
Request chunks ahead
↓
Proxy already visible
↓
GPU generates fine height
↓
Coarse surface morphs into fine surface
↓
GPU generates detailed materials
↓
Materials reveal through irregular world-space mask
↓
Vegetation gradually appears through dither
↓
CPU builds navmesh/gameplay data in parallel
↓
Gameplay activates invisibly
↓
Chunk becomes FULL READY

Игрок никогда не должен видеть момент:
"вот здесь закончился один chunk и появился другой".

--------------------------------------------------
19. IMPORTANT CONSTRAINTS
--------------------------------------------------

Do NOT:

- block CPU waiting for GPU terrain generation;
- read high-resolution terrain data back from GPU without a real gameplay need;
- switch LOD meshes instantly;
- fade the whole chunk as a rectangle;
- spawn all vegetation in one frame;
- wait for navmesh before showing terrain;
- use chunk-local UV that exposes chunk borders;
- let independently generated chunks calculate incompatible edge normals/materials;
- regenerate global geography for each chunk;
- make visual GPU micro-detail authoritative for gameplay.

--------------------------------------------------
20. EXPECTED IMPLEMENTATION RESULT
--------------------------------------------------

После реализации система должна позволять:

- быстрый полёт камеры по огромному миру;
- seamless terrain streaming;
- отсутствие пустых зон;
- отсутствие очевидных chunk squares;
- отсутствие height/material popping;
- постепенное появление high-frequency detail;
- асинхронную подготовку navmesh/gameplay;
- прямой compute -> GPU resources -> render flow;
- масштабирование streaming quality в зависимости от GPU budget и camera speed.

Перед реализацией сначала проанализируй текущую архитектуру chunk manager, renderer, GPU resource ownership и terrain generation pipeline.

Не переписывай всё вслепую.

Сначала определи:
1. какие chunk states уже существуют;
2. где сейчас происходит generation;
3. где CPU ждёт GPU;
4. какие resources пересоздаются;
5. где возможен direct compute -> render;
6. где сейчас возникает popping;
7. как устроены LOD и camera movement.

После анализа предложи минимальный план изменений, затем реализуй его поэтапно.