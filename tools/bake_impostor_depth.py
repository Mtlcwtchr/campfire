#!/usr/bin/env python3
"""Add optional depth impostors to an existing SCM2 catalogue, without reimporting meshes.

Colour, normal and depth are baked from the SAME visible fragments. Existing
material layers and grove proxies remain untouched; manifest replacement is last.
Requires the same numpy/Pillow environment as prepare_scene_models.py.
"""
import argparse
import json
import math
import struct
import uuid
from pathlib import Path

import numpy as np
from PIL import Image
import prepare_scene_models as scene
import hemisphere_impostor as hemi

ENCODING = 'rg16-view-b-coverage-a-v1'


def resource(root, name):
    if not isinstance(name, str) or Path(name).name != name or name in ('', '.', '..'):
        raise ValueError('unsafe atlas resource name')
    return root / name


def read_mesh(path):
    data = path.read_bytes()
    if len(data) < 12:
        raise ValueError('truncated SCM2 header')
    magic, vertices, levels = struct.unpack_from('<4sII', data)
    if magic != b'SCM2' or not 0 < vertices <= 200000 or not 0 < levels <= 8:
        raise ValueError('invalid SCM2 header')
    if len(data) < 12 + levels * 4:
        raise ValueError('truncated SCM2 level table')
    counts = struct.unpack_from('<' + 'I' * levels, data, 12)
    offset = 12 + levels * 4
    if any(n == 0 or n % 3 or n > 600000 for n in counts) or sum(counts) > 900000 or len(data) != offset + vertices * 48 + sum(counts) * 4:
        raise ValueError('invalid SCM2 sizes')
    v = np.frombuffer(data, dtype='<f4', count=vertices * 12, offset=offset).reshape(-1, 12)
    indices = np.frombuffer(data, dtype='<u4', count=counts[0], offset=offset + vertices * 48)
    if not np.isfinite(v).all() or np.any(indices >= vertices):
        raise ValueError('invalid SCM2 geometry')
    return v, indices


def encode_depth(zbuf, coverage, width, view, *, views=8):
    if not math.isfinite(width) or width <= 0 or views not in (8,hemi.VIEW_COUNT) or not 0 <= view < views:
        raise ValueError('invalid depth frame')
    valid = (coverage > 0) & np.isfinite(zbuf)
    if np.any(np.abs(zbuf[valid]) > width * .5 + 1e-5):
        raise ValueError('depth exceeds the declared frame')
    normalized = np.where(valid, zbuf / width + .5, .5)
    code = np.rint(np.clip(normalized, 0, 1) * 65535).astype(np.uint16)
    packed = np.empty((*zbuf.shape, 4), dtype=np.uint8)
    packed[..., 0] = code >> 8
    packed[..., 1] = code & 255
    packed[..., 2] = view
    packed[..., 3] = np.where(valid, coverage, 0)
    return Image.fromarray(packed)


