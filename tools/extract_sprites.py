#!/usr/bin/env python3
"""Cuts the game's sprites out of the source art sheets.

The sheets in `assets/sprites/source/` are labelled catalogues: rows of variants,
several megabytes each, most of it not needed at any one moment. This cuts the
pieces the game actually draws into small trimmed PNGs named after the content
definitions they belong to, and writes the manifest the renderer reads.

Run it after changing the mapping below or after replacing a source sheet:

    python3 tools/extract_sprites.py

Nothing at runtime depends on this script or on the source sheets - the game
loads only the extracted PNGs and `assets/sprites/sprites.json`.

Addressing a piece:
    (row, col)                  the whole blob the grid finder saw
    (row, col, [cols, rows], [cx, cy])
                                blob split into a grid, one cell taken - the
                                sheets often draw a labelled group of variants
                                touching each other, so the grid finder returns
                                them as one blob
    ..., 'inset'                shave the card's outline off after cutting

`inset` is what ground tiles need. The sheet draws them as cards with a dark
rounded outline, and a hex filled with the whole card shows that outline as a
dark rim - which on a hex grid appears as a field of vertical bars, because a
pointy-top hex is widest halfway up its sides. How deep the outline goes differs
between sheets and is deepest at the rounded corners, so it is measured rather
than guessed.
"""
import json
import sys
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import numpy as np  # noqa: E402

from sprite_grid import alpha_mask, bands, grid, keyed, row_bands  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / 'assets' / 'sprites' / 'source'
OUT = ROOT / 'assets' / 'sprites'

# Source sheets, by the name the mapping uses.
SHEETS = {
    'terrain': 'terrain_atlas.png',
    'mudbrick': 'buildings_mudbrick.png',
    'weapons': 'weapons_tools.png',
    'reed': 'buildings_reed_hearth.png',
    'walls': 'wall_atlas.png',
    'animals_sheep': 'animals_sheep.png',
    'animals_goat': 'animals_goat.png',
    'animals_deer': 'animals_deer.png',
    'animals_wolf': 'animals_wolf.png',
    'animals_boar': 'animals_boar.png',
    'animals_dog': 'animals_dog.png',
    'animals_pig': 'animals_pig.png',
    'animals_cattle': 'animals_cattle.png',
    'animals_horse': 'animals_horse.png',
    'animals_donkey': 'animals_donkey.png',
    'animals_chicken': 'animals_chicken.png',
    'animals_bear': 'animals_bear.png',
    'animals_moose': 'animals_moose.png',
    'animals_yak': 'animals_yak.png',
    'village': 'buildings_village.png',
    'fortress': 'buildings_fortress.png',
    # walls_mudbrick_blue.png is deliberately not here: it is a fine modular
    # wall kit drawn flat and front on, and everything else on the map is
    # drawn from the same raised three-quarter view. Its pieces read as a
    # different game beside the fortress sheet's wall, which covers the same
    # ground and matches the buildings.
    'plants': 'plants_trees.png',
    'crops': 'crop_stages.png',
    'stone': 'stone_ore_game.png',
    'extras': 'pawn_extras.png',
}

# Most sheets space their variants out generously; the wall catalogue does not,
# and it prints a caption over every piece. Its own gap and floor keep the four
# bands - palisade and mud-brick, under construction and finished - apart.
GRID_OPTIONS = {
    'walls': dict(min_size=20, gap=6, min_cell=40),
}

# The catalogue sheets: a printed name under every drawing, and rows that are
# not equal fractions of the sheet. True means the sheet draws each building
# twice - under construction and finished, touching - so each cell is a pair.
# `pairs` means the sheet draws each building twice under one caption, under
# construction and finished. `caption_gap` is how far apart two runs of
# lettering have to be to be two captions rather than one caption with a space
# in it - which depends on how the sheet is laid out and so is per sheet.
CAPTIONED = {
    'village': dict(pairs=True, caption_gap=90),
    'fortress': dict(pairs=False, caption_gap=60),
}

# --------------------------------------------------------------------------
# What the game draws, and where each piece comes from.
#
# Keys are content definition names wherever one exists, so a reed hut and a
# mud-brick house cannot end up sharing a sprite the way they did when the
# mapping was keyed by render category.
# --------------------------------------------------------------------------
# Ground tiles are cut back until their border carries no outline. See
# `shave_outline`.
TERRAIN_INSET = 'inset'

# Cards whose drawing has a direction in it: furrows, ripples, grain. The
# renderer turns a ground card by a random quarter turn per tile so the same
# card does not read as a lattice of identical medallions - but turning a
# ploughed field makes a patchwork of furrows running four ways, so these say
# "do not turn me".
DIRECTIONAL = {
    'terrain/tilled',
    'terrain/sand',
}

# Cards that are ground, not things standing on it. The sheets draw a sown field
# and a dug channel as complete tiles - soil and all - so drawing them as
# standing sprites gives a floating box with its own patch of earth inside the
# field, and a channel reads as a small separate puddle sitting on the dirt.
# These fill their tile instead. Crops are added below, every stage of every one.
GROUND = {
    'buildings/irrigation_canal',
}

