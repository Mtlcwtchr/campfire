# Handoff: «псевдо-наниты», FPS, сверка с UE — 2026-09-25

Ветка `feature/terrain-gen-rework`. Всё ниже — **незакоммиченные изменения** поверх
начального коммита `fba20f0` (см. «Git»).

## Задача (от пользователя)

1. Починить сборку, завести git, текущий стейт — отправная точка.
2. Взять из UE-проекта `~/Documents/UProjects/AncientSettlement` (контекстом, не копированием)
   идеи по виду/рендеру террейна.
3. Стабильные 60–120 FPS в любом виде (рощи, леса, ближний/дальний план), без просадок,
   в т.ч. при загрузке террейна.
4. Довести «псевдо-наниты» (cluster DAG, `src/engine/geometry/cluster_dag.*`) для террейна и
   моделей; сверять наш smart mesh bake с Nanite bake на камнях/траве/деревьях из UE.
5. Превью — в отдельном микро-клиенте, не в explore.

Ограничения пользователя: машина 18 ГБ, **сборка одна за раз, `-j 2`, `nice`**; не
плодить папки сборки (использовать только `cmake-build-relwithdebinfo`, CLion — `cmake-build-debug`);
**не открывать окна/клиенты в фореграунде** (пользователь смотрит видео) — всё headless;
длинные тяжёлые прогоны не устраивать, визуально проверяет сам пользователь.

## Продолжение Copilot — проверено 2026-09-25

**Этот раздел актуальнее исходного handoff ниже. Коммитов не делалось.**

- Выбран путь `exactFinestLevel`, не откат профиля. Собраны `asr_smart_mesh_tests`,
  `asr_mesh_lab`, `scene_model_clusters`, `asr_client` в существующей папке,
  последовательно, через `nice`, с `-j 2`.
- Найдена и исправлена потеря материальных границ: позиционная сварка теперь
  учитывает `hardBoundaryKeys`, но не сохраняет каждый UV/normal-разрыв на грубых LOD.
  Новый тест воспроизвёл ошибку до исправления и прошёл после него.
- **74/74 теста прошли** (раньше 70). Четыре новые регрессии проверяют точность L0
  после пропуска вырожденных треугольников, замкнутость смешанных срезов на границах
  ошибок, материальные швы и сериализацию обоих путей построения sidecar.
- Лаба теперь передаёт материальные ключи SCM2, как игровой бейкер, и пишет
  `source_topology`, топологию срезов и, при `MESH_LAB_LEVELS=1`, отдельных уровней.
  Считаются геометрически открытые/неманифолдные рёбра, а не разрывы атрибутных индексов.
- Пересобраны **все 5 игровых sidecar-файлов и 2 grove-агрегата**. В записанных SCC6
  побайтно проверены координаты L0 и совпадение исходных ориентированных треугольников
  с L0 + карточками для четырёх моделей с solid-частью. `Bush_Common` — crown-only,
  его исходная дискретная цепочка не изменялась.
- Копия файлов **до этого перебейка** (уже повреждённый предыдущий профиль, не старый
  исправный бэкап): `assets/generated/scene_models_backup_before_exact_2026-09-25/`.
  Это постоянная папка проекта, не `/tmp`.

### Короткие прогоны лабы

Прогоны ограничены `--only`, по `--samples 5000`, без окон:

- игровые: `assets/generated/mesh_lab/exact_finest_game/{report.md,report.json,*.png}`;
- UE: `assets/generated/mesh_lab/exact_finest_ue/{report.md,report.json,*.png}`.

| Меш | Исходник | Старый профиль @4% | Исправленный профиль @4% | UE @4% |
|---|---:|---:|---:|---:|
| Rock_Medium_1 | 342 | 342 | 170 | — |
| Pine_1 (solid) | 3947 | 3947 | 243 | — |
| kite_boulder | 1185 | 1185 | 148 | 440 |
| mwam_grass | 780 | 780 | 184 | 273 |
| beech | 9256 | 8344 | 3965 | 377 |

