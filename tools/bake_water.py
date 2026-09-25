#!/usr/bin/env python3
"""Bakes the water surface into the three layers the water shader reads.

Different from the ground bakery next door, and the difference is the point: the
ground has its light baked in because the ground does not move, and water is
nothing but movement. What a shader needs from water is the *shape* of the
surface - which way each ripple faces - so the light can be worked out at the
moment the ripple is under it. So no lighting is baked here. The normal maps are
kept as normal maps.

Three layers of one array, because the surface is three things at once:

  ripple  Water/1  - long streaks, the fine grain the wind drags across the top
  swell   Water/3  - broad cells, the slow shape the whole surface has
  foam    Water/7  - the white net that gathers at the shore

The two normal maps are scrolled past each other in opposite directions in the
shader; that is what makes a surface look alive rather than like a photograph of
water. The foam layer carries its mask in the alpha channel: the white lines of
the painting, stretched, so the shader can decide where foam is rather than
fading a white wash in.

    python3 tools/bake_water.py

Reads assets/sprites/raw_srcs/Water/, writes assets/sprites/water/.
"""
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from bake_materials import SIZE, SOURCE, seams

OUT = Path('assets/sprites/water')

# Which pack each layer comes from. Named here rather than guessed, because
# "the water textures" is nine painted variants of very different things and
# only these three do these three jobs.
LAYERS = (
    ('ripple', '1', 'the fine grain the wind drags across the top'),
    ('swell', '3', 'the broad cells the whole surface has'),
    ('foam', '7', 'the white net that gathers at the shore'),
)


def load(variant, kind):
    """One map of one variant of the water pack.

    Its own loader rather than the ground bakery's, for one reason: the first
    variant's directory is named `1` and its files are named `1+_normal.bmp`.
    Whatever that plus meant to whoever packed it, a file name here is looked
    for rather than assumed.
    """
    base = SOURCE / 'Water' / variant
    for prefix in (variant, variant + '+'):
        for ext in ('bmp', 'png'):
            path = base / f'{prefix}{kind}.{ext}'
            if path.exists():
                return Image.open(path).convert('RGB').resize((SIZE, SIZE), Image.LANCZOS)
    raise SystemExit(f'missing {base}/{variant}{kind}.(bmp|png)')


def normalMap(variant):
    """The normal map as a direction, green negated.

    The same turn as the ground bakery: these sets are written the OpenGL way
    round and this world is drawn from above with y running south, so the green
    channel is flipped to put a ripple's lit side to the north. If the two
    disagree, the water catches the light from one side and the shore it laps
    against from the other.
    """
    raw = np.asarray(load(variant, '_normal')).astype(np.float32) / 255
    n = np.stack([raw[:, :, 0] * 2 - 1,
                  -(raw[:, :, 1] * 2 - 1),
                  raw[:, :, 2] * 2 - 1], axis=2)
    return n / np.maximum(np.sqrt((n * n).sum(axis=2, keepdims=True)), 1e-6)


def stretched(channel):
    """A map spread across the whole range it has room for.

    The painted maps use a third of the range - the height of a ripple in a
    pool is not the height of a wave - and a shader multiplying that by a
    strength it can tune wants the whole of it.
    """
    low, high = float(channel.min()), float(channel.max())
    return (channel - low) / max(1e-6, high - low)


def foamMask(variant):
    """Where the foam is, from the painting itself.

    The water is blue and the foam is white, so the red channel alone tells them
    apart: blue paint has little red in it and white paint is all three. Taken
    off the diffuse rather than the height map because foam is not where the
    surface is high - it is where it has broken - and the painter drew that.
    """
    diffuse = np.asarray(load(variant, '_diffuseOriginal')).astype(np.float32) / 255
    red = diffuse[:, :, :1]
    # Stretched between two percentiles rather than between the darkest and
    # brightest pixel in the painting. Between the extremes, the net came out at
    # a fifth of full scale - the white lines are a small part of the picture and
    # the rest of it is blue - and a shader multiplying that up again multiplies
    # the blue with it. Where the ends are put is what decides how much of the
    # painting is foam: a fifth of it here.
    low = float(np.percentile(red, 45))
    high = float(np.percentile(red, 90))
    mask = np.clip((red - low) / max(1e-6, high - low), 0, 1)
    # And with its own edges kept: a net with soft sides reads as steam.
    return np.clip(mask * mask * (3 - 2 * mask), 0, 1)