MAPPING = {
    # Ground. Each labelled group holds three variants of one terrain; the middle
    # one is the least distinctive and tiles best. The card outline is shaved off:
    # a tile with a rim is not a tile, it is a tile with a rim.
    'terrain/grass':        ('terrain', 0, 0, [3, 1], [1, 0], TERRAIN_INSET),
    'terrain/forest':       ('terrain', 0, 1, [3, 1], [1, 0], TERRAIN_INSET),
    'terrain/dirt':         ('terrain', 0, 2, [3, 1], [1, 0], TERRAIN_INSET),
    'terrain/rock':         ('terrain', 0, 3, [3, 1], [1, 0], TERRAIN_INSET),
    'terrain/water':        ('terrain', 1, 0, [3, 1], [0, 0], TERRAIN_INSET),
    'terrain/sand':         ('terrain', 1, 1, [3, 1], [1, 0], TERRAIN_INSET),
    'terrain/marsh':        ('terrain', 1, 2, [3, 1], [1, 0], TERRAIN_INSET),
    'terrain/tilled':       ('terrain', 1, 3, [3, 1], [1, 0], TERRAIN_INSET),

    # Shore. Each of these is a whole tile with water on one side and land on the
    # other, so a boundary tile gets exactly one of them, turned to face the
    # water. The renderer picks by the neighbour mask; see kShorePieces.
    'terrain/shore_sand_edge':   ('terrain', 2, 0, [4, 1], [0, 0], TERRAIN_INSET),
    'terrain/shore_dirt_edge':   ('terrain', 2, 0, [4, 1], [1, 0], TERRAIN_INSET),
    'terrain/shore_marsh_edge':  ('terrain', 2, 0, [4, 1], [2, 0], TERRAIN_INSET),
    'terrain/shore_rock_edge':   ('terrain', 2, 0, [4, 1], [3, 0], TERRAIN_INSET),

    # The sheet's own "inner corners" are not inner corners: measured, they put
    # the water on the same two sides as the outer ones and differ only in how
    # the line curves, so a tile with water at one diagonal only was drawn with
    # a whole quarter of water. The real inner corner is built below out of the
    # straight edge, which is the one piece the sheet does give honestly.
    'terrain/shore_sand_outer':  ('terrain', 2, 3, [4, 1], [0, 0], TERRAIN_INSET),
    'terrain/shore_dirt_outer':  ('terrain', 2, 3, [4, 1], [1, 0], TERRAIN_INSET),
    'terrain/shore_marsh_outer': ('terrain', 2, 3, [4, 1], [2, 0], TERRAIN_INSET),
    'terrain/shore_rock_outer':  ('terrain', 2, 3, [4, 1], [3, 0], TERRAIN_INSET),


    # Natural features, by resource definition.
    'nodes/fallen_branch':  ('plants', 0, 0),
    'nodes/oak_tree':       ('plants', 0, 1),
    'nodes/berry_bush':     ('plants', 1, 0),
    'nodes/reed_bed':       ('plants', 1, 2),
    # A stretch of bank where the fish run: reeds standing in the shallows.
    'nodes/fishing_shallows': ('plants', 1, 4),
    'nodes/wild_flax':      ('plants', 1, 3),
    'nodes/date_palm':      ('plants', 2, 2),
    'nodes/tamarisk':       ('plants', 3, 1),
    # The willow at the water and the yarrow on open ground: the two physic
    # plants (D98). The sheet has no drawing of either, so they take the
    # nearest thing it does draw - a small silvered tree and a low green clump.
    'nodes/willow_stand':   ('plants', 3, 0),
    'nodes/yarrow':         ('plants', 1, 1),
    'nodes/wild_emmer':     ('plants', 3, 3),
    'nodes/wild_einkorn':   ('plants', 3, 3),

    # Tools and arms, from weapons_tools.png. Row 0 is clubs, maces and blades,
    # row 1 poles and ranged, row 2 axes and the staff.
    'items/wooden_club':    ('weapons', 0, 0, [3, 1], [0, 0]),
    'items/stone_mace':     ('weapons', 0, 0, [3, 1], [2, 0]),
    'items/bronze_mace':    ('weapons', 0, 1),
    'items/flint_knife':    ('weapons', 0, 2, [3, 1], [0, 0]),
    'items/bronze_dagger':  ('weapons', 0, 2, [3, 1], [1, 0]),
    'items/bronze_sickle':  ('weapons', 0, 2, [3, 1], [2, 0]),
    'items/wooden_spear':   ('weapons', 1, 0),
    'items/hunting_bow':    ('weapons', 1, 3),
    'items/sling':          ('weapons', 1, 6),
    'items/stone_axe':      ('weapons', 2, 0),
    'items/bronze_axe':     ('weapons', 2, 1),

    'nodes/stone_outcrop':  ('stone', 0, 0),
    'nodes/clay_bank':      ('stone', 0, 3, [3, 1], [1, 0]),
    'nodes/copper_vein':    ('stone', 1, 0),
    'nodes/tin_vein':       ('stone', 2, 0),
    'nodes/deer_herd':      ('stone', 3, 1),
    # Worked out, waiting to regrow. The sheet draws these explicitly, so a spent
    # vein no longer has to be faded guesswork.
    'nodes/copper_vein_spent': ('stone', 1, 3),
    'nodes/tin_vein_spent':    ('stone', 2, 3, [2, 1], [0, 0]),

    # Buildings, by definition. `_site` is the same building under
    # construction, which these sheets draw for every one of them.
    #
    # buildings_village.png is a labelled catalogue: a name under each pair, and
    # the pair is the building going up and the building finished. Row by row -
    # hearth, store pit, thatched hut, quern stand; kiln, granary, reed hut,
    # small house; the three larger houses; granary, oven, brick kiln, brick
    # quern; smithy, dairy, quarry, mine.
    'buildings/campfire':              ('village', 0, 1),
    'buildings/campfire_site':         ('village', 0, 0),
    'buildings/store_pit':             ('village', 0, 3),
    'buildings/store_pit_site':        ('village', 0, 2),
    'buildings/reed_pit':              ('village', 0, 3),
    'buildings/reed_pit_site':         ('village', 0, 2),
    'buildings/wattle_hut':            ('village', 0, 5),
    'buildings/wattle_hut_site':       ('village', 0, 4),
    'buildings/quern_stand':           ('village', 0, 7),
    'buildings/quern_stand_site':      ('village', 0, 6),
    'buildings/kiln':                  ('village', 1, 1),
    'buildings/kiln_site':             ('village', 1, 0),
    'buildings/granary':               ('village', 1, 3),
    'buildings/granary_site':          ('village', 1, 2),
    'buildings/reed_granary':          ('village', 1, 3),
    'buildings/reed_granary_site':     ('village', 1, 2),
    'buildings/hut':                   ('village', 1, 5),
    'buildings/hut_site':              ('village', 1, 4),
    'buildings/mudbrick_house':        ('village', 1, 7),
    'buildings/mudbrick_house_site':   ('village', 1, 6),
    'buildings/timber_house':          ('village', 2, 1),
    'buildings/timber_house_site':     ('village', 2, 0),
    'buildings/mudbrick_manor':        ('village', 2, 3),
    'buildings/mudbrick_manor_site':   ('village', 2, 2),
    'buildings/mudbrick_granary':      ('village', 3, 1),
    'buildings/mudbrick_granary_site': ('village', 3, 0),
    'buildings/bakery':                ('village', 3, 3),
    'buildings/bakery_site':           ('village', 3, 2),
    'buildings/mudbrick_bakery':       ('village', 3, 3),
    'buildings/mudbrick_bakery_site':  ('village', 3, 2),
    'buildings/brick_kiln':            ('village', 3, 5),
    'buildings/brick_kiln_site':       ('village', 3, 4),
    'buildings/brick_quern':           ('village', 3, 7),
    'buildings/brick_quern_site':      ('village', 3, 6),
    'buildings/smokehouse':            ('village', 4, 1),
    'buildings/smokehouse_site':       ('village', 4, 0),
    'buildings/reed_dairy':            ('village', 4, 3),
    'buildings/reed_dairy_site':       ('village', 4, 2),
    'buildings/lean_to':               ('mudbrick', 0, 1),
    # The steppe tent: nothing on any sheet is a yurt, and the domed thatch is
    # the closest thing to one drawn - a shelter over a frame, not a house.
    'buildings/felt_tent':             ('village', 0, 5),
    'buildings/felt_tent_site':        ('village', 0, 4),

    # buildings_fortress.png, the same kind of catalogue for the wall. Row 1
    # holds the wall pieces: a short run, a long one, the inner and outer
    # corners, and the connector a gate stands in. Row 4 is everything under
    # construction. A run of wall receding from the view is the far arm of the
    # inner corner, which is the only place that direction is drawn.
    'buildings/courtyard_wall':       ('fortress', 1, 2),
    'buildings/courtyard_wall_side':  ('fortress', 1, 4, [2, 1], [0, 0]),
    'buildings/courtyard_wall_site':  ('fortress', 4, 3),
    'buildings/city_gate':            ('fortress', 0, 2),
    'buildings/city_gate_site':       ('fortress', 4, 1),

    'buildings/palisade':            ('walls', 1, 0),
    'buildings/palisade_side':       ('walls', 1, 1, [2, 1], [1, 0]),
    'buildings/palisade_site':       ('walls', 0, 0),

    'buildings/irrigation_canal': ('terrain', 1, 0, [3, 1], [1, 0], TERRAIN_INSET),

    # Still from buildings_reed_hearth.png: the byre is the long reed-and-post
    # shed, which the village catalogue has no drawing for.
    'buildings/reed_byre':       ('reed', 2, 4),

    # Sown ground, one sprite per stage. The sheet draws four stages twice; the
    # second of each pair is the fuller drawing.
    # A growing crop is not ground: the ripe cards draw ears standing above the
    # soil, taller than the card itself. So these keep their own alpha and are
    # drawn as things standing on a tilled hex, which is also why ripe grain is
    # visibly taller than a seedling.
    'crops/emmer_0':   ('crops', 0, 1),
    'crops/emmer_1':   ('crops', 0, 3),
    'crops/emmer_2':   ('crops', 0, 5),
    'crops/emmer_3':   ('crops', 0, 7),
    'crops/flax_0':    ('crops', 1, 1),
    'crops/flax_1':    ('crops', 1, 3),
    'crops/flax_2':    ('crops', 1, 5),
    'crops/flax_3':    ('crops', 1, 7),
    'crops/einkorn_0': ('crops', 2, 1),
    'crops/einkorn_1': ('crops', 2, 3),
    'crops/einkorn_2': ('crops', 2, 5),
    'crops/einkorn_3': ('crops', 2, 7),

}