У Rock все 10 уровней имеют **0 открытых и 0 неманифолдных рёбер**; L1 error =
0.467519 м. У Pine исходник имеет 645 открытых рёбер; в проверенных срезах нет
неманифолдных рёбер, открытых становится меньше (73 при 4%). Это не доказательство
визуального качества открытой листвы: **PNG должен оценить пользователь**.
В частности, sampled p95 / allowance при 4% равен ~2.59 у Pine и ~1.67 у mwam_grass;
ошибка прореживания не является строгой границей отклонения всей поверхности.
Настройка `thinCards`/накопления ошибки остаётся отдельной задачей.

### Headless smoke, не FPS-бенчмарк

Запущен `asr_client --explore --headless 1280x800 --bench 60 --seed 11` с
`--bench-json assets/generated/mesh_lab/exact_finest_game/bench_smoke.json`.
Окон не создавалось; лог подтверждает Metal headless и создание мира (~5.63 с).
За жёсткий лимит **60 секунд** полный отчёт не появился, процесс остановлен.
Лог: `assets/generated/mesh_lab/exact_finest_game/bench_smoke.log`.
JSON с результатами не получен; **FPS до/после и стабильные 60–120 FPS не подтверждены**.
Повторять длинный прогон без исследования загрузки/первых кадров не нужно.

Следующее: визуально оценить PNG и ошибку листвы, исследовать длительность headless
загрузки/кадров, затем провести сопоставимый замер. Оптимизации группировки, CPU/GPU
отбора объектов и бюджетов стриминга из исходного плана пока не реализовывались.

---

Ниже сохранён **исходный handoff до продолжения**; пометки «не собрано» и «не проверено»
описывают состояние на момент передачи, а не результаты выше.

## Git

- Старый `.git` был сломан (нет `.git/objects`, история не восстановима). Перенесён целиком в
  `/Users/antonslauta/CLionProjects/untitled.git-broken-2026-09-25` (refs/reflog там есть).
- `git init -b main`, автор Mtlcwtchr, git-lfs local; начальный коммит `fba20f0` = всё
  состояние на 2026-09-25 (ассеты через LFS). Рабочая ветка `feature/terrain-gen-rework`.

## Сборка

- Полная сборка `cmake-build-relwithdebinfo` с нуля проходит (1843/1843). Причина прежней
  «поломки» не установлена: обе папки сборки были пустыми (без `.ninja_log`), вероятно
  стёрты; или мешал сломанный `.git`. `cmake-build-debug` (профиль CLion) не собиралась.
- Команда: `nice cmake --build cmake-build-relwithdebinfo -j 2 --target <t>`.
- `asr_smart_mesh_tests`: 70/70 проходили после добавления опций DAG (до `exactFinestLevel`,
  см. ниже — после него тесты не перезапускались).

## Что сделано

### 1. UE-референс: `tools/ue_nanite_reference.py` (новый)
Headless-скрипт для `UnrealEditor-Cmd -run=pythonscript -nullrhi` (команда в шапке файла).
Ничего не сохраняет в UE-проект. Для 7 мешей (quarry_rock_dense 1M tri, mossy_rock,
kite_boulder, mwam_grass, wild_grass, beech, spruce) пишет в `assets/generated/ue_reference/`:
исходник `<name>@source.uem` и **срезы Nanite DAG** при relative error 0.02…16%
(fallback Nanite = `FClusterDAG::FindCut` того же DAG, NaniteBuilder.cpp
`BuildCoarseRepresentation`), `index.json` со статистикой. Экспорт уже выполнен, файлы есть.

