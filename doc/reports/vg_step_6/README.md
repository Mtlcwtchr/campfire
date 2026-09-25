# VG6 — explicit instance DAG adjacency and GPU replacement roots

Дата: 2026-09-21.

## Закрыто в этом подшаге

- `InstanceHierarchy` теперь хранит явную deterministic adjacency:
  `children` плюс `childFirst/childCount` в каждом group node. Runtime cut
  больше не обязан восстанавливать direct children только из
  `bornOf/replacedBy` maps.
- Добавлен adjacency-backed `cutAtInstanceHierarchy(hierarchy, allowances,
  chosen)`. Старый span-of-nodes API оставлен для compatibility и тестовых
  клиентов; renderer selection переведен на explicit adjacency path.
- Добавлен `InstanceRootCuller` и `scene_instance_root_expand.hlsl`.
  Instance replacement path теперь такой:

  CPU hierarchy/proxy roots
  `->` one root record per merged/canopy replacement
  `->` GPU candidate buffer
  `->` shared frustum/error cut + dense compaction
  `->` hierarchy gather + indirect draw.

- CPU больше не собирает world-space `GeometryCluster` records для instance
  replacement path; он передает только bounds/error/bucket/payload roots.
- Existing mesh `MeshRootCuller` и instance `InstanceRootCuller` имеют один и
  тот же downstream producer contract: `GeometryCluster` candidate buffer.

## Проверки

- Explicit adjacency instance tests: `9/9`.
- GPU root tests (mesh + instance): `2/2`.
- `build/shader_lint assets/shaders`: `48/48`.
- Scene smoke на Metal: `gpu-source=1`, `gpu-hierarchy=1`, `gpu-clusters=81110`,
  frame 240 завершился без crash, vegetation population присутствует.

## Честные ограничения

- Hierarchy topology и population пока строятся CPU-side из streamed scene
  data. GPU root pass уже убирает CPU candidate packing, но не является еще
  GPU child traversal/page-table walker.
- Individual source roots и replacement roots пока проходят двумя
  complementary cuts; единый branch cut с object geometry + merged/canopy
  representation еще не принят как visual gate.
- Не закрыты Hi-Z occlusion, residency/page requests, normal parent targets,
  close-up mixed-cut visual parity и terrain-integrated very-far shading
  audit.

## Коммит

- будет создан после полного checkpoint build/test.