def shave_outline(crop, limit=26):
    """Cuts a card back until its border ring is part of the drawing.

    The ground cards carry a dark stroke and rounded corners. A hex filled with
    the whole card shows that stroke, and on a hex grid it reads as a field of
    vertical bars. The depth differs per sheet and is worst at the corners, so
    this grows the inset until the darkest pixel on the ring is plausibly ground
    rather than outline.
    """
    luminance = np.asarray(crop.convert('L')).astype(int)
    field = float(np.median(luminance))
    # Dark art is not an outline. Tilled soil is genuinely dark, so the floor is
    # relative to the field with an absolute guard against a near-black stroke.
    floor = max(field * 0.35, 12.0)

    for inset in range(1, limit + 1):
        if crop.width <= inset * 2 + 8 or crop.height <= inset * 2 + 8:
            break
        view = luminance[inset:-inset, inset:-inset]
        ring = np.concatenate([view[0], view[-1], view[:, 0], view[:, -1]])
        if ring.min() >= floor:
            return crop.crop((inset, inset, crop.width - inset, crop.height - inset)), inset
    return crop, 0


def strip_caption(crop):
    """Drops the printed caption a sheet puts above its art.

    The sheets are labelled - "VARIANT 1", "GRASS", "DEER" - and the caption sits
    close enough above the drawing that the grid finder takes both as one blob.
    Splitting the piece at its horizontal gaps and keeping the tallest part
    leaves the art and throws the lettering away.
    """
    mask = np.asarray(crop.getchannel('A')) > 24
    rows = bands(mask.sum(axis=1), 1, 4)
    if len(rows) < 2:
        return crop
    y0, y1 = max(rows, key=lambda r: r[1] - r[0])
    return crop.crop((0, y0, crop.width, y1))