### 2. Микро-клиент `asr_mesh_lab` (`tools/mesh_lab.cpp`, новый, цель в CMake)
Без окна и GPU: строит наш DAG по профилям, режет на той же абсолютной ошибке
(формула Nanite: `e% * 0.01 * sqrt(min(2*area, bboxArea))`), меряет двустороннее
семплированное расстояние до исходника (mean/p95), CPU-растеризует PNG
(строки = ошибка; колонки = source | UE Nanite | ours:current | ours:nanite),
пишет `report.md/json` в `assets/generated/mesh_lab/`.
- `asr_mesh_lab [assets/generated/ue_reference] [--only a,b] [--out DIR]`
- `--scm assets/generated/scene_models` — игровые меши (SCM2), без UE-колонки.
- `MESH_LAB_LEVELS=1` — ошибки по уровням DAG.
- Осторожно: `quarry_rock_dense` (1M tri) строится ~1.5–3 мин на профиль и ест память;
  последний полный прогон был убит (exit 137). Для итераций использовать `--only`.

### 3. Опции DAG (`cluster_dag.hpp/.cpp`) — отличия от Nanite, по умолчанию выключены
- `carryStalledGroups` — группа, которую нельзя упростить, переходит на след. уровень
  (а не становится корнем). Стоп после 3 уровней подряд с убылью < 5%.
- `lockSourceRims=false` — открытые края не блокируются, держатся edge-квадриками
  (`simplifyLocked(..., rimQuadrics)`); проверка: общие с соседями рёбра сохраняются.
- `thinCards` / `cardTriangles` — прореживание мелких компонент с сохранением площади
  (аналог Nanite Preserve Area), ошибка = среднее двустороннее расстояние.
- `travelError=false` — убирает член «путь вершины» из conservative error (он раздувал
  ошибку до длины ребра: гладкий камень не упрощался до 8%).
- `exactFinestLevel` — топология по позициям, L0 переписывается на исходные вершины
  (для flat-shaded мешей, где каждая вершина раздвоена по нормали). **Написано, но НЕ
  собрано и НЕ проверено.**
- `naniteProfile()` — всё вышеперечисленное + группы по 16, maxLevels 32.
- `groupClusters` у Nanite 8–32 (ClusterDAG.cpp `MinGroupSize/MaxGroupSize`), у нас было 4.

### 4. Результаты лабы (ключевые)
| меш | UE @4% | ours current @4% | ours nanite @4% |
|---|---|---|---|
| kite_boulder (1185) | 440 | 1185 | 148 |
| mwam_grass (780) | 273 | 780 (7 корней, не упрощается никогда) | 780 (388 при 2–4%) |
| beech (9256) | 377 (dev p95 2.6× допуска) | 8344 (56 корней держат 4751 tri навсегда) | 3387 (dev p95 0.5×) |

Вывод: текущий бейк не сходится для листвы/деревьев — дальний план стоит тысячи
треугольников на объект. Nanite-профиль сходится до 1 корня; по «треугольники при равном
видимом отклонении» бук сопоставим с UE (наш 16% ≈ их 8%). Наша ошибка консервативнее UE,
это можно компенсировать `kLevelPixelError` (сейчас 3 px).

### 5. ВНИМАНИЕ: игровые sidecar'ы пересобраны Nanite-профилем
`tools/scene_model_clusters.cpp` теперь передаёт `naniteProfile()` в `buildClusterAsset`
и `buildSourceClusterAsset`, и `assets/generated/scene_models/*.clusters` уже
перезаписаны. У flat-shaded моделей (Rock_Medium_1, Pine_1) без `exactFinestLevel` это
даёт **дыры на грубых уровнях** (мельчайший уровень цел) и завышенные ошибки
(Rock L1 0.75 м); CommonTree solid застревает на 3176 tri. Бэкап старых sidecar'ов
потерян (был в temp). Варианты:
- откатить: вернуть `{}` вместо `naniteProfile()` в двух местах и перезапустить
  `./cmake-build-relwithdebinfo/scene_model_clusters assets/generated/scene_models`;
- или довести `exactFinestLevel` (п.3) и проверить лабой `--scm`.

