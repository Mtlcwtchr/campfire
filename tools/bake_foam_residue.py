#!/usr/bin/env python3
"""Bake the PHX residue layer only; existing water normals/foam stay untouched.

Requires Pillow and numpy, like bake_water.py. Run from any directory.
"""
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'assets/PHX_Alphas_LITE/Details'
OUT = ROOT / 'assets/sprites/water'


def stretch(mask):
    low, high = np.percentile(mask, [20, 95])
    return np.clip((mask - low) / max(float(high - low), 1e-6), 0, 1)


def tileable(mask):
    mask = mask.copy()
    # Match opposing borders without reflecting the whole texture into a grid.
    for axis in (0, 1):
        view = mask if axis == 0 else mask.T
        band = min(12, view.shape[0] // 4)
        for i in range(band):
            a, b = view[i].copy(), view[-i - 1].copy()
            weight = 0.5 * (1 - i / band) ** 2
            view[i] = a * (1 - weight) + b * weight
            view[-i - 1] = b * (1 - weight) + a * weight
    return mask


def main():
    # All four array layers must have identical dimensions and mip counts.
    with Image.open(OUT / 'foam.png') as image:
        size = image.size
    fields = []
    names = ('QuantumFoam.png', 'Bubbles.png')
    for name in names:
        with Image.open(SOURCE / name) as image:
            fields.append(np.asarray(image.convert('L').resize(size, Image.Resampling.LANCZOS),
                                     dtype=np.float32) / 255)
    # QuantumFoam supplies porous patches; inverted Bubbles supplies thin rims,
    # rather than the almost solid white interior of the original alpha.
    mask = tileable(stretch(fields[0]) * 0.70 + stretch(1 - fields[1]) * 0.30)
    alpha = Image.fromarray(np.rint(mask * 255).astype(np.uint8))
    for step in (1, 2, 4, 8, 16):
        dimensions = (size[0] // step, size[1] // step)
        level = alpha.resize(dimensions, Image.Resampling.BOX)
        packed = Image.new('RGBA', dimensions, (128, 128, 255, 255))
        packed.putalpha(level)
        suffix = '' if step == 1 else f'@{step}'
        packed.save(OUT / f'residue{suffix}.png', optimize=True)
    manifest = {
        'layer': 'residue', 'channel': 'alpha', 'size': list(size),
        'sources': [str((SOURCE / name).relative_to(ROOT)) for name in names],
        'processing': '70% stretched QuantumFoam + 30% inverted Bubbles; periodic borders; box mips',
        'provenance': 'User-supplied PHX_Alphas_LITE; no license file found in the supplied folder. Verify redistribution terms.',
        'generator': 'python3 tools/bake_foam_residue.py',
    }
    (OUT / 'residue_sources.json').write_text(json.dumps(manifest, indent=2) + '\n')
    water_path = OUT / 'water.json'
    water = json.loads(water_path.read_text())
    water['layers'] = ['ripple', 'swell', 'foam', 'residue']
    water['residue_manifest'] = 'residue_sources.json'
    water_path.write_text(json.dumps(water, indent=2) + '\n')
    print(f'Baked residue: {size[0]}x{size[1]}, 5 mip levels, independent PHX mask')


if __name__ == '__main__':
    main()
