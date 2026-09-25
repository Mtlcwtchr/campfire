#!/usr/bin/env python3
"""Bakes the ground materials into single textures the renderer can use.

The material packs come as five maps apiece - albedo, normal, ambient
occlusion, specular, displacement - which is what a shader eats. The terrain is
drawn through SDL_Renderer, which has no shader: it can multiply a texture by a
colour per vertex and nothing else.

So the shading is done here instead, once, and baked in. The sun in this world
does not move, so lighting the surface detail at tool time gives the same answer
as lighting it every frame would: the grain of the grass, the pits in the rock
and the ruts in the dirt all catch the light from the north-west, and the
renderer then multiplies the whole thing by how much light the hillside itself
receives. What is lost against a real shader is that the detail cannot relight
when the sun moves, which it does not, and that it cannot respond to the slope,
which the vertex colour already does.

    python3 tools/bake_materials.py

Reads assets/sprites/FreeRealisticOutdoorMaterials/, writes assets/sprites/ground/
and adds the results to the sprite manifest on the next extract_sprites run.
"""
import json
import sys
from pathlib import Path

import json

import numpy as np
from PIL import Image

SOURCE = Path('assets/sprites/raw_srcs')
OUT = Path('assets/sprites/ground')

# The sun, as the renderer has it: from the north-west and fairly high. Kept in
# one place here and in client/explorer.cpp; if they disagree the baked grain is
# lit from one side and the hillside from the other, which reads as the ground
# being made of something shiny.
SUN = np.array([-0.55, -0.55, 0.63])
SUN = SUN / np.linalg.norm(SUN)

# How far the baked lighting is allowed to go.
#
# Gently, and more gently than it was: these materials are painted rather than
# photographed, and a painted material already has its light in it - the artist
# put the shadow under every pebble. Lighting it again from the normal map adds
# a second sun, and two suns on one pebble is mud. What is left of it is there
# to agree with the hillside's own shading, not to model the grain.
AMBIENT = 0.82
DIRECT = 0.26
# And the same for the ambient occlusion, which is likewise already painted in.
OCCLUSION = 0.45

# The size the game actually uses. The packs are four thousand pixels square,
# which is a hundred and eighty times more than a twelve-metre patch of ground
# ever shows on screen, and sixteen of them would be a gigabyte of texture.
SIZE = 512

# What the ground is made of comes from content/config/ground.json - the same
# file the renderer reads every frame. Two ends of one decision: this end turns
# a painting into a texture, that end decides how one material meets the next,
# and split across two files they drift apart.
GROUND = Path('content/config/ground.json')


def materials():
    with open(GROUND) as f:
        table = json.load(f)
    return [(m['name'], m['set'], m.get('variant', '1'),
             tuple(m.get('tint', (1.0, 1.0, 1.0))), float(m.get('keep_colour', 1.0)))
            for m in table if 'name' in m]


def load(pack, variant, kind):
    """One map of one material. The sets are not consistent about the format -
    some diffuses are png and some bmp - so both are tried rather than one being
    assumed and the whole bake failing on the third material."""
    base = SOURCE / pack / variant
    for ext in ('png', 'bmp'):
        path = base / f'{variant}{kind}.{ext}'
        if path.exists():
            return Image.open(path).convert('RGB').resize((SIZE, SIZE), Image.LANCZOS)
    raise SystemExit(f'missing {base}/{variant}{kind}.(png|bmp)')


def seams(image):
    """How different the opposite edges are, as a fraction of full scale.

    A material is meant to tile. If its left edge does not match its right, the
    ground gets a grid of hard lines at whatever spacing the texture is drawn
    at - which is the same artefact as a visible lattice, arriving from the
    other end.
    """
    a = np.asarray(image).astype(np.int16)
    across = np.abs(a[:, 0] - a[:, -1]).mean() / 255
    down = np.abs(a[0, :] - a[-1, :]).mean() / 255
    return across, down


