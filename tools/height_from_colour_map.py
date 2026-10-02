#!/usr/bin/env python3
"""A height sheet for a region that has only a flat coloured map: blue sea and
lakes, land in yellows and greens, mountains drawn as grey triangles, letters
in black (doc/references/maps, Xairinis).

    python3 tools/height_from_colour_map.py MAP.jpg OUT.png [--size 1254] [--peak 2600]

Written as 16-bit grey for tools/assemble_heightmap.py: 0 is sea, and the
land runs from the layout's "sea_below" up to white at "peak_m" with its
"gamma" (the defaults here match a sheet entry with sea_below 14, gamma 1.25).
The relief is made, not read:
  - the land rises from its coast to a low plateau, rolling with noise;
  - the pale heart of the map (sand) is flatter and lower, rippled as dunes;
  - each grey triangle is a peak, its height from the triangle's size, its
    flanks ridged; a cluster of triangles is a range, their union;
  - triangles standing in the sea are rocky islets;
  - blue the land closes round is a lake: a hollow, its shore a little up.
"""
import argparse
import math

import numpy as np
from PIL import Image
from scipy import ndimage


def noise(shape, seed, scale):
    rng = np.random.default_rng(seed)
    h, w = shape
    total = np.zeros(shape, np.float32)
    amp, weight, s = 1.0, 0.0, float(scale)
    while s >= 3:
        gh, gw = int(math.ceil(h / s)) + 2, int(math.ceil(w / s)) + 2
        grid = rng.random((gh, gw)).astype(np.float32)
        up = np.asarray(Image.fromarray(grid, "F").resize((int(gw * s), int(gh * s)), Image.BICUBIC))[:h, :w]
        total += up * amp
        weight += amp
        amp *= 0.5
        s /= 2
    return total / weight


