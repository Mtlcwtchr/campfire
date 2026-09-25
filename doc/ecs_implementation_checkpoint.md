# ECS migration checkpoint

Дата: 2026-09-09

## Что завершено в текущем батче

- Введена разбитая по доменам ECS-модель EnTT: identity/markers, spatial, needs, jobs, inventory, social, construction и render-компоненты.
- `EcsWorld` стал фасадом над `entt::registry`: snapshot для чтения, стабильные entity handles, планировщик пространственных батчей, локальные worker-фазы, boundary-фаза и детерминированный commit команд.
- Обработка животных переведена на local batch + локальные результаты + последовательное применение команд; граничные случаи обрабатываются после локальной фазы.
- Витальные состояния людей и животных разложены на независимые компоненты (`Health`, `Hunger`, `Thirst`, `Fatigue`, `SleepState`, `BodyTemperature`, `AilmentState`). Старый агрегат `PersonState` удалён.
- Инвентарь и социальные связи людей разложены на атомарные компоненты (`Carrying`, `EquippedTool`, `SettlementMember`, `HouseholdMember`, `Parentage`, `Spouse`); агрегированные ECS-типы удалены.
- Расчёт movement capacity теперь читает `Carrying` из ECS, сохраняя legacy-поле только как fallback для переходных вызовов.
- Проверка инструмента в work planner также читает `EquippedTool` из ECS с тем же fallback.
- `workCapacity()` теперь использует ECS-виталы как источник расчёта производительности и движения; legacy `Person` остаётся только fallback.
- Population conception checks теперь берут settlement и spouse из ECS; pairing обновляет `Spouse` сразу, чтобы изменения были видны в том же тике.
- Добавлен единый inventory adapter; planner и capacity используют `Carrying`/`EquippedTool` через него, а изменения обновляют ECS и legacy mirror одновременно.
- Базовые операции job execution (`pickUp`, `putDownAt`, выбор/износ инструмента) переведены на тот же adapter.
- Все оставшиеся inventory-проверки и записи в `job_execution.cpp` переведены на adapter; прямых обращений к `Person::carrying/equippedTool` там больше нет.
- `World::destroyStack()` теперь очищает соответствующий ECS inventory-компонент одновременно с legacy mirror.
- Удаление stack из мира больше не оставляет висячие ссылки в `Carrying`/`EquippedTool`.
- Construction job execution теперь читает и обновляет `ConstructionProgress` из ECS, одновременно поддерживая legacy `Building::workDone`.
- Work planner использует тот же ECS-прогресс при оценке оставшейся работы на стройке.
- Добавлен отдельный `JobLinks` для ссылок assignment; подсчёт занятости рабочих мест больше не читает `Person::job.building`.
- Добавлен `World::syncEcsJob()`: новые и освобождённые assignments публикуются в ECS сразу, поэтому несколько назначений в одном planner-проходе видят актуальную занятость; тест проверяет все ссылки `JobLinks`.
- Полная projection-синхронизация и точечная синхронизация job используют одну реализацию `syncEcsJob()`, без расхождения наборов компонентов.
- Инвариантные проверки и health/ailment-сводка в `report.cpp` читают hot needs, ailments и inventory из ECS.
- Детальный population report также выводит satiety/hydration/rest/health из ECS, чтобы диагностика отражала authoritative state.
- Детальный job report теперь читает kind/category/progress/target из ECS; добавлен отдельный `JobCategory`.
- Пространственный scheduler теперь выдаёт local/boundary батчи в детерминированном порядке колец от focus для progressive center-out обновления.
- `tryPersonalNeed()` использует единый `NeedsSnapshot` из ECS для hunger/thirst/rest/health/ailment решений.
- Убран массовый legacy→ECS needs sync из `assignJobs()`: planner теперь читает ECS authority, а legacy bridge вызывается явно.
- Планировщик при interrupt/приоритизации также использует `NeedsSnapshot`; `mealPortions()` и `eatFrom()` читают и обновляют ECS hot needs, сохраняя legacy mirror.
- Добавлен узкий `syncEcsNeeds()` для переходных legacy-вызовов; planner и job execution публикуют только needs-компоненты, не перезаписывая job/construction/inventory ECS-состояние.
- Abstract simulation входит и выходит через ECS hot-needs bridge; расчёт доступных рук использует ECS Health.
- Daily abstract food accounting записывает Hunger/Thirst/Fatigue/Health непосредственно в ECS, сохраняя legacy mirror.
- Work eligibility вынесена в ECS-aware `canWork(World, Person)`: сон и тяжесть ailment больше не читаются planner напрямую из legacy aggregate.
- Медицинский поиск пациентов и starvation-проверка inventory также читают ailment/severity/satiety через `NeedsSnapshot`.
- Внутренний disease pass `tickNeeds()` теперь изменяет `AilmentState` напрямую; legacy `Person::ailment*` обновляется только как mirror.
- `BodyTemperature` в needs pass также является прямым ECS write с legacy mirror.
- Job execution напрямую записывает ECS `AilmentState`, `Thirst`, `Health` и `SleepState`; legacy-поля синхронизируются только для совместимости.
- Медицинский `Tend` job теперь читает patient через `NeedsSnapshot` и обновляет `ecs::AilmentState` напрямую; legacy ailment-поля используются только fallback-мостом.
- Livestock daily condition теперь читается из ECS `Health`, считается локально в batch и публикуется командой `SetHealthComponent`; `Animal::condition` — mirror.
- Livestock report и checksum читают penned/condition из ECS (`SleepState`/`Health`), с legacy fallback только для непроецированных сущностей.
- `syncEcs()` больше не перезаписывает существующие animal `Health`/`SleepState`; projection только инициализирует отсутствующие компоненты и обновляет legacy mirror.
- World checksum теперь хеширует ECS needs, включая sleep/ailment, поэтому fingerprint отражает runtime authority.
- Состояние работы и прогресс строительства публикуются обратно в ECS после job execution (`JobState`, `JobProgress`, `ConstructionProgress`).
- Добавлен отдельный AI-слой с LOD (`Focus/Near/Mid/Far/Dormant`), интервалами мышления и elapsed time для удалённых сущностей.
- AI planner теперь передаёт фактический overdue elapsed span при пропущенных cadence boundaries, а не только минимальный LOD interval.
- World получил явный `setAiFocus(CellId)` seam для клиентской камеры; headless simulation сохраняет fallback на `localCell()`.
- Добавлен neutral render extraction: ECS собирает отсортированные sprite/mesh batches, а сам renderer о ECS не знает.
- Save/load обновлён до версии 3 с восстановлением ECS-совместимых animal needs.
- Документация архитектуры: `doc/ecs_parallel_architecture_plan.md`, `doc/entt_ecs_migration.md`.

