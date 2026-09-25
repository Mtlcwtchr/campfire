# Комплексная архитектура ECS и параллельной симуляции

Этот документ задаёт целевую архитектуру следующего этапа. Текущий EnTT слой
остаётся переходным: `World` ещё хранит legacy-векторы, а `syncEcs()` строит из
них проекцию. Перед распараллеливанием нужно сделать ECS владельцем runtime
состояния и разделить данные по ответственности.

## Текущее состояние реализации

Первый фундаментальный срез уже находится в рабочем дереве: компоненты
разложены по пакетам `src/game/ecs`, `SpatialCell` добавляется к ECS-проекции,
а `BatchScheduler` строит стабильные local/boundary batches. `CommandBuffer`
задаёт запрет на прямую worker-мутацию registry. Livestock movement и daily
обработка уже используют batches с детерминированным commit; needs, jobs и
population читают независимые ECS-компоненты. AI вынесен в отдельный planner с
distance LOD, а ECS-side extraction формирует нейтральные render batches.
Legacy-векторы и `syncEcs()` остаются в исходниках как reference/serialization
boundary, но больше не участвуют в runtime tick. Неперенесённые legacy-проходы
намеренно исключены из `World::tick()` до появления их ECS-реализаций.

## 1. Главный принцип

Один кадр симуляции делится на фазы с явными барьерами:

```mermaid
flowchart LR
  A[Snapshot / spatial batches] --> B[Independent local systems]
  B --> C[Boundary and shared intents]
  C --> D[Deterministic commit]
  D --> E[AI planning at distance LOD]
  E --> F[Render extraction]
```

Потоки не пишут в один registry одновременно. Каждый worker получает read-only
snapshot ECS и собственный набор команд (`CommandBuffer`). Команды применяются
одним commit-проходом в фиксированном порядке. Это сохраняет детерминизм,
избавляет от гонок EnTT и позволяет менять scheduler без изменения систем.

## 2. Пространственное разбиение

Для симуляции используются квадратные spatial cells, например 32×32 или 64×64
тайла. Это внутреннее разбиение и не обязано совпадать с кольцами terrain.
Сущность имеет `SpatialCell` и попадает ровно в один owner batch. Кольца камеры
только выбирают приоритет и частоту обновления:

| Зона | Частота | Состав работы |
|---|---:|---|
| фокус камеры | каждый тик | движение, столкновения, needs, jobs |
| внутреннее кольцо | каждый тик или через тик | обычный AI и локальные системы |
| средние кольца | 2–8 тиков | агрегированные needs, jobs, path intent |
| дальние кольца | 8–32 тика | coarse AI с `elapsedTime`, производство и статистика |
| вне активного радиуса | по событию | только глобальные timers/рынок/погода |

Квадратные cells удобны для ownership и соседей. Scheduler сортирует их по
радиальным кольцам от focus. Радиальная маска поверх квадратных cells даёт
нужное кольцевое поведение без попытки заставить ECS хранить «кольца» как
тип сущности. Для дальних областей можно объединять несколько cells в coarse
regions, но это отдельный уровень агрегации.

## 3. Компоненты по пакетам

`ecs.hpp` больше не должен быть общим складом. Предлагаемая структура:

```text
src/game/ecs/
  registry.hpp
  entity.hpp
  spatial/components.hpp
  life/components.hpp
  needs/components.hpp
  jobs/components.hpp
  inventory/components.hpp
  social/components.hpp
  combat/components.hpp
  construction/components.hpp
  render/components.hpp
  commands.hpp
  snapshot.hpp
  scheduler.hpp
  systems/
```

Базовые компоненты должны быть маленькими и ортогональными:

```cpp
struct Position { core::WorldPos value; };
struct SpatialCell { CellId value; };
struct Velocity { core::Fixed dx, dy; };
struct Health { core::Fixed current, maximum; };
struct Hunger { core::Fixed value, rate; };
struct Thirst { core::Fixed value, rate; };
struct Fatigue { core::Fixed value, recoveryRate; };
struct SleepState { bool asleep; };
struct Job { JobKind kind; JobPhase phase; };
struct NeedsRequest { NeedKind kind; core::Fixed urgency; };
```

