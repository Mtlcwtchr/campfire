#!/usr/bin/env python3
"""Offline import of the selected static glTF 2 models; eight relightable impostors.
Uses the existing terrain-reference Python environment (numpy + Pillow).
Archives remain untouched. This deliberately rejects unsupported glTF features.

Each model also gets a level-of-detail chain from `mesh_lod`, because the source
geometry is authored for a close-up and a forest draws it at tens of pixels. The
levels share one vertex buffer - collapses land on existing vertices, and only
the grown leaf cards add any - so a model stays one upload and one binding, with
a range of the index buffer per level.
"""
import argparse
import io
import json
import math
import struct
import zipfile
from pathlib import Path, PurePosixPath

import numpy as np
from PIL import Image

import mesh_lod

MODELS = [('CommonTree_1', 1.8, True), ('Pine_1', 1.6, True),
          ('Bush_Common', 1.0, True), ('Rock_Medium_1', 2.0, False),
          ('Mushroom_Common', 1.0, False)]
SIZE = 256
# Triangle ratios for the levels below the imported mesh. Coarser steps than
# halving: two levels that reduce the same shape to the same measured error are
# one level and a wasted index range, which is what a halving chain produced
# here. The renderer picks between them by projected error, not by these ratios.
LOD_RATIOS = [0.25, 0.10, 0.04]


def rasterize(vertices, indices, textures, width, height, angle, *, coverage=None):
    """Orthographic side view with depth, alpha testing and object-space normals."""
    c, s = math.cos(angle), math.sin(angle)
    p = vertices[:, :3]
    screen = np.column_stack(((p[:, 0]*c+p[:, 1]*s)/width+.5, 1-p[:, 2]/height))*(SIZE-1)
    depth = p[:, 0]*s-p[:, 1]*c
    zbuf = np.full((SIZE, SIZE), -np.inf)
    colour = np.zeros((SIZE, SIZE, 4), dtype=np.uint8)
    normals = np.full_like(colour, 128)
    normals[:, :, 3] = 255
    for tri in indices.reshape(-1, 3):
        a, b, d = screen[tri]
        det = (b[1]-d[1])*(a[0]-d[0])+(d[0]-b[0])*(a[1]-d[1])
        if abs(det) < 1e-8:
            continue
        lo = np.maximum(0, np.floor(screen[tri].min(axis=0)).astype(int))
        hi = np.minimum(SIZE-1, np.ceil(screen[tri].max(axis=0)).astype(int))
        if np.any(hi < lo):
            continue
        yy, xx = np.mgrid[lo[1]:hi[1]+1, lo[0]:hi[0]+1]
        wa = ((b[1]-d[1])*(xx-d[0])+(d[0]-b[0])*(yy-d[1]))/det
        wb = ((d[1]-a[1])*(xx-d[0])+(a[0]-d[0])*(yy-d[1]))/det
        w = np.stack((wa, wb, 1-wa-wb), axis=-1)
        uv = w @ vertices[tri, 6:8]
        layer = int(vertices[tri[0], 11])
        texture = textures[layer]
        coords = (np.mod(uv, 1)*(SIZE-1)).astype(int)
        texel = texture[coords[:, :, 1], coords[:, :, 0]].copy()
        texel[:, :, :3] = np.clip(texel[:, :, :3]*(w @ vertices[tri, 8:11]), 0, 255)
        z = w @ depth[tri]
        region = np.s_[lo[1]:hi[1]+1, lo[0]:hi[0]+1]
        opacity = 1.0 if coverage is None else w @ coverage[tri]
        mask = (w.min(axis=-1) >= -1e-5) & (z > zbuf[region]) & (texel[:, :, 3]*opacity >= 51)
        normal = w @ vertices[tri, 3:6]
        normal /= np.maximum(np.linalg.norm(normal, axis=-1, keepdims=True), 1e-8)
        zbuf[region][mask] = z[mask]
        colour[region][mask] = texel[mask]
        normals[region][mask, :3] = np.clip((normal[mask]*.5+.5)*255, 0, 255)
    # Dilate RGB into transparent texels to avoid black fringes in mipmaps.
    for _ in range(4):
        filled = colour[:, :, 3] > 0
        for axis, offset in ((0, 1), (0, -1), (1, 1), (1, -1)):
            neighbour = np.roll(colour, offset, axis=axis)
            mask = ~filled & (neighbour[:, :, 3] > 0)
            colour[mask, :3] = neighbour[mask, :3]
    return Image.fromarray(colour), Image.fromarray(normals)


