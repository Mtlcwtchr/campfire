#!/usr/bin/env python3
"""Prepare downloaded moss/debris through the existing terrain map packer.

Keeps archive metadata, provenance and opacity. Writes AO/roughness/height/mask
plus renormalized normal mipmaps, in the same format the terrain already loads.
    python3 tools/prepare_environment_textures.py
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import zipfile

import numpy as np
from PIL import Image
import pack_terrain

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "content/config/environment_textures.json"


def prepare(entry, downloads, size):
    archive = downloads / entry["archive"]
    folder = ROOT / "assets/terrain/environment" / entry["id"]
    source = folder / "src"
    source.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as zipped:
        notes = [n for n in zipped.namelist() if n.endswith('.json')]
        if len(notes) != 1:
            raise ValueError(str(archive) + ': expected one scan metadata file')
        metadata = json.loads(zipped.read(notes[0]))
        if metadata['id'].lower() != entry['asset_id'].lower():
            raise ValueError(str(archive) + ': scan id disagrees with configuration')
        (source / "scan.json").write_text(json.dumps(metadata, indent=2) + '\n')
        names = {"diffuse": "BaseColor", "normal": "Normal", "mask": "Opacity",
                 "ao": "AO", "rough": "Roughness", "displacement": "Displacement"}
        files = {}
        for target, scan in names.items():
            candidates = [n for n in zipped.namelist() if Path(n).stem.lower().endswith('_' + scan.lower())]
            if not candidates:
                if target in ('diffuse', 'normal', 'mask'):
                    raise ValueError(str(archive) + ': missing ' + scan)
                continue
            if len(candidates) != 1:
                raise ValueError(str(archive) + ': ambiguous ' + scan)
            name = candidates[0]
            raw = zipped.read(name)
            file = entry['id'] + '_' + target + '.png'
            image = Image.open(io.BytesIO(raw))
            if target == 'normal':
                pixels = np.array(image.convert('RGB'))
                if entry['normal_convention'] == 'dx':
                    pixels[..., 1] = 255 - pixels[..., 1]
                elif entry['normal_convention'] != 'gl':
                    raise ValueError('normal_convention must be dx or gl')
                image = Image.fromarray(pixels)
            image.save(source / file)
            files[target] = {"file": file, "original": name, "sha256": hashlib.sha256(raw).hexdigest()}
    provenance = {"site": "fab", "asset_id": metadata['id'], "asset_url": entry['url'],
                  "license": "Fab / original Quixel asset licence (not CC0)",
                  "archive": str(archive.relative_to(ROOT)) if archive.is_relative_to(ROOT) else str(archive),
                  "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                  "original_physical_metres": [entry['metres'], entry['metres']],
                  "source_normal_convention": entry['normal_convention'], "normal_convention": "gl", "files": files}
    (source / 'source.json').write_text(json.dumps(provenance, indent=2) + '\n')
    pack_terrain.pack({"id": entry['id'], "group": 'environment/' + entry['id'],
                       "world_scale": entry['metres']}, size, False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--downloads', type=Path, default=ROOT / 'assets/downloaded')
    # TerrainPass uses 2048 colour / 1024 normal+properties by default.
    parser.add_argument('--size', type=int, choices=[512, 1024, 2048], default=2048)
    args = parser.parse_args()
    for entry in json.loads(CONFIG.read_text())['textures']:
        prepare(entry, args.downloads.resolve(), args.size)
