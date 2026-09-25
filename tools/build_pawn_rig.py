#!/usr/bin/env python3
"""Builds the pawn rig out of assets/sprites/source/pawn_layers.png.

The sheet is a catalogue of components, not a rig: head-and-torso, arms, a kilt
and a robe, each drawn three times for south, east and north. Nothing in it says
where the pieces meet. This script decides that once, here, and writes atlases
that share one pivot - so the renderer draws every layer into the same rectangle
and the composition is correct by construction.

That matters because the alternative was tried and failed: measuring rectangles
out of an unrigged sheet by hand and scaling each part to the body's height put a
head on a stretched skirt with no torso between them, and swapped the tunic with
the kilt.

    python3 tools/build_pawn_rig.py
"""

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from sprite_grid import grid, keyed, row_bands
import numpy as np
from PIL import Image

SHEET = Path('assets/sprites/source/pawn_layers.png')
OUT = Path('assets/sprites/pawn')

# The garment sheets are the same drawings as the rig's own robe row, continued:
# three views to a row in the rig's own order, and a row for each thing somebody
# might be wearing. They are hung on the body exactly the way the robe is, so a
# new garment is a row here and nothing else.
GARMENT_SHEETS = {
    'woven': Path('assets/sprites/source/garments_woven.png'),
    'reed': Path('assets/sprites/source/garments_reed.png'),
}
GARMENT_ROWS = 8
# Layer name -> (sheet, row). The kilt and the robe stay where they were: they
# come off the rig sheet and are what a community starts out wearing.
GARMENTS = {
    'garment_tunic': ('woven', 0),
    'garment_wool': ('woven', 4),
    'garment_fine': ('woven', 7),
    'garment_hide': ('reed', 3),
}
# What a person is wearing, drawn. A garment nothing maps to is never seen.
CONTENT_MAPPING = {
    'loincloth': 'garment_kilt',
    'linen_tunic': 'garment_tunic',
    'wool_cloth': 'garment_wool',
    'felt_cloak': 'garment_wool',
    'hide_cloak': 'garment_hide',
}

CELL = 512
PIVOT = (256, 456)
# Child, adult, elder. A child is smaller; an elder has lost a little height.
AGE_SCALE = [0.72, 1.0, 0.94]
# The head is cut off the torso at the empty band between them, so both pieces
# carry that gap; put it back by overlapping them.
NECK_OVERLAP = 16
# Where on the torso the arms attach, as a fraction of its height. The drawing
# puts a pair of shoulder stumps about a third of the way down; hung any higher
# the arms read as wings sticking up past the head.
ARM_ATTACH = 0.24
ARM_INSET = 6          # how far the arm's centre sits inside the shoulder stump
KILT_LIFT = 20         # a kilt stops short of the ankles
# A garment has to be at least as wide as the body it is wrapped round. Seen from
# the side the sheet draws both of them narrower than the torso, which left the
# figure's chest sticking out of its own clothes.
GARMENT_MARGIN = 1.06


