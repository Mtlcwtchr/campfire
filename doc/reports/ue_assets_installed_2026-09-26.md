# UE-природа и материалы установлены — 2026-09-26

## Последующее исправление листвы

Первый импорт ниже перенёс базовые меши без Nanite assembly-кроны.
Это исправлено, добавлены лесные детали и отключён тинт земли:
[актуальный отчёт](foliage_close_view_fix_2026-09-26.md).
Числа и проверки ниже относятся к первоначальной установке.

## Первоначальный результат

**Активный набор 3D-эксплорера заменён на подготовленные ресурсы AncientSettlement UE.**
Это уже установленные файлы, не только инструменты/план. Коммит не создавался.

- `assets/generated/scene_models/.ue-imported` активен; manifest имеет
  `source: AncientSettlement UE export` и source_asset для каждой модели.
- Основной каталог: бук 9256, ель 8961, куст 408, камень 2500, гриб 300 треугольников
  на L0; у всех четырёхуровневая дискретная цепочка плюс новые cluster DAG/crown sidecars.
- Трава: шесть видов, запечённых из уже урезанного MWAM GrassA (780 треугольников).
- Террейн: MW Grass, Dirt, SandA, Rock, Snow; wet mud — Dirt с отдельной roughness.
  Заменены шесть активных групп, albedo/normal/properties и mip-уровни.
- Геометрия всего выбранного исходного комплекта: **22205 треугольников**.
- Подготовленный пакет с sidecar-файлами: около **40 МБ**, модели — около **11 МБ**.
  Это размеры файлов, не измерение VRAM. Высокополигональные оригиналы не экспортировались.

Превью установленного набора: `assets/generated/ue_assets_preview.png`.
Исходные ограниченные экспорты: `assets/source/ancient_settlement/`.
Сохранённый подготовленный комплект для повторной установки:
`assets/generated/ue_assets_stage/`.

## Что мешало экспорту

Основной UE-проект завершался до Python с ошибкой:
`Plugin AncientEngine failed to load because module AERender could not be found`.

Вместо изменения/пересборки UE-проекта создан минимальный commandlet-проект
`.cache/ue_asset_reader/AssetReader.uproject`, с PythonScriptPlugin,
EditorScriptingUtilities и GeometryScripting. Его Content — ссылка на исходный UE Content,
не многогигабайтная копия. Игровые модули не загружаются.

Использованный каталог взят из UE `Saved/AssetBudget/meshes.json`:
проверены ID роли, путь меша, material overrides и совпадение текущего количества
треугольников Source LOD0 с отчётом. Это проверка сохранённого каталога, не чтение
недоступного native DataAsset-класса. HiRes не используется; Source LOD0 уже физически
перезаписан UE cut_assets.py в урезанный вариант.

`ue_asset_session.py` позволил исправлять экспорт без повторной инициализации UE.
После успешного job-003 сессия закрыта job-004. UE-пакеты не сохранялись.
Журнал: `.cache/ue_asset_session_20260926/`.

## Материалы и проверки

- Исправлен вызов GeometryScript: в установленном UE доступен `get_num_triangle_i_ds`,
  не предполагавшийся `get_triangle_count`.
- Megaplant PackedNormal: нормаль в RG, B — AO/translucency; Z восстанавливается из RG,
  затем применяется преобразование DirectX → OpenGL.
- Light Foliage: цвет запечён из реального графа — base, Tint_2/R, Tint_3/G, Tint_1/B;
  сохранена маска прозрачности. Камерные fake shadows/dither остаются задачей рендера.
- MW grass: учтён static switch отдельной opacity texture; неиспользуемый placeholder
  не попал в игровой набор.
- **12/12 CPU-тестов импортера прошли**, включая новые преобразования материалов.
- Проверены размеры/индексы SCM2/SCC6, все PNG и mip-уровни, бюджеты ролей,
  порядок уровней/ошибок и ограничения runtime-каталога.
- Активные модели/текстуры побайтно сверены с проверенным staging перед удалением старого набора.
- `asr_client` собран с `nice`, `-j 2`; повторная сборка после удаления старого архива
  проверяет отсутствие зависимости от него.
- Headless smoke: `--explore --world tiny --headless 640x400 --shot-frame 3` успешно
  загрузил ресурсы и сохранил `assets/generated/ue_install_smoke.png`.
  На третьем кадре ещё нет размещённых моделей: это проверка загрузки, **не визуальная
  приёмка леса и не FPS-бенчмарк**. Цель 60–120 FPS этим не подтверждалась.

## Старый набор удалён

Удалено около **664.9 МиБ** файлов (логический объём, не гарантия освобождения всех
блоков APFS/Git LFS):

- `assets/models/Stylized Nature MegaKit[Standard].zip`;
- отставленный при установке старый каталог моделей и шесть старых terrain-групп
  из `assets/retired/before-ue-20260926-004702-632839/`;
- `assets/generated/scene_models_backup_before_exact_2026-09-25/`;
- `assets/generated/scene_models_clusters_before_nanite/`;
- старые `sprite_0052.png` … `sprite_0057.png` в Kenney foliage sprites.

FoliagePass, вспомогательный SpritePass и шесть записей grass в общем sprites.json
используют новые UE-виды. CMake не распаковывает старый архив при наличии `.ue-imported`.
`extract_sprites.py` сохраняет новую привязку; `fetch_terrain.py`/`pack_terrain.py`
не восстанавливают старые Poly Haven карты поверх шести UE-групп.

Другие библиотечные поверхности, вода, интерфейс, персонажи, животные, здания и
2D-тайлы/иконки не удалялись: они не входят в заменённый набор 3D-природы.
CREDITS.md и источник метаданных terrain_materials.json обновлены; импортированные
пакеты не объявляются CC0, их исходные лицензии сохраняют силу.

