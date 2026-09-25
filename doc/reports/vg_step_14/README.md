# VG14 — GPU page table, request feedback и parent fallback

Дата: 2026-09-22.

## Закрыто

- `ClusterCuller` теперь владеет GPU page table с состояниями:
  `missing=0`, `resident=1`, `fallback=2`.
- Page table и feedback buffer имеют отдельные read/write bindings; feedback
  сбрасывается в reset dispatch каждого frame.
- Missing page после frustum/Hi-Z проверки не попадает в indirect output и
  выставляет `pageFeedback[bucket]`, чтобы loader мог запросить geometry page.
- Fallback page bypass-ит обычный screen-error cut и остаётся видимым как
  resident parent frontier, не создавая hole пока child отсутствует.
- Добавлены CPU update/readback APIs для integration с `GeometryPageResidency`.
- Existing scene стартует с all-resident table, поэтому текущая картинка и
  payload/bucket layout не меняются до подключения реального streamer.

## Проверки

- Metal GPU cluster tests: `10/10`, включая:
  - missing page request + zero draw;
  - fallback page draw despite an error that would fail the normal cut;
  - полный старый cut/compact/frustum набор.
- Scene smoke на Metal:
  `gpu-source=1`, `gpu-hierarchy=1`, `gpu-clusters=74620`, `1840 models drawn`,
  без shader/resource ошибок; screenshot `/tmp/asr_vg14_page_table.png`.
- MSAA safety: обычный запуск не bind-ит Hi-Z depth для MSAA target, поэтому
  прежние фиолетовые прямоугольники/летающие terrain triangles не повторяются.

## Что ещё не закрыто

- Scene пока не маркирует реальные mesh clusters как missing и не загружает
  страницы асинхронно; page table используется в all-resident режиме.
- Feedback пока доступен через synchronous readback API для integration/test;
  production path должен читать его с frame latency, без stall.
- Следующий шаг — разрезать cluster asset на logical pages, связать bucket с
  page ID/parent и подключить loader + GPU geometry upload/retire.
