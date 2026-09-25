# VG4 — per-object mesh clusters and GPU submission

Дата: 2026-09-21.

## Закрыто в этом подшаге

- SceneModelsPass больше не выбирает один cluster cut на batch: каждый
  отдельный объект получает собственный screen-space cut из своего DAG.
- Для clustered source meshes добавлен GPU path без readback:

  cluster x instance -> frustum/error cut -> visible payloads -> compacted
  model-instance stream -> indirect draw arguments.

- Каждый cluster имеет свой indirect bucket и index range. scene_models.hlsl
  получает тот же instance layout, что и CPU path.
- Входной frame instance arena читается как фиксированные четыре `float4`-ряда
  на `ModelInstance`; byte offset переводится в row offset, поэтому
  предшествующие sprite/grass записи с другими strides не ломают source offset.
- Integer-поля gather uniform block и 20-byte indirect arguments передаются как
  bit-exact `uint`, а не как числовые float. Это отдельно проверено на Metal.
- Добавлен GPU prefix для bucket counts: `firstInstance` переводится из
  фиксированных visibility slices в плотный output offset. Поэтому gather
  stream ограничен `clusterCapacity`, а не `bucketCount * bucketCapacity`;
  массовая сцена больше не создаёт гигабайтный worst-case instance buffer.
- Ограничения GPU buffers фиксированы. При превышении clusterCapacity или
  bucketCapacity используется проверенный CPU fallback.
- Карточки и grove/canopy representations остаются отдельными представлениями:
  GPU source path не подменяет alpha-card chain и не дублирует shell.
- Для source mesh DAG добавлен cluster-local morph asset. Вершины, входящие в
  разные replacement families, дублируются с разными target positions; target
  строится проекцией на поверхность конкретного replacement cluster. GPU
  gather вычисляет morph внутри того же [error,parentError) interval.
- Для instance hierarchy добавлен отдельный `InstanceHierarchyCuller`: он
  использует тот же conservative bounds/error cut, но пишет replacement-node
  payloads в отдельный GPU instance stream. Merged и canopy buckets получают
  свои indirect arguments; CPU readback для этой части отсутствует.
- Исправлена фактическая scene binding-схема: `clusterMorph` находится в
  read slot `t2`, а InstanceArena в `t3`; прежняя перестановка давала null
  source buffer и была причиной падения массового GPU smoke.
- После reconstruction error в crown DAG строго восстанавливается
  `parentError > error`, если float rounding схлопнул узкий интервал.

## Проверки

- build/shader_lint assets/shaders: 48/48 shaders compiled.
- asr_height_page_gpu_tests focused cluster/gather/hierarchy set: 11/11.
- asr_height_page_gpu_tests full target: 36/38; все новые cluster/gather
  проверки проходят. Два оставшихся падения — существующие pixel assertions
  height/water в `test_height_page_atlas_gpu.cpp`, не связанные с этим шагом.
- asr_render_system_tests: 60/60.
- geometry/smart-mesh suite: 54/54.
- RelWithDebInfo client smoke на Metal: frame 240 достиг `1840` selected
  models, `gpu-source=1`, `gpu-hierarchy=1`, `81110` submitted source clusters;
  draw завершился без crash после исправления binding и prefix compaction.
  GPU кадр сопоставлен с CPU fallback: распределение vegetation silhouettes
  совпадает на проверенном forest view.
- SceneModelsPass C++ object compiled with diagnostics ON and OFF.
- git diff --check: clean.

## Что еще не является закрытым Nanite gate

- Vertex geomorph закрыт для source mesh DAG и проверен на correspondence
  module/unit уровне. Отдельный visual screenshot gate для mixed cuts еще не
  снят; normals пока не имеют отдельного parent target и используют нормали
  child geometry во время morph.
- GPU path всё еще получает source object population и transition coverage из
  CPU. GPU replacement cut уже работает readback-free, но CPU/GPU boundary
  agreement еще не доказан визуально на mixed cuts.
- Hi-Z occlusion, residency/page-table driven traversal и полноценный GPU
  expansion individual object roots остаются следующими этапами.
- End-to-end broad screenshot теперь проходит GPU/CPU parity gate, но отдельный
  close-up mixed-cut gate всё ещё не принят: нужно проверить near mesh,
  merged shell, canopy и dither crossover на одном ракурсе.

## Коммиты

- 98f5314 — Select mesh clusters per scene instance
- ccefec2 — Run scene mesh cluster selection on GPU
- 9c1159b — Run instance hierarchy replacement cut on GPU
- 16fe7c3 — Fix GPU mesh gather compaction and scene bindings
