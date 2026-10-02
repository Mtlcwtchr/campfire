#!/usr/bin/env python3
"""Lays several height pictures out on one world and writes it as an
authoring package (doc/world_authoring_import_export_spec.docx, D158).

    python3 tools/assemble_heightmap.py doc/references/heights/layout.json [--fit]

With --fit, and a "guide" in the layout (a coloured map of the whole
continent: warm land, cool sea) and a "segment" box on each sheet (in the
guide's pixels), each sheet is first turned, stretched and moved until its
land lies over its segment, and the placement is written back to the layout.
Land the guide has that no sheet covers is raised as low country.

The layout says where each picture goes and how to read it:

    {
      "size": 8192,                  # samples a side, 256 m each (8192 = 2097 km), or [across, down]
      "low_m": -200, "high_m": 3200, # what 0 and 65535 of the written PNG stand for
      "out": "assembled",            # a directory beside the layout
      "sheets": [
        {"file": "a.png", "x": 5600, "y": 500, "size": 2048,
         "peak_m": 3000,             # white on the picture
         "sea_below": 14,            # grey under this is sea (the coast's glow is not land)
         "water": [28, 66],          # optional: grey in this band is water too (a painted lake ring)
         "cut": ["left", "top"],     # optional: edges the picture cuts land at - faded into a coast
         "turn": [-10, 10],          # optional: how far --fit may turn it, degrees
         "relief": {"scale": 0.35, "grain": 0.5, "smooth_px": 8,   # optional: lowered and smoothed ...
                    "keep": [[cx, cy, rx, ry, degrees]]}}          # ... but inside these ellipses
      ]
    }

Sea is not a height in the pictures (it is black), so it is made here: twenty
metres at the coast, down to the open sea's depth fifteen kilometres out.
What is written: <out>/world.json, <out>/raster/height.png (16-bit grey) and
<out>/preview.png (a small coloured picture to look at).
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage


def noise(shape, seed, scale):
    """Fractal value noise in 0..1, `scale` pixels for the widest octave."""
    rng = np.random.default_rng(seed)
    h, w = shape
    total = np.zeros(shape, np.float32)
    amp, weight = 1.0, 0.0
    s = scale
    while s >= 4:
        gh, gw = int(math.ceil(h / s)) + 2, int(math.ceil(w / s)) + 2
        grid = rng.random((gh, gw)).astype(np.float32)
        up = np.asarray(Image.fromarray(grid, "F").resize((int(gw * s), int(gh * s)), Image.BICUBIC))[:h, :w]
        total += up * amp
        weight += amp
        amp *= 0.5
        s /= 2
    return total / weight


def sheet_heights(base, sheet, index):
    picture = Image.open(base / sheet["file"])
    # A 16-bit sheet keeps its fine steps, read on the same 0..255 scale.
    if picture.mode in ("I;16", "I;16B", "I"):
        grey = np.asarray(picture).astype(np.float32) / 257.0
    else:
        grey = np.asarray(picture.convert("L")).astype(np.float32)
    grey = ndimage.gaussian_filter(grey, 0.7)   # the picture's own grain, not relief
    sea_below = sheet.get("sea_below", 14)
    # The coast as such pictures draw it: a bright rim a pixel or two wide
    # along every shore - a wall a kilometre or two high, read as height -
    # and a dim fringe outside it, which came out as a skirt of low land
    # round every island. Within three pixels of the sea the ground is no
    # higher than the land just inside it, and what is still dim there is sea.
    shore_px = sheet.get("shore_px", 3)
    open_water = grey < sea_below
    from_sea = ndimage.distance_transform_edt(~open_water)
    inner = (from_sea > shore_px) & (from_sea <= shore_px + 5)
    if inner.any():
        _, (iy, ix) = ndimage.distance_transform_edt(~inner, return_indices=True)
        near = (from_sea > 0) & (from_sea <= shore_px)
        grey = np.where(near, np.minimum(grey, grey[iy, ix]), grey)
        grey = np.where(near & (grey < sea_below + sheet.get("fringe", 20)), 0.0, grey)
    land = grey >= sea_below
    if "water" in sheet:
        lo, hi = sheet["water"]
        land &= ~((grey >= lo) & (grey <= hi))
    t = np.clip((grey - sea_below) / (255.0 - sea_below), 0, 1)
    metres = 2.0 + t ** sheet.get("gamma", 1.25) * sheet.get("peak_m", 3000)
    # The relief taken down where the picture has more mountain than the
    # country should: smoothed and lowered, keeping some of its grain for
    # hills, everywhere but the "keep" ellipses ([cx, cy, rx, ry, degrees]
    # in the picture's pixels), which stand as drawn with a soft edge.
    if "relief" in sheet:
        r = sheet["relief"]
        smooth = ndimage.gaussian_filter(metres, r.get("smooth_px", 8))
        lowered = 2.0 + (smooth - 2.0) * r.get("scale", 0.35) + (metres - smooth) * r.get("grain", 0.5)
        h, w = metres.shape
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        keep = np.zeros_like(metres)
        for cx, cy, rx, ry, deg in r.get("keep", []):
            a = math.radians(deg)
            u = ((xs - cx) * math.cos(a) + (ys - cy) * math.sin(a)) / rx
            v = (-(xs - cx) * math.sin(a) + (ys - cy) * math.cos(a)) / ry
            d = np.sqrt(u * u + v * v)
            keep = np.maximum(keep, np.clip((1.3 - d) / 0.5, 0, 1))
        keep = keep * keep * (3 - 2 * keep)
        metres = np.maximum(2.0, lowered * (1 - keep) + metres * keep)
    # An edge the picture cuts land at: the land fades to a coast over a band
    # whose width wanders, so it ends as a shore and not as a ruler.
    h, w = grey.shape
    fade = np.ones_like(grey)
    wobble = (noise(grey.shape, 17 + index, 96) - 0.5) * 0.16 * w
    for edge in sheet.get("cut", []):
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        d = {"left": xs, "right": w - 1 - xs, "top": ys, "bottom": h - 1 - ys}[edge]
        band = 0.14 * w
        fade = np.minimum(fade, np.clip((d + wobble - 0.04 * w) / band, 0, 1))
    fade = fade * fade * (3 - 2 * fade)
    metres = metres * fade
    land &= metres > 3.0
    # Water the picture means: its water band, and sea it closes in (a lake
    # has no way out to the picture's edge). Nothing is filled over it.
    sea = ~land
    lab, _ = ndimage.label(sea)
    edge = np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))
    lakes = sea & ~np.isin(lab, edge)
    if "water" in sheet:
        lo, hi = sheet["water"]
        lakes |= (grey >= lo) & (grey <= hi) & (grey >= sea_below)
    # Lakes the picture draws only as a dark hollow: [cx, cy, rx, ry] ellipses
    # in its pixels, water wherever they cover land.
    if sheet.get("lakes"):
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        for cx, cy, rx, ry in sheet["lakes"]:
            inside = ((xs - cx) / rx) ** 2 + ((ys - cy) / ry) ** 2 <= 1.0
            lakes |= inside & land
            land &= ~inside
    return metres, land, lakes


def world_shape(layout):
    """Samples across and down: "size" is one number for a square world or
    [across, down]."""
    size = layout.get("size", 8192)
    return (int(size[0]), int(size[1])) if isinstance(size, list) else (int(size), int(size))


def guide_land(layout, base):
    """The continent's outline from a coloured map: warm (land) against cool
    (sea), its grid lines closed over. Returns the mask and where the map lies
    in the world (scale, offset x, offset y in world samples)."""
    g = layout["guide"]
    a = np.asarray(Image.open(base / g["file"]).convert("RGB")).astype(int)
    r, b = a[..., 0], a[..., 2]
    land = (r > b + 25) & (r > 90)
    for bx0, by0, bx1, by1 in g.get("ignore", []):   # labels and smudges that are not land
        land[by0:by1, bx0:bx1] = False
    land = ndimage.binary_opening(ndimage.binary_closing(land, iterations=6), iterations=3)
    lab, count = ndimage.label(land)
    sizes = ndimage.sum(land, lab, range(1, count + 1))
    land = np.isin(lab, 1 + np.where(sizes > g.get("min_area", 2000))[0])
    nx, ny = world_shape(layout)
    ys, xs = np.nonzero(land)
    # How big the continent is: its land's height in kilometres, when given,
    # else the map's height the world's. Centred either way.
    if "continent_km" in g:
        scale = g["continent_km"] * 1000.0 / 256.0 / (ys.max() - ys.min() + 1)
    else:
        scale = g.get("scale", ny / a.shape[0])
    ox = g.get("x", (nx - (xs.min() + xs.max() + 1) * scale) / 2)
    oy = g.get("y", (ny - (ys.min() + ys.max() + 1) * scale) / 2)
    return land, scale, ox, oy


def fill_from_guide(world, water, layout, base):
    """Land the outline has and no picture covers - the necks between the
    pieces - raised as low country, its coast softened and torn."""
    if not layout["guide"].get("fill", True):
        return world
    land, scale, ox, oy = guide_land(layout, base)
    nx, ny = world_shape(layout)
    h, w = land.shape
    # Where the map's pixels land in the world, blurred smooth at its scale.
    canvas = np.zeros((ny, nx), np.float32)
    img = Image.fromarray(land.astype(np.float32), "F").resize((int(w * scale), int(h * scale)), Image.BILINEAR)
    part = np.asarray(img)
    x0, y0 = int(round(ox)), int(round(oy))
    xa, ya, xb, yb = max(0, x0), max(0, y0), min(nx, x0 + part.shape[1]), min(ny, y0 + part.shape[0])
    canvas[ya:yb, xa:xb] = part[ya - y0:yb - y0, xa - x0:xb - x0]
    q = 4
    small = canvas[::q, ::q]
    small = ndimage.gaussian_filter(small, scale / q)
    small += (noise(small.shape, 91, 64) - 0.5) * 0.5
    guide = np.asarray(Image.fromarray(small.astype(np.float32), "F").resize((nx, ny), Image.BILINEAR)) > 0.5
    gap = guide & np.isnan(world) & ~water
    lowland = 8.0 + noise((ny // q, nx // q), 7, 128) * 180.0
    lowland = np.asarray(Image.fromarray(lowland.astype(np.float32), "F").resize((nx, ny), Image.BICUBIC))
    world = world.copy()
    world[gap] = lowland[gap]
    print(f"guide: {gap.mean() * 100:.1f} % of the world filled in as low country between the pieces")
    return world


def fit(layout, base):
    """Each sheet turned, stretched and moved until its land lies over its
    segment of the guide's outline (its box, in guide pixels): the best
    overlap on a coarse grid. Writes x, y, w, h and rotate into the layout."""
    guide, scale, ox, oy = guide_land(layout, base)
    nx, ny = world_shape(layout)
    k = 16                                     # the grid it is fitted on: 512 x 512 for 8k
    mx, my = nx // k, ny // k
    gh, gw = guide.shape
    gimg = Image.fromarray(guide.astype(np.float32), "F").resize((max(1, int(gw * scale / k)), max(1, int(gh * scale / k))),
                                                                  Image.BILINEAR)
    world_guide = np.zeros((my, mx), np.float32)
    gp = np.asarray(gimg)
    x0, y0 = int(round(ox / k)), int(round(oy / k))
    xa, ya, xb, yb = max(0, x0), max(0, y0), min(mx, x0 + gp.shape[1]), min(my, y0 + gp.shape[0])
    world_guide[ya:yb, xa:xb] = gp[ya - y0:yb - y0, xa - x0:xb - x0]
    world_guide = world_guide > 0.5
    for i, sheet in enumerate(layout["sheets"]):
        if "segment" not in sheet:
            continue
        bx0, by0, bx1, by1 = sheet["segment"]
        box = [int((bx0 * scale + ox) / k), int((by0 * scale + oy) / k), int((bx1 * scale + ox) / k), int((by1 * scale + oy) / k)]
        target = np.zeros_like(world_guide)
        target[box[1]:box[3], box[0]:box[2]] = world_guide[box[1]:box[3], box[0]:box[2]]
        ty, tx = np.nonzero(target)
        _, land, _ = sheet_heights(base, dict(sheet, cut=[]), i)
        src = Image.fromarray(land.astype(np.float32), "F")

        def render(rot, cx, cy, w, h):
            img = src.rotate(rot, Image.BILINEAR, expand=True) if rot else src
            a = np.asarray(img.resize((max(1, int(w)), max(1, int(h))), Image.BILINEAR)) > 0.5
            out = np.zeros((my, mx), bool)
            x, y = int(round(cx - w / 2)), int(round(cy - h / 2))
            xa, ya, xb, yb = max(0, x), max(0, y), min(mx, x + a.shape[1]), min(my, y + a.shape[0])
            if xb > xa and yb > ya:
                out[ya:yb, xa:xb] = a[ya - y:yb - y, xa - x:xb - x]
            return out

        def score(p):
            s = render(*p)
            inter = (s & target).sum()
            union = (s | target).sum()
            return inter / max(1, union)

        best, best_p = -1.0, None
        # How far it may be turned: the sheet's own "turn" [lo, hi] in
        # degrees, when the way it is drawn is already the way it lies.
        turn_lo, turn_hi = sheet.get("turn", [-40, 40])
        # And how far out of its own proportions: 1 keeps them.
        stretch = float(sheet.get("stretch", 1.3))
        for rot in range(int(turn_lo), int(turn_hi) + 1, 5):
            img = src.rotate(rot, Image.BILINEAR, expand=True) if rot else src
            a = np.asarray(img) > 0.5
            yy, xx = np.nonzero(a)
            # The rotated picture's land box onto the target's box.
            sx = (tx.max() - tx.min() + 1) / (xx.max() - xx.min() + 1)
            sy = (ty.max() - ty.min() + 1) / (yy.max() - yy.min() + 1)
            if stretch <= 1.0:   # its own proportions: one scale for both
                sx = sy = math.sqrt(sx * sy)
            w, h = a.shape[1] * sx, a.shape[0] * sy
            cx = (tx.min() + tx.max()) / 2 - ((xx.min() + xx.max()) / 2 - a.shape[1] / 2) * sx
            cy = (ty.min() + ty.max()) / 2 - ((yy.min() + yy.max()) / 2 - a.shape[0] / 2) * sy
            p = (rot, cx, cy, w, h)
            sc = score(p)
            if sc > best:
                best, best_p = sc, p
        # And a walk downhill from there.
        steps = [2.0, 1.0, 1.0, 0.04, 0.04, 0.04]
        for _ in range(60):
            moved = False
            for d in range(6):
                for sign in (-1, 1):
                    q = list(best_p)
                    if d == 5:   # both together
                        q[3] *= 1 + sign * steps[d]
                        q[4] *= 1 + sign * steps[d]
                    else:
                        q[d] = q[d] * (1 + sign * steps[d]) if d >= 3 else q[d] + sign * steps[d]
                    if not turn_lo <= q[0] <= turn_hi:
                        continue
                    img = src.rotate(q[0], expand=True) if q[0] else src
                    own = img.size[1] / img.size[0]
                    if q[4] / q[3] > stretch * own * 1.000001 or q[3] / q[4] > stretch / own * 1.000001:
                        continue   # no further out of its own shape than allowed
                    sc = score(tuple(q))
                    if sc > best:
                        best, best_p, moved = sc, tuple(q), True
            if not moved:
                break
        rot, cx, cy, w, h = best_p
        sheet.update({"rotate": rot, "w": int(round(w * k)), "h": int(round(h * k)),
                      "x": int(round((cx - w / 2) * k)), "y": int(round((cy - h / 2) * k))})
        sheet.pop("size", None)
        print(f"fitted {sheet['file']}: turned {rot:.0f} deg, {w * k * 0.256:.0f} x {h * k * 0.256:.0f} km, "
              f"overlap {best * 100:.0f} %")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    layout_file = Path(args[0] if args else "doc/references/heights/layout.json")
    base = layout_file.parent
    layout = json.loads(layout_file.read_text())
    if "--fit" in sys.argv:
        fit(layout, base)
        layout_file.write_text(json.dumps(layout, indent=1, ensure_ascii=False))
    nx, ny = world_shape(layout)
    low, high = layout.get("low_m", -200.0), layout.get("high_m", 3200.0)
    out = base / layout.get("out", "assembled")
    (out / "raster").mkdir(parents=True, exist_ok=True)

    world = np.full((ny, nx), np.nan, np.float32)
    water = np.zeros((ny, nx), bool)   # lakes and water the pictures mean
    for i, sheet in enumerate(layout["sheets"]):
        metres, land, lakes = sheet_heights(base, sheet, i)
        heights_img = Image.fromarray(np.where(land, metres, 0).astype(np.float32), "F")
        cover_img = Image.fromarray(land.astype(np.float32), "F")
        lakes_img = Image.fromarray(lakes.astype(np.float32), "F")
        if sheet.get("rotate", 0):
            heights_img = heights_img.rotate(sheet["rotate"], Image.BICUBIC, expand=True)
            cover_img = cover_img.rotate(sheet["rotate"], Image.BILINEAR, expand=True)
            lakes_img = lakes_img.rotate(sheet["rotate"], Image.BILINEAR, expand=True)
        sw = int(sheet.get("w", sheet.get("size", 2048)))
        sh = int(sheet.get("h", sheet.get("size", 2048)))
        size = sw
        scaled = np.asarray(heights_img.resize((sw, sh), Image.BICUBIC))
        cover = np.asarray(cover_img.resize((sw, sh), Image.BILINEAR)) > 0.5
        wet = np.asarray(lakes_img.resize((sw, sh), Image.BILINEAR)) > 0.5
        x, y = int(sheet["x"]), int(sheet["y"])
        x0, y0, x1, y1 = max(0, x), max(0, y), min(nx, x + sw), min(ny, y + sh)
        if x1 <= x0 or y1 <= y0:
            print("sheet", sheet["file"], "is outside the world", file=sys.stderr)
            continue
        part = scaled[y0 - y:y1 - y, x0 - x:x1 - x]
        keep = cover[y0 - y:y1 - y, x0 - x:x1 - x]
        target = world[y0:y1, x0:x1]
        target[keep] = np.fmax(target[keep], np.maximum(part[keep], 2.0))
        water[y0:y1, x0:x1] |= wet[y0 - y:y1 - y, x0 - x:x1 - x] & ~keep
        print(f"{sheet['file']}: {sw * 0.256:.0f} x {sh * 0.256:.0f} km at ({x * 0.256:.0f}, {y * 0.256:.0f}) km, "
              f"land {keep.mean() * 100:.0f} % of it, highest {part[keep].max() if keep.any() else 0:.0f} m")

    if "guide" in layout:
        world = fill_from_guide(world, water, layout, base)

    # Lakes - the water the pictures mean inside the land - are not the sea:
    # they have a bed, falling from their shore a few metres a sample to at
    # most sixty, and never to the sea's level (a lake is land under water).
    # Marked in the water flags, so the drainage keeps them as basins and the
    # generator holds water in them (D161).
    lakes = water & np.isnan(world)
    lake_px = int(lakes.sum())
    if lake_px:
        landed = ~np.isnan(world)
        from_shore, (sy, sx) = ndimage.distance_transform_edt(~landed, return_indices=True)
        shore = world[sy, sx]
        bed = np.maximum(2.0, shore - np.clip(3.0 + 6.0 * from_shore, 3.0, 60.0))
        world = np.where(lakes, bed, world)
    print(f"lakes: {lake_px * 0.0655:.0f} km2 with a bed")

    # The sea: the source's own default, exactly, everywhere. (A shelf at
    # twenty metres was shallow enough for the ground's detail to come up
    # through it as a ring of land round every coast.)
    land = ~np.isnan(world)
    open_sea = -60.0
    world = np.where(land, world, open_sea)

    # Rounded as the importer rounds (half away from nought), so the open sea
    # is the very number it takes for "nothing here".
    code = lambda m: np.floor(np.clip((m - low) / (high - low), 0, 1) * 65535.0 + 0.5)
    stored = code(world).astype(np.uint16)
    Image.fromarray(stored, "I;16").save(out / "raster" / "height.png", optimize=False)
    Image.fromarray(lakes.astype(np.uint8), "L").save(out / "raster" / "water.png", optimize=False)
    wide_m, high_m = float(nx * 256), float(ny * 256)
    manifest = {
        "format": "campfire.world-authoring", "version": 1,
        "world": {"width_m": wide_m, "height_m": high_m, "sample_m": 256, "chunk_m": 32768},
        "origin_m": [0.0, 0.0], "size_m": [wide_m, high_m],
        "rasters": {"height": {"file": "raster/height.png", "type": "u16", "kind": "height",
                               "min_m": low, "max_m": high, "default_m": -60.0,
                               "interpolation": "bicubic", "optional": False, "version": 1},
                    "water": {"file": "raster/water.png", "type": "u8", "kind": "flags",
                              "default": 0, "bits": {"lake": 0}, "optional": True, "version": 1}},
        "vectors": {},
    }
    (out / "world.json").write_text(json.dumps(manifest, indent=1))

    # A picture to look at: sea in blues, land from green to white.
    k = max(1, max(nx, ny) // 1024)
    view = world[::k, ::k]
    rgb = np.zeros(view.shape + (3,), np.float32)
    sea = view <= 0
    d = np.clip(-view / -low, 0, 1)[..., None]
    rgb[sea] = (np.array([70, 120, 170]) * (1 - d) + np.array([20, 40, 80]) * d)[sea]
    stops = np.array([0, 300, 900, 1800, 2600, 3200], np.float32)
    colours = np.array([[88, 128, 70], [130, 150, 84], [160, 138, 92], [130, 110, 90], [200, 196, 190], [250, 250, 250]])
    for c in range(3):
        rgb[..., c] = np.where(sea, rgb[..., c], np.interp(view, stops, colours[:, c]))
    wet = lakes[::k, ::k]
    rgb[wet] = np.array([86, 140, 190])
    Image.fromarray(rgb.astype(np.uint8)).save(out / "preview.png")
    print(f"written: {out / 'raster' / 'height.png'} ({nx} x {ny}, {nx * 0.256:.0f} x {ny * 0.256:.0f} km, "
          f"{low:.0f} .. {high:.0f} m), "
          f"land {land.mean() * 100:.1f} % of the world")


if __name__ == "__main__":
    main()