def caption_lines(a):
    """The lines of printed caption running across a labelled sheet.

    These catalogues name every drawing underneath it. The lettering is the one
    thing on them made of short runs - a building is drawn in long ones - which
    is enough to find it without reading it, and it is printed in flat white,
    which is what says where the line really begins and ends. Both are needed:
    the run test finds the body of a line and misses its ascenders, and a
    caption left half wiped comes through inside the cut.
    """
    mask = a[:, :, 3] > 24
    starts = mask & ~np.pad(mask[:, :-1], ((0, 0), (1, 0)))
    runs = starts.sum(axis=1)
    cover = mask.sum(axis=1)
    text = (runs >= 8) & (cover / np.maximum(runs, 1) < 10)
    lines = []
    for y in np.flatnonzero(text):
        if lines and int(y) - lines[-1][1] <= 5:
            lines[-1][1] = int(y)
        else:
            lines.append([int(y), int(y)])
    # A line of lettering is a few pixels tall. Anything deeper is a row of
    # scaffolding poles, which are short runs too.
    lines = [line for line in lines if 4 <= line[1] - line[0] <= 34]

    rgb = a[:, :, :3].astype(np.int16)
    printed = ((rgb.min(axis=2) > 150) & (rgb.max(axis=2) - rgb.min(axis=2) < 26) &
               mask).sum(axis=1)
    for line in lines:
        while line[0] > 0 and printed[line[0] - 1] > 8:
            line[0] -= 1
        while line[1] + 1 < len(printed) and printed[line[1] + 1] > 8:
            line[1] += 1
    return lines


