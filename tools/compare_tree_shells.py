#!/usr/bin/env python3
"""Isolated, equal-frame tree/LOD comparisons (CPU, not game screenshots).

Uses the importer's alpha-tested rasterizer, runtime shell UVs and coverage.
No terrain, wind, lighting or mip filtering: useful for geometry/material faults,
not a substitute for in-client visual acceptance. Requires numpy and Pillow.
"""
import argparse
import json
import math
import struct
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

from prepare_scene_models import SIZE, rasterize


def mesh(path):
    raw = path.read_bytes()
    magic, count, levels = struct.unpack_from('<4sII', raw)
    if magic != b'SCM2' or not 0 < levels <= 8:
        raise ValueError(f'{path}: expected SCM2')
    counts = struct.unpack_from(f'<{levels}I', raw, 12)
    offset = 12 + 4*levels
    vertices = np.frombuffer(raw, '<f4', count*12, offset).reshape(-1, 12).copy()
    offset += count*48
    ranges = []
    for n in counts:
        indices = np.frombuffer(raw, '<u4', n, offset).copy()
        if n % 3 or (n and indices.max() >= count):
            raise ValueError(f'{path}: invalid indices')
        ranges.append(indices)
        offset += n*4
    if offset != len(raw):
        raise ValueError(f'{path}: unexpected payload size')
    return vertices, ranges


