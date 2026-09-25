# VG9 — GPU traversal of instance hierarchy adjacency

Дата: 2026-09-22.

## Закрыто в этом подшаге

- Добавлен `InstanceRootCuller::setupHierarchy/dispatchHierarchy` и shader
  `scene_instance_dag_expand.hlsl`.
- Scene instance hierarchy теперь строит per-frame node table с explicit
  `childFirst/childCount`, root ranges и payload offsets. GPU traversal:

  final hierarchy roots
  `->` error test
  `->` explicit child descent
  `->` merged/canopy candidate or invalid individual/density sentinel
  `->` shared frustum/error cut, dense compaction, gather and indirect draw.

- Individual и density nodes остаются в DAG traversal как invalid leaves:
  они не рисуются replacement geometry, но не дают parent branch потерять
  правильный путь descent.
- CPU сохраняет hierarchy population, replacement coverage и transition
  metadata; CPU больше не формирует instance replacement `GeometryCluster`
  candidate list и не выбирает replacement node для GPU draw.
- Added fixed-budget fallback: если per-frame instance node/candidate table не
  помещается в GPU capacity, используется существующий GPU root path или CPU
  fallback, без unbounded allocation.

## Проверки

- Instance DAG GPU readback: `instance_hierarchy_dag_gpu_walks_explicit_children`
  pass на Metal.
- Focused GPU cluster/mesh-DAG/instance-DAG/gather set: `13/13`.
- Smart-mesh suite: `55/55`.
- Render-system suite: `60/60`.
- `build/shader_lint assets/shaders`: `48/48`.
- Scene smoke frame 240: `gpu-source=1`, `gpu-hierarchy=1`, `81110` source
  candidates, `1840` selected/drawn models, no crash/blank vegetation.

## Честные ограничения

- GPU instance traversal сейчас работает по resident per-frame node table;
  page-table residency requests, eviction feedback и persistent hierarchy
  cache еще не подключены.
- Hi-Z occlusion remains open.
- Fixed local traversal stack needs content validation for pathological DAGs.
- Near/mid/far mixed-cut close-up visual gate и quantitative CPU/GPU parity
  capture еще не принят.

## Коммит

- будет создан после checkpoint commit.
