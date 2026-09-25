# VG5 — GPU mesh roots and candidate expansion

Дата: 2026-09-21.

## Закрыто в этом подшаге

- Убран CPU-generated `object x cluster` world-space список для source meshes.
  `SceneModelsPass::collect()` теперь формирует один `MeshRootInstance` на
  отдельный объект: transform, диапазон local clusters, bucket base, output
  offset и byte payload в InstanceArena.
- Добавлен модуль `engine::MeshRootCuller` и compute shader
  `scene_mesh_root_expand.hlsl`:

  immutable local cluster table + one root/object
  `->` GPU world-space candidate buffer
  `->` existing frustum/error cut + dense bucket compaction
  `->` existing gather + indirect draw.

- Static cluster records загружаются один раз при setup. Per-frame upload
  содержит только root records; transform, yaw, scale для bounds/errors и
  payload assignment выполняются на GPU.
- `ClusterCuller::dispatchGpu()` теперь владеет общей последовательностью
  reset/cull/optional compact для CPU и GPU producers. Это оставляет producer
  contract стабильным и позволяет в следующем шаге заменить flat expansion на
  DAG/page traversal без изменений draw/gather слоя.
- Добавлен Metal readback test для двух roots и двух local clusters. Проверяются
  world-space center, radius, bucket и payload после GPU expansion.

## Проверки

- `build/shader_lint assets/shaders`: 48/48 shaders compiled.
- Focused GPU cluster/root/gather set: 11/11.
- Полный `asr_height_page_gpu_tests`: 38/40. Новые root/cluster/gather тесты
  проходят; два известных падения остаются в water pixel assertions на строке
  734 `test_height_page_atlas_gpu.cpp` и не затрагивают этот код.
- Scene smoke на Metal: `gpu-source=1`, `gpu-hierarchy=1`, `gpu-clusters=81110`,
  frame 240 записан в `/tmp/asr_gpu_root240.png`, без crash и с видимой
  vegetation population.

## Что еще не является закрытым Nanite gate

- Expansion сейчас плоско перечисляет все resident clusters модели. Это уже
  GPU root expansion и убирает CPU cross-product, но еще не настоящий GPU DAG
  child traversal с page requests.
- CPU по-прежнему формирует population/transition coverage и instance roots.
  Следующий слой — GPU instance-hierarchy roots и общий residency/page-table
  contract.
- Не закрыты Hi-Z occlusion, streaming residency, explicit child adjacency,
  normal parent targets и close-up visual gate для mixed near/mid/far cuts.

## Коммит

- будет создан после полного build/test checkpoint этого подшага.