def shell(path):
    raw = path.read_bytes()
    if raw[:4] not in (b'SCC3', b'SCC4', b'SCC5'):
        raise ValueError(f'{path}: expected SCC3/4/5')
    hierarchy = raw[:4] == b'SCC5'
    cluster_bytes = 44 if hierarchy else 36
    h = struct.unpack_from('<13I2f', raw, 4)
    ni, nc, cards, ranges, nv, crown_indices, clusters = h[5:12]
    offset = 64 + (ni+cards)*4 + ranges*8 + nc*cluster_bytes
    remaining = len(raw)-offset-crown_indices*4-clusters*cluster_bytes
    stride = remaining//nv if nv else 0
    if not nv or stride not in (28, 32) or remaining != stride*nv:
        raise ValueError(f'{path}: invalid crown vertices')
    if raw[:4] == b'SCC4' and stride != 32:
        raise ValueError(f'{path}: SCC4 requires material layers')
    data = np.frombuffer(raw, '<f4', nv*(stride//4), offset).reshape(nv, -1)
    vertices = np.ones((nv, 12), dtype=np.float32)
    vertices[:, :6] = data[:, :6]
    vertices[:, 6] = np.arctan2(data[:, 4], data[:, 3])*(1/6.2831853)*4
    vertices[:, 7] = np.arccos(np.clip(data[:, 5], -1, 1))*(1/3.14159265)*4
    vertices[:, 11] = data[:, 7] if stride == 32 else h[13]
    offset += nv*stride
    indices = np.frombuffer(raw, '<u4', crown_indices, offset)
    offset += crown_indices*4
    by_level = {}
    for n in range(clusters):
        layout = '<II6fIII' if hierarchy else '<II6fI'
        first, count, *fields = struct.unpack_from(layout, raw, offset+n*cluster_bytes)
        if count % 3 or first+count > len(indices):
            raise ValueError(f'{path}: invalid cluster range')
        # SCC5 appends bornOf/replacedBy after the level. The level is the
        # integer immediately after the six float fields in both layouts.
        level = fields[6]
        by_level.setdefault(level, []).append(indices[first:first+count])
    levels = {level: np.concatenate(parts) for level, parts in by_level.items()}
    if any(len(ix) and ix.max() >= nv for ix in levels.values()):
        raise ValueError(f'{path}: invalid crown index')
    return vertices, levels, data[:, 6].copy()


def grove(vertices, indices):
    copies, faces = [], []
    for y in (-1, 0, 1):
        for x in (-1, 0, 1):
            v = vertices.copy()
            v[:, :3] *= 0.86 + ((x+2*y) % 5)*0.06
            v[:, :2] += (x*7.5+(y % 2)*1.3, y*7.5)
            faces.append(indices + len(copies)*len(vertices))
            copies.append(v)
    return np.concatenate(copies), np.concatenate(faces)


def compare(args, textures, model, is_grove):
    vertices, levels = mesh(args.assets/model['mesh'])
    suffix = '-grove' if is_grove else ''
    original, indices = grove(vertices, levels[0]) if is_grove else (vertices, levels[0])
    variants = [('original', original, indices, None)]
    if not is_grove:
        variants += [(f'mesh-LOD{i}', vertices, ix, None) for i, ix in enumerate(levels[1:], 1)]
    for label, directory in [('before', args.before), ('after', args.after)]:
        if directory is None:
            continue
        v, ranges, coverage = shell(directory/(args.model+suffix+'.clusters'))
        selected = sorted(set([min(ranges), sorted(ranges)[len(ranges)//2], max(ranges)]))
        variants += [(f'{label}-shell-L{i}', v, ranges[i], coverage) for i in selected]
    # A shared metric frame, with room for shell expansion on all sides.
    points = np.concatenate([v[:, :3] for _, v, _, _ in variants])
    low = min(0.0, float(points[:, 2].min()))-0.5
    height = float(points[:, 2].max())-low+0.5
    width = max(float(np.linalg.norm(points[:, :2], axis=1).max())*2+1, height)
    sheet = Image.new('RGB', (SIZE*len(variants), (SIZE+36)*len(args.angles)), '#dce2e6')
    draw = ImageDraw.Draw(sheet)
    # Diagnostic layer map preserves texture alpha, so counts describe visible
    # material rather than hidden interior vertices. IDs are encoded in red.
    material_textures = [t.copy() for t in textures]
    for layer, t in enumerate(material_textures):
        t[:, :, :3] = (layer+1, 0, 0)
    rows = []
    for row, angle in enumerate(args.angles):
        reference = None
        for column, (name, v, ix, coverage) in enumerate(variants):
            v = v.copy()
            v[:, 2] -= low
            image, _ = rasterize(v, ix, textures, width, height, math.radians(angle), coverage=coverage)
            v[:, 8:11] = 1
            material, _ = rasterize(v, ix, material_textures, width, height, math.radians(angle), coverage=coverage)
            pixels = np.array(material)
            mask = pixels[:, :, 3] > 0
            ids = pixels[:, :, 0].astype(int)-1
            if reference is None:
                reference = mask.copy()
            union = np.count_nonzero(mask | reference)
            record = dict(variant=name, angle=angle, triangles=len(ix)//3,
                          visible_pixels=int(mask.sum()),
                          silhouette_iou=float(np.count_nonzero(mask & reference)/union) if union else 1.0,
                          visible_material_pixels={str(int(i)): int(np.count_nonzero(mask & (ids == i)))
                                                   for i in np.unique(ids[mask])})
            rows.append(record)
            target = (column*SIZE, row*(SIZE+36))
            sheet.paste(image, target, image.getchannel('A'))
            draw.text((target[0]+4, target[1]+SIZE+2), f'{name} / {angle:g} deg', fill='black')
            draw.text((target[0]+4, target[1]+SIZE+17), f'{len(ix)//3} tri; IoU {record["silhouette_iou"]:.2f}', fill='black')
    stem = args.model+suffix
    sheet.save(args.output/(stem+'.png'))
    (args.output/(stem+'.json')).write_text(json.dumps(rows, indent=2)+'\n')
    print(stem, '->', args.output/(stem+'.png'), flush=True)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=root/'assets/generated/scene_models')
    parser.add_argument('--before', type=Path)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--model', default='CommonTree_1')
    parser.add_argument('--angles', type=float, nargs='+', default=[0, 45, 90])
    parser.add_argument('--grove', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.assets/'manifest.json').read_text())
    model = next(m for m in manifest['models'] if m['name'] == args.model)
    textures = [np.array(Image.open(args.assets/name).convert('RGBA')) for name in manifest['colours']]
    if len(textures) > 254 or any(t.shape != (SIZE, SIZE, 4) for t in textures):
        raise ValueError('expected at most 254 RGBA textures at importer resolution')
    compare(args, textures, model, args.grove)


if __name__ == '__main__':
    main()
