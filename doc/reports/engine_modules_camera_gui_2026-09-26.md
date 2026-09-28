# Engine modules, unified camera selection и GUI — 2026-09-26

## Реализованный этап

Основание: `doc/unified_camera_representation_lod_system.docx`.
Это работающий этап реализации, не объявление всех новых DOCX завершёнными.

### Камера и представления

- Реализация камеры физически перенесена в `src/engine/camera/`.
  Старый `game/client/camera.hpp` содержит только совместимые aliases.
- `ViewState` содержит положение, оптическую ось, FOV, viewport, скорость,
  тип проекции, near plane и quality profile. Его получает реальный WorldRenderer.
- `asr_representation`: экранная ошибка по консервативной сфере, сравнение стоимости
  resident geometry/impostor/aggregate, угловая ошибка, ошибка параллакса,
  гистерезис, прогноз положения и запрос более подробного представления.
- Обход `RepresentationNode` выбирает покрывающий срез; недоступные parent proxies
  приводят к раскрытию детей. Отсутствие представления у листа явно помечает
  срез неполным, а не выдаётся за успешный рендер.
- В SceneModelsPass фактический выбор mesh/impostor подключён к этому API.
  Стоимость геометрии оценивается по её source-cluster cut, а не только по LOD-chain.
  Сохранены complementary dither переходы и ограниченная временем история выбора.
- Профиль камеры задаёт допуск scene-object LOD, density и aggregate cuts.
  Прогноз положения используется для приоритета размещения регионов.
- Текущий восьмиракурсный atlas не имеет depth и верхних видов. Его погрешность
  оценивается консервативно; на неподходящих ракурсах сохраняется геометрия.
  Коэффициенты стоимости — оценки, не GPU timestamps.

### Настоящие библиотечные модули

CPU-цели определены в `src/engine/modules.cmake`:

| CMake target | Публичное имя | Содержимое |
|---|---|---|
| `asr_camera` | `Campfire::Camera` | проекции, rays, pan/orbit/free, ViewState |
| `asr_representation` | `Campfire::Representation` | оценка и выбор представления, обход иерархии |
| `asr_terrain_generator` | `Campfire::Terrain` | tectonic plates, ranges, erosion, mountain skeletons, drainage incisions, общий шум |
| `asr_virtual_geometry` | `Campfire::VirtualGeometry` | cluster DAG/assets/morphs, instance hierarchy, region/quad mass, логическая резидентность страниц |
| `asr_virtual_geometry_gpu` | `Campfire::VirtualGeometryGPU` | GPU cluster/root/hierarchy culling и Hi-Z; требует render backend |
| `asr_engine_gui` | `Campfire::Gui` | immediate-mode UI и module workspace |

`asr_geometry` сохраняет reconstruction mesh и транзитивную совместимость с
виртуальной геометрией. Перенесённые алгоритмы террейна не копируются второй
реализацией в game: старые заголовки только перенаправляют вызовы.
Численные алгоритмы и fixed-point расчёты при переносе не изменены.

CPU-модули не линкуют игру, content DB, ECS, SDL или DXC. Это проверено самостоятельной
линковкой `asr_engine_module_tests`, а не только именами папок.

### GUI движка

Запуск: `./run.sh engine-editor`.
Цель: `campfire_engine_editor`. Своя build directory, без переконфигурации клиента.
При первом запуске используются уже скачанные SDL/EnTT, если они доступны.

- самостоятельное окно без игровой симуляции и генерации континента;
- Map/Orbit/Free, изменение FOV, reset, управление мышью и WASD/QE;
- настройка geometry error и просмотр quality profile;
- seed/relief/rebuild генератора;
- реальный 33×33 terrain mesh, построение DAG существующим Nanite-style builder;
- выбор cluster cut по экранной ошибке, счётчики, wireframe;
- построение в worker, публикация только результата актуальных настроек;
- сохранение настроек, `--settings`, `--shot` для автоматического preview.

Preview использует CPU-проекцию и SDL drawing. Это не viewport полного игрового
GPU-рендера, не редактор существующих миров и не live remote inspector клиента.
GUI-настройки принадлежат собственному workspace редактора.

## Проверки

- `asr_engine_module_tests`: **41/41**.
- CTest `engine_modules`, `smart_mesh_components`, `scene_view`, `render_systems`: **4/4**.
- Выбранные terrain/hybrid/plate/range/erosion тесты: **34/34**.
- Standalone GUI собран без shadercross/LLVM/DXC; выполнены два реальных запуска
  с разными seed, relief, FOV, wireframe и допуском.
- Проверка компиляции исходников клиента после alias-переноса: пройдена.
- Полностью скомпилированы 133 единицы трансляции проекта и слинкован
  `cmake-build-relwithdebinfo/campfire_client` с готовыми сторонними библиотеками.
  Сбой повторного FetchContent скачивания SDL_image обойдён конфигурацией
  с явными локальными source directories; сторонние LLVM/DXC не пересобирались.
- Пересобранный клиент прошёл Metal smoke: seed 11, 640×400, 32 загрузочных кадра
  и по 16 кадров map-far/orbit/forest/flight/zoom-out, exit 0.
  Это проверка работоспособности, не доказательство ускорения или отсутствия спайков.
- Контроль с 180 загрузочными кадрами и 60 кадрами на вид также завершился с exit 0.
  Mean / max, мс: map-far 2.57 / 13.60; orbit 14.22 / 48.35;
  forest 5.80 / 11.28; flight 7.47 / 12.72; zoom-out 13.89 / 27.18.
  Streaming на маршруте не достигает settled; это не steady-state benchmark и
  не A/B сравнение с прежней версией. Пики остаются.
- GUI-бинарник дополнительно собран в основной папке:
  `cmake-build-relwithdebinfo/campfire_engine_editor`.

Артефакты: `.cache/engine-modules-20260926/`:
`engine-editor.png`, `engine-editor-alternate.png`, build/test/syntax logs.
Снимки действительно отрендерены; ручной визуальный аудит не проводился.

## Что ещё не реализовано

- Hemispherical/octahedral color-normal-depth atlas, depth reprojection и recursive
  runtime baking лесных impostors. Для них требуется расширение формата ресурса,
  baker, residency cache и shader path, не просто новый порог выбора.
- Полное объединение существующего terrain streaming и spatial mass baking
  в новый generic RepresentationNode traversal. Индивидуальные scene objects
  уже используют селектор; существующие aggregate/density paths используют
  общий quality allowance, но ещё сохраняют собственную организацию кэшей.
- Prefetch parent-proxy ресурсов и их полный residency grace lifecycle:
  selector выдаёт прогноз/указание, а текущий placement использует прогноз для приоритета.
- Hybrid/foundation world orchestration остаётся в game: она читает и изменяет
  WorldMapData, климат, породы и игровые биомы. Следующий перенос требует выделения
  отдельного heightfield input/output контракта; прятать game-зависимость внутри
  engine target в этом этапе не стали.
- Полноценный сценовый редактор, asset browser, undo/redo, docking и GPU viewport.
- Незавершённые пункты shadow/ecology DOCX из предыдущего отчёта не считаются
  закрытыми этим архитектурным этапом.

Изменения не закоммичены; независимые правки assets/shaders/ecology не откатывались.