## Проверка

- `cmake --build build --target asr_tests -j 8` — успешно.
- `asr_ecs_tests` после последнего чанка: **44/44 passed**; targeted simulation checks `the_community_feeds_itself_without_any_order` и `people_carry_a_meal_home_and_sleep_in_the_house` проходят. `they_build_without_being_told_where` остается красным из-за отсутствия стартовых веток для `campfire` в текущем river-valley generation. Полный `asr_tests` собирается; полный прогон остаётся длительным.
- ECS: последний зафиксированный зелёный набор — 20/20. После добавления projection regression-тестов focused target требует повторной стабилизации: общий build сейчас блокируется параллельными изменениями Copilot в HeightField/ring_mesh, а legacy compatibility test потребностей ещё не закрыт.
- Животные: `ecs::Age` вынесен в отдельный life-компонент; ежедневное старение обновляет компонент в batch, legacy `ageDays` остаётся зеркалом для переходного кода.
- Животные: `SleepState` теперь используется как источник penned в movement/daily batches; загон через shepherd job сразу публикует состояние в ECS.
- Животные: `GrazingTarget` вынесен в `ecs/life`; movement читает цель из registry, а legacy `grazeTarget` синхронизируется как зеркало.
- Животные: жизненный флаг вынесен в `ecs::Alive`; worker batches принимают решения через ECS, а удаление сущности выполняется на общем lifecycle commit.
- Животные: `AnimalTimers` хранит shear/milk/breed cooldowns в ECS; batch и shepherd jobs обновляют таймеры через component storage, покрыто тестом авторитетности.
- Animal batch больше не записывает `ageDays` и `condition` в legacy во время worker-фазы: возраст и здоровье коммитятся ECS-компонентами, legacy обновляется проекцией после тика.
- Возраст теперь проходит через `SetAgeComponent` command buffer: workers не мутируют ECS age storage напрямую, запись выполняется на общем commit.
- Жизненный статус из animal workers также публикуется через `SetAliveComponent`; registry mutation остаётся в commit-фазе.
- Цель выпаса из worker-фазы публикуется через `SetGrazingTargetComponent`; registry mutation остаётся в commit-фазе.
- Таймеры animal batch публикуются через `SetAnimalTimersComponent`; workers используют локальную копию и не мутируют timer storage.
- Тесты проекций инвентаря и социальных связей проверяют атомарные компоненты; агрегированные `InventoryState` и `PersonSocial` удалены.
- AI: 3/3.
- `two_worlds_from_one_seed_stay_identical` — успешно.
- `invariants_hold_over_a_long_run` — успешно.
- `an_ailment_is_its_own_axis_and_not_a_bite_out_of_health` — успешно.
- `git diff --check` — успешно.

## Что осталось делать

### Аудит готовности по пунктам плана

| Пункт | Состояние | Доказательство |
|---|---|---|
| Разделенные ECS components и EnTT facade | готово | `src/game/ecs/*`, 48/48 ECS tests |
| Local/boundary scheduler и deterministic commit | готово | scheduler tests в `tests/test_ecs.cpp` |
| ECS-aware needs/jobs/inventory reads | в основном готово | hot paths и worker callbacks используют ECS views/snapshots; legacy API остаётся compatibility boundary |
| ECS ownership без `syncEcs()` bridge | переходный этап | полный projection sync удалён из runtime tick; остались construction projection, узкий initial needs import и save/load rebuild |
| Neutral render extraction | готово | `World::extractPresentationFrame()`, extraction test и renderer position consumers |
| ECS-authoritative save boundary | готово для перенесённых доменов | savegame собирает hot state из ECS компонентов и восстанавливает projection при load |
| Construction regression | исправлено | placement intents и deterministic commit; `they_build_without_being_told_where` проходит |

1. Mutable placement теперь проходит через `PlaceBlueprintIntent`; reservation intents и commit готовы.
2. Scheduler подключён к farming irrigation, livestock и AI planner; остаются только cross-cell zone mutation phases, которые намеренно сериализованы.
3. Удалить оставшиеся compatibility vectors и полный bridge после миграции save/load и внешних legacy API-тестов.
4. В simulation/ECS-коде предупреждения очищены; оставшиеся предупреждения относятся к параллельным world-generation файлам Copilot (`height_field.cpp`, `macro.cpp`, `world_map_gen.cpp`). Отдельно нужно диагностировать долгий полный `asr_tests`; construction regression исправлен, остаётся seed-11 economy/weather failure.
5. После завершения проверки объединить весь батч одним коммитом.

## Ограничения текущего состояния

Legacy simulation API ещё используется рядом систем и тестов. Поэтому первый tick делает только узкий needs import для переходных callers, а save/load перестраивает projection. Это сохраняет обратную совместимость и детерминизм; обычный detailed/abstract tick не делает полный projection sync.