def prepare(archive, output):
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'version': 2, 'views': 8, 'models': [], 'groves': [], 'colours': [], 'normals': [],
                'source': archive.name, 'author': 'Quaternius', 'license': 'CC0-1.0'}
    images, layers = [], {}
    with zipfile.ZipFile(archive) as source:
        (output/'LICENSE.txt').write_bytes(source.read('License_Standard.txt'))
        def image(uri):
            path = PurePosixPath('glTF')/uri
            if '..' in path.parts or path.is_absolute():
                raise ValueError('unsafe image path')
            return Image.open(io.BytesIO(source.read(str(path)))).convert('RGBA').resize((SIZE, SIZE), Image.Resampling.LANCZOS)
        def add_layer(key, colour, normal):
            if key in layers:
                return layers[key]
            n = len(images)
            colour.save(output/f'colour-{n}.png')
            normal.save(output/f'normal-{n}.png')
            manifest['colours'].append(f'colour-{n}.png')
            manifest['normals'].append(f'normal-{n}.png')
            images.append(np.array(colour))
            layers[key] = n
            return n
        for name, scale, vegetation in MODELS:
            doc = json.loads(source.read(f'glTF/{name}.gltf'))
            if doc['asset']['version'] != '2.0' or len(doc['nodes']) != 1:
                raise ValueError('only single static nodes are supported')
            node = doc['nodes'][0]
            if any(k in node for k in ('matrix', 'rotation', 'scale', 'translation', 'skin', 'children')):
                raise ValueError('node transform must be baked by the author')
            def accessor(n):
                acc = doc['accessors'][n]
                if 'sparse' in acc or acc.get('normalized', False):
                    raise ValueError('unsupported accessor')
                view = doc['bufferViews'][acc['bufferView']]
                uri = doc['buffers'][view['buffer']]['uri']
                if PurePosixPath(uri).name != uri:
                    raise ValueError('unsafe buffer path')
                data = source.read('glTF/'+uri)
                dtype = {5126: '<f4', 5123: '<u2', 5125: '<u4'}[acc['componentType']]
                components = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[acc['type']]
                item = np.dtype(dtype).itemsize
                stride = view.get('byteStride', components*item)
                offset = view.get('byteOffset', 0)+acc.get('byteOffset', 0)
                return np.ndarray((acc['count'], components), dtype=dtype, buffer=data,
                                  offset=offset, strides=(stride, item)).copy()
            vertices, indices, count = [], [], 0
            for primitive in doc['meshes'][node['mesh']]['primitives']:
                if primitive.get('mode', 4) != 4:
                    raise ValueError('triangles required')
                material = doc['materials'][primitive['material']]
                pbr = material['pbrMetallicRoughness']
                def uri(texture):
                    return doc['images'][doc['textures'][texture['index']]['source']]['uri']
                colour_uri = uri(pbr['baseColorTexture'])
                normal_uri = uri(material['normalTexture']) if 'normalTexture' in material else ''
                layer = add_layer((colour_uri, normal_uri), image(colour_uri),
                                  image(normal_uri) if normal_uri else Image.new('RGBA', (SIZE, SIZE), (128, 128, 255, 255)))
                attrs = primitive['attributes']
                p, normal, uv = (accessor(attrs[k]) for k in ('POSITION', 'NORMAL', 'TEXCOORD_0'))
                # glTF Y-up -> right-handed Z-up. Preserve metres and winding.
                p = p[:, [0, 2, 1]]*np.array([1, -1, 1])*scale
                normal = normal[:, [0, 2, 1]]*np.array([1, -1, 1])
                colour = accessor(attrs['COLOR_0'])[:, :3] if 'COLOR_0' in attrs else np.ones_like(p)
                colour *= np.array(pbr.get('baseColorFactor', [1, 1, 1, 1]))[:3]
                vertices.append(np.column_stack((p, normal, uv, colour, np.full(len(p), layer))))
                index = accessor(primitive['indices']).reshape(-1)
                if len(index)%3 or np.any(index >= len(p)):
                    raise ValueError('invalid indices')
                indices.append(index.astype(np.uint32)+count)
                count += len(p)
            v = np.concatenate(vertices).astype('<f4')
            ind = np.concatenate(indices).astype('<u4')
            v[:, 2] -= v[:, 2].min()
            if not np.isfinite(v).all():
                raise ValueError('invalid geometry')
            # The imported mesh is level zero and the impostors are baked from
            # it, so the coarser levels below never change what a card looks
            # like - only how much geometry a mid-distance object costs.
            grown, coarser = mesh_lod.build_levels(v, ind, LOD_RATIOS)
            buffer = np.concatenate((v, grown)).astype('<f4') if len(grown) else v
            # A shared, unchanging frame for all azimuths avoids billboard
            # pumping, and it has to hold every level: a grown leaf card can
            # reach past the imported silhouette, and a card clipped by the
            # impostor frame would be one size on the mesh and another on the
            # billboard. The impostor is still baked from level zero.
            width = float(np.linalg.norm(buffer[:, :2], axis=1).max()*2.04)
            height = float(buffer[:, 2].max()*1.02)
            if width <= 0 or height <= 0:
                raise ValueError('invalid geometry')
            levels = [{'indices': ind, 'triangles': len(ind)//3, 'cards': -1,
                       'error': (0.0, 0.0, 0.0)}] + coarser
            with (output/f'{name}.mesh').open('wb') as f:
                f.write(struct.pack('<4sII', b'SCM2', len(buffer), len(levels)))
                for level in levels:
                    f.write(struct.pack('<I', len(level['indices'])))
                f.write(buffer.tobytes())
                for level in levels:
                    f.write(level['indices'].astype('<u4').tobytes())
            first = len(images)
            for view in range(8):
                colour, normal = rasterize(v, ind, images, width, height, view*math.tau/8)
                add_layer((name, view), colour, normal)
            manifest['models'].append({'name': name, 'mesh': name+'.mesh', 'width': width,
                                       'height': height, 'vegetation': vegetation,
                                       'impostor': first, 'vertices': len(buffer),
                                       'triangles': len(ind)//3,
                                       # `error_m` is the 95th percentile distance from the
                                       # imported surface, which is what the renderer projects
                                       # to pixels; the outlier and the RMS are kept beside it
                                       # so a level cannot be judged by one number alone.
                                       'levels': [{'triangles': level['triangles'],
                                                   'cards': level['cards'],
                                                   'error_m': level['error'][1],
                                                   'error_max_m': level['error'][0],
                                                   'error_rms_m': level['error'][2]}
                                                  for level in levels]})
            print(name, ' + '.join(str(level['triangles']) for level in levels),
                  'triangles; 8 impostor views', flush=True)
            if name in ('CommonTree_1', 'Pine_1'):
                # Nine real tree silhouettes, not one tree stretched into a giant.
                grove_width, grove_height = width+24.0, height*1.12
                grove_first = len(images)
                positions = [(x*7.5+(y%2)*1.3, y*7.5, 0.86+((x+2*y)%5)*0.06)
                             for y in (-1, 0, 1) for x in (-1, 0, 1)]
                for view in range(8):
                    angle = view*math.tau/8
                    colour = Image.new('RGBA', (SIZE, SIZE))
                    normal = Image.new('RGBA', (SIZE, SIZE), (128, 128, 255, 0))
                    tree = Image.fromarray(images[first+view])
                    tree_normal = Image.open(output/f'normal-{first+view}.png').convert('RGBA')
                    tree_normal.putalpha(tree.getchannel('A'))
                    for x, y, factor in sorted(positions, key=lambda p: p[0]*math.sin(angle)-p[1]*math.cos(angle)):
                        w = max(1, round(SIZE*width*factor/grove_width))
                        h = max(1, round(SIZE*height*factor/grove_height))
                        at = (round(SIZE*(0.5+(x*math.cos(angle)+y*math.sin(angle))/grove_width)-w/2), SIZE-h)
                        colour.alpha_composite(tree.resize((w,h),Image.Resampling.LANCZOS),at)
                        normal.alpha_composite(tree_normal.resize((w,h),Image.Resampling.BILINEAR),at)
                    add_layer((name, 'grove', view), colour, normal)
                manifest['groves'].append({'model': len(manifest['models'])-1,
                    'impostor': grove_first, 'width': grove_width, 'height': grove_height,
                    'trees': len(positions)})
    (output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    root = Path(__file__).resolve().parent.parent
    parser.add_argument('--archive', type=Path, default=root/'assets/models/Stylized Nature MegaKit[Standard].zip')
    parser.add_argument('--output', type=Path, default=root/'assets/generated/scene_models')
    args = parser.parse_args()
    prepare(args.archive, args.output)