### 6. Headless-рендер и бенчмарк explore (собрано, НЕ запускалось)
- `Device::openHeadless(w,h)`, `Device::waitInFlight`, `Runner::frame` без swapchain.
- `asr_client --explore --headless 1280x800 --bench 240 [--bench-json out.json]`:
  без окна (и `SDL_HINT_MAC_BACKGROUND_APP`), маршрут `load / map-far / orbit / forest /
  flight / zoom-out` (`src/game/client/explore_bench.*`), печатает mean/p50/p95/p99/max,
  кадры >16.7/>33 мс, cpu, треугольники. Направление движения в free-режиме может быть
  инвертировано — проверить.

## Выводы по UE-проекту (контекст)
- Там свой процедурный террейн (DynamicMesh-блоки + RVT, не Landscape), Nanite для мешей,
  ассеты обрезаны до 0.85M tri. Их FPS убивает не геометрия, а перерисовка RVT-страниц
  тяжёлым шейдером земли (~22 мс/страницу, до 29 страниц/кадр) + Lumen/скейлабилити
  (`Docs/Perf/2026-09-25-*.md`). Урок для нас: кэшировать дорогое шейдирование земли
  (у нас `pageShapeAt` зовёт `detailBedAt` 9 раз, до ~27 выборок на пиксель).

## План (по приоритету)

1. **Довести DAG:** собрать `exactFinestLevel`, `asr_mesh_lab --scm assets/generated/scene_models`,
   убедиться что Rock/Pine без дыр и ошибки разумные; затем `--only kite_boulder,mwam_grass,beech`.
   Прогнать `asr_smart_mesh_tests`. Пересобрать sidecar'ы. Для группировки заменить BFS
   на рост с максимизацией общих рёбер (приближение METIS), `gather` сейчас O(P²).
2. **Замер:** `asr_client --explore --headless 1280x800 --bench 240` до/после (фон, nice).
3. **Объекты, CPU:** `SceneModelsPass::collect` (`scene_models_pass.cpp:1035+`) — несколько
   проходов по всем инстансам каждый кадр (horizon, cullToFrustum, selectLevels, sizing,
   hash-map lookups по регионам, бюджет, cutAtHierarchy с unordered_map). При смене
   placement — `publishScatter/gatherInstances/updateRegionMembers` по всем объектам.
   Цель как у Nanite: постоянный GPU-буфер инстансов, отбор/LOD на GPU, CPU O(изменений).
4. **Объекты, GPU:** `MeshRootCuller` разворачивает каждый объект во ВСЕ его кластеры без
   обхода DAG (лимит 262k кандидатов). Нужен иерархический обход (BVH по группам /
   per-instance выбор полосы уровней по ошибке). Hi-Z выключен при 4x MSAA — нужен
   depth resolve (min/max) в single-sample для Hi-Z.
5. **Террейн, хитч загрузки:** `GpuTerrain::update` — `accept()` грузит все меши плана за
   кадр без бюджета; `publishTable` пересобирает всю indirection-таблицу; тройной проход
   по residency. Бюджетировать по байтам/кадр, обновлять таблицу инкрементально
   (`ASR_TERRAIN_PROFILE` печатает тайминги).
6. **Листва:** карточки в игровом бейке вынесены из DAG (компоненты ≤2 tri → discrete chain).
   Попробовать листву целиком в DAG с `thinCards`, сравнить в лабе; в перспективе —
   voxel/aggregate уровни как Nanite Foliage 5.6+ для дальних рощ.

## Файлы
Новые: `tools/ue_nanite_reference.py`, `tools/mesh_lab.cpp`,
`src/game/client/explore_bench.{hpp,cpp}`, этот файл.
Изменённые: `CMakeLists.txt`, `src/engine/geometry/cluster_dag.{hpp,cpp}`,
`src/engine/render/device.{hpp,cpp}`, `src/engine/pipeline/runner.cpp`,
`src/game/client/{main.cpp,explore_view.cpp,explore_view.hpp}`,
`src/game/render/world_renderer.{hpp,cpp}`, `tools/scene_model_clusters.cpp`.
Сгенерированное (в .gitignore): `assets/generated/ue_reference/`, `assets/generated/mesh_lab/`.