def captioned_cells(image, pairs, caption_gap=60):
    """Rows of drawings on a sheet that prints a name under each of them.

    The captions do two jobs here. They say where one row of the catalogue ends
    and the next begins - cutting by equal fractions instead put the foot of a
    tall keep into the row below it - and each one sits under the thing it
    names, so where they are is how many drawings the row holds and roughly
    where each of them stands. The drawings themselves often touch, so nothing
    can be found by looking for the gaps between them.

    `pairs` is for the sheets that draw each building twice, under construction
    and finished, under one caption: the pair is split at the thinnest column
    near its middle.
    """
    a = np.asarray(image.convert('RGBA')).copy()
    mask = a[:, :, 3] > 24
    lines = caption_lines(a)
    # Where each caption stands has to be read before it is wiped. The gap that
    # merges runs into one caption is wide enough to carry "Wall Section
    # (Short)" across its spaces and narrow enough to keep it off its neighbour.
    lettering = [[(x0 + x1) // 2 for x0, x1 in
                  bands(mask[top:bottom + 1].sum(axis=0), 3, caption_gap)]
                 for top, bottom in lines]
    for top, bottom in lines:
        a[top:bottom + 1, :, 3] = 0
        mask[top:bottom + 1, :] = False
    image = Image.fromarray(a)

    rows = []
    top = 0
    for (line_top, line_bottom), centres in zip(lines, lettering):
        strip = mask[top:line_bottom + 1]
        if strip.any() and centres:
            # Halfway between two captions is only roughly where the two
            # drawings part; a wide one reaches past it. Each boundary is
            # walked to the thinnest column near it, which is the gap.
            profile = strip.sum(axis=0)
            edges = [0]
            for first, second in zip(centres, centres[1:]):
                middle = (first + second) // 2
                reach = max(8, (second - first) // 4)
                window = profile[middle - reach:middle + reach]
                edges.append(middle - reach + int(np.argmin(window)))
            edges.append(image.width)
            cells = []
            for left, right in zip(edges, edges[1:]):
                cells += _cells_in(strip, left, right, top, pairs)
            if cells:
                rows.append(cells)
        top = line_bottom + 1
    return image, rows


def _cells_in(strip, left, right, top, pairs):
    """The drawing, or the pair of them, standing between two columns."""
    block = strip[:, left:right]
    if not block.any():
        return []
    halves = [(left, right)]
    if pairs:
        profile = block.sum(axis=0)
        span = right - left
        middle = slice(int(span * 0.32), int(span * 0.68))
        split = left + middle.start + int(np.argmin(profile[middle]))
        halves = [(left, split), (split, right)]
    out = []
    for (x0, x1) in halves:
        part = strip[:, x0:x1]
        tall = [b for b in bands(part.sum(axis=1), 4, 3) if b[1] - b[0] >= 60]
        if not tall:
            continue
        y0, y1 = max(tall, key=lambda b: b[1] - b[0])
        columns = np.flatnonzero(part[y0:y1].any(axis=0))
        out.append((x0 + int(columns[0]), top + y0,
                    int(columns[-1] - columns[0]) + 1, y1 - y0))
    return out


def piece(sheet_rows, image, spec):
    """The trimmed image for one mapping entry."""
    row, col = spec[1], spec[2]
    if row >= len(sheet_rows):
        raise KeyError(f'row {row} beyond the {len(sheet_rows)} rows found')
    cells = sheet_rows[row]
    if col >= len(cells):
        raise KeyError(f'row {row} has {len(cells)} cells, asked for {col}')
    x, y, w, h = cells[col]

    if len(spec) > 3 and spec[3] is not None:
        cols, rows = spec[3]
        cx, cy = spec[4]
        w //= cols
        h //= rows
        x += w * cx
        y += h * cy

    inset = spec[5] if len(spec) > 5 else 0
    crop = image.crop((x, y, x + w, y + h))
    # The source sheets are opaque catalogues for the eye; on screen a sprite
    # needs its own alpha and no surrounding paper.
    if crop.getchannel('A').getextrema()[1] == 0:
        raise ValueError('piece is fully transparent')

    # Order matters: find what the drawing actually occupies first, then shave the
    # rim off that. Insetting before trimming eats into the art on one side while
    # leaving the caption band on the other.
    crop = strip_caption(crop)
    bbox = crop.getchannel('A').point(lambda v: 255 if v > 24 else 0).getbbox()
    if bbox:
        crop = crop.crop(bbox)

    if inset == 'inset':
        crop, depth = shave_outline(crop)
        if depth == 0:
            raise ValueError('could not find a clean border to cut back to')
    return crop


def centre_slice(card, axis):
    """One tile's worth of a run, cut flush out of the middle of a longer piece.

    The wall pieces are drawn several tiles long, so scaled to one tile they came
    out as low kerbs - and each kept the finished ends and the outline it was
    drawn with, so a run of them read as separate blocks with daylight between.
    A square window from the middle is one tile of wall, as tall as it is wide,
    and its cut edges are flush, so the next tile carries straight on.
    """
    keep = min(card.width, card.height)
    if axis == 'y':
        top = (card.height - keep) // 2
        return card.crop((0, top, card.width, top + keep))
    # 'x_left' keeps the finished end the piece was drawn with; 'x' cuts flush
    # both sides, which is what a middle-of-the-run tile wants.
    left = 0 if axis == 'x_left' else (card.width - keep) // 2
    return card.crop((left, 0, left + keep, card.height))


# Pieces that are a run rather than an object: which way the run goes, and so
# which way the middle is cut out.
SLICED = {
    'buildings/courtyard_wall': 'x',
    'buildings/courtyard_wall_site': 'x',
    'buildings/courtyard_wall_side': 'y',
    'buildings/palisade': 'x',
    'buildings/palisade_site': 'x',
    'buildings/palisade_side': 'y',
    'buildings/city_gate': 'x',
    'buildings/city_gate_site': 'x',
}


# --- sheets cut by fractions rather than by blobs --------------------------
#
# The animal sheets are a regular grid - three views across, one row per age or
# state - drawn on black. Finding the cells by looking for the art fails on them:
# a bear's front view touches its side view, so the two come back as one cell and
# every index after it shifts. Cutting the picture into equal parts and trimming
# each to what it contains is exact, because the grid really is regular.
# Three views across on every one of them. The rows differ: a beast that is
# shorn has four - grown in fleece, grown shorn, young in fleece, young shorn -
# and one that is not has two, grown and young. The hens have three, because a
# hen, a cock and a chick are three different birds to look at.
FIXED_SHEETS = {
    'animals_sheep': (3, 4),
    'animals_goat': (3, 4),
    'animals_yak': (3, 4),
    'animals_donkey': (3, 4),
    'animals_deer': (3, 2),
    'animals_wolf': (3, 2),
    'animals_boar': (3, 2),
    'animals_dog': (3, 2),
    'animals_pig': (3, 2),
    'animals_cattle': (3, 2),
    'animals_horse': (3, 2),
    'animals_chicken': (3, 3),
    'animals_bear': (3, 2),
    'animals_moose': (3, 2),
}

# What is cut out of them. Column one is the side view, which is what the game
# draws; the rows are the ages and states the simulation actually tracks.
FIXED_MAPPING = {
    # The name is the animal definition's own, with the states the simulation
    # tracks hung off it: `_young` before it is grown, `_shorn` while its fleece
    # is off. The renderer walks back up that chain, so a kind the sheet draws
    # only once is drawn that way at every age.
    'animals/sheep':             ('animals_sheep', 1, 0),
    'animals/sheep_shorn':       ('animals_sheep', 1, 1),
    'animals/sheep_young':       ('animals_sheep', 1, 2),
    'animals/sheep_young_shorn': ('animals_sheep', 1, 3),
    'animals/goat':              ('animals_goat', 1, 0),
    'animals/goat_shorn':        ('animals_goat', 1, 1),
    'animals/goat_young':        ('animals_goat', 1, 2),
    'animals/goat_young_shorn':  ('animals_goat', 1, 3),
    'animals/yak':               ('animals_yak', 1, 0),
    'animals/yak_shorn':         ('animals_yak', 1, 1),
    'animals/yak_young':         ('animals_yak', 1, 2),
    'animals/yak_young_shorn':   ('animals_yak', 1, 3),
    # The wild kinds a tamed one comes from are the same beast until somebody
    # takes it in hand, and the sheets draw them once.
    'animals/wild_yak':          ('animals_yak', 1, 0),
    'animals/wild_yak_young':    ('animals_yak', 1, 2),
    'animals/deer':              ('animals_deer', 1, 0),
    'animals/deer_young':        ('animals_deer', 1, 1),
    'animals/moose':             ('animals_moose', 1, 0),
    'animals/moose_young':       ('animals_moose', 1, 1),
    'animals/wolf':              ('animals_wolf', 1, 0),
    'animals/wolf_young':        ('animals_wolf', 1, 1),
    'animals/bear':              ('animals_bear', 1, 0),
    'animals/bear_young':        ('animals_bear', 1, 1),
    'animals/boar':              ('animals_boar', 1, 0),
    'animals/boar_young':        ('animals_boar', 1, 1),
    'animals/pig':               ('animals_pig', 1, 0),
    'animals/pig_young':         ('animals_pig', 1, 1),
    'animals/shepherd_dog':      ('animals_dog', 1, 0),
    'animals/shepherd_dog_young': ('animals_dog', 1, 1),
    # The dark horned beast on the cattle sheet is the wild one; the spotted
    # cow below it is what a herded one becomes.
    'animals/aurochs':           ('animals_cattle', 1, 0),
    'animals/cattle':            ('animals_cattle', 1, 1),
    'animals/horse':             ('animals_horse', 1, 0),
    'animals/horse_young':       ('animals_horse', 1, 1),
    'animals/wild_horse':        ('animals_horse', 1, 0),
    'animals/wild_horse_young':  ('animals_horse', 1, 1),
    'animals/donkey':            ('animals_donkey', 1, 1),
    'animals/donkey_young':      ('animals_donkey', 1, 3),
    'animals/chicken':           ('animals_chicken', 1, 0),
    'animals/rooster':           ('animals_chicken', 1, 1),
    'animals/chicken_young':     ('animals_chicken', 1, 2),
}


# What the three columns of an animal sheet are, as sprite name endings. The
# side view carries no ending: it is what most of them are drawn as, and every
# other name falls back to it.
VIEW_FRONT = '_front'
VIEW_SIDE = ''
VIEW_BACK = '_back'


def _split_thinnest(strip, pieces, wanted):
    """Cuts the widest piece at its thinnest column until there are enough.

    For the rows a sheet draws touching - a bear's front view leaning into its
    side view - where there is no gap to find and the parting is only the
    narrowest place between the two.
    """
    pieces = list(pieces)
    while len(pieces) < wanted:
        widest = max(range(len(pieces)), key=lambda i: pieces[i][1] - pieces[i][0])
        left, right = pieces[widest]
        span = right - left
        if span < 40:
            break
        profile = strip[:, left:right].sum(axis=0)
        middle = slice(int(span * 0.3), int(span * 0.7))
        cut = left + middle.start + int(np.argmin(profile[middle]))
        pieces[widest:widest + 1] = [(left, cut), (cut, right)]
    return pieces


def fixed_cell(image, columns, rows, column, row):
    """One cell of a plain grid sheet, found rather than measured.

    Neither axis is equal fractions. The rows are drawn at whatever height the
    animal needs, and the side view runs past its third of the sheet - cut at
    the fraction, every bull lost its tail and every bear its nose. So the rows
    are found across the sheet and the columns inside the row that was found.
    """
    mask = np.asarray(image.getchannel('A')) > 24
    lines = row_bands(mask, rows)
    if len(lines) == rows:
        top, bottom = lines[row]
    else:
        height = image.height // rows
        top, bottom = row * height, (row + 1) * height
    strip = mask[top:bottom]

    found = []
    for gap in (4, 6, 10, 16):
        candidate = bands(strip.sum(axis=0), 20, gap)
        if len(candidate) == columns:
            found = candidate
            break
        if candidate and len(candidate) < columns and not found:
            found = _split_thinnest(strip, candidate, columns)
    if len(found) != columns:
        width = image.width // columns
        found = [(i * width, (i + 1) * width) for i in range(columns)]
    left, right = found[column]

    cell = image.crop((left, top, right, bottom))
    box = cell.getchannel('A').point(lambda v: 255 if v > 24 else 0).getbbox()
    return cell.crop(box) if box else cell


def _shore_bands(card):
    """Where the water ends and the land begins in a straight edge card.

    The card has water at the north; what comes back is the row the water gives
    out at, the row the land takes over at, and the colour of the surf between
    them - which is what the corner has to be painted with.
    """
    a = np.asarray(card.convert('RGB')).astype(np.int16)
    water = (a[:, :, 2] > a[:, :, 0] + 20) & (a[:, :, 2] > 90)
    share = water.mean(axis=1)
    rows = np.flatnonzero((share > 0.05) & (share < 0.95))
    if rows.size == 0:                       # no shoreline found: split down the middle
        mid = card.height // 2
        return mid, mid, (255, 255, 255)
    top, bottom = int(rows[0]), int(rows[-1])
    band = a[max(0, top - 1):bottom + 2]
    return top, bottom, tuple(int(v) for v in band.reshape(-1, 3).mean(axis=0))


def _fill(card, y0, y1, size):
    """A square of one plain texture, taken from a band of the card and stretched
    to fill it. Repeating the band instead put a mirrored row of pebbles across
    the middle of the ground, which reads as a seam; stretching noise does not."""
    y0, y1 = max(0, y0), min(card.height, y1)
    if y1 - y0 < 2:
        y0, y1 = min(card.height - 2, y0), min(card.height, y0 + 2)
    return card.crop((0, y0, card.width, y1)).resize((size, size), Image.LANCZOS)


def inner_corner(edge):
    """The corner where the water pokes into the land at one diagonal only.

    The sheet has no such piece - what it labels an inner corner puts the water
    on two whole sides, the same as its outer one - so it is painted here: a cove
    at the north-east, arced rather than square, out of the water, the land and
    the surf colour of the straight edge card. Two straight edges laid over each
    other give the right ground but meet in a right angle, and a shoreline with a
    square elbow in it is what the drawing was avoiding in the first place.
    """
    size = max(edge.width, edge.height)
    card = edge.resize((size, size), Image.LANCZOS)
    top, bottom, surf = _shore_bands(card)

    clear = max(2, int(size * 0.06))        # keep the surf out of the plain fills
    water = _fill(card, 0, max(2, top - clear), size)
    land = _fill(card, min(size - 2, bottom + clear), card.height, size)

    # The cove: a quarter disc at the north-east corner, wobbled so the line does
    # not read as compass work, and about as deep as the straight edge's own
    # shoreline sits from its side.
    ys, xs = np.mgrid[0:size, 0:size]
    dx = (size - 1 - xs) / size
    dy = ys / size
    dist = np.sqrt(dx * dx + dy * dy)
    angle = np.arctan2(dy, np.maximum(dx, 1e-6))
    reach = (1.0 - (top + bottom) / (2.0 * size)) * (1.0 + 0.06 * np.sin(angle * 5.0))

    edge_px = 1.5 / size
    band_px = max(1.5, size * 0.03) / size
    wet = np.clip((reach - dist) / edge_px * 0.5 + 0.5, 0.0, 1.0)
    line = np.clip((band_px - np.abs(dist - reach)) / edge_px * 0.5 + 0.5, 0.0, 1.0)

    w = np.asarray(water).astype(np.float32)
    l = np.asarray(land).astype(np.float32)
    out = l + (w - l) * wet[:, :, None]
    foam = np.array([surf[0], surf[1], surf[2], 255], dtype=np.float32)
    out = out + (foam - out) * (line * 0.85)[:, :, None]
    return Image.fromarray(out.round().astype('uint8')).convert('RGBA')


SYNTHESIZED = {
    f'terrain/shore_{material}_inner': (f'terrain/shore_{material}_edge', inner_corner)
    for material in ('sand', 'dirt', 'marsh', 'rock')
}


def main():
    if not SOURCE.exists():
        print(f'no source sheets in {SOURCE}', file=sys.stderr)
        return 1

    loaded = {}
    for key, filename in SHEETS.items():
        path = SOURCE / filename
        if not path.exists():
            print(f'missing sheet {path}', file=sys.stderr)
            return 1
        image, _ = alpha_mask(path)
        _, _, rows = grid(path, **GRID_OPTIONS.get(key, {}))
        if key in CAPTIONED:
            image, rows = captioned_cells(keyed(Image.open(path).convert('RGBA')),
                                          **CAPTIONED[key])
        elif key in FIXED_SHEETS:
            # These are cut by grid, and their subjects are outlined in the same
            # black the background is: keyed by colour they come out full of
            # holes, so the ground is found by where it reaches instead.
            image = keyed(Image.open(path).convert('RGBA'))
        elif image.getchannel('A').getextrema()[0] > 8:
            # A keyed RGB sheet needs the key applied to the pixels too, not
            # only to the mask used for finding the grid.
            _, mask = alpha_mask(path)
            image.putalpha(Image.fromarray((mask * 255).astype('uint8')))
        loaded[key] = (image, rows)
        print(f'{filename}: {len(rows)} rows')

    manifest = {
        'format_version': 3,
        '_comment': ('Written by tools/extract_sprites.py. Keys are content '
                     'definition names; the renderer falls back to the '
                     'category when a definition has no sprite of its own.'),
        'sprites': {},
    }

    ue_models = ROOT / 'assets/generated/scene_models'
    if (ue_models / '.ue-imported').exists():
        views = json.loads((ue_models / 'manifest.json').read_text())['grass']
        if len(views) != 6:
            raise ValueError('UE grass requires six views')
        for variant, name in enumerate(views):
            if Path(name).name != name:
                raise ValueError('unsafe UE grass path')
            with Image.open(ue_models / name) as image:
                manifest['sprites'][f'foliage/grass_{variant}'] = {
                    'file': '../generated/scene_models/' + name,
                    'size': [image.width, image.height],
                }
    else:
        foliage = OUT / 'raw_srcs' / 'kenney_foliageSprites' / 'PNG' / 'Shaded'
        for variant, number in enumerate(range(52, 58)):
            source = foliage / f'sprite_{number:04}.png'
            if not source.exists():
                continue
            with Image.open(source) as image:
                manifest['sprites'][f'foliage/grass_{variant}'] = {
                    'file': source.relative_to(OUT).as_posix(),
                    'size': [image.width, image.height],
                }

    failures = []
    for name, spec in MAPPING.items():
        image, rows = loaded[spec[0]]
        try:
            cut = piece(rows, image, spec)
        except (KeyError, ValueError) as exc:
            failures.append(f'{name}: {exc}')
            continue
        if name in SLICED:
            cut = centre_slice(cut, SLICED[name])
        destination = OUT / f'{name}.png'
        destination.parent.mkdir(parents=True, exist_ok=True)
        cut.save(destination, optimize=True)
        entry = {
            'file': f'{name}.png',
            'size': [cut.width, cut.height],
        }
        if name in DIRECTIONAL or name.startswith('crops/'):
            entry['directional'] = True
        if name in GROUND or name.startswith('crops/'):
            entry['ground'] = True
        manifest['sprites'][name] = entry

    # The animal sheets: a regular grid of three views to a row, drawn from the
    # front, from the side and from behind - the same three the pawn rig uses.
    # Each of them is cut, so an animal walking towards the view is drawn
    # walking towards the view; the plain name is the side, which is what a
    # beast standing about is drawn as.
    for name, (sheet, column, row) in FIXED_MAPPING.items():
        if sheet not in loaded:
            failures.append(f'{name}: no sheet {sheet}')
            continue
        columns, rows = FIXED_SHEETS[sheet]
        for suffix, which in ((VIEW_SIDE, column), (VIEW_FRONT, column - 1),
                              (VIEW_BACK, column + 1)):
            if which < 0 or which >= columns:
                continue
            cut = fixed_cell(loaded[sheet][0], columns, rows, which, row)
            full = name + suffix
            destination = OUT / f'{full}.png'
            destination.parent.mkdir(parents=True, exist_ok=True)
            cut.save(destination, optimize=True)
            manifest['sprites'][full] = {'file': f'{full}.png',
                                         'size': [cut.width, cut.height]}

    # Pieces the sheet does not hold, assembled out of the ones it does.
    for name, (source, build) in SYNTHESIZED.items():
        if source not in manifest['sprites']:
            failures.append(f'{name}: nothing cut for {source}')
            continue
        cut = build(Image.open(OUT / f'{source}.png').convert('RGBA'))
        destination = OUT / f'{name}.png'
        destination.parent.mkdir(parents=True, exist_ok=True)
        cut.save(destination, optimize=True)
        manifest['sprites'][name] = {'file': f'{name}.png', 'size': [cut.width, cut.height],
                                     'ground': True}

    # The ground materials, baked separately by tools/bake_materials.py from the
    # material packs. Nothing is cut here - they are whole textures, tiled over
    # the terrain rather than placed on it - but they belong in the manifest so
    # that the renderer finds them the same way it finds everything else.
    ground = OUT / 'ground'
    if ground.is_dir():
        for texture in sorted(ground.glob('*.png')):
            name = f'ground/{texture.stem}'
            with Image.open(texture) as image:
                manifest['sprites'][name] = {'file': f'{name}.png',
                                             'size': [image.width, image.height],
                                             'tiles': True}

    # Sprites the mapping no longer produces are removed. Leaving them behind
    # means dead files nothing references and a directory that stops matching the
    # manifest - which is how a drawing for a building that no longer exists sat
    # in the tree after the mapping was corrected.
    managed = {'terrain', 'nodes', 'buildings', 'crops', 'animals'}   # not 'ground': baked elsewhere
    # Everything the manifest now names, which is the only honest answer: the
    # animal sheets each produce three sprites per entry, one per view.
    wanted = {OUT / entry['file'] for entry in manifest['sprites'].values()}
    removed = 0
    for directory in sorted(managed):
        folder = OUT / directory
        if not folder.exists():
            continue
        for existing in sorted(folder.glob('*.png')):
            if existing not in wanted:
                existing.unlink()
                removed += 1

    (OUT / 'sprites.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'wrote {len(manifest["sprites"])} sprites and sprites.json'
          + (f', removed {removed} stale' if removed else ''))
    for problem in failures:
        print(f'  FAILED {problem}', file=sys.stderr)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
