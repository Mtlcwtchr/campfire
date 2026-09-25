# VG11 — previous-frame Hi-Z pyramid infrastructure

Дата: 2026-09-22.

## Закрыто в этом подшаге

- `RenderPipeline` получил отдельную post-render compute фазу. Она выполняется
  после последнего graphics stage в том же command buffer, поэтому результат
  текущего depth target становится доступен следующему кадру без CPU readback.
- Добавлены storage texture read/write bindings в `ComputeDispatch` с выбором
  mip-level для write target.
- Добавлен `HiZPyramid`: два ping-pong R32 depth texture, полный mip chain,
  depth copy в mip 0 и max-depth reduction по следующим mip levels.
- Scene models pass строит pyramid после рендера, а resize корректно
  пересоздает ресурсы. Depth target получил compute read usage.
- Первое frame intentionally не имеет occlusion pyramid; после завершения
  первого frame появляется валидный previous-frame texture.

## Проверки

- Metal scene smoke после resource/mip fix: `gpu-source=1`,
  `gpu-hierarchy=1`, `gpu-clusters=74620`, `1840 models drawn`, no crash/
  blank frame.
- `asr_smart_mesh_tests`: `56/56`.
- `asr_render_system_tests`: `60/60`.
- Shader pipeline creation и runtime dispatch прошли на Metal.

## Честные ограничения

- Этот шаг только строит и сохраняет pyramid. Cluster culler пока не читает ее,
  поэтому GPU visibility numbers еще не показывают Hi-Z savings.
- Occlusion будет conservative и previous-frame only: camera/instance motion
  должен расширять bounds либо отключать rejection, иначе возможны temporal
  holes.
- Persistent geometry page residency/feedback по-прежнему не подключен.

## Коммит

- будет создан после checkpoint commit.