`Person`, `Animal`, `Building`, `Wall` и `ResourceNode` должны быть marker/tag
компонентами. Человек и животное получают одинаковые `Hunger`, `Thirst`,
`Fatigue`, `Health`, а видовые отличия выражаются конфигурацией и отдельными
компонентами (`HumanSocial`, `AnimalSpecies`). Здание получает `Health` и,
если применимо, `ConstructionProgress`; стена — тот же `Health` плюс
`WallMaterial`. Старый агрегат витальных полей уже удалён.

## 4. Что можно выполнять параллельно

Полностью независимы при immutable inputs:

- генерация шума, высот, биомов и material candidates по координате;
- локальный decay needs (`Hunger`, `Thirst`, `Fatigue`) без соседних эффектов;
- анимационное состояние и подготовка renderable данных;
- локальная статистика и производство, если ресурс заранее принадлежит cell;
- построение mesh/sprite batches из immutable snapshot;
- AI scoring кандидатов, если результатом является только command/intent.

Внутри одного owner batch такие системы могут идти worker pool-ом. GPU подходит
для регулярной per-cell/per-vertex математики и render extraction, но не для
изменения EnTT registry или принятия конфликтующих решений.

## 5. Что требует соседей или барьера

- движение между cells и collision resolution;
- pathfinding через границу ownership;
- reservations на кровати, рабочие места, склады и ресурсы;
- обмен предметами и inventory transfer;
- лечение, атаки, урон и разрушение зданий;
- рождение, смерть, создание/уничтожение entity;
- социальные связи и выбор пары;
- строительство, где несколько workers пишут в один объект.

Такие системы работают в два шага: worker создаёт intent, затем boundary/commit
сводит intents. Граничные сущности не нужно отдавать одному «главному потоку»
целиком: лучше отдельная boundary-фаза с повышенным приоритетом и ограниченным
набором конфликтующих данных. При конфликте используется стабильный порядок:
`system priority`, `CellId`, `EntityId`.

## 6. Обёртка над EnTT

Нужен не потокобезопасный `entt::registry`, а runtime слой:

```cpp
class EcsWorld {
public:
    template <typename... Required, typename Fn>
    void forEachLocal(CellId focus, int radius, size_t workers, Fn&& fn);
    template <typename... Required, typename Fn>
    void forEachBoundary(CellId focus, int radius, Fn&& fn);
    Registry& writeRegistry();
    Snapshot snapshot() const;
    BatchSchedule schedule(const SpatialIndex&, TickBudget) const;
    void apply(CommandBuffer&& commands);
};
```

`BatchView` фильтрует entity по `SpatialCell` и нужным компонентам. Worker не
получает mutable registry; он читает `Snapshot` и пишет только в свой
`CommandBuffer`. Registry mutation, создание/удаление entity и перенос между
cells происходят в commit. Это устраняет ложное ощущение, что обычный EnTT
`.view()` сам по себе безопасен для параллельного доступа.

## 7. AI как отдельный слой

AI не должен быть частью `World` и не должен напрямую менять компоненты. Слои:

```text
ECS snapshot -> perception -> planner -> intents -> ECS commit
```

`Perception` выдаёт ограниченное представление мира, `Planner` выбирает action,
`Executor` превращает его в команды jobs/movement/inventory. Для дальних зон
planner получает `elapsedTime`, агрегированные ресурсы и coarse facts. Он не
симулирует каждый промежуточный кадр: выполняется эквивалентный fast-forward с
ограничением максимального шага и последующим commit.

## 8. Render extraction

ECS не передаётся в renderer. Нужен ECS-side `RenderExtractionSystem` или
`PresentationBridge`, который читает ECS snapshot и формирует уже существующие
рендерные структуры (`SpriteQueue`, mesh cache input, overlay data). Renderer
получает эти структуры обычным API и ничего не знает о `entt::registry`,
компонентах и spatial batches. В текущем коде это пока обратная зависимость:
`Renderer::draw(const sim::World&)` сам обходит `World`; при миграции меняется
только этот upstream-контракт, а сами render passes остаются потребителями
готовых очередей.

Ввести отдельные компоненты в `ecs/render/components.hpp`:

```cpp
struct SpriteRender { core::DefId sprite; std::uint8_t layer; bool visible; };
struct MeshRender { core::DefId mesh; MaterialId material; bool visible; };
struct RenderBounds { core::Aabb bounds; };
struct RenderDirty {};
```