def ridged(shape, seed, scale):
    n = noise(shape, seed, scale)
    return 1.0 - np.abs(n * 2.0 - 1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("map")
    ap.add_argument("out")
    ap.add_argument("--size", type=int, default=1254)
    ap.add_argument("--peak", type=float, default=2600.0)   # the sheet's peak_m
    ap.add_argument("--gamma", type=float, default=1.25)
    ap.add_argument("--sea-below", type=float, default=14.0)
    ap.add_argument("--plateau", type=float, default=220.0)  # metres the inland rises to
    ap.add_argument("--seed", type=int, default=5)
    ap.add_argument("--green-hills", type=float, default=0.0)  # metres green (forest) ground rises by
    ap.add_argument("--brown-low", type=float, default=0.0)    # 0..1: how much lower brown (sandy) ground lies
    ap.add_argument("--pits", default="")                      # x0,y0,x1,y1 on the drawing: dark-brown circles there are craters
    ap.add_argument("--pit-depth", type=float, default=45.0)
    a = ap.parse_args()

    src = Image.open(a.map).convert("RGB")
    src_size = src.size[0]
    src = src.resize((a.size, a.size), Image.BICUBIC)
    rgb = np.asarray(src).astype(np.float32)
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    k = a.size / 512.0                                   # pixels per pixel of the drawing
    blue = (b > 140) & (r < 90)
    ink = rgb.max(2) < 70
    grey = (np.abs(r - g) < 18) & (np.abs(g - b) < 26) & (rgb.max(2) > 80) & (rgb.max(2) < 175) & ~blue
    grey = ndimage.binary_opening(grey, iterations=max(1, int(k)))
    # The antialiased rim of a letter is grey too; not a mountain.
    grey &= ~ndimage.binary_dilation(rgb.max(2) < 70, iterations=int(3 * k))
    # Letters and outlines are neither land nor sea: what is round them.
    unknown = ink | ndimage.binary_dilation(ink, iterations=int(2 * k))
    known = ~unknown
    _, (iy, ix) = ndimage.distance_transform_edt(~known, return_indices=True)
    blue = np.where(known, blue, blue[iy, ix])
    # The drawing's frame: the outermost line of pixels is usually white.
    blue[:2, :], blue[-2:, :], blue[:, :2], blue[:, -2:] = True, True, True, True
    land = ~blue
    land = ndimage.binary_opening(land, iterations=int(k))

    lab, _ = ndimage.label(~land)
    edge = np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))
    lakes = (~land) & ~np.isin(lab, edge)
    sea = (~land) & ~lakes

    shape = land.shape
    from_coast = ndimage.distance_transform_edt(~sea).astype(np.float32) / k     # drawing pixels
    rolling = noise(shape, a.seed, 160 * k)
    metres = 4.0 + a.plateau * (1 - np.exp(-from_coast / 45.0)) * (0.55 + 0.9 * rolling)
    metres += (noise(shape, a.seed + 1, 40 * k) - 0.5) * 60.0

    # Green ground (forest) stands up in hills; brown (sand and soil at the
    # shore) lies low. Both by how much of each is round a point.
    green = (g > r + 60) & (g > b + 50) & land
    brown = (r > 140) & (g > 85) & (g < 150) & (b < 125) & (r > g + 25) & land
    if a.green_hills > 0:
        cover = ndimage.gaussian_filter(green.astype(np.float32), 8 * k)
        metres += cover * a.green_hills * (0.45 + 0.9 * noise(shape, a.seed + 9, 50 * k))
    if a.brown_low > 0:
        cover = ndimage.gaussian_filter(brown.astype(np.float32), 5 * k)
        metres *= 1.0 - a.brown_low * cover

    # Sand: the pale, yellow heart. Lower, flatter, rippled.
    pale = (r > 225) & (g > 225) & (b > 130) & land
    pale = ndimage.gaussian_filter(pale.astype(np.float32), 6 * k)
    yy, xx = np.mgrid[0:shape[0], 0:shape[1]].astype(np.float32)
    dunes = np.sin((xx * 0.8 + yy * 0.6) / (2.2 * k) + noise(shape, a.seed + 2, 30 * k) * 9.0) * 0.5 + 0.5
    metres = metres * (1 - 0.35 * pale) + pale * dunes * 18.0

    # Mountains: the grey triangles' own silhouette. Height grows inward from
    # its edge, so each triangle's middle line becomes a ridge and its tip a
    # peak; a cluster of triangles is one range. How high: the cluster's
    # tallest triangle, by its size on the drawing.
    peaks, count = ndimage.label(ndimage.binary_dilation(grey, iterations=1))
    mountain = np.zeros(shape, np.float32)
    rocky = ridged(shape, a.seed + 3, 14 * k) * 0.6 + ridged(shape, a.seed + 4, 40 * k) * 0.4
    for i, box in enumerate(ndimage.find_objects(peaks), start=1):
        if box is None:
            continue
        part = peaks[box] == i
        if part.sum() < 12 * k * k:
            continue
        pad = int(30 * k)
        y0, y1 = max(0, box[0].start - pad), min(shape[0], box[0].stop + pad)
        x0, x1 = max(0, box[1].start - pad), min(shape[1], box[1].stop + pad)
        foot = peaks[y0:y1, x0:x1] == i
        inside = ndimage.distance_transform_edt(foot).astype(np.float32)
        tall = (box[0].stop - box[0].start) / k
        top = min(a.peak, 600.0 + tall * 24.0)
        rise = (inside / max(1.0, inside.max())) ** 0.7
        # And skirts beyond the drawn shape: foothills a few kilometres out.
        out = ndimage.distance_transform_edt(~foot).astype(np.float32) / k
        skirt = np.exp(-out / 9.0) * 0.28
        shape_here = np.maximum(rise * 0.72 + 0.28 * foot, skirt)
        mountain[y0:y1, x0:x1] = np.maximum(mountain[y0:y1, x0:x1], shape_here * top)
    # Not the drawing's triangles: the field pushed about by noise, so a
    # range wanders, and broken by ridges.
    warp_px = 10.0 * k
    wx = (noise(shape, a.seed + 7, 36 * k) - 0.5) * 2 * warp_px
    wy = (noise(shape, a.seed + 8, 36 * k) - 0.5) * 2 * warp_px
    mountain = ndimage.map_coordinates(ndimage.gaussian_filter(mountain, 2.5 * k), [yy + wy, xx + wx], order=1, mode="nearest")
    mountain = mountain * (0.45 + 0.75 * rocky)
    # Ranges standing in the sea: rocky islets, their coast torn by noise.
    islets = sea & (mountain * (0.7 + 0.6 * noise(shape, a.seed + 6, 12 * k)) > 260.0)
    islets = ndimage.binary_opening(islets, iterations=int(k))
    land = land | islets
    sea &= ~islets
    metres = np.where(islets, 3.0, metres)
    metres = np.maximum(metres, metres * 0.4 + mountain)

    # Craters: dark-brown circles inside a box of the drawing, each a pit
    # with a raised rim.
    if a.pits:
        x0, y0, x1, y1 = [float(v) * a.size / src_size for v in a.pits.split(",")]
        box = np.zeros(shape, bool)
        box[int(y0):int(y1), int(x0):int(x1)] = True
        pit = (r > 130) & (g < 112) & (b < 95) & (r > g + 40) & land & box
        pit = ndimage.binary_opening(pit, iterations=int(2 * k))
        bowl = ndimage.gaussian_filter(pit.astype(np.float32), 2.5 * k)
        rim = ndimage.gaussian_filter(ndimage.binary_dilation(pit, iterations=int(3 * k)) & ~pit, 2.0 * k)
        metres += rim * a.pit_depth * 0.6 - bowl * a.pit_depth
        metres = np.maximum(metres, 3.0)
        np.save(a.out + ".pits.npy", pit)   # where the craters are, for the control maps
        print(f"craters: {ndimage.label(pit)[1]}")

    # Lakes: a hollow with a raised rim.
    if lakes.any():
        near = ndimage.distance_transform_edt(~lakes).astype(np.float32) / k
        metres += np.exp(-near / 4.0) * 12.0 * ~lakes
    metres = ndimage.gaussian_filter(metres, 0.8 * k)

    t = np.clip((metres - 2.0) / a.peak, 0, 1) ** (1.0 / a.gamma)
    value = a.sea_below + 1 + t * (255.0 - a.sea_below - 1)
    value = np.where(land, value, 0.0)   # sea and lakes alike: the assembler tells them apart
    out = np.round(value * 257.0).astype(np.uint16)
    Image.fromarray(out).save(a.out)
    print(f"{a.out}: {a.size} px, land {land.mean() * 100:.0f} %, lakes {lakes.sum()} px, "
          f"{count} mountain marks, highest {metres[land].max():.0f} m")


if __name__ == "__main__":
    main()
