# Campfire и внешний профайлер — 2026-09-26

## Решение пользователя

Движок называется **Campfire / «Костёр»**. Профилирование — отдельная внешняя тулза,
без SDK, RAII-зон, printf и compiler hooks в исходниках движка.

## Что сделано

- Полностью удалена ранее начатая интеграция Tracy: profiler.hpp/.cpp, тесты встроенного
  профайлера, зависимости CMake, поля pass/pipeline, зоны и учёт аллокаций в Device.
- Проверен `src/`: новых `ASR_PROFILE_*`, `profile::`, `profileName`, TracyClient нет.
  Прежние FrameTiming/FPS/headless/bench оставлены как существующая функциональность;
  внешняя тулза от них не зависит.
- CMake project: `campfire`. Исполняемые файлы: `campfire_client`, `campfire_editor`.
- Окна клиента, редактора, FPS-заголовок и стартовый экран переименованы в Campfire.
- Старые `asr_client`/`asr_editor` — aliases для совместимости с CLion и скриптами;
  внутренние asr_* цели/библиотеки, сериализация, ресурсы и пути не переименовывались.
- Исходный UE-проект-донор **AncientSettlement** и его provenance сохранены.

## Отдельная тулза

`tools/campfire_profiler.py`, Python standard library + установленный Xcode/Instruments.
Ни один файл тулзы не линкуется с движком.

- `processes`: список PID Campfire и совместимых старых имён.
- `record --pid PID`: подключиться к работающему процессу.
- `record --launch EXECUTABLE -- ARGS`: запуск под Instruments.
- `--mode cpu|memory|gpu|system|counters`: Time Profiler, Allocations,
  Metal System Trace, System Trace, CPU Counters соответственно.
- `--seconds 1..300`: ограниченная запись в новый каталог, без перезаписи старых.
- `--open`: открыть native trace в Instruments.
- CPU-запись автоматически получает локальные `report.html` и `summary.json`;
  таймлайн, call tree/flame graph, память и GPU подробно смотрятся в Instruments.
- Ошибки доступа/пустые CPU-записи не выдаются за нулевые успешные замеры.
- Никакой статической рефлексии для измерения времени не требуется. Имена берутся
  из debug symbols/стеков; короткие/inlined функции могут быть неразличимы в семплах.

### Аллокации и подпись

Allocations на обычном бинарнике в этой среде сообщил `Failed to attach to target`.
Проверен вариант `--debug-copy`: тулза копирует executable внутрь capture directory
и выдаёт **только копии** ad-hoc подпись с `com.apple.security.get-task-allow`.
Исходники и основной бинарник не меняются, SIP не отключается.

Instruments иногда завершает запущенный им процесс по лимиту и возвращает код 54,
хотя trace сохранён. Тулза отмечает такой результат как `recorded_with_warning` и
сохраняет код/предупреждение. Это не то же самое, что нормальный выход приложения.
Для продолжения работы существующего клиента используйте `--pid`.
Данные Allocations не обязаны экспортироваться в xctrace XML: native .trace остаётся
основным форматом memory/GPU-профиля.

## Проверки

- Client и Editor собраны в `cmake-build-relwithdebinfo`, `nice`, `-j 2`.
- Новые `--help` и старые aliases проверены.
- **10/10 CPU unit-тестов** внешнего инструмента: argv, лимиты, attach/launch,
  отказ от перезаписи, ошибки, таймауты, debug-copy, XML refs и HTML escaping.
- Финальная проверка: повторная сборка клиента/редактора и CTest `external_profiler`
  прошли. Тест регистрируется при наличии Python независимо от клиента и UE-импорта.
- Настоящая CPU-запись нового `campfire_client` прошла без source hooks:
  `.cache/profiles/campfire-cpu-verified/capture.trace` и `report.html`.
- Настоящая запись Allocations через `--debug-copy` сохранена:
  `.cache/profiles/campfire-memory-tool-verified/capture.trace` (предупреждение 54).
- Настоящий Metal System Trace через `--debug-copy` сохранён:
  `.cache/profiles/campfire-gpu-verified/capture.trace` (предупреждение 54).
- `system`/`counters` представлены в тулзе, но аппаратные счётчики в этой итерации
  не измерялись; доступность зависит от платформы/прав/версии Xcode.

Первое attach-семплирование старого имени `asr_client` показало ~62% суммарного
семплированного CPU-времени в `bakeRegionMass`, ~46% в `buildClusterDag`.
Inclusive-значения вложены и **не складываются**; это не доля wall-time кадра и не
измерение GPU. Исходная запись: `.cache/profiles/asr-baseline-20260926.trace`.

Размер memory trace может быть большим (в проверке ~759 МБ за 5 секунд со stacks),
поэтому длительности ограничены. Это данные профайлера, не игровые ресурсы.
Все результаты локальны. Профайлер и branding выделены в отдельный коммит;
посторонние изменения ассетов, геометрии и benchmark harness в него не включены.
