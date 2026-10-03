#!/usr/bin/env python3
"""Rebuild the distance chain of the solid scene props and orient every model.

    python3 tools/rebuild_prop_lods.py [--dir assets/generated/scene_models]
                                       [--config content/config/scene_model_lod.json]
                                       [--only Rock_Medium_1,...] [--dry-run]

Two faults made decorations look shuffled or turned into shapes:

1. Texture seams. Both the cluster DAG (engine/geometry/cluster_dag) and the old
   chain (tools/mesh_lod.py) weld vertices by position and give every welded
   vertex the attributes of ONE of its copies. A scanned rock or a stump is a
   mosaic of UV islands; once a collapse crosses a seam, the triangle on the
   other side takes UVs from the wrong island. From the first coarse level on,
   37-55 % of their triangles spanned more than half the texture, and the
   coarsest DAG levels folded a rock down to four triangles.

   The chain built here keeps level zero bit for bit and simplifies it with
   meshoptimizer's attribute-aware simplifier on the UNWELDED index buffer:
   vertices that share a position but not a UV are a seam, and a seam may only
   collapse along itself, so both sides keep their own islands. Every level
   indexes the original vertex buffer, as before; the impostors (baked from
   level zero) are untouched. These props are listed `chain_only`: their
   cluster sidecars are retired and tools/scene_model_clusters skips them, so
   the renderer draws the chain up close and the baked impostors far away.

2. Winding. The renderer turns the normal of a back face around
   (scene_models.hlsl), and its front face in world space is the one wound
   counter-clockwise about the way it faces (SDL clockwise in window
   coordinates, y flipped by the projection; measured with SV_IsFrontFace).
   The UE exports - rocks, deadwood, mushrooms, tree bark - are wound the
   other way round from their own normals, so their outer surface was taken
   for a back face and lit from inside. Each texture layer whose verdict is
   clear is turned, in the chain and in a kept sidecar's cluster and card
   streams; a sidecar's shell is judged against its own normals. Leaf cards
   with normals bent round the crown give no clear verdict and keep their
   order.

A backup of every file touched goes to <dir>/.backup/<time>/.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import shutil
import struct
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mesh_lod  # noqa: E402  (surface_error: the same metric the chain always published)

try:
    import meshoptimizer
except ImportError:  # pragma: no cover - reported to the user
    meshoptimizer = None

VERTEX_FLOATS = 12


# --- SCM2 -------------------------------------------------------------------

def read_scm(path: Path):
    data = path.read_bytes()
    magic, count, levels = struct.unpack_from('<4sII', data)
    if magic != b'SCM2':
        raise ValueError(f'{path.name}: not an SCM2 mesh')
    spans = struct.unpack_from(f'<{levels}I', data, 12)
    at = 12 + levels * 4
    vertices = np.frombuffer(data, dtype='<f4', count=count * VERTEX_FLOATS, offset=at)
    vertices = vertices.reshape(count, VERTEX_FLOATS).copy()
    at += count * VERTEX_FLOATS * 4
    indices = np.frombuffer(data, dtype='<u4', count=sum(spans), offset=at).copy()
    chain, first = [], 0
    for span in spans:
        chain.append(indices[first:first + span].reshape(-1, 3).copy())
        first += span
    return vertices, chain


def write_scm(path: Path, vertices: np.ndarray, chain: list[np.ndarray]):
    with path.open('wb') as out:
        out.write(struct.pack('<4sII', b'SCM2', len(vertices), len(chain)))
        for level in chain:
            out.write(struct.pack('<I', level.size))
        out.write(vertices.astype('<f4').tobytes())
        for level in chain:
            out.write(level.astype('<u4').tobytes())


# --- winding ----------------------------------------------------------------

def face_normals(positions: np.ndarray, triangles: np.ndarray) -> np.ndarray:
    a, b, c = positions[triangles[:, 0]], positions[triangles[:, 1]], positions[triangles[:, 2]]
    return np.cross(b - a, c - a)


def against_normals(positions, normals, triangles) -> float:
    """Share of triangles whose counter-clockwise normal opposes their vertex normals.

    Judged on triangles whose vertex normals are clearly parallel or clearly
    opposed to the face: foliage cards often carry normals bent round the
    crown, which say nothing about which way the card itself is wound.
    """
    if len(triangles) == 0:
        return 0.0
    face = face_normals(positions, triangles)
    vertex = normals[triangles[:, 0]] + normals[triangles[:, 1]] + normals[triangles[:, 2]]
    weight = np.linalg.norm(face, axis=1)
    cosine = np.einsum('ij,ij->i', face, vertex) / np.maximum(weight * np.linalg.norm(vertex, axis=1), 1e-12)
    keep = (weight > 1e-14) & (np.abs(cosine) > 0.7)
    if keep.sum() < 8:
        keep = weight > 1e-14
    if not keep.any():
        return 0.0
    return float(np.average(cosine[keep] < 0, weights=weight[keep]))


def wants_turn(share_opposed: float, winding: str) -> bool:
    # The pipeline's front face is SDL_GPU_FRONTFACE_CLOCKWISE in window
    # coordinates; the projection flips y on the way there, so in world space
    # the front face is the one wound COUNTER-clockwise about the way it
    # faces (right-hand rule). Measured, not assumed: SV_IsFrontFace against
    # the visible side. Only a clear verdict turns anything.
    if winding == 'clockwise':
        return share_opposed < 0.15
    return share_opposed > 0.85


def turned(triangles: np.ndarray) -> np.ndarray:
    return triangles[:, [0, 2, 1]].copy()


def layers_to_turn(vertices: np.ndarray, triangles: np.ndarray, winding: str) -> dict[int, float]:
    """Texture layers wound against the renderer, judged one layer at a time.

    A tree is bark and leaves from one exporter: the bark is solid and its
    verdict is clear; the leaves' bent normals make theirs a coin toss, and
    they are drawn two-sided anyway, so they keep their order.
    """
    positions, normals = vertices[:, 0:3], vertices[:, 3:6]
    layer = vertices[triangles[:, 0], 11].astype(np.int64)
    verdicts = {}
    for value in np.unique(layer):
        share = against_normals(positions, normals, triangles[layer == value])
        if wants_turn(share, winding):
            verdicts[int(value)] = share
    return verdicts


def orient(vertices: np.ndarray, triangles: np.ndarray, verdicts: dict[int, float]) -> np.ndarray:
    if not verdicts or len(triangles) == 0:
        return triangles
    layer = vertices[triangles[:, 0], 11].astype(np.int64)
    flip = np.isin(layer, list(verdicts))
    result = triangles.copy()
    result[flip] = turned(triangles[flip])
    return result


# --- SCC6 sidecar (in place: only triangle order changes) --------------------

def turn_sidecar(path: Path, vertices: np.ndarray, verdicts: dict[int, float], winding: str) -> list[str]:
    data = bytearray(path.read_bytes())
    if data[:4] != b'SCC6':
        return [f'{path.name}: not SCC6, left alone']
    head = struct.unpack_from('<13I2f', data, 4)
    ni, nc, ncard, ncardlev, crown_v, crown_i = head[5], head[6], head[7], head[8], head[9], head[10]
    at = 64
    indices_at = at
    at += ni * 4
    has_positions = len(data) >= 64 + ni * 16 + nc * 44 + ncard * 4 + ncardlev * 8 + crown_v * 32 + crown_i * 4
    positions_at = at if has_positions else None
    if has_positions:
        at += ni * 12
    cards_at = at
    at += ncard * 4 + ncardlev * 8 + nc * 44
    crown_vertices_at = at
    at += crown_v * 32
    crown_indices_at = at
    notes = []

    def read(offset, count):
        return np.frombuffer(bytes(data[offset:offset + count * 4]), dtype='<u4').reshape(-1, 3).copy()

    def write(offset, before, after, label, parallel=None):
        flip = np.any(before != after, axis=1)
        if not flip.any():
            return
        data[offset:offset + after.size * 4] = after.astype('<u4').tobytes()
        if parallel is not None:
            p = np.frombuffer(bytes(data[parallel:parallel + after.size * 12]), dtype='<f4').reshape(-1, 3, 3).copy()
            p[flip] = p[flip][:, [0, 2, 1], :]
            data[parallel:parallel + after.size * 12] = p.astype('<f4').tobytes()
        notes.append(f'{label}: {int(flip.sum())} triangles turned')

    # Streams over the model's own vertices take the model's per-layer verdict.
    if ni >= 3 and verdicts:
        tris = read(indices_at, ni)
        write(indices_at, tris, orient(vertices, tris, verdicts), 'clusters', positions_at)
    if ncard >= 3 and verdicts:
        tris = read(cards_at, ncard)
        write(cards_at, tris, orient(vertices, tris, verdicts), 'cards')
    # The shell has its own vertices and normals and its own verdict.
    if crown_v and crown_i >= 3:
        crown = np.frombuffer(bytes(data[crown_vertices_at:crown_vertices_at + crown_v * 32]),
                              dtype='<f4').reshape(-1, 8)
        tris = read(crown_indices_at, crown_i)
        share = against_normals(crown[:, 0:3].copy(), crown[:, 3:6].copy(), tris)
        if wants_turn(share, winding):
            write(crown_indices_at, tris, turned(tris), f'crown ({share:.2f} opposed)')
    if notes:
        path.write_bytes(bytes(data))
    return notes


# --- the chain --------------------------------------------------------------

def seam_report(vertices: np.ndarray, triangles: np.ndarray) -> float:
    """Share of triangles whose UVs span more than half the texture: seam damage."""
    if len(triangles) == 0:
        return 0.0
    uv = vertices[:, 6:8]
    corners = uv[triangles]
    span = (corners.max(axis=1) - corners.min(axis=1)).max(axis=1)
    return float(np.mean(span > 0.5))


def simplify_with_attributes(indices, positions, attributes, weights, target_count, target_error):
    """meshopt_simplifyWithAttributes with its C signature declared.

    The Python binding passes this one through ctypes undeclared, so size_t
    arguments would cross as 32-bit ints and a float not at all.
    """
    from meshoptimizer import simplifier
    function = simplifier.lib.meshopt_simplifyWithAttributes
    size = ctypes.c_size_t
    floats = ctypes.POINTER(ctypes.c_float)
    uints = ctypes.POINTER(ctypes.c_uint)
    function.argtypes = [uints, uints, size, floats, size, size, floats, size, floats, size,
                         ctypes.POINTER(ctypes.c_ubyte), size, ctypes.c_float, ctypes.c_uint, floats]
    function.restype = size
    destination = np.zeros(indices.size, dtype=np.uint32)
    error = ctypes.c_float(0.0)
    count = function(destination.ctypes.data_as(uints), indices.ctypes.data_as(uints), indices.size,
                     positions.ctypes.data_as(floats), len(positions), positions.strides[0],
                     attributes.ctypes.data_as(floats), attributes.strides[0],
                     weights.ctypes.data_as(floats), weights.size, None,
                     int(target_count), float(target_error), 0, ctypes.byref(error))
    return destination[:count].copy(), float(error.value)


def build_chain(vertices: np.ndarray, base: np.ndarray, ratios, normal_weight, uv_weight):
    positions = np.ascontiguousarray(vertices[:, 0:3], dtype=np.float32)
    attributes = np.ascontiguousarray(np.column_stack((vertices[:, 3:6], vertices[:, 6:8])),
                                      dtype=np.float32)
    weights = np.array([normal_weight] * 3 + [uv_weight] * 2, dtype=np.float32)
    flat = np.ascontiguousarray(base.reshape(-1), dtype=np.uint32)
    levels = []
    previous = len(base)
    for ratio in ratios:
        target = max(4, int(round(len(base) * ratio))) * 3
        simplified, _ = simplify_with_attributes(flat, positions, attributes, weights, target, 0.25)
        simplified = simplified.reshape(-1, 3)
        # A level that does not save a fifth over the last costs a draw and
        # buys nothing: the chain stops where the simplifier stalls.
        if len(simplified) == 0 or len(simplified) > previous * 0.8:
            break
        optimized = np.zeros(simplified.size, dtype=np.uint32)
        meshoptimizer.optimize_vertex_cache(optimized, simplified.reshape(-1), vertex_count=len(vertices))
        levels.append(optimized.reshape(-1, 3))
        previous = len(simplified)
    return levels


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--dir', type=Path, default=root / 'assets/generated/scene_models')
    parser.add_argument('--config', type=Path, default=root / 'content/config/scene_model_lod.json')
    parser.add_argument('--only', default='', help='comma-separated model names')
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if meshoptimizer is None:
        print('meshoptimizer is required: python3 -m pip install meshoptimizer', file=sys.stderr)
        return 2
    config = json.loads(args.config.read_text())
    chain_only = set(config.get('chain_only', []))
    ratios = [float(r) for r in config.get('ratios', [0.4, 0.16, 0.06])]
    normal_weight = float(config.get('normal_weight', 0.5))
    uv_weight = float(config.get('uv_weight', 0.5))
    winding = config.get('winding', 'counter_clockwise')
    only = {n for n in args.only.split(',') if n}

    manifest_path = args.dir / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    backup = args.dir / '.backup' / time.strftime('%Y%m%d-%H%M%S')
    touched = []

    def save(path: Path):
        if args.dry_run or not path.exists():
            return
        backup.mkdir(parents=True, exist_ok=True)
        if path not in touched:
            shutil.copy2(path, backup / path.name)
            touched.append(path)

    changed_manifest = False
    for model in manifest['models']:
        name = model['name']
        if only and name not in only:
            continue
        mesh_path = args.dir / model['mesh']
        vertices, chain = read_scm(mesh_path)
        positions = vertices[:, 0:3]
        verdicts = layers_to_turn(vertices, chain[0], winding)
        line = f'{name:16s} L0 {len(chain[0]):6d} tri'
        if verdicts:
            line += ', turned layers ' + ' '.join(f'{k} ({v:.2f} opposed)' for k, v in sorted(verdicts.items()))
        rebuilt = name in chain_only
        if rebuilt:
            before = [(len(t), seam_report(vertices, t)) for t in chain]
            base = orient(vertices, chain[0], verdicts)
            # The simplifier keeps the winding it is given, so every level
            # inherits level zero's.
            chain = [base] + build_chain(vertices, base, ratios, normal_weight, uv_weight)
            points = positions.astype(np.float64)
            published = [{'triangles': len(base), 'cards': -1, 'error_m': 0.0,
                          'error_max_m': 0.0, 'error_rms_m': 0.0}]
            last = 0.0
            for level in chain[1:]:
                worst, p95, rms = mesh_lod.surface_error(points, points, level)
                last = max(last, p95)
                published.append({'triangles': len(level), 'cards': 0, 'error_m': last,
                                  'error_max_m': worst, 'error_rms_m': rms})
            after = [(len(t), seam_report(vertices, t)) for t in chain]
            line += ('\n    chain before: ' + '  '.join(f'{n} tri/{s:.0%} seam' for n, s in before) +
                     '\n    chain after:  ' + '  '.join(f'{n} tri/{s:.0%} seam' for n, s in after) +
                     '\n    errors (p95 m): ' + ' '.join(f'{p["error_m"]:.4f}' for p in published))
            if not args.dry_run:
                save(mesh_path)
                write_scm(mesh_path, vertices, chain)
                model['levels'] = published
                model['lod'] = 'chain'
                changed_manifest = True
                for sidecar in (mesh_path.with_suffix('.clusters'), args.dir / (name + '.source.clusters')):
                    if sidecar.exists():
                        save(sidecar)
                        sidecar.unlink()
                        line += f'\n    retired {sidecar.name}'
        elif verdicts and not args.dry_run:
            save(mesh_path)
            write_scm(mesh_path, vertices, [orient(vertices, t, verdicts) for t in chain])
        sidecar = mesh_path.with_suffix('.clusters')
        if not rebuilt and sidecar.exists() and not args.dry_run:
            save(sidecar)
            for note in turn_sidecar(sidecar, vertices, verdicts, winding):
                line += f'\n    sidecar {note}'
        print(line, flush=True)
    # Grove shells (nine trees merged): a sidecar with a crown and nothing else.
    for grove in sorted(args.dir.glob('*-grove.clusters')):
        if only and grove.name.replace('-grove.clusters', '') not in only:
            continue
        if args.dry_run:
            continue
        save(grove)
        for note in turn_sidecar(grove, np.zeros((0, VERTEX_FLOATS), dtype=np.float32), {}, winding):
            print(f'{grove.stem:16s} sidecar {note}')
    if changed_manifest:
        save(manifest_path)
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    if touched:
        print(f'backup of {len(touched)} files: {backup}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

