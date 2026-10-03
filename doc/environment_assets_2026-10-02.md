# Ассеты для детализации окружения и очередь скачивания

Общий документ для скачивания и подключения. Ставь `+` в первом столбце после скачивания; рядом можно указать путь. Уже подтверждённые локальные файлы отмечены `+`. Для Fab выбирай 2K, карты Base Color, Normal, Roughness, AO и Opacity; для моделей — glTF либо добавление в AncientSettlement UE-проект. Проверка доступности ссылок: 2 октября 2026.

| Done                                         | Asset | Why it fits / ingest target | Source |
|----------------------------------------------|---|---|---|
| +                                            | Moss 01 (mesh + texture) | Подключён как Moss Clump: подушка из 49 побегов, 840 треугольников, LOD и depth impostors. | [Poly Haven Moss 01 (CC0)](https://polyhaven.com/a/moss_01) |
| +                                            | Fern 02 | Подключён как Fern Clump: лесной папоротник, 1496 треугольников в выбранном варианте. | [Poly Haven Fern 02 (CC0)](https://polyhaven.com/a/fern_02) |
| +                                            | Shrub 03 | Оптимизированная дополнительная библиотека; в текущую сцену выбраны Greenleaf Vision A/B. | [Poly Haven Shrub 03 (CC0)](https://polyhaven.com/a/shrub_03) |
| +                                            | Shrub Sorrel 01 | Оптимизированная дополнительная библиотека для влажного низкого подлеска; сохранён для расширения набора. | [Poly Haven Shrub Sorrel 01 (CC0)](https://polyhaven.com/a/shrub_sorrel_01) |
| +                                            | Forest Leaves 02 | Forest Leaves 02 уже используется для лесной подстилки; новый архив 1K сохранён в assets/downloaded. | [Poly Haven Forest Leaves 02 (CC0)](https://polyhaven.com/a/forest_leaves_02) |
| +                                            | Nordic Moss / Moss surface | Подключён из moss_readl2_1k.zip как мелкая декаль fine_moss, масштаб 0.25 м. | [Fab / Quixel Moss (free listing)](https://www.fab.com/listings/29307c07-c07f-4b0d-8d91-6581481a7431) |
| +                                            | Tileable Moss Patches | Подключён из tileable_moss_patches_sfdnqii_1k.zip: opacity, нормали и свойства; шейдинг камня и террейна, масштаб 2 м. | [Quixel Moss Patches](https://www.fab.com/listings/a9a94514-dfba-43cb-92bc-c550d79b8d5d) |
| +                                            | Moss Patch | Подключён из moss_patch_tjxpndh_1k.zip: лесные пятна мха, масштаб 0.5 м. | [Quixel Moss Patch](https://www.fab.com/listings/b5fad446-5ca0-44b1-96cf-74051936b0e6) |
| +                                            | Fallen Branches decal | Подключён из fallen_branches_se1kxrh_1k.zip: прозрачные веточки с нормалями и AO; дополняет объёмный сухостой. | [Quixel Fallen Branches](https://www.fab.com/listings/874d948f-b6a2-4846-bdeb-49840d70658b) |
| + (добавлено в ue проект ancient settlement) | Free Shrubs Pack | Девять полных вариантов из AncientSettlement экспортированы и оптимизированы, 261 ракурс импостеров. A/B подключены в сцену. | [Greenleaf Vision Shrubs](https://www.fab.com/listings/7ca465ab-fb9c-4d6b-bddb-82c20f604657) |

Подходящая бесплатная декаль следов животных с ясной лицензией не найдена. В существующую систему добавлен процедурный тип `trail`: извилистая тропа с рваными краями и чередующимися отпечатками копыт. Это декоративные тропы по местности; фактическое движение животных пока не создаёт следы.

## Local cache check

В Unreal Vault Cache подтверждены скачанные Beech Fern, Mossy Forest Rock, Nordic Forest Ledge Rock Large, Nordic Forest Cliff Large, Nordic Forest Rock Small, Mossy Embankment, Wild Grass, Thatching Grass, Bolete Mushrooms, Wood Sorrel и сухостой. Для части из них есть результаты в `assets/generated/simplified_models/`; Nordic Forest Cliff Large и Nordic Forest Ledge Rock Large ещё не подключены в каталог сцены. Повторное скачивание не требуется.

## Подключённый набор

Основной каталог расширен с 8 до 14 моделей; дополнительные ресурсы собраны скриптом `tools/prepare_environment_models.py`. Клифы, папоротник и объёмный мох — Poly Haven, CC0; кусты — Greenleaf Vision из UE-проекта, с исходной лицензией Fab. Карты Quixel сохраняют исходную лицензию Fab. В атласе 469 слоёв при лимите 512. Каждый визуал имеет 8 боковых и 21 полусферический импостер с глубиной, цепочку LOD и кластеры.

| Визуал | Источник | Физические размеры рамки |
|---|---|---|
| Cliff Face Grey | [Rock Face 01](https://polyhaven.com/a/rock_face_01) | 5.42 × 5.18 м |
| Cliff Face Warm | [Rock Face 02](https://polyhaven.com/a/rock_face_02) | 4.00 × 3.15 м |
| Moss Clump | [Moss 01](https://polyhaven.com/a/moss_01), 49 побегов в подушке | 0.38 × 0.038 м |
| Fern Clump | [Fern 02](https://polyhaven.com/a/fern_02) | 1.30 × 0.44 м |
| Shrub Dense | Greenleaf Vision, Shrub B | 4.53 × 3.08 м; целевая высота подлеска 0.75–1.30 м |
| Shrub Low | Greenleaf Vision, Shrub A | 3.48 × 2.15 м; целевая высота подлеска 0.25–0.50 м |

Исходный наклон сканов (33–45°) убран жёстким поворотом при подготовке: средняя нормаль лицевой поверхности горизонтальна, геометрия не растягивается. Клифы ориентируются по градиенту высоты, масштабируются по перепаду 8 м окрестности, заглубляются относительно нижней опоры и занимают разреженную сетку 16 м. Нижняя опора учитывает реальный выступ лицевой стороны: 47 см у Grey и 10 см у Warm. Отбор требует камня и склона; вода и снег исключают размещение. Подлесок использует отдельные ID и сетку 4 м, пятна 12 и 64 м, влажность, опушку, уклон, открытый грунт, настройку `undergrowth` биома и кисть плотности. Экспозиция мха постоянна и не следует движущемуся солнцу. Мох в шейдинге террейна смешивает цвет, нормали, AO и roughness Tileable Moss Patches с opacity; на моделях камня смешивает цвет и нормали по постоянному экологическому коэффициенту, включая depth impostors.

В библиотеки добавлены растения `forest_fern`, `dense_understory`, `low_understory`, `moss_cushion` и пропсы `cliff_face_grey`, `cliff_face_warm`. Они доступны для биомов и ручных закреплений `source/details`. Маленькие растения заглубляются с учётом их высоты. У подлеска порог перехода в меш — 60 пикселей вместо порога крупных крон; общий бюджет геометрии сохраняется, стоимость учитывает реальный срез кластеров.

Восстановление после переустановки базового UE-каталога:

```sh
cmake --build cmake-build-relwithdebinfo --target model_simplify impostor_bake scene_model_clusters
python3 tools/fetch_models.py --only rock_face_01 rock_face_02 moss_01 fern_02
# gv_free_shrubs: сначала экспорт из AncientSettlement через export_ue_foliage.py
# затем AS_UE_FOLIAGE_TEXTURES_ONLY=1 для дополнительных нормалей (PNG/EXR)
python3 tools/prepare_ue_foliage.py --only gv_free_shrubs
python3 tools/prepare_environment_textures.py
python3 tools/simplify_models.py --only rock_face_01 rock_face_02 moss_01 fern_02 gv_free_shrubs
python3 tools/prepare_environment_models.py
```

Последний скрипт сам печёт импостеры выбранных визуалов и строит кластеры. Повторный запуск заменяет расширение каталога и сохраняет число слоёв; имена и ID первых восьми моделей сохраняются. Конфигурация выбора — `content/config/environment_models.json`. Для Greenleaf Vision выбран бюджет 24000 треугольников на вариант: A — 23175, B — 24000. IoU их силуэтов по трём проекциям — 0.86–0.89 и 0.83–0.86. Остальные семь вариантов тоже оптимизированы и остаются в локальной библиотеке.

## Подключение новых загрузок

- Архивы найдены в `assets/downloaded`: четыре Quixel-набора и `forest_leaves_02_1k.blend.zip`. Плюсы пользователя сохранены.
- Все четыре Quixel-набора распакованы и упакованы через существующий `pack_terrain.py`: AO / roughness / height / opacity, нормали OpenGL, цепочки mip. Исходники 1K; базовые runtime-карты 2K для совместимости с массивом террейна, без добавления детализации. Происхождение, размеры и SHA-256 сохранены в `assets/terrain/environment/*/src/source.json`.
- Tileable Moss Patches (2 м) используется в шейдинге террейна и моделей камня; Moss Patch (0.5 м), Moss (0.25 м) и Fallen Branches (2 м) — текстурные декали в существующих декор-биомах.
- Из `AncientSettlement/Content/GV_FreeShrubsPack` экспортированы девять полных кустов, 13.84 млн исходных треугольников. Цвет и нормали выгружены из UE, включая HDR-нормали через EXR. Выбраны компактные Shrub A/B; исходные UE-пакеты не пересохранялись.

## Pipeline notes

1. Acquire source under `assets/models/` (retain its license/provenance file).
2. Simplify with the existing scene model optimizer and confirm silhouette/triangle budgets.
3. Bake the 8-view ring and hemisphere impostors using `tools/bake_model_impostors.py` and the native `impostor_bake` binary.
4. Register the model in the scene visual catalogue and biome tables, then place through the stable seeded scatter field. Moss should follow rock, moisture and aspect; shrubs/ferns should follow light, canopy edge, soil depth and moisture. Track decals should follow animal routes/terrain corridors with broken edges and variable width rather than uniform stamps.

## Placement mechanisms to preserve

The existing detail candidates are on an 8 m stable grid, independently hashed by world position, grouped by low-frequency ecology fields, and queried against ground materials, slope, forest, moisture and hand-painted density. Object IDs are coordinate-derived, so new species should consume only their own candidate stream and preserve page seams. Avoid one uniform Poisson field across every prop: use cluster/gap masks, habitat suitability and scale/yaw variation at the species level. Large cliff meshes should be aligned to the local steepest direction and bedded into the terrain; small rocks/scree should collect at cliff feet, with spacing correlated to slope and outcrop noise.

## Результат подключения

- В сцене 14 моделей и 469 слоёв атласа; для шести новых визуалов есть SCM2, LOD, depth impostors и кластеры. У всех девяти вариантов Greenleaf Vision отдельно испечено по 29 импостеров (261 ракурс).
- В террейне 26 текстурных слоёв и 12 типов/наборов декалей. `worldtool biomes --check-assets assets` сообщает 0 проблем; клиент собран и запускается с Metal.
- Для HDR-нормалей UE подготовщик использует OpenCV с поддержкой EXR; для обычных карт достаточно NumPy/Pillow.
- `forest_leaves_02_1k.blend.zip` сохранён в очереди загрузок; Forest Leaves 02 уже подключён более подробным локальным набором, повторная замена карт не нужна.
- Просмотр результата: [лес с загруженным террейном](previews/environment_2026-10-02/forest-review.png), [опушка](previews/environment_2026-10-02/forest-edge-final.png), [клиф после выравнивания](previews/environment_2026-10-02/cliff-review.png). Для кадра клифа выбран свет с азимутом 60°; настройки обычного просмотра не менялись. Параметры кадров и логи находятся в той же папке.

Последняя сборка клиента завершилась успешно. Компиляция `TerrainPS`, варианта террейна со страницами и `ModelDepthPS` в SPIR-V/Metal прошла; запуск с ожиданием загрузки террейна завершился с кодом 0. Финальные визуальные кадры и лог: `doc/previews/environment_2026-10-02/review-*`.
