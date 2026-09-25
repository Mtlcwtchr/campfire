# VG13 — geometry page residency contract

Дата: 2026-09-22.

## Закрыто

- Выделен `GeometryPageResidency` в движковый render/geometry модуль.
- Каталог страниц валидирует уникальные IDs, parent references, root
  termination и отсутствие циклов до публикации нового состояния.
- Root pages pin-ятся и служат гарантированным fallback.
- `request()` идемпотентен и сохраняет детерминированный порядок запросов.
- `commit()` учитывает byte budget и освобождает oldest unpinned resident page
  перед загрузкой новой.
- `fallback(page)` поднимается по parent chain до первого resident page,
  поэтому отсутствие fine page не создаёт hole.
- Frame touch timestamps и deterministic ID tie-breaker задают LRU policy.

## Проверки

- `asr_smart_mesh_tests`: `60/60`, включая четыре новых residency tests:
  fallback chain, request dedup/order, LRU eviction и malformed catalogue.
- `asr_render_system_tests`: `60/60`.
- `git diff --check`: clean.

## Что ещё не закрыто этим шагом

- Residency пока CPU-side contract: GPU page table, request feedback buffer и
  asynchronous geometry upload ещё не подключены к `ClusterCuller`.
- Текущие scene meshes всё ещё загружают общий vertex/index buffer целиком;
  следующий VG14 разделит logical bucket residency и GPU culler bindings.
- Fallback semantics уже определены и протестированы, но их нужно передать в
  GPU cut так, чтобы недоступный child выбирал resident parent.
