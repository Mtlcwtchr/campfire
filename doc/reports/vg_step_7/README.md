# VG7 — GPU traversal of per-mesh cluster DAGs

Дата: 2026-09-21.

## Закрыто в этом подшаге

- `MeshRootCuller` получил DAG mode. Для каждого source mesh setup строит
  compact family→children table из `MeshCluster::bornOf/replacedBy`, а root
  record хранит local cluster range и family base.
- `scene_mesh_dag_expand.hlsl` стартует с coarsest clusters (`parentError =
  infinity`), проверяет screen-space error, и при несоответствии allowance
  спускается через family children до fitting cluster. Каждый object root имеет
  фиксированный output range; невыбранные записи получают invalid-bucket
  sentinel и безопасно отбрасываются shared culler.
- Existing GPU frustum test, bucket overflow handling, prefix compaction,
  instance gather и indirect draw остались общими для mesh DAG и flat/root
  producers.
- Добавлен GPU readback test: coarse root при большой ошибке выбирает child,
  sentinel не попадает в valid bucket.

## Проверки

- `mesh_dag_gpu_walks_from_a_coarse_root_to_the_fitting_child`: pass на Metal.
- `build/shader_lint assets/shaders`: 48/48 shaders compiled.
- Scene smoke после DAG integration: `gpu-source=1`, `gpu-hierarchy=1`,
  `gpu-clusters=81110`; frame 240 записан в `/tmp/asr_gpu_dag240.png`, без
  crash и без blank vegetation output.
- До этого этапа regression baselines: smart mesh `55/55`, render systems
  `60/60`, focused GPU root/cluster/gather `11/11`.

## Честные ограничения

- GPU DAG traversal сейчас выполняет traversal по resident mesh DAG и не
  делает page-table/residency requests. Hi-Z occlusion и streaming eviction
  остаются отдельными Nanite gates.
- Traversal ограничен fixed local stack; content validation должна отклонять
  DAG, который превышает этот budget, либо нужен следующий paged traversal
  implementation.
- Instance hierarchy пока использует GPU replacement roots и explicit CPU
  adjacency, но его child traversal еще не перенесен на card.
- Normals parent targets и close-up mixed near/mid/far visual gate еще не
  закрыты.

## Коммит

- будет создан после полного checkpoint build/test.