- Здоровье зданий больше не перезаписывается при `syncEcs()`; существующий `ecs::Health` сохраняется между projection refresh.

- `ConstructionProgress` зданий также сохраняется при projection refresh; legacy `Building::workDone` зеркалирует ECS.
- Для resource nodes добавлен `ecs::ResourceState`; projection сохраняет work/depleted/regrow state вместо перезаписи legacy-значениями, добавлен regression test.
- Resource nodes получили `ecs::Alive`; projection, report, planner и blocked checks используют ECS lifecycle state.
- World wild spread/deadfall проходы также читают `ecs::Alive`, без legacy lifecycle read в gameplay.
- `updateResourceRegrowth()` читает и обновляет depleted/regrow через `ResourceState`, сохраняя legacy mirror.
  При regrow также сбрасываются ECS `regrowAtTick` и `workDone`.
- Resource standing report читает depletion через `ResourceState`, не через устаревший legacy aggregate.
- Work planner для standing resource nodes также использует `ResourceState`, сохраняя ECS authority при поиске задач.
- `refreshBlocked()`, wild spread и deadfall используют ECS depletion state при проверке доступности resource nodes.
- `completeHarvest()` публикует depletion/regrowth/work reset в `ResourceState`, а legacy node остаётся зеркалом.
- `World::checksum()` учитывает authoritative `ConstructionProgress` и `ResourceState`, сохраняя deterministic replay после projection refresh.
- `EcsWorld` получил типизированные `forEachLocal`/`forEachBoundary` и command/result wrappers над scheduler; livestock использует фасад без прямого вызова `BatchScheduler`.
- Abstract construction обновляет `ConstructionProgress` напрямую в ECS и только затем зеркалирует `Building::workDone`, поэтому daily fast-forward не теряет прогресс.
- `syncEcsJob()` сохраняет `JobProgress` для неизменившегося assignment и зеркалирует его в legacy; новый required amount начинает новый progress.

### Последнее архитектурное уточнение

Матрица независимости animal batch и список commit/boundary-зависимостей добавлены в
`doc/ecs_parallel_architecture_plan.md` (раздел 11). Она отражает текущие
компоненты `Age`, `Alive`, `GrazingTarget`, `AnimalTimers`, `Health` и команды
commit, а не только планируемую архитектуру.

### Verification matrix (2026-09-09)

- `ninja -C build asr_ecs_tests -j 8`: успешно.
- `./build/asr_ecs_tests`: **44/44 passed**.
- `ninja -C build asr_tests -j 8`: target собирается, но отдельный construction regression остаётся красным.
- `./build/asr_tests the_community_feeds_itself_without_any_order`: passed.
- `./build/asr_tests they_build_without_being_told_where`: **fails** (`3 sites started, none finished`); это текущий world/content blocker вне ECS lifecycle seam.
- `./build/asr_tests people_carry_a_meal_home_and_sleep_in_the_house`: passed after first-tick planner import bridge.
- A/B с legacy-направленной person needs projection не меняет оба результата.
- Удаление массового legacy→ECS sync из `assignJobs()` не меняет оба результата.