def encode(n, alpha):
    """A direction and a number, as the four channels of a texture."""
    rgb = np.clip(n * 0.5 + 0.5, 0, 1)
    return np.concatenate([rgb, np.clip(alpha, 0, 1)], axis=2)


def smaller(packed, step, keepThin=False):
    """One step down the mip chain, with the direction kept a direction.

    Averaging four encoded normals gives a vector that is no longer unit
    length, and a shader that trusts it to be one lights the water wrong at
    every distance but the nearest. Decoded, averaged, normalised, encoded
    again - which is also why this is not four calls to PIL's resize.

    `keepThin` is for the foam, and it is the difference between foam that is
    there at every zoom and foam that is only there close up. The net is thin -
    a few texels to a line - and a line a few texels wide, averaged down four
    times, is a line a quarter of a texel wide and a grey wash is what comes
    out. Read from up high, that is a sea with no surf on it at all. Taken as
    mostly the brightest of the four instead, the line stays a line and only
    grows: which is what a coverage mask wants and what an average never gives
    it.
    """
    side = SIZE // step
    block = packed.reshape(side, step, side, step, 4)
    mean = block.mean(axis=(1, 3))
    n = mean[:, :, :3] * 2 - 1
    n = n / np.maximum(np.sqrt((n * n).sum(axis=2, keepdims=True)), 1e-6)
    alpha = mean[:, :, 3:]
    if keepThin:
        alpha = np.maximum(alpha, 0.80 * block[:, :, :, :, 3:].max(axis=(1, 3)))
    return np.concatenate([n * 0.5 + 0.5, np.clip(alpha, 0, 1)], axis=2)


def main():
    if not (SOURCE / 'Water').exists():
        raise SystemExit(f'no water pack at {SOURCE / "Water"}')
    OUT.mkdir(parents=True, exist_ok=True)

    written = {}
    for name, variant, what in LAYERS:
        n = normalMap(variant)
        if name == 'foam':
            alpha = foamMask(variant)
        else:
            alpha = stretched(np.asarray(load(variant, '_height'))
                              .astype(np.float32)[:, :, :1] / 255)
        packed = encode(n, alpha)
        image = Image.fromarray((packed * 255).astype(np.uint8))
        across, down = seams(image)
        image.save(OUT / f'{name}.png', optimize=True)
        for step in (2, 4, 8, 16):
            Image.fromarray((smaller(packed, step, name == 'foam') * 255).astype(np.uint8)) \
                 .save(OUT / f'{name}@{step}.png', optimize=True)
        written[name] = (across, down)
        print(f'{name:7s} from Water/{variant:2s} -> {OUT / (name + ".png")}  '
              f'edges differ by {across * 100:.1f}% across, {down * 100:.1f}% down'
              + ('   NOT TILEABLE' if max(across, down) > 0.09 else '')
              + f'   ({what})')

    manifest = {
        'format_version': 1,
        '_comment': ('The water surface, baked by tools/bake_water.py. RGB is a '
                     'tangent-space normal with green negated for a world drawn '
                     'from above; alpha is the surface height, except on the foam '
                     'layer where it is the foam mask. No lighting is baked in: '
                     'water moves, so the light is worked out where the ripple is.'),
        'layers': [name for name, _, _ in LAYERS],
        'size': SIZE,
    }
    (OUT / 'water.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'wrote {len(written)} water layers at {SIZE}x{SIZE}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