def main():
    if not SOURCE.exists():
        raise SystemExit(f'no material packs at {SOURCE}')
    OUT.mkdir(parents=True, exist_ok=True)

    written = {}
    jobs = materials()
    for name, pack, variant, tint, keepColour in jobs:
        albedo = np.asarray(load(pack, variant, '_diffuseOriginal')).astype(np.float32) / 255
        occlusion = np.asarray(load(pack, variant, '_ao')).astype(np.float32)[:, :, :1] / 255
        normals = np.asarray(load(pack, variant, '_normal')).astype(np.float32) / 255
        relief = np.asarray(load(pack, variant, '_height')).astype(np.float32)[:, :, :1] / 255
        relief = (relief - relief.min()) / max(1e-6, float(relief.max() - relief.min()))

        # The normal map, from its stored form back to a direction. Green up:
        # these sets are written the OpenGL way round, and the world is drawn
        # from above with y running south, so the green channel is negated to
        # put a bump's lit side to the north.
        n = np.stack([normals[:, :, 0] * 2 - 1,
                      -(normals[:, :, 1] * 2 - 1),
                      normals[:, :, 2] * 2 - 1], axis=2)
        n = n / np.maximum(np.sqrt((n * n).sum(axis=2, keepdims=True)), 1e-6)
        # Only part of the occlusion, because the painting has its own.
        occlusion = 1.0 - (1.0 - occlusion) * OCCLUSION

        # Drained of its own colour, where the material is standing in for
        # something the packs do not have. Multiplying a brown by a grey gives a
        # darker brown, not a stone: what makes a photograph read as rock is
        # losing the hue, not dimming it.
        if keepColour < 1.0:
            grey = (albedo[:, :, :1] * 0.30 + albedo[:, :, 1:2] * 0.59 + albedo[:, :, 2:3] * 0.11)
            albedo = albedo * keepColour + grey * (1 - keepColour)

        lit = AMBIENT + DIRECT * np.clip((n * SUN).sum(axis=2, keepdims=True), 0, 1)
        baked = np.clip(albedo * occlusion * lit * np.array(tint, dtype=np.float32), 0, 1)

        # The height of the material, in the alpha channel.
        #
        # Nothing draws with it as transparency - the ground is opaque - and that
        # is exactly why the channel is free. What reads it is the blend between
        # two materials: where dirt meets grass, the dirt should be lying in the
        # hollows of the grass and the grass standing on the ridges, which is
        # what ground does. Without a height per material the only thing a
        # blend can do is fade one into the other evenly, and an even fade
        # between two textures does not look like either of them - it looks like
        # soap.
        withHeight = np.concatenate([baked, np.clip(relief, 0, 1)], axis=2)
        image = Image.fromarray((withHeight * 255).astype(np.uint8))
        across, down = seams(image)
        destination = OUT / f'{name}.png'
        image.save(destination, optimize=True)
        # And smaller copies of it.
        #
        # SDL's renderer has no mipmaps: a texture drawn at a fifteenth of its
        # size is point-sampled down to a shimmer, and a hillside covered in it
        # reads as a smooth wash rather than as ground. The renderer picks the
        # size whose texels are about the size of the screen's pixels, which is
        # what a mipmap chain would have done for it.
        for step in (2, 4, 8, 16):
            smaller = image.resize((SIZE // step, SIZE // step), Image.LANCZOS)
            smaller.save(OUT / f'{name}@{step}.png', optimize=True)
        written[name] = (across, down)
        print(f'{name:6s} from {(pack or "noise"):12s} -> {destination}  '
              f'edges differ by {across * 100:.1f}% across, {down * 100:.1f}% down'
              + ('   NOT TILEABLE' if max(across, down) > 0.09 else ''))

    manifest = {
        'format_version': 1,
        '_comment': ('Ground materials, baked by tools/bake_materials.py from the '
                     'material packs: albedo times ambient occlusion times the '
                     'surface lit by this world\'s fixed sun. The renderer '
                     'multiplies these by how much light the hillside itself gets.'),
        'materials': sorted(written),
        'size': SIZE,
    }
    (OUT / 'ground.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'wrote {len(written)} ground materials at {SIZE}x{SIZE}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