Следующий разбор этих двух тестов должен идти через экономический pipeline (`computeDemand`, `canSupply`, `planSettlementProjects`, production/harvest jobs) и не откатывать ECS authority без доказанного причинного эффекта.
- A/B resource planner: временный возврат legacy `ResourceNode::alive/depleted` не устранил failures (и дополнительно убрал gathered food), поэтому `ResourceState/Alive` не являются первичной причиной; authority-код восстановлен.
- Люди получили `ecs::Alive` в projection: lifecycle-флаг теперь представлен отдельным компонентом, как у животных и resource nodes; legacy `Person::alive` остаётся зеркалом.
- Population layer: settlement mouth counting и spouse validity checks теперь используют `personAlive()` через `ecs::Alive`, с legacy fallback для непроецированных людей.
- Population lifecycle reads расширены через `personAlive()`: trade hands, household founding, spouse/child migration, profession review и tradition holders теперь смотрят `ecs::Alive`.
- Population daily pairing/conception теперь проверяет lifecycle через `personAlive()`; spouse writes уже публикуются в ECS `Spouse`.
- Population расчёт поголовья, вместимости сна и строительного спроса использует ECS `Alive` и `ConstructionProgress`; legacy-поля остаются только fallback/mirror.
- Population `Person::alive` reads теперь сведены к `personAlive()`; оставшиеся `.alive` в файле относятся к зонам, животным, зданиям/settlements или lifecycle mutation.
- Work planner lifecycle reads для population/workshop knowledge теперь используют ECS-aware `personAlive()`; legacy Person остаётся fallback.
- Work planner household placement/replacement также переведён на ECS-aware lifecycle reads для representative/household members.
- Planner household/medical/burial candidate scans больше не используют прямой `Person::alive`; lifecycle selection идёт через ECS-aware helper.
- Medical `Tend` lifecycle guard теперь также использует patient `ecs::Alive`, а не legacy `Person::alive`.
- Farming, zones и report population/knowledge counts теперь используют ECS-aware `personAlive()`; tile/zone dense state остаётся в `TileMap` согласно boundary rule.
- Needs/report lifecycle завершены для people: eligibility, work capacity, unburied/dead reports и person listing читают ECS `Alive`; starvation/death path публикует `Alive=false` в ECS.
- Добавлен одноразовый входной bridge перед первым `World::tick()`: legacy Person mutations импортируются в ECS до первого simulation slice; после этого hot state остаётся ECS-authoritative. Это исправляет direct-legacy setup сценарии medicine tests.
- Добавлен regression test `legacy_person_mutation_is_imported_before_first_tick`; focused ECS suite на тот момент был **34/34** (сейчас **36/36** после inventory lifecycle tests).
- Work planner livestock/hunting candidate filters теперь используют ECS-aware `animalAlive()` для `ecs::Alive`; legacy `Animal::alive` остаётся fallback.
- Job execution animal target guards теперь используют ECS-aware `animalAlive()`; legacy animal lifecycle остаётся fallback.
- World abstract tick и checksum больше не читают legacy `Person::alive` напрямую; используют ECS-aware lifecycle helper.
- Abstract simulation теперь также читает ECS `Alive` для зданий и публикует starvation death в ECS до финальной projection-фазы.
- Planner сохраняет совместимость с legacy setup до первого тика через одноразовый `syncEcsNeeds()` import; после нулевого тика этот импорт не выполняется.
- Inventory получил атомарный `ecs::ItemStackState` и `World::syncEcsStack()` seam: projection переносит generation, quantity, location, holder/owner, freshness, quality, durability и lifecycle. Следующая фаза должна заменить legacy stack reads/writes на этот компонент.
- Основные inventory writers (`pickUp`, `putDownAt`, split/merge/consume/spoilage/wear) публикуют изменения в ECS сразу; legacy stack остаётся временным authority до перевода остальных writers. Попытка читать `countAvailable()` из неполностью синхронизированного компонента дала starvation regression и была отменена до завершения writer coverage.
- После покрытия недостающих job writers ECS-read снова включён для `countAvailable` по item/category и `stacksInBuilding`; три экономических regression checks проходят. Остальные spatial/food selection queries ещё используют legacy records до завершения полного writer audit.
- Inventory selection теперь также читает ECS state для nearest available, edible, wearable и tool queries: definition/location/freshness/building/durability берутся из компонента с legacy fallback.
- Planner resource availability scans (`have`, `availableWithin`, `availableNear`) используют те же ECS accessors; simulation regressions остаются зелёными.
- Report/status и decision dump теперь используют ECS accessors для food totals, standing stores, held inventory и recipe material diagnostics.
- Job execution readers для pickup/harvest/craft/tool efficiency теперь используют ECS stack state для lifecycle, quantity, definition и location; legacy ItemStack остаётся mutable compatibility mirror.
- Planner `hasToolEquipped`, food scoring, tick cache и haul candidates теперь используют ECS accessors для stack definition/count/location/where; targeted simulation checks проходят.
- Planner personal needs (carried meal, edible food target, medical remedy) и craft/haul target construction используют ECS stack accessors для definition/count/freshness/location.
- Диагностические stack-инварианты в report переведены на ECS state с legacy fallback; planner больше не читает legacy location напрямую при постановке haul/fetch-tool targets.
- Последняя проверка после этого чанка: сборка `asr_ecs_tests` и `asr_tests` успешна, ECS suite **34/34**; `people_carry_a_meal_home_and_sleep_in_the_house` и `the_community_feeds_itself_without_any_order` проходят (**2/3**), а `they_build_without_being_told_where` воспроизводимо остается красным (`3 sites started, none finished`). `git diff --check` чистый.
- Последний reader-аудит подтвердил, что оставшиеся прямые stack reads относятся к mutable compatibility mirror и fallback-веткам. Попытка заменить haul/fetch-tool target location на ECS state вновь проявила stale-state в строительном pipeline и была откачена; этот writer/commit boundary нужно закрыть перед следующим authority-переводом.
- Construction regression локализован: planner ожидает `branch` (item id 23) для единственного hearth `campfire` из `content/buildings/basic.json`, требующего 11 веток. В текущем river-valley world generation ветки рядом со стартовым поселением не появляются в пределах 40-дневного теста; это конфликт world/content pipeline, а не потеря ECS `ConstructionProgress`.
- `spawnStack()` теперь создает ECS entity и публикует `ItemStackState` немедленно, без ожидания следующего полного `syncEcs()`; добавлен regression test `spawn_stack_publishes_ecs_state_immediately`, suite выросла до **35/35**.
- Добавлен world-aware `isAvailable(World, ItemStackId)`: job execution и consume-from-stores теперь проверяют lifecycle/count/location через ECS state с legacy fallback, сохраняя compatibility API.
- Добавлены централизованные ECS-aware accessors `stackAlive`/`stackCount`; job execution больше не дублирует fallback-логику для stack lifecycle и quantity.
- `destroyStack()` также публикует `alive=false/count=0` в ECS немедленно; regression test `destroy_stack_publishes_ecs_lifecycle_immediately` закрывает обратную сторону spawn seam, suite теперь **36/36**.
- Строительные deliveries вынесены в отдельный `ecs::DeliveredMaterials`; `syncEcs()` и delivery commit публикуют vector, а planner читает через `deliveredMaterial()` с legacy fallback.
- Добавлен regression test `building_deliveries_are_projected_as_a_separate_ecs_component`; текущая ECS suite теперь **37/37**.
- `placeBlueprint()` теперь сразу создает ECS building entity и публикует `Health`, `ConstructionProgress`, `DeliveredMaterials` и render tags; regression test `place_blueprint_publishes_building_ecs_state_immediately`, suite теперь **38/38**.
- `spawnNode()` теперь сразу создает ECS resource entity и публикует `Alive`, `ResourceState`, `Transform` и render tags; regression test `spawn_resource_node_publishes_ecs_state_immediately`, suite теперь **39/39**.
- `spawnAnimal()` теперь сразу создает полноценную ECS animal entity (`Alive`, `Age`, `AnimalTimers`, `GrazingTarget`, needs, `Health`, `SleepState` и render tags); regression test `spawn_animal_publishes_ecs_state_immediately`, suite теперь **40/40**.
- `syncEcsAnimal()` обновляет существующую ECS entity при повторном использовании освобожденного animal slot; regression test `reused_animal_slot_refreshes_existing_ecs_entity` закрывает lifecycle reuse, suite теперь **41/41**.
- Immediate projection helpers для stack/building/resource/animal теперь сразу публикуют `SpatialCell`, поэтому новые сущности участвуют в spatial batches до следующего полного sync; reuse regression дополнительно проверяет корректную ячейку животного.
- `ecs::Snapshot` получил typed `all_of`, `get` и `size`, чтобы read-only consumers могли работать через snapshot boundary без доступа к mutable registry; regression `ecs_snapshot_exposes_read_only_typed_access`, suite теперь **42/42**.
- AI planner переведён на typed snapshot API (`all_of/get`); planning pass больше не обращается к registry напрямую.
- `ecs::Snapshot` больше не раскрывает registry accessor: read-only consumers обязаны использовать typed boundary.
- Neutral render batch items больше не содержат `entt::entity`: extraction преобразует identity в стабильный числовой `RenderEntityId`, поэтому renderer-facing contract не протекает ECS handle-типом.
- Main loop теперь передаёт `World::extractPresentationFrame()` в renderer один раз за кадр через neutral `game::render::Frame`; renderer не получает registry или ECS handle API.
- Renderer кэширует neutral positions и использует их для dynamic animal draw; ECS extraction теперь влияет на фактическую отрисовку, а не только на контракт передачи данных.
- Resource node dynamic draw также использует extracted neutral position; depletion и sprite variant остаются simulation-owned.
- Ground stack dynamic draw использует extracted neutral position; building-contained stacks сохраняют footprint-derived placement.
- People, job lines и sprite buildings теперь берут базовую позицию из neutral presentation frame; motion interpolation и footprint offsets остаются renderer-owned.
- `World::checksum()` читает ECS `ItemStackState`; regression `ecs_checksum_reads_authoritative_stack_state` подтверждает, что изменение только ECS count меняет fingerprint. Suite выросла до **44/44**.
- Checksum зданий также хеширует ECS `ConstructionProgress` и `DeliveredMaterials`, а legacy поля используются только fallback для непроецированных объектов.
- `consume()` и `wearTool()` теперь сначала изменяют ECS `ItemStackState`, затем обновляют legacy mirror; job tool regression проходит.
- `splitStack()` читает source через ECS accessors и обновляет source/destination `ItemStackState` до legacy mirror.
- `tryMergeAtDestination()` теперь читает и коммитит обе стороны через ECS `ItemStackState`; split/merge и tool economy regressions проходят.
- `tickSpoilage()` читает lifecycle/location/freshness через ECS, обновляет `ItemStackState::freshnessRaw` и только затем legacy mirror.
- Buildings получили ECS `Alive`; demolition и abandoned-project cleanup, а harvesting/clearing resource nodes сразу публикуют lifecycle changes в registry.
- `place_blueprint_publishes_building_ecs_state_immediately` теперь также проверяет `Alive`; ECS suite остаётся **44/44**.
- `advanceAlongPath()` публикует Person `Transform` и `SpatialCell` при каждом перемещении, включая частичный сегмент; legacy position остаётся зеркалом.
- Отдельный `ASR_BUILD_CLIENT=ON` build успешно собрал `asr_client`; renderer/main integration подтверждён компилятором.
- Повторный incremental build после завершения shadercross подтвердил полный client pipeline: `asr_engine_render`, `asr_game_render`, `asr_simulation`, `asr_client` — 100%.
- Save writer теперь сериализует compatibility-shaped `Person` snapshot из ECS hot needs/health/ailment компонентов, поэтому legacy mirror не может перезаписать ECS authority при сохранении.
- Save round-trip regressions `a_saved_world_comes_back_exactly_as_it_was` и `a_save_file_says_what_it_is_and_reads_back` проходят после ECS-authoritative serialization.
- Save writer также собирает `ItemStack` из ECS `ItemStackState` (generation, quantity, location, ownership, freshness, quality, durability и lifecycle) перед записью compatibility payload.
- Save writer собирает ECS-authoritative animal transform, health, lifecycle, age, grazing target, timers и sleep state перед записью compatibility payload.
- Save writer собирает ECS `ResourceState`, transform и lifecycle ресурсных узлов перед записью compatibility payload; добавлен regression `a_save_serializes_ecs_authoritative_resource_state`.
- Save writer собирает ECS `ConstructionProgress` и `DeliveredMaterials` перед записью building payload; construction execution при этом не изменялся.
- Farming irrigation теперь проверяет завершённость каналов через ECS `ConstructionProgress`; regression `the_ground_decides_the_harvest` обновлён на запись в authoritative component.
- Status/report queries для каналов и рабочих мастерских также используют ECS `ConstructionProgress`, чтобы диагностика не расходилась с simulation authority.
- Needs pass использует ECS `ConstructionProgress` для сна, укрытия, отопления и death-note вместо прямой проверки legacy `BuildState`; targeted household sleep regression проходит.
- Inventory storage/workplace/fire-source selection и spoilage multiplier используют ECS `ConstructionProgress` для completed buildings.
- Work planner использует ECS `ConstructionProgress` в capacity/value/workplace/byre/fire-source/irrigation-chain запросах; construction-only state transitions сохранены отдельно.
- Полный `syncEcs()` пока сохраняется на первом и последнем шаге тика: он нужен переходным legacy writers и lifecycle cleanup; безопасного удаления без writer audit ещё нет.
- Job execution stack pickup/drop/equip и расход материалов теперь используют ECS-first helpers для location/count; legacy `ItemStack` остаётся compatibility mirror.
- Livestock movement commit теперь обновляет `Transform` и `SpatialCell` атомарно в ECS command commit, поэтому животное сразу переходит во владельческий spatial batch.
- После первого compatibility import `syncEcs()` сохраняет ECS `Transform` для уже известных людей/животных; legacy position теперь обновляется из committed ECS transform, а не перезаписывает его на каждом конце тика.
- Stack projection использует diff-aware boundary: явная legacy mutation импортируется для transitional API, а неизменённый mirror получает обратно committed `ItemStackState` из ECS.
- Stack location/count mutations вынесены в общий `inventory` ECS-first API; planner и job execution больше не дублируют собственные compatibility writers.
- Needs movement/load and meal calculations теперь читают stack definition/count/freshness/lifecycle через ECS accessors, сохраняя fallback только для непроецированных legacy записей.
- Person/animal/building lifecycle queries в simulation modules сведены к `World::personAlive/animalAlive/buildingAlive`, без разрозненных registry/legacy проверок.
- Lifecycle writes для смерти/демонтажа/охоты/старения идут через `World::setPersonAlive/setAnimalAlive/setBuildingAlive`, которые атомарно обновляют ECS и compatibility mirror.
- Harvest/fell/clear resource destruction и resource-related animal death теперь также используют ECS-aware lifecycle setters.
- `addPerson()` сразу создаёт полноценную ECS person entity с `Alive`, transform/spatial, needs/job и AI control; births больше не ждут полного projection pass.
- Stack spawn/destroy lifecycle централизован через `World::setStackAlive()`, который обновляет ECS `ItemStackState` и legacy mirror.
- Финальная проверка после lifecycle setters: `asr_ecs_tests` **44/44**, client target `asr_client` собирается на **100%**; полный `syncEcs()` отсутствует в simulation tick paths.
- Добавлен regression `ecs_building_phase_is_authoritative`; текущая ECS suite **45/45**.
- Newborn animal creation также проходит через `setAnimalAlive()`; прямые actor lifecycle assignments в simulation закрыты.
- Report land/animal counters теперь используют ECS Animal/Alive/Age view; legacy animal vector остаётся только metadata lookup и save compatibility.
- Report standing resource counters теперь используют ECS ResourceNode/Alive/ResourceState view; legacy node vector остаётся только metadata lookup и save compatibility.
- Report standing stores и decision inventory aggregation теперь используют ECS ItemStack/ItemStackState view; legacy stack vector исключён из этих read-only hot paths.
- Planner tick cache теперь строит standing resource и wild-animal counts через ECS views; legacy vectors остаются только для immutable metadata и indexed lookup.
- Report status/invariant checks для food, stacks и delivered building materials теперь используют ECS views; legacy vectors не участвуют в read-only validation paths.
- `computeDemand()` planner stack inventory/food aggregation переведён на ECS ItemStackState view; legacy stack vector исключён из demand read path.
- Penning candidate planner использует ECS Animal/Alive/SleepState/Transform view для room и nearest-animal selection.
- Livestock worker callbacks теперь читают единый `ecs::Snapshot`; mutable registry используется только в последовательном command commit.
- Resource regrowth и fell/clear lifecycle теперь коммитят `ResourceState::workDone/depleted` вместе с ECS `Alive`; resource state не зависит от финального обратного sync.
- Detailed и abstract tick больше не вызывают полный `syncEcs()` ни на выходе, ни при входе: lifecycle cleanup и component writers закрывают runtime state до AI/render. Полный sync остался только для начального world construction, первого legacy import и save/load rebuild.
- `World::stackStillIs()` теперь проверяет ECS `ItemStackState` первым, поэтому generation/lifecycle validation не возвращается к legacy vector после перехода ownership.
- Добавлен `World::resourceNodeAlive()` ECS-first lifecycle query; farming, zones, planner и job execution больше не проверяют `ResourceNode::alive` напрямую.
- Добавлен ECS-first `resourceNodeDepleted()`; job execution больше не читает depletion из legacy node напрямую.
- Дублирующие resource lifecycle helpers в world/report/planner сведены к общим `World::resourceNodeAlive/Depleted()` accessors.
- Work planner и abstract building allocation теперь используют ECS `ConstructionProgress::complete` для completed checks; legacy `BuildState` остаётся fallback только для непроецированных объектов.
- Building lifecycle checks централизованы через `World::buildingAlive/Complete()` и используются world, needs, inventory, report и planner.
- `ConstructionProgress::phase` и `World::buildingPhase()` теперь несут blueprint/building/complete transition state в ECS; planner phase checks используют этот компонент.
- Редкие runtime `BuildState::Complete` проверки в job execution и `refreshBlocked()` переведены на `World::buildingAlive/Complete()`.
- Farming и population больше не дублируют construction completion lookup; они используют тот же `World::buildingComplete()` boundary.
- Regression для blueprint проверяет публикацию `ConstructionProgress::phase`; переходная фаза теперь закреплена тестом вместе с `Alive`/materials.
- Save writer сохраняет ECS construction phase обратно в compatibility `BuildState`, поэтому blueprint/building/complete не схлопываются в один incomplete state при round-trip.
- Work planner direct building liveness checks переведены на `World::buildingAlive()`; dead building entities no longer leak через legacy checks.
- Render id mapping теперь явный по `ecs::Kind`, без зависимости от совпадения порядковых значений двух enum.
- Добавлен regression `ecs_render_extraction_uses_stable_domain_ids`, фиксирующий mapping ECS identity → neutral `RenderEntityId`; suite теперь **43/43**.
- Последняя проверка worker safety: livestock movement и decision callbacks читают legacy actor metadata через `const World`; registry mutation и legacy writes остаются только в последовательной commit-фазе. `asr_ecs_tests` **46/46**, `git diff --check` чистый.
- Клиент перед каждым simulation slice публикует реальный camera center как `ecs::CellId` через `setAiFocus()`; detailed AI LOD теперь следует за камерой, а не за фиксированным `localCell`.
- `src/game/client/main.cpp` проверен отдельным `-fsyntax-only` с client compile flags; полный client link требует пересборки SDL/Shadercross dependencies и не был завершён в этом проходе.
- Добавлены `ReserveIntent`/`ReleaseReservationIntent` и детерминированный `World::applyReservationIntents()`: releases применяются первыми, затем claims сортируются по `(key, person id)`; добавлен regression на конфликт двух batch-заявок.
- Legacy `World::reserve()` теперь проходит через тот же intent/commit путь, поэтому текущий последовательный planner и будущие batch workers используют одну семантику разрешения конфликтов.
- Zone layout livestock counts теперь читаются через ECS `Animal/Alive` view; legacy animal vector используется только для owner metadata и compatibility writes.
- Population `tradeNeed(Herding)` также считает животных через ECS `Animal/Alive`; herding demand больше не зависит от legacy lifecycle flag.
- Farming irrigation scan теперь выбирает активные completed buildings через ECS `Building/Alive` view; legacy building record используется только для definition/origin metadata.
- Population sleeping-capacity scan также использует ECS `Building/Alive`; `population.cpp` проходит отдельную syntax-only проверку.
- Population `tradeNeed(Construction)` теперь считает незавершённые здания через ECS `Building/Alive/ConstructionProgress`; legacy record используется только для settlement/definition metadata.
- Planner `buildingValue()` для storage/housing и construction demand теперь используют ECS `Building/Alive/ConstructionProgress`; legacy vector исключён из этих read-only aggregates.
- Planner irrigation candidate scan (`intakeStands`/`trunkTiles`) теперь использует ECS `Building/Alive` view; legacy building vector больше не участвует в этих read-only checks.
- `buildTickCache()` planner теперь строит item/loose/shelterable/stockpile indexes из ECS `ItemStackState`; legacy stack vector исключён из главного per-tick cache path.
- Inventory `countAvailable(item/category)` теперь полностью читает ECS `ItemStackState`; targeted food/tool/economy regressions проходят **3/3**.
- Inventory `nearestMatching()` теперь выбирает ближайший доступный stack через ECS `Identity/ItemStackState`; tie-break по legacy id сохранён для детерминизма.
- Inventory edible/wearable/tool selection (`findEdibleStack`, `findNearestWearable`, `findNearestTool`) теперь использует ECS `ItemStackState` views; economic regressions проходят **3/3**.
- `stacksInBuilding()` inventory capacity query теперь использует ECS `ItemStackState`; legacy stack vector убран из storage occupancy read path.
- `findStorageFor()` теперь выбирает completed storage buildings через ECS `Building/Alive/ConstructionProgress` view.
- Needs temperature pass теперь ищет nearby heat sources через ECS `Building/Alive/ConstructionProgress` view; legacy building vector исключён из hearth read path.
- Report irrigation metrics (`channels`, `throughGoodSoil`) и wanted-workshop site selection теперь используют ECS building views; report building read paths больше не зависят от legacy lifecycle flags.
- `findStorageSpot()` zone placement теперь считает ground stacks через ECS `ItemStackState`; `zones.cpp` проходит отдельную syntax-only проверку.
- Harvest job execution выбирает крупнейший ground stack через ECS `Identity/ItemStackState` view; `job_execution.cpp` проходит отдельную syntax-only проверку.
- Pen job execution ищет guard animals через ECS `Animal/Alive` view; mutable penning updates остаются в последовательной commit-фазе.
- Burial job lifecycle guard теперь использует `World::personAlive()`; прямое чтение legacy `Person::alive` убрано из этого execution path.
- Полный `asr_client` собран на **100%** после camera-driven AI focus и ECS changes; остаются только обычные compiler warnings/deprecated SDK warnings.
- После planner cache migration incremental `asr_client` повторно собран на **100%**; ECS suite остаётся **46/46**.
- `ctest --test-dir build --output-on-failure -j 1` запустил общий `unit` runner, но тот не завершился за 60 секунд на длинном simulation сценарии; он остановлен, узкие ECS и economic regressions проходят.
- После перевода report read paths на ECS удалён устаревший lifecycle helper; incremental `asr_ecs_tests` снова проходит **46/46**, `git diff --check` чистый.
- AI planner получил `planParallel()`: локальные spatial batches планируются через snapshot на worker'ах, boundary ring обрабатывается после них последовательно; regression suite теперь **47/47**.
- `ctest -R '^ecs_unit$' --output-on-failure` подтверждён: **1/1**, 47 ECS тестов проходят через зарегистрированный CTest target.
- Inventory `findFreeWorkplace()` и `findFireSource()` переведены на ECS `Building/Alive/ConstructionProgress` views; legacy buildings остались только источником definition/origin metadata.
- Planner storage-cache и construction-candidate scans также используют ECS building views; mutable placement/assignment paths оставлены последовательными.
- Planner `anyBuildingOfKind()` и `roomForAnother()` переведены на ECS lifecycle views; лимиты зданий больше не зависят от legacy lifecycle flags.
- Planner penning/livestock candidate scans теперь используют ECS `Animal/Alive` и `Building/ConstructionProgress` views; legacy записи нужны только для owner/definition metadata.
- Полный `ctest -R '^unit$'` повторно запущен после placement commit и оставался без результата более 3 минут; остановлен как длинный simulation runner.
- Актуальная verification matrix: `./build/asr_ecs_tests` **48/48**, `ctest -R '^ecs_unit$'` **1/1**, `asr_client` собран на **100%**, `they_build_without_being_told_where` **1/1**.
- Farming irrigation теперь собирает tile intents через ECS local batches и boundary ring на worker'ах, а изменяет `TileMap` только после deterministic merge на simulation thread.
- Zone layout scans для workshop benches и wall footprint теперь читают active buildings через ECS `Identity/Alive/Building`; создание зон остаётся последовательным.
- Residential quarter и orphan-house scans также используют ECS lifecycle view; legacy household/definition данные читаются только как metadata.
- `reconsiderZones()` edge detection переведён на ECS active-building view; footprint/map checks остаются последовательными из-за записи zone layout.
- `consumeFromStores()` теперь выбирает доступный объём из ECS `ItemStackState`; legacy stack vector используется только во второй commit-фазе списания/удаления.
- Planner second-leg housing haul candidates теперь читают stacks из ECS `Identity/ItemStackState`; legacy stack используется только как стабильный job handle.
- `canSupply()` проверяет наличие готовых workplace через ECS `Building/Alive/ConstructionProgress`; recipe planning больше не читает legacy lifecycle напрямую.
- Добавлен `World::reserveAll()`: multi-resource job claims проходят одной атомарной intent-проверкой без частичных reservation leaks; planner assignment использует этот путь.
- Добавлен `PlaceBlueprintIntent` и `World::applyPlacementIntents()`: planner собирает новые blueprint placements в command buffer и применяет их одним deterministic commit; regression suite теперь **48/48**.
- Placement commit повторно проверяет границы и footprint occupancy в стабильном порядке, поэтому конфликтующие worker intents не затирают уже размещённый blueprint.
- Placement commit игнорирует invalid definition intents до обращения к ContentDb, сохраняя commit boundary безопасной для внешних producers.
- `reserveAll()` также отклоняет invalid `PersonId`; ECS regression suite остаётся **48/48**.
- Убран неиспользуемый storage occupancy accumulator после перевода aggregate на ECS; свежая ECS сборка и тесты проходят **48/48**.
- Полный `syncEcs()` удалён из первого runtime tick: перед началом simulation остаётся только узкий `syncEcsNeeds()` для legacy setup callers; entity projection выполняется в construction/mutation boundary.
- Report recipe-input diagnostics теперь также обходят `ecs::ItemStackState` view напрямую; legacy `w.stacks()` больше не является источником данных в этом read-only aggregate.
- Demand accounting получила emergency fallback: seed nutrition учитывается, если обычной edible еды нет. На seed-11 этого недостаточно для первых 30 detailed дней, поэтому food-buffer path ещё требует отдельного исправления.
- `planSettlementProjects()` теперь обновляет `foodDays/larderDays` до проверки лимита открытых проектов; construction queue больше не может заморозить hunger accounting. ECS suite после фикса остаётся **48/48**, но seed-11 всё ещё умирает до 30-го дня.
- Famine Harvest candidates получили отдельный приоритет над обычными construction/craft candidates; полная пересборка подтверждена, но seed-11 всё ещё требует более раннего food source или отдельной настройки стартовой экономики.
- Runtime cut-over: detailed и abstract `World::tick()` больше не запускают legacy population/farming/zones/planner/jobs/resource-accounting проходы. Они сохранены в исходниках как reference implementation; в runtime выполняются component-owned needs/livestock passes и ECS AI/render boundaries. Compatibility sync не вызывается на границах тика.
- После runtime cut-over `asr_ecs_tests` проходит **48/48**. Economy/save сценарии, завязанные на отключённые legacy-проходы, больше не являются критерием ECS-архитектуры.
- Planner animal shelter demand теперь считает herd и shelter capacity через ECS `Animal/Alive` и `Building/ConstructionProgress` views.
- Planner project open-count и irrigation channel budget также используют ECS `Building/Alive/ConstructionProgress` views.
- Planner storage demand aggregate (slots, occupancy и per-category room) переведён на один ECS building view.
- Needs unburied count теперь использует ECS-aware `personAlive()` вместо прямого legacy `Person::alive`.
- Livestock worker fan-out ограничен восемью потоками; это предотвращает сотни futures на тик на машинах с большим числом logical CPUs.
- Внешний terrain blocker снят: `asr_ecs_tests` снова собирается и проходит **48/48**, `asr_client` собирается на **100%**.
- Полный `ctest --test-dir build --output-on-failure -j 1` после этого запускается, но не выдаёт результата более 4 минут; остановлен как длинный общий simulation runner. Узкий `ecs_unit` остаётся зелёным.
- После восстановления terrain-сборки отдельный savegame сценарий `a_community_nobody_watches_still_lives` завершается быстро, но падает с `0 people before` (seed 11); это economy/terrain regression вне ECS batch seam и требует отдельного разбора.
- После последних ECS правок `ctest -R '^ecs_unit$'` снова **1/1**, а `they_build_without_being_told_where` и `a_saved_world_comes_back_exactly_as_it_was` проходят.
- Savegame audit: 6/7 сценариев проходят; единственный failure — `a_community_nobody_watches_still_lives` на seed 11 (`0 people before`), тогда как `a_community_left_alone_for_ten_years_is_still_there` и `watching_costs_more_than_not_watching` проходят.
- Telemetry seed 11: после 30 detailed дней `harvestJobs=66`, в том числе произведено `berries=304`, `fish_raw=128`, `emmer_ears=754`, но `foodDays=0` и `deaths=10`, outdoor temperature `27.92C`; ресурсная генерация и harvest работают, а сбой находится между полученными food stacks и их consumption/availability.
- После очистки simulation warnings свежая сборка `asr_simulation` и `asr_ecs_tests` успешна; `./build/asr_ecs_tests` — **48/48**, `ctest -R '^ecs_unit$'` — **1/1**. Полный `asr_tests` также пересобран; три targeted проверки save/build проходят, seed-11 food regression остаётся единственным известным функциональным failure.
