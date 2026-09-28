# Модульный GPU-viewport движка — 2026-09-27

Продолжение `engine_modules_camera_gui_2026-09-26.md` и реализации
`unified_camera_representation_lod_system.docx`. Это законченный этап редактора,
а не объявление всех camera/shadow/ecology документов реализованными.

## Реализовано

- `engine::DagViewport` / `Campfire::Viewport`: независимый просмотр cluster DAG
  через существующий SDL_GPU backend. Не линкует WorldMapData, content или симуляцию.
- Вершины и индексы всех уровней находятся в резидентных GPU-буферах. Новая ревизия
  mesh загружается целиком и публикуется только после успешной загрузки.
  Перемещение камеры не загружает mesh повторно.
- Рендер использует `Camera::viewProjection`, depth buffer, viewport/scissor,
  аппаратный near/far clipping, solid/wireframe, resize и оконный swapchain.
  CPU-проекции вершин и painter-sort в этом режиме нет.
- Выбор среза остаётся CPU-вызовом существующего `geometry::cutAt` с общей
  экранной ошибкой. Это НЕ новый GPU-driven traversal и НЕ полный игровой рендер.
- `PipelineWanted::depthClip` делает clipping явным свойством прохода.
  По умолчанию сохранён прежний depth clamp, необходимый игровым terrain skirts;
  новый viewport включает clipping. Тест воспроизвёл ошибку clamp на near plane.
- `src/engine/gpu_modules.cmake` определяет `Campfire::Render`,
  `Campfire::VirtualGeometryGPU`, `Campfire::Viewport` независимо от игрового клиента.
  GPU-редактор доступен при `ASR_BUILD_CLIENT=OFF`.
- UI-виджеты программно рисуются в прозрачный RGBA-слой. Этот слой кэшируется,
  композится на GPU и загружается повторно только при изменении пикселей.
  Ввод и логика кнопок работают каждый кадр, даже когда рисование пропущено.
  Изменение размеров сбрасывает кэш; внешняя смена темы требует `invalidateUi()`.
- GPU→CPU readback выполняется только для явного screenshot/тестов, не в рабочем кадре.
- Сохранён CPU fallback; GPU-ошибки не маскируются автоматическим переключением на CPU.

## Запуск

- `./run.sh engine-editor` — GPU по умолчанию, переиспользует основную GPU-сборку,
  если она есть. Новая GPU-сборка требует SDL_shadercross/DXC.
- Готовый бинарник: `./cmake-build-relwithdebinfo/campfire_engine_editor`.
- `--cpu-preview` — прежний CPU renderer внутри того же бинарника.
- `CAMPFIRE_ENGINE_EDITOR_GPU=OFF ./run.sh engine-editor` — отдельная сборка без DXC.
- `--settings FILE`, `--shot FILE.png` сохранены.
- `--bench 240 --profile result.json` — автоматический orbit-маршрут без swapchain,
  30 прогревочных кадров плюс 240 измеряемых. Настройки workspace не перезаписываются.

## Профилирование и оптимизация

Внешний Time Profiler: 7671 CPU-семпл. `FlushRenderCommands` — 83.69% inclusive,
`SDL_BlendFillRect_RGBA` — 38.01% self, `SDL_SW_FillTriangle` — 24.91% self.
Основная стоимость была в повторном рисовании неизменного GUI, а не в DAG.
После введения кэша GUI:

| Прогон GPU-редактора, 1440×900 | Медиана, мс | p99, мс | Max, мс |
|---|---:|---:|---:|
| До кэширования, 240 кадров | 8.333 | 9.595 | 11.587 |
| После, 240 кадров | 0.428 | 3.265 | 4.659 |
| Длинный контроль, 2000 кадров | 0.468 | 3.644 | 5.741 |

В длинном контроле: 1 загрузка DAG, 2 перерисовки GUI, 2 загрузки UI-текстуры,
83 868 байт mesh-буферов. Пиксели контрольного GPU-кадра до/после оптимизации
полностью совпадают. Это preview сетки 33×33, не benchmark игрового мира.
Время кадра включает CPU, GPU и ожидание fence; это не чистое GPU timestamp-время.
Редкие пики остаются, их полное устранение этим прогоном не доказано.

Trace и HTML-отчёт сохранены в `profile-baseline/`. xctrace вернул 54 при завершении
записи по лимиту; trace прочитан и проанализирован. Это не успешный самостоятельный
выход профилируемого процесса. Обычные benchmark-прогоны завершились успешно.

## Проверки

- Новые GPU/GUI-тесты: **7/7**, включая реальное окно Metal/vsync и resize.
- Depth независимо от порядка кластеров, Map/Orbit/Free и scissor, частичное
  отсечение near plane, revision/cache, очистка mesh, UI upload/resize,
  прозрачность UI, работа кнопок при кэшировании и явная инвалидизация.
- Независимые CPU-модули: **41/41**.
- Пересобранные CTest-группы `engine_modules`, `smart_mesh_components`,
  `scene_view`, `render_systems`: **4/4**.
- CPU-only редактор собран обычным CMake и успешно записал PNG без DXC.
- GPU-бинарник и тесты собраны командами, сгенерированными CMake, с уже готовыми
  сторонними библиотеками; LLVM/DXC не пересобирались. Оконный и headless пути проверены.
- `bash -n run.sh` и targeted `git diff --check`: пройдены.
- Полный CTest, интеграционный игровой benchmark и ручной визуальный аудит не проводились.

Артефакты: `.cache/engine-viewport-20260927/` — PNG, JSON benchmark,
trace/HTML-профиль, логи сборки и тестов.

## Следующие пункты документов

Остаются hemispherical color/normal/depth atlas, depth reprojection, recursive
forest impostor baking, generic representation/residency traversal и выделение
hybrid/foundation heightfield-контракта из game. Из GUI остаются asset browser,
редактирование сцены, undo/redo, docking и полностью GPU-native отрисовка виджетов.
Shadow/ecology backlog предыдущих отчётов не закрыт этим этапом.

Изменения не закоммичены. Предыдущие правки assets, материалов, теней и экологии
не откатывались и не включались в утверждение о завершении этого этапа.
