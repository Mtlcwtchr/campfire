# VG10 — optimal QEM replacement positions in the mesh DAG

Дата: 2026-09-22.

## Закрыто в этом подшаге

- Исправлена ключевая причина плохой геометрии coarse levels: QEM больше не
  использует только endpoint collapse. Для interior vertices решается
  оптимальная позиция объединенного quadric; boundary/seam vertices остаются
  pinned.
- Перемещенные survivors получают реальные DAG vertex ids и участвуют во всех
  последующих уровнях. Для source mesh сохраняется mapping на исходный vertex,
  поэтому атрибуты/UV/material остаются в существующем vertex buffer.
- Sidecar переведен на `SCC6`: добавлен parallel stream
  `clusterPositions`, по одному `float3` на occurrence в cluster index stream.
  Это дает runtime точные replacement surfaces без второго material vertex
  format и сохраняет чтение SCC3–SCC5.
- `buildClusterMorph` использует фактические позиции parent triangles из
  sidecar, а не старые source endpoints. В shader продолжает работать тот же
  плавный geomorph, но его endpoint теперь совпадает с построенной coarse
  геометрией.
- Добавлена topology post-validation группы: для closed/manifold geometry
  replacement не принимается, если он меняет boundary signature; для imported
  open vegetation internal fan может retopologize, но source silhouette остается
  locked. Mixed cuts остаются watertight.

## Проверки

- `asr_smart_mesh_tests`: `56/56`.
- `asr_render_system_tests`: `60/60`.
- Generated solid DAG: `CommonTree_1` — 5 levels, `Pine_1` — 5 levels;
  crown/grove hierarchy продолжает строиться.
- Проверен sidecar encode/decode roundtrip вместе с новым
  `clusterPositions` stream.
- Shader changes не требовались: morph target уже был частью vertex stream;
  изменен источник его target positions.

## Честные ограничения

- QEM replacement positions хранятся parallel к cluster index occurrences,
  поэтому sidecar больше и пока не использует Nanite-style compressed page
  encoding.
- Для открытых vegetation components остается conservative silhouette policy;
  это гарантирует границу, но не заменяет полноценную production-grade
  feature-aware remeshing.
- Hi-Z occlusion, persistent page residency/feedback и close-up visual parity
  gate остаются отдельными незакрытыми этапами.

## Коммит

- будет создан после checkpoint commit.
