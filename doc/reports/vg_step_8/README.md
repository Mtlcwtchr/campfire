# VG8 — coherent position and normal geomorph

Дата: 2026-09-21.

## Закрыто в этом подшаге

- `ClusterMorphAsset` теперь хранит не только closest-point target position,
  но и geometric normal replacement triangle для каждой expanded vertex.
- Scene vertex stream получил `morphNormal`; `scene_models.hlsl` интерполирует
  position и normal одним и тем же morph factor до object rotation.
- Root/no-replacement vertices сохраняют исходную normal через zero-target
  fallback; replacement vertices получают нормаль конкретной parent surface,
  а не stale child normal.
- Layout contract обновлен централизованно в `SceneModelStreams`:
  `16 -> 19` floats per vertex, instance attributes сдвинуты на slot 7..10.

## Проверки

- `cluster_morph_expands_families_with_local_parent_targets`: pass, включая
  target-normal correspondence.
- `scene_model_shader_still_builds_with_the_streams_the_pass_declares`: pass
  на Metal.
- `build/shader_lint assets/shaders`: 48/48.
- Scene smoke: `gpu-source=1`, `gpu-hierarchy=1`, `81110` mesh candidates,
  `1840` models drawn, `/tmp/asr_gpu_normals240.png` без blank output/crash.
- Regression baselines перед checkpoint: smart mesh `55/55`, render systems
  `60/60`, focused GPU DAG/root/gather `12/12`.

## Честные ограничения

- Normal target сейчас geometric и per-parent triangle; tangent-space normal
  map остается material reconstruction в fragment shader.
- Close-up mixed-cut screenshot gate и quantitative normal-pop metric еще не
  заведены.
- Hi-Z, residency/page-table requests и GPU child traversal instance hierarchy
  остаются следующими инфраструктурными gates.

## Коммит

- будет создан после полного checkpoint build/test.
