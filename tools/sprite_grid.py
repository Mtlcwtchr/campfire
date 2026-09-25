#!/usr/bin/env python3
"""Finds the variant grid in a source art sheet.

The sheets are catalogues: rows of separated objects on a transparent - or flat
colour - background. Rather than measuring rectangles by hand, which is how the
first pass produced swapped garments and skirts stretched over the whole body,
this projects the alpha mask onto each axis and cuts at the gaps.

    python3 tools/sprite_grid.py <sheet.png> ...          # print the grid
    python3 tools/sprite_grid.py --contact out.png <...>  # numbered contact sheet
"""
import sys

import numpy as np
from PIL import Image, ImageDraw


def alpha_mask(path, tolerance=26):
    """Boolean mask of where the art is. RGB sheets are keyed on a corner pixel."""
    im = Image.open(path).convert('RGBA')
    a = np.asarray(im)
    if a[1, 1, 3] > 8:
        # Authored without transparency: key out the background colour.
        bg = a[1, 1, :3].astype(np.int16)
        d = np.abs(a[:, :, :3].astype(np.int16) - bg).max(axis=2)
        return im, d > tolerance
    return im, a[:, :, 3] > 24


def _background(black):
    """The black that reaches the border, spread through it run by run.

    Grown a run at a time rather than a pixel at a time: within one row a black
    run is background whole or not at all, so four sweeps carry the ground round
    a corner and the whole sheet settles in a dozen of them.
    """
    seen = np.zeros_like(black)
    seen[0, :] = seen[-1, :] = True
    seen[:, 0] = seen[:, -1] = True
    seen &= black
    for _ in range(60):
        before = seen.sum()
        for across in (True, False):
            run = black if across else black.T
            reached = seen if across else seen.T
            starts = run & ~np.pad(run[:, :-1], ((0, 0), (1, 0)))
            ids = np.where(run, np.cumsum(starts.ravel()).reshape(run.shape), 0)
            hit = np.bincount(ids.ravel(), weights=reached.ravel(),
                              minlength=int(ids.max()) + 1) > 0
            hit[0] = False
            grown = run & hit[ids]
            seen = grown if across else grown.T
        if seen.sum() == before:
            break
    return seen


def keyed(image):
    """The sheet with its ground taken out.

    Half of the sheets arrived with the background baked in as opaque black, and
    anything cut from one of those carries a black box with it. The drawings'
    own outlines are just as black, so the ground cannot be told from them by
    colour - it is told by being the black that reaches the border.
    """
    a = np.asarray(image.convert('RGBA')).copy()
    if (a[:, :, 3] > 24).mean() <= 0.98:
        return image
    a[:, :, 3] = np.where(_background(a[:, :, :3].max(axis=2) <= 14), 0, 255)
    return Image.fromarray(a)


def bands(profile, min_run, gap):
    """Occupied runs along one axis, merged across gaps smaller than `gap`."""
    occupied = profile > 0
    runs = []
    start = None
    empty = 0
    for i, v in enumerate(occupied):
        if v:
            if start is None:
                start = i
            empty = 0
        elif start is not None:
            empty += 1
            if empty >= gap:
                if i - empty - start >= min_run:
                    runs.append((start, i - empty))
                start = None
                empty = 0
    if start is not None and len(occupied) - start >= min_run:
        runs.append((start, len(occupied)))
    return runs


def row_bands(mask, rows):
    """Where one column of a fixed sheet parts into its rows.

    The rows are not equal fractions - a standing goat and a lying kid need
    different heights - so cutting by fraction brings the hooves of the row
    below into the picture. Most of the sheets leave clear ground between the
    rows and can be cut there, though how much differs: a goat's rows nearly
    touch and a bear's are a hand apart. The donkeys leave none at all, and for
    those the thinnest places between the drawings are close enough.
    """
    profile = mask.sum(axis=1)
    for gap in (2, 3, 4, 6, 8, 12):
        found = bands(profile, 10, gap)
        if len(found) == rows:
            return found
    filled = np.flatnonzero(profile)
    if len(filled) == 0:
        return []
    low, high = int(filled[0]), int(filled[-1]) + 1
    apart = (high - low) // (rows * 2)
    cuts = []
    for y in sorted(range(low + apart, high - apart), key=lambda y: (profile[y], y)):
        if all(abs(y - other) >= apart for other in cuts):
            cuts.append(y)
            if len(cuts) == rows - 1:
                break
    edges = [low] + sorted(cuts) + [high]
    return [(edges[i], edges[i + 1]) for i in range(rows)]


def grid(path, min_size=40, gap=14, min_cell=64):
    """Rows of tight bounding boxes, in reading order.

    `min_cell` drops blobs too small to be art. The sheets carry printed captions
    - "VARIANT 1", "DEER" - and without this they come through as cells and shift
    every index after them, which is how a caption ended up on screen as the
    sprite for deer.
    """
    im, mask = alpha_mask(path)
    rows = []
    for (y0, y1) in bands(mask.sum(axis=1), min_size, gap):
        strip = mask[y0:y1]
        cells = []
        for (x0, x1) in bands(strip.sum(axis=0), min_size, gap):
            block = strip[:, x0:x1]
            ys = np.flatnonzero(block.any(axis=1))
            xs = np.flatnonzero(block.any(axis=0))
            w = int(xs[-1] - xs[0]) + 1
            h = int(ys[-1] - ys[0]) + 1
            if w < min_cell or h < min_cell:
                continue
            cells.append((x0 + int(xs[0]), y0 + int(ys[0]), w, h))
        if cells:
            rows.append(cells)
    return im, mask, rows


def contact_sheet(paths, out_path, scale=0.42):
    """One image per sheet with every cell boxed and numbered `row.col`."""
    panels = []
    for path in paths:
        im, _, rows = grid(path)
        canvas = im.copy()
        draw = ImageDraw.Draw(canvas)
        for ri, cells in enumerate(rows):
            for ci, (x, y, w, h) in enumerate(cells):
                draw.rectangle([x, y, x + w, y + h], outline=(255, 90, 90, 255), width=3)
                draw.text((x + 4, y + 2), f'{ri}.{ci}', fill=(255, 255, 120, 255))
        canvas = canvas.resize((int(canvas.width * scale), int(canvas.height * scale)),
                               Image.LANCZOS)
        panels.append(canvas)

    width = max(p.width for p in panels)
    height = sum(p.height for p in panels)
    sheet = Image.new('RGBA', (width, height), (36, 40, 36, 255))
    y = 0
    for p in panels:
        sheet.alpha_composite(p, (0, y))
        y += p.height
    sheet.save(out_path)
    return sheet.size


if __name__ == '__main__':
    args = sys.argv[1:]
    if args and args[0] == '--contact':
        print(contact_sheet(args[2:], args[1]))
    else:
        for path in args:
            im, _, rows = grid(path)
            print(f'{path}  {im.size[0]}x{im.size[1]}  {len(rows)} rows')
            for ri, cells in enumerate(rows):
                print(f'  row {ri}: ' + ' '.join(f'[{i}]{c[2]}x{c[3]}' for i, c in enumerate(cells)))