def split_head(piece):
    """The sheet draws head and torso in one cell; separate them."""
    alpha = piece.getchannel('A').load()
    w, h = piece.size
    band = [y for y in range(h)
            if 0.2 * h < y < 0.6 * h and all(alpha[x, y] < 24 for x in range(w))]
    if not band:
        raise SystemExit('no empty band between head and torso - has the sheet changed?')
    cut = band[len(band) // 2]
    return piece.crop((0, cut, w, h)), piece.crop((0, 0, w, cut))


def main():
    im, _mask, rows = grid(str(SHEET), min_size=30, gap=10, min_cell=40)
    if len(rows) < 4:
        raise SystemExit(f'expected four rows of components, found {len(rows)}')

    def cut(cell):
        x, y, w, h = cell
        return im.crop((x, y, x + w, y + h))

    torsos, heads = [], []
    for d in range(3):
        torso, head = split_head(cut(rows[0][d]))
        torsos.append(torso)
        heads.append(head)
    arms = rows[1]
    kilts = [cut(c) for c in rows[2]]
    robes = [cut(c) for c in rows[3]]

    # The garment sheets are three equal columns across; the rows are not equal,
    # because a long robe and a short kilt are drawn at the height each needs.
    # So the columns are cut by fraction and the rows are found by looking.
    def garment_row(sheet, row):
        image = keyed(Image.open(GARMENT_SHEETS[sheet]).convert('RGBA'))
        width = image.width // 3
        views = []
        for column in range(3):
            strip = image.crop((column * width, 0, (column + 1) * width, image.height))
            mask = np.asarray(strip.getchannel('A')) > 24
            found = row_bands(mask, GARMENT_ROWS)
            if len(found) != GARMENT_ROWS:
                raise SystemExit(f'{sheet}: found {len(found)} rows of garments, '
                                 f'expected {GARMENT_ROWS}')
            top, bottom = found[row]
            cell = strip.crop((0, top, strip.width, bottom))
            box = cell.getchannel('A').point(lambda v: 255 if v > 24 else 0).getbbox()
            views.append(cell.crop(box) if box else cell)
        return views

    extra = {name: garment_row(*where) for name, where in GARMENTS.items()}

    # Every layer is anchored on the shoulder line, the one place they all agree:
    # the robe hangs from it, the torso begins at it, the head sits on it.
    shoulder = PIVOT[1] - robes[0].height

    def shoulder_half(d):
        """Half the width of the torso at its widest row: the shoulder line."""
        torso = torsos[d]
        alpha = torso.getchannel('A').load()
        w, h = torso.size
        widest = 0
        for y in range(h):
            xs = [x for x in range(w) if alpha[x, y] > 40]
            if xs:
                widest = max(widest, xs[-1] - xs[0] + 1)
        return widest / 2

    def widen(garment, torso):
        wanted = torso.width * GARMENT_MARGIN
        if garment.width >= wanted:
            return garment
        factor = wanted / garment.width
        return garment.resize((int(garment.width * factor), garment.height), Image.LANCZOS)

    def blank():
        return Image.new('RGBA', (CELL, CELL), (0, 0, 0, 0))

    def place(canvas, piece, cx, top):
        canvas.alpha_composite(piece, (int(cx - piece.width / 2), int(top)))

    def arm_pieces(d):
        return {0: (0, 1), 1: (2,), 2: (3, 4)}[d]

    def layer_cell(name, d):
        c = blank()
        if name == 'body':
            place(c, torsos[d], PIVOT[0], shoulder)
        elif name == 'head':
            place(c, heads[d], PIVOT[0], shoulder - heads[d].height + NECK_OVERLAP)
        elif name == 'arms':
            pair = arm_pieces(d)
            for i, index in enumerate(pair):
                arm = cut(arms[index])
                # Measured, not guessed. The torso is drawn with a pair of
                # shoulder stumps where the arms belong; the widest row of the
                # silhouette is exactly that line, and the arm hangs from it with
                # its centre just inside the stump's tip. Placing them by
                # fractions of the torso's width put them above the shoulders
                # like wings, or hid one of them behind the chest.
                reach = shoulder_half(d) - ARM_INSET
                if len(pair) == 1:
                    # Seen from the side the one arm drawn is the near one: it
                    # hangs at the body's near edge, overlapping it by a third,
                    # rather than out beside it or flat on the chest.
                    reach = shoulder_half(d) - arm.width * 0.8
                cx = PIVOT[0] + (reach if len(pair) == 1 else (-reach if i == 0 else reach))
                place(c, arm, cx, shoulder + torsos[d].height * ARM_ATTACH)
        elif name == 'garment_kilt':
            k = widen(kilts[d], torsos[d])
            place(c, k, PIVOT[0], PIVOT[1] - KILT_LIFT - k.height)
        elif name == 'garment_robe':
            r = widen(robes[d], torsos[d])
            place(c, r, PIVOT[0], PIVOT[1] - r.height)
        elif name in extra:
            # Hung the way the robe is: from the shoulder line down to the feet,
            # scaled to the same height, so a person changing clothes does not
            # change size.
            g = extra[name][d]
            g = g.resize((int(g.width * robes[d].height / g.height), robes[d].height),
                         Image.LANCZOS)
            g = widen(g, torsos[d])
            place(c, g, PIVOT[0], PIVOT[1] - g.height)
        else:
            raise KeyError(name)
        return c

    def scaled_about_pivot(cell, factor):
        if factor == 1.0:
            return cell
        small = cell.resize((int(CELL * factor), int(CELL * factor)), Image.LANCZOS)
        out = blank()
        # The pivot must land on the pivot, so the figure keeps its feet on the
        # ground whatever its height.
        out.alpha_composite(small, (int(PIVOT[0] - PIVOT[0] * factor),
                                    int(PIVOT[1] - PIVOT[1] * factor)))
        return out

    layers = ['body', 'head', 'arms', 'garment_kilt', 'garment_robe'] + list(extra)
    OUT.mkdir(parents=True, exist_ok=True)
    for name in layers:
        atlas = Image.new('RGBA', (CELL * 3, CELL * 3), (0, 0, 0, 0))
        for row, factor in enumerate(AGE_SCALE):
            for d in range(3):
                atlas.alpha_composite(scaled_about_pivot(layer_cell(name, d), factor),
                                      (d * CELL, row * CELL))
        atlas.save(OUT / f'{name}.png', optimize=True)
        print(f'wrote {name}.png')

    manifest = {
        'format_version': 2,
        '_comment': ('One rig for every person, built by tools/build_pawn_rig.py from '
                     'source/pawn_layers.png. Each atlas is a 3x3 grid of 512px cells '
                     'sharing one pivot, so all layers are drawn into the same rectangle '
                     'and the composition is correct by construction.'),
        'cell': CELL,
        'pivot': list(PIVOT),
        'columns': ['south', 'east', 'north'],
        'rows': ['child', 'adult', 'elder'],
        'west': {'source': 'east', 'mirror_x': True},
        'layers': {name: f'{name}.png' for name in layers},
        'content_mapping': CONTENT_MAPPING,
        'draw_order': {
            'south': ['body', 'garment', 'arms', 'head'],
            'east': ['body', 'garment', 'arms', 'head'],
            'west': ['body', 'garment', 'arms', 'head'],
            'north': ['head', 'body', 'garment', 'arms'],
        },
        'figure_height_in_cell': round((PIVOT[1] - (shoulder - heads[0].height + NECK_OVERLAP)) / CELL, 3),
    }
    (OUT / 'pawn.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')

    # Layers this run no longer writes are removed. An old atlas left behind is
    # a drawing nothing produces any more that still passes for part of the rig.
    written = {f'{name}.png' for name in layers} | {'pawn.json'}
    for stale in sorted(OUT.glob('*.png')):
        if stale.name not in written:
            stale.unlink()
            print(f'removed stale {stale.name}')
    print('wrote pawn.json; shoulder line at', shoulder)


if __name__ == '__main__':
    main()
