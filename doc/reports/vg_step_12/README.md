# VG12 — previous-frame Hi-Z в GPU cluster culler

Дата: 2026-09-22.

## Что закрыто

- GPU cluster culler теперь принимает предыдущий depth pyramid и делает
  conservative screen-space occlusion test до LOD cut.
- В culler передаётся настоящий clip-space `z` row. Сравнивается `z / w`, а
  не clip-space `w`; это устраняет ошибочное отбрасывание всей растительности.
- Первый кадр и resize используют безопасный 1-float fallback: отсутствие
  готовой pyramid отключает occlusion, но не меняет layout compute bindings.
- Hi-Z строится как packed buffer с floor-halved уровнями. Из-за ограничения
  Metal на одновременный bind одного structured buffer как `t0` и `u0` уровни
  сначала строятся в раздельных scratch buffers, затем копируются в packed
  buffer. Это устраняет shader-library ошибки и read/write aliasing.
- `readIndex_` публикуется только на следующем кадре: culler не читает
  pyramid, которую этот же кадр ещё строит.

## Проверки

- `asr_smart_mesh_tests`: `56/56`.
- `asr_render_system_tests`: `60/60`.
- `build/shader_lint assets/shaders`: `48 shaders compiled, 0 would not`.
- Metal scene smoke:

  `gpu-source=1`, `gpu-hierarchy=1`, `gpu-clusters=74620`, `1840 models drawn`,
  `857 off screen`, `244 behind the ground`; screenshot:
  `/tmp/asr_gpu_hiz_final.png`.

- В smoke нет Metal shader/resource ошибок и нет blank frame. Визуальная
  проверка показывает terrain, water и vegetation.

## Ограничения

- Это previous-frame conservative Hi-Z, без motion-vector расширения bounds и
  без отдельного visibility counter, поэтому savings пока не считаются как
  отдельная метрика и могут быть небольшими.
- Packed pyramid сейчас собирается несколькими compute dispatches; это рабочий
  Metal-safe вариант, но следующим performance-шагом нужно сократить число
  промежуточных копий либо перейти на multi-buffer mip bindings.
- Persistent virtual-geometry residency, page table, feedback и eviction ещё
  не являются частью этого checkpoint.

## Коммит

- создаётся отдельным checkpoint после прохождения smoke и регрессии.