`RenderExtractSystem` читает snapshot, группирует entities по layer/material,
строит `SpriteBatch` и `MeshBatch`, после чего передаёт готовые массивы
рендереру через presentation API. Рендерер не видит registry. Система
extraction параллельна по cells; финальная сортировка и upload — короткая
последовательная фаза.

## 9. Порядок реализации

1. Разложить компоненты и registry по `src/game/ecs`, сохранив временные aliases.
2. Добавить `SpatialCell`, spatial index, snapshot и command buffers.
3. Перенести authoritative state людей, животных и зданий из legacy-векторов.
4. Реализовать scheduler: local batches → boundary intents → deterministic commit.
5. Переписать needs, movement, jobs, inventory и damage на component views.
6. Вынести AI в perception/planner/executor и добавить distance LOD.
7. Добавить `SpriteRender`/`MeshRender` и render extraction batches.
8. Перевести save/load на component serializers.
9. Удалить `syncEcs()`, legacy vectors и переходные lookup по индексам.
10. Добавить deterministic, race, spatial-boundary и performance tests.

На каждом шаге сохраняется старый рендер и формат сохранений, пока новая система
не проходит сравнительный deterministic replay.

## 10. Критерии готовности

- ни одна worker system не мутирует общий registry;
- один entity имеет одного spatial owner;
- boundary conflicts разрешаются детерминированно;
- AI дальних зон даёт тот же результат, что и bounded fast-forward;
- renderer получает только batches, не ECS;
- `syncEcs()` и legacy simulation vectors не вызываются runtime;
- replay одного seed/tick sequence даёт одинаковый hash при разном числе workers.

## 11. Фактическая матрица animal batch

После переноса animal state граница между локальной работой и commit выглядит так:

| Домен | Worker-фаза | Нужна boundary/commit-фаза |
|---|---|---|
| `Health`, `Age`, `Alive` | Чтение собственного entity, расчёт нового значения | `SetHealthComponent`, `SetAgeComponent`, `SetAliveComponent`; затем lifecycle cleanup |
| `SleepState`, `GrazingTarget`, `AnimalTimers` | Чтение локального состояния и вычисление решения | `SetGrazingTargetComponent`, `SetAnimalTimersComponent`; registry меняется централизованно |
| Grass | Локальная агрегация по tile внутри batch | Сведение `AdjustGrassIntent` по общим tile |
| Movement transform | Расчёт следующей позиции entity | Применение transform и обновление spatial ownership |
| Breeding | Проверка локального snapshot/perception | Birth intents после сведения результатов |
| Predation | Поиск prey по read-only perception | Death intents и освобождение entity после полного decision pass |
| Shepherd penning | Проверка ownership/дистанции и формирование решения | Обновление `SleepState`/`GrazingTarget` для затронутых животных |

Локальный batch не должен читать незакоммиченные результаты соседнего batch.
Любой конфликт по grass, birth/death, spatial transfer или reservation проходит
через deterministic commit либо boundary phase. Это позволяет распараллеливать
обычные entity-local вычисления и при этом не скрывать межклеточные зависимости
за небезопасной записью в legacy-векторы.

### Dense tile state boundary

Farming and zone tile fields (`fertility`, `tilled`, `crop`, `cropGrowth`,
`irrigated`, zone mask) remain in `TileMap` as dense spatial data. They are not
EnTT entities: converting every tile into an ECS entity would destroy locality,
expand registry overhead, and make the existing cell/chunk ownership less
useful. Farming systems process disjoint tile ranges in spatial batches and
publish actor/building/resource effects through ECS commands where needed.

ECS authority therefore covers people, animals, buildings, resource nodes,
items, and render-facing actors; `TileMap` remains the authoritative backing
store for the map itself.

## 12. Текущий статус перехода

Компоненты, snapshot, local/boundary scheduler, deterministic reservation и
placement commits уже работают в ECS runtime. `World::tick()` запускает только
перенесённые component-owned проходы; population/farming/zones/planner/jobs и
старый aggregate fast-forward оставлены как reference code и исключены из
исполнения. `syncEcs()` используется только на construction/mutation boundary и
при save/load rebuild. Полное удаление lookup-векторов остаётся отдельной
очисткой публичного legacy API, не влияющей на ECS runtime authority.