def bake(root, *, hemisphere=False):
    root = Path(root)
    manifest_path = root / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('version') != 2 or manifest.get('views') != 8:
        raise ValueError('requires a version-2 eight-view catalogue')
    colours, normals = manifest['colours'], manifest['normals']
    if not 0 < len(colours) <= 512 or len(colours) != len(normals):
        raise ValueError('invalid atlas layers')
    images = []
    for name in colours:
        with Image.open(resource(root, name)) as image:
            images.append(np.asarray(image.convert('RGBA').resize((scene.SIZE, scene.SIZE))))
    prefix = 'depth-' + uuid.uuid4().hex[:8]
    written = []

    def save(image, suffix):
        name = prefix + '-' + suffix + '.png'
        path = root / name
        image.save(path); written.append(path)
        return name

    temporary = root / (prefix + '-manifest.tmp')
    try:
        blank = save(Image.new('RGBA', (scene.SIZE, scene.SIZE), (128, 0, 0, 0)), 'empty')
        layers = [blank] * len(colours)
        occupied = set()
        protected = set()
        for grove in manifest.get('groves', []):
            first=grove['impostor']
            if not isinstance(first,int) or first<0 or first+8>len(colours):
                raise ValueError('invalid grove range')
            protected.update(range(first,first+8))
        for model in manifest['models']:
            width, height = float(model['width']), float(model['height'])
            first = int(model['impostor'])
            if not math.isfinite(width + height) or min(width, height) <= 0 or first < 0 or first + 8 > len(colours):
                raise ValueError('invalid model frame')
            if occupied.intersection(range(first, first + 8)):
                raise ValueError('overlapping impostor frames')
            occupied.update(range(first, first + 8))
            v, indices = read_mesh(resource(root, model['mesh']))
            if np.any(v[:, 11] < 0) or np.any(v[:, 11] >= len(images)) or np.any(v[:, 11] != np.floor(v[:, 11])):
                raise ValueError('invalid source texture layers')
            protected.update(int(layer) for layer in np.unique(v[:,11]))
            for view in range(8):
                # The runtime camera's right vector implies eye=(-sin, cos).
                # Re-bake all channels, not just depths of the legacy opposite side.
                colour, normal, depth = scene.rasterize(v, indices, images, width, height,
                    view * math.tau / 8, return_depth=True, eye_sign=-1)
                layer = first + view
                colours[layer] = save(colour, f'colour-{layer}')
                normals[layer] = save(normal, f'normal-{layer}')
                layers[layer] = save(encode_depth(depth, np.asarray(colour)[..., 3], width, view), f'z-{layer}')
            model['depth_impostor'] = True
            print(model['name'], '8 colour/normal/depth views', flush=True)
        # Separate ranges preserve every existing material/grove/side-view index.
        # Reusing previously allocated ranges makes repeated bakes size-stable.
        occupied.update(protected)
        for model in manifest['models']:
            previous = model.get('hemisphere_impostor')
            if not hemisphere and not previous:
                continue
            width, height = float(model['width']), float(model['height'])
            side = math.hypot(width,height)
            if previous:
                if previous.get('layout') != hemi.LAYOUT or previous.get('views') != hemi.VIEW_COUNT:
                    raise ValueError('unknown hemisphere layout')
                first = previous['first']
                if not isinstance(first,int) or first<0 or first+hemi.VIEW_COUNT>len(colours):
                    raise ValueError('invalid hemisphere range')
            else:
                first = len(colours)
                if first + hemi.VIEW_COUNT > 512:
                    raise ValueError('hemisphere atlas exceeds layer budget')
                colours.extend([blank]*hemi.VIEW_COUNT)
                normals.extend([blank]*hemi.VIEW_COUNT)
                layers.extend([blank]*hemi.VIEW_COUNT)
            if occupied.intersection(range(first,first+hemi.VIEW_COUNT)):
                raise ValueError('hemisphere overlaps an existing view range')
            occupied.update(range(first,first+hemi.VIEW_COUNT))
            v, indices = read_mesh(resource(root,model['mesh']))
            for view in range(hemi.VIEW_COUNT):
                colour, normal, depth = scene.rasterize(v,indices,images,side,side,0,
                    return_depth=True,view_basis=hemi.basis(view),origin=(0,0,height*.5))
                layer=first+view
                colours[layer]=save(colour,f'colour-{layer}')
                normals[layer]=save(normal,f'normal-{layer}')
                layers[layer]=save(encode_depth(depth,np.asarray(colour)[...,3],side,view,
                    views=hemi.VIEW_COUNT),f'z-{layer}')
            model['hemisphere_impostor']={'first':first,'views':hemi.VIEW_COUNT,'layout':hemi.LAYOUT,
                'side':side,'center_z':height*.5,'resolution':scene.SIZE}
            print(model['name'], '21 hemisphere colour/normal/depth views', flush=True)
        manifest['depth_atlas'] = {'encoding': ENCODING, 'layers': layers}
        temporary.write_text(json.dumps(manifest, indent=2) + '\n')
        temporary.replace(manifest_path)
    except Exception:
        temporary.unlink(missing_ok=True)
        for path in written:
            path.unlink(missing_ok=True)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent / 'assets/generated/scene_models')
    parser.add_argument('--hemisphere', action='store_true', help='append upper and top-down views')
    args=parser.parse_args()
    bake(args.root,hemisphere=args.hemisphere)
