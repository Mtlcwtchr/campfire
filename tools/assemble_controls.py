#!/usr/bin/env python3
"""Control maps for an assembled world, read off hand-drawn region maps
(doc/references/maps, their colours as legend.md describes them).

    python3 tools/assemble_controls.py doc/references/heights/layout.json

Run after tools/assemble_heightmap.py: it reads the same layout, and each
sheet that has a "controls" entry gets its hand map laid over it:

    "controls": {
      "map": "../maps/x.jpg",          # the hand map, beside the layout
      "crop": [x0, y0, x1, y1],        # the drawn area: inside the frame, above the legend
      "fit": [tx, ty, sx, sy, deg],    # optional: hand map onto the sheet (written by --fit)
      "classes": [[r, g, b, "fertile"], ...],   # the map's colours, after cleaning, by class
      "values": {"fertile": [moisture, forest, erosion], ...},   # this region's reading of them
      "zones": [[cx, cy, rx, ry, deg, "conifer", {"forest": 0.8}]],  # sheet pixels
      "rock_above_pct": 55,            # rock only above this percentile of the region's heights
      "fallback": "ash",               # optional: the class of land the map draws as sea
      "forest": {"fertile": "old_mixed"},   # this region's forest biome on a category
      "water_type": {"swamp": "bog"},       # ... its kind of water
      "decor": {"rough": "stones_stumps"},  # ... its decor
      "ground": {"rock": "atrod_rock"}      # this region's own category for a class
    }

The ground ids are terrain categories (content/config/terrain/categories.json,
engine/biomes): the values a class gives the channels are its category's
`controls`, one source of truth with the engine. Zones that say only what
forest grows (conifer, old oak, the eerie wood, charred trunks) go to the
forest layer - and old oak's litter to the decor layer - over the ground the
map has there, rather than becoming ground categories of their own.

The height sheet is what the shapes are: where land is, where the lakes and
the mountains are. The hand map only says what covers the land - fertile
ground, sparse woodland, rough soil, rock, swamp, the Wastes' grey powder -
and where it disagrees with the heights (a grey "mountain" over a valley the
sheet has low), the heights win and the class round it is taken.

The hand map is laid on the sheet by a fit (--fit): shift, scale and a small
turn, scored by how well its land - told from its sea by colours learnt from
the sheet's own land and sea - lies over the sheet's land.

Written beside the heights:
  raster/control_0.png  RGBA8: moisture_bias, forest_bias, mountain_strength,
                        erosion_strength (0..1; 0.5, 0.5, 0, 0.5 at rest)
  raster/ground.png     u8 ids: the terrain category (ground_legend.json)
  raster/forest.png, water_type.png, decor.png
                        u8 ids: the layers' biomes, 0 - as the category says
                        (written only when something says otherwise)
  world.json            all of them added to the manifest, each categorical
                        layer with its legend ("ids") for the import to
                        renumber by name
  controls_preview.png  the ground ids in colour beside the four channels
"""
import json
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
from assemble_heightmap import sheet_heights, world_shape  # noqa: E402

# What covers the ground, by id, with a colour for the preview. Some are one
# region's own reading of a colour (the Wastes' brown is grey powder, not
# soil), and the zones come from marks the maps draw over the colour.
GROUND = [
    (0, "none", (40, 70, 120)),
    (1, "fertile", (96, 168, 64)),          # bright green: valleys, plains, where forest grows best
    (2, "sparse", (138, 140, 70)),          # brown-green: sparse woodland, transition, often damp
    (3, "rough", (150, 112, 74)),           # brown: coarse open soil, stony, dryish
    (4, "rock", (170, 170, 170)),           # grey to white: mountains and rock massifs
    (5, "swamp", (70, 120, 100)),           # blue-green: bog, marsh, flooded ground
    (6, "wastes", (120, 116, 110)),         # the Wastes: grey dead powder, grit, stone
    (7, "conifer", (40, 100, 70)),          # Elnor, purple marks: conifer forest
    (8, "old_oak", (90, 60, 40)),           # Elnor, red mark: rare old dark oak on bare dark soil
    (9, "eerie", (150, 150, 120)),          # Elnor, green mark by the outpost: bare eerie trees
    (10, "charred", (70, 60, 56)),          # the Wastes' far north-east: rare charred trees
    (11, "jagged", (215, 205, 195)),        # Atrod's south ridge: sharper, toothed rock
    (12, "lake", (80, 140, 190)),           # the sheet's lakes
    (13, "tropical_forest", (40, 130, 60)), # the Flame Islands: tropical forest
    (14, "tropical_soil", (150, 120, 80)),  # the Flame Islands: sand and wet tropical soil, mixed
    (15, "savanna", (196, 176, 96)),        # Xairinis: dry grass over dry soil and sand
    (16, "dunes", (226, 206, 150)),         # Xairinis: the sandy heart
    (17, "savanna_wood", (140, 150, 70)),   # Xairinis: dry savanna woodland by the coasts
    (18, "beach", (225, 205, 160)),         # sand along a coast ("coast" in a sheet's controls)
    (19, "scarlet", (200, 40, 40)),         # the Flame Islands, P and A: scarlet grodomantite in the ground
    (20, "crater", (45, 35, 40)),           # the Flame Islands, E: craters dark with the black wind's power
    # Regions' own readings of a class ("ground" in a sheet's controls):
    (21, "desert", (230, 200, 120)),        # generated worlds' deserts; not painted here
    (22, "atrod_valley", (150, 175, 90)),   # Atrod's valleys: grass, shrub, stones
    (23, "atrod_rock", (185, 185, 185)),    # Atrod's smoother rock
    (24, "volcanic_rock", (80, 72, 70)),    # the Flame Islands' dark rock
    (25, "warm_rock", (190, 150, 110)),     # Xairinis' warm cliffs
    (26, "elnor_fertile", (100, 160, 80)),  # Elnor's beech valleys
    (27, "kagar_fertile", (70, 150, 60)),   # Kagar's old forests
]
GROUND_ID = {name: i for i, name, _ in GROUND}
DEFAULTS = {"moisture": 0.5, "forest": 0.5, "mountain": 0.0, "erosion": 0.5}

# The engine's registry of terrain categories and layer biomes: the ids the
# maps are painted with, and the channels' values on each category.
TERRAIN = Path(__file__).resolve().parent.parent / "content" / "config" / "terrain"
LAYERS = ("forest", "water_type", "decor")
LAYER_FILES = {"forest": "forest_biomes", "water_type": "water_biomes", "decor": "decor_biomes"}
# Zones that say what grows, not what the ground is: (forest biome, decor biome).
FOREST_ZONES = {"conifer": ("conifer", None), "old_oak": ("old_oak", "oak_litter"),
                "eerie": ("eerie", "elnor_deadwood"), "charred": ("charred_dead", None)}


def load_registry():
    raw = {c["name"]: c for c in json.loads((TERRAIN / "categories.json").read_text())["categories"]}

    def resolved(name, path=()):
        c = raw[name]
        out = {}
        if "inherit" in c and c["inherit"] not in path:
            out = {k: (dict(v) if isinstance(v, dict) else v) for k, v in resolved(c["inherit"], path + (name,)).items()}
        for k, v in c.items():
            out[k] = {**out.get(k, {}), **v} if isinstance(v, dict) and isinstance(out.get(k), dict) else v
        return out

    categories = {n: resolved(n) for n in raw}
    biomes = {}
    for layer, file in LAYER_FILES.items():
        entries = json.loads((TERRAIN / (file + ".json")).read_text())[file]
        biomes[layer] = {e["name"]: e["id"] for e in entries}
    return categories, biomes


CATEGORIES, BIOMES = load_registry()
for _i, _name, _ in GROUND[1:]:
    if CATEGORIES.get(_name, {}).get("id") != _i:
        sys.exit(f"assemble_controls: ground id {_i} is \"{_name}\" here but not in {TERRAIN / 'categories.json'}")

# Each class's reading of the channels, before a region says otherwise -
# [moisture, forest, erosion], its category's `controls` (mountain strength
# comes from the heights).
BASE_VALUES = {}
for _i, _name, _ in GROUND[1:]:
    _k = CATEGORIES[_name].get("controls", {})
    BASE_VALUES[_name] = [_k.get("moisture", DEFAULTS["moisture"]), _k.get("forest", DEFAULTS["forest"]),
                          _k.get("erosion", DEFAULTS["erosion"])]


def clean(path, crop):
    """The hand map's drawn area with its ink taken out - grid lines, letters,
    marks: thin and dark - and the pencil's strokes softened into colour."""
    a = np.asarray(Image.open(path).convert("RGB").crop(tuple(crop))).astype(np.float32)
    ink = a.max(2) < 110
    ink &= ~ndimage.binary_opening(ink, iterations=4)     # a dark mass (Kagar's massif) is not ink
    ink = ndimage.binary_dilation(ink, iterations=2)
    _, (iy, ix) = ndimage.distance_transform_edt(ink, return_indices=True)
    a = a[iy, ix]
    a = ndimage.median_filter(a, (5, 5, 1))
    return ndimage.uniform_filter(a, (9, 9, 1))


def warp(img, p, shape, order=1, outside=None):
    """The hand map seen from the sheet: p = [tx, ty, sx, sy, deg], hand
    pixels per sheet pixel about both centres. `outside` marks what falls
    off the hand map (returned as a second array)."""
    tx, ty, sx, sy, deg = p
    a = math.radians(deg)
    c, s = math.cos(a), math.sin(a)
    H, W = shape
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    X, Y = xx - W / 2, yy - H / 2
    u = (c * X - s * Y) * sx + w / 2 + tx
    v = (s * X + c * Y) * sy + h / 2 + ty
    planes = [img] if img.ndim == 2 else [img[..., k] for k in range(img.shape[2])]
    out = [ndimage.map_coordinates(q, [v, u], order=order, mode="nearest") for q in planes]
    out = out[0] if img.ndim == 2 else np.stack(out, -1)
    if outside is None:
        return out
    return out, (u < 0) | (v < 0) | (u > w - 1) | (v > h - 1)


def fit_map(a, land, grid=256):
    """Shift, scale and turn that lay the hand map's land over the sheet's."""
    h, w = a.shape[:2]
    lr = np.asarray(Image.fromarray(land.astype(np.float32)).resize((grid, grid), Image.BILINEAR)) > 0.5
    deep_land = ndimage.distance_transform_edt(lr) > 8
    deep_sea = ndimage.distance_transform_edt(~lr) > 8

    def landness(p):
        aw = warp(a, p, (grid, grid))
        L, S = aw[deep_land].reshape(-1, 3), aw[deep_sea].reshape(-1, 3)
        mu1, mu0 = L.mean(0), S.mean(0)
        cov = np.cov(np.concatenate([L - mu1, S - mu0]).T) + np.eye(3)
        wv = np.linalg.solve(cov, mu1 - mu0)
        return 1.0 / (1.0 + np.exp(-(a @ wv - (mu1 + mu0) @ wv / 2)))

    def score(p, ln):
        lw = warp(ln, p, (grid, grid))
        return (lw * lr).mean() + ((1 - lw) * ~lr).mean()

    p = [0.0, 0.0, w / grid, h / grid, 0.0]
    steps = [w / grid * 2, h / grid * 2, w / grid * 0.02, h / grid * 0.02, 1.0]
    for _ in range(3):
        ln = landness(p)
        best = score(p, ln)
        for _ in range(40):
            moved = False
            for d in range(5):
                for sign in (-1, 1):
                    q = list(p)
                    q[d] += sign * steps[d]
                    if abs(q[4]) > 12:
                        continue
                    sc = score(q, ln)
                    if sc > best:
                        best, p, moved = sc, q, True
            if not moved:
                break
        steps = [x * 0.5 for x in steps]
    # In sheet pixels rather than the fitting grid's.
    return [p[0], p[1], p[2] * grid / land.shape[1], p[3] * grid / land.shape[0], p[4]], best


def ellipse(shape, cx, cy, rx, ry, deg, soft=0.25):
    h, w = shape
    ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
    a = math.radians(deg)
    u = ((xs - cx) * math.cos(a) + (ys - cy) * math.sin(a)) / rx
    v = (-(xs - cx) * math.sin(a) + (ys - cy) * math.cos(a)) / ry
    t = np.clip((1.0 + soft - np.sqrt(u * u + v * v)) / (2 * soft), 0, 1)
    return t * t * (3 - 2 * t)


def sheet_controls(base, sheet, index):
    """The sheet's four channels and ground ids in its own pixels, and its land."""
    c = sheet["controls"]
    metres, land, lakes = sheet_heights(base, sheet, index)
    a = clean(base / c["map"], c["crop"])
    if "fit" not in c:
        c["fit"], quality = fit_map(a, land | lakes)
        print(f"  fitted the hand map: {quality * 100:.0f} % agreement")
    shape = land.shape
    picture, off = warp(a, c["fit"], shape, outside=True)

    # Classes by the nearest of the map's own colours; off the map, and
    # wherever the map draws sea over the sheet's land, nothing yet.
    protos = np.array([q[:3] for q in c["classes"]], np.float32)
    names = [q[3] for q in c["classes"]]
    flat = picture.reshape(-1, 3)
    nearest = np.empty(flat.shape[0], np.int32)
    for i in range(0, flat.shape[0], 1 << 18):
        d = ((flat[i:i + (1 << 18), None, :] - protos[None]) ** 2).sum(-1)
        nearest[i:i + (1 << 18)] = d.argmin(1)
    # "none": a colour that is a mark, not ground (an ellipse's outline).
    cls = np.array([-1 if n == "none" else GROUND_ID[n] for n in names], np.int32)[nearest].reshape(shape)
    cls[off] = -1
    # Land the sheet has where the hand map draws sea: the region's own
    # fallback class when it names one (the Flame Islands' heights join
    # islands the map keeps apart - that ground is ash, not the nearest
    # mountain mark), else whatever is nearest.
    drawn_sea = np.isin(nearest.reshape(shape), [i for i, n in enumerate(names) if n == "none"]) & ~off

    # Rock only where the heights have it: the region's upper ground. A grey
    # the map puts over the sheet's low country is a misfit of the two.
    high = metres > np.percentile(metres[land], c.get("rock_above_pct", 55)) if land.any() else land
    cls[(cls == GROUND_ID["rock"]) & ~high] = -1
    if "fallback" in c:
        cls[drawn_sea & land] = GROUND_ID[c["fallback"]]
    known = (cls >= 0) & land
    if known.any():
        _, (iy, ix) = ndimage.distance_transform_edt(~known, return_indices=True)
        cls = np.where(known, cls, cls[iy, ix])

    # Soft majority: each class's share round a point, a few pixels wide, so
    # the pencil's ragged edges become a blend and not a speckle.
    sigma = c.get("blend_px", 4.0)
    used = sorted(set(np.unique(cls[land]).tolist()))
    share = {k: ndimage.gaussian_filter((cls == k).astype(np.float32), sigma) for k in used}
    values = dict(BASE_VALUES)
    values.update(c.get("values", {}))
    total = sum(share.values()) + 1e-6
    channel = {"moisture": 0.0, "forest": 0.0, "erosion": 0.0}
    for k in used:
        v = values[GROUND[k][1]]
        for j, name in enumerate(("moisture", "forest", "erosion")):
            channel[name] = channel[name] + share[k] * v[j]
    channel = {n: x / total for n, x in channel.items()}
    ground = np.array(used, np.int32)[np.argmax(np.stack([share[k] for k in used]), 0)]

    # Sand along the coast: a class within so many pixels of the sea becomes
    # another ("coast": {"from": [...], "to": "beach", "px": 14}), its values
    # blended in over the same band.
    if "coast" in c:
        co = c["coast"]
        sea_px = ndimage.distance_transform_edt(land | lakes).astype(np.float32)
        band = np.clip(1.0 - sea_px / co["px"], 0, 1)
        affected = np.isin(ground, [GROUND_ID[n] for n in co["from"]])
        w = band * ndimage.gaussian_filter(affected.astype(np.float32), sigma)
        v = values[co["to"]]
        for j, name in enumerate(("moisture", "forest", "erosion")):
            channel[name] = channel[name] * (1 - w) + v[j] * w
        ground = np.where(affected & (band > 0.5), GROUND_ID[co["to"]], ground)

    # Marks made with the heights (a mask in the sheet's pixels, saved by
    # tools/height_from_colour_map.py): [file, class].
    for file, name in c.get("marks", []):
        mark = np.load(base / file)
        if mark.shape != shape:
            mark = np.asarray(Image.fromarray(mark.astype(np.uint8) * 255).resize(shape[::-1], Image.NEAREST)) > 127
        soft = ndimage.gaussian_filter(mark.astype(np.float32), 1.5)
        v = values[name]
        for j, cname in enumerate(("moisture", "forest", "erosion")):
            channel[cname] = channel[cname] * (1 - soft) + v[j] * soft
        ground = np.where(mark & land, GROUND_ID[name], ground)

    # Mountain strength from the heights - how far up the region's own range
    # a point stands - and a little more where the map draws rock.
    if land.any():
        lo, hi = np.percentile(metres[land], 50), np.percentile(metres[land], 98)
        up = np.clip((metres - lo) / max(1.0, hi - lo), 0, 1)
    else:
        up = np.zeros(shape, np.float32)
    rock = share.get(GROUND_ID["rock"], 0) / total
    channel["mountain"] = np.clip(up * 0.8 + rock * 0.2, 0, 1)

    # The layers' biomes: this region's reading of its categories, then the
    # zones below.
    layers = {name: np.zeros(shape, np.int32) for name in LAYERS}
    for layer in LAYERS:
        for cname, biome in c.get(layer, {}).items():
            layers[layer] = np.where(ground == GROUND_ID[cname], BIOMES[layer][biome], layers[layer])

    # Zones the maps mark over the colour: an ellipse each, its own values.
    for cx, cy, rx, ry, deg, name, set_to in c.get("zones", []):
        z = ellipse(shape, cx, cy, rx, ry, deg)
        for k, v in set_to.items():
            channel[k] = channel[k] * (1 - z) + v * z
        inside = (z > 0.5) & land & (ground != GROUND_ID["rock"])
        if name in FOREST_ZONES:
            # What grows, over the ground the map has there.
            forest, decor = FOREST_ZONES[name]
            layers["forest"] = np.where(inside, BIOMES["forest"][forest], layers["forest"])
            if decor:
                layers["decor"] = np.where(inside, BIOMES["decor"][decor], layers["decor"])
            continue
        # Woods are not marked over bare rock; lava and a toothed ridge are rock.
        over_rock = name in ("lava", "jagged")
        ground = np.where((z > 0.5) & land & (over_rock | (ground != GROUND_ID["rock"])), GROUND_ID[name], ground)

    # This region's own categories for a class: a variant that inherits it
    # (Atrod's rock is smoother, the Flame Islands' dark).
    for cname, variant in c.get("ground", {}).items():
        ground = np.where(ground == GROUND_ID[cname], GROUND_ID[variant], ground)

    # The sheet's lakes: wet, no forest.
    channel["moisture"] = np.where(lakes, 0.95, channel["moisture"])
    channel["forest"] = np.where(lakes, 0.0, channel["forest"])
    ground = np.where(lakes, GROUND_ID["lake"], ground)
    # The region's readings again for what only now has its class (the lakes),
    # where no zone has said otherwise.
    for layer in LAYERS:
        for cname, biome in c.get(layer, {}).items():
            layers[layer] = np.where((ground == GROUND_ID[cname]) & (layers[layer] == 0), BIOMES[layer][biome], layers[layer])
    for layer in LAYERS:
        layers[layer] = np.where(land | lakes, layers[layer], 0)
    return channel, ground, layers, land | lakes, np.where(land, metres, 0.0)


def place(img, sheet, nx, ny, resample):
    """A sheet-sized picture into the world the way the heights were."""
    im = Image.fromarray(img)
    if sheet.get("rotate", 0):
        im = im.rotate(sheet["rotate"], resample, expand=True)
    sw, sh = int(sheet["w"]), int(sheet["h"])
    part = np.asarray(im.resize((sw, sh), resample))
    x, y = int(sheet["x"]), int(sheet["y"])
    x0, y0, x1, y1 = max(0, x), max(0, y), min(nx, x + sw), min(ny, y + sh)
    return part[y0 - y:y1 - y, x0 - x:x1 - x], (slice(y0, y1), slice(x0, x1))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    layout_file = Path(args[0] if args else "doc/references/heights/layout.json")
    base = layout_file.parent
    layout = json.loads(layout_file.read_text())
    if "--fit" in sys.argv:
        for sheet in layout["sheets"]:
            sheet.get("controls", {}).pop("fit", None)
    nx, ny = world_shape(layout)
    out = base / layout.get("out", "assembled")
    names = ("moisture", "forest", "mountain", "erosion")
    world = {n: np.full((ny, nx), DEFAULTS[n], np.float32) for n in names}
    weight = np.full((ny, nx), -1.0, np.float32)   # the height of the sheet each point took
    ground = np.zeros((ny, nx), np.uint8)
    biome_ids = {layer: np.zeros((ny, nx), np.uint8) for layer in LAYERS}
    for i, sheet in enumerate(layout["sheets"]):
        if "controls" not in sheet:
            continue
        print(f"{sheet.get('name', sheet['file'])}:")
        channel, ids, layer_ids, cover, metres = sheet_controls(base, sheet, i)
        cov, where = place(cover.astype(np.float32), sheet, nx, ny, Image.BILINEAR)
        lifted, _ = place(metres.astype(np.float32), sheet, nx, ny, Image.BILINEAR)
        # Where two sheets meet, the one whose ground the world took: the
        # heights keep the higher of the two (assemble_heightmap), and so do
        # these. Lakes count as low land, so they still claim their place.
        lifted = np.where(cov > 0.5, np.maximum(lifted, 1.0), -1.0)
        take = lifted > weight[where]
        for n in names:
            part, _ = place(channel[n].astype(np.float32), sheet, nx, ny, Image.BILINEAR)
            world[n][where] = np.where(take, part, world[n][where])
        part, _ = place(ids.astype(np.uint8), sheet, nx, ny, Image.NEAREST)
        ground[where] = np.where(take, part, ground[where])
        for layer in LAYERS:
            part, _ = place(layer_ids[layer].astype(np.uint8), sheet, nx, ny, Image.NEAREST)
            biome_ids[layer][where] = np.where(take, part, biome_ids[layer][where])
        weight[where] = np.where(take, lifted, weight[where])
        counts = np.bincount(ids[cover].ravel(), minlength=len(GROUND))
        print("  " + ", ".join(f"{GROUND[k][1]} {counts[k] * 100 / max(1, counts.sum()):.0f} %"
                               for k in range(len(GROUND)) if counts[k]))
    if "--fit" in sys.argv:
        layout_file.write_text(json.dumps(layout, indent=1, ensure_ascii=False))

    # Only land carries a control; the sea is the defaults exactly, so the
    # source keeps none of it (D159: every per-world array sparse by land).
    height = np.asarray(Image.open(out / "raster" / "height.png")).astype(np.float32)
    meta = json.loads((out / "world.json").read_text())
    h = meta["rasters"]["height"]
    metres = h["min_m"] + height / 65535.0 * (h["max_m"] - h["min_m"])
    sea = metres <= h.get("default_m", -60.0) + 0.05
    # Two regions meet over a few kilometres, not along a sheet's edge: each
    # channel blurred over the land alone, so the coast does not pull it
    # toward the sea's rest values.
    land = (~sea).astype(np.float32)
    spread = layout.get("controls_blend_samples", 6.0)
    under = ndimage.gaussian_filter(land, spread)
    for n in names:
        mixed = ndimage.gaussian_filter(world[n] * land, spread) / np.maximum(under, 1e-4)
        world[n] = np.where(sea, DEFAULTS[n], mixed)
    ground[sea] = 0
    for layer in LAYERS:
        biome_ids[layer][sea] = 0

    rgba = np.stack([np.round(np.clip(world[n], 0, 1) * 255) for n in names], -1).astype(np.uint8)
    (out / "raster").mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba).save(out / "raster" / "control_0.png", optimize=False)
    Image.fromarray(ground).save(out / "raster" / "ground.png", optimize=False)
    channel = lambda name, default, affects: {"name": name, "range": [0, 1], "default": default,
                                               "interpolation": "bilinear", "optional": True, "affects": affects}
    meta["rasters"]["control_0"] = {
        "file": "raster/control_0.png", "type": "rgba8", "kind": "control", "version": 1,
        "channels": {"R": channel("moisture_bias", 0.5, ["climate", "ecology"]),
                     "G": channel("forest_bias", 0.5, ["ecology", "vegetation"]),
                     "B": channel("mountain_strength", 0.0, ["terrain", "drainage"]),
                     "A": channel("erosion_strength", 0.5, ["terrain"])}}
    # Each categorical layer with its legend: the import renumbers by name.
    legend = {str(i): ("default" if i == 0 else n) for i, n, _ in GROUND}
    meta["rasters"]["ground"] = {"file": "raster/ground.png", "type": "u8", "kind": "categorical",
                                 "default": 0, "optional": True, "version": 1, "ids": legend}
    for layer in LAYERS:
        path = out / "raster" / (layer + ".png")
        if not biome_ids[layer].any():
            meta["rasters"].pop(layer, None)
            path.unlink(missing_ok=True)
            continue
        Image.fromarray(biome_ids[layer]).save(path, optimize=False)
        meta["rasters"][layer] = {"file": "raster/" + layer + ".png", "type": "u8", "kind": "categorical",
                                  "default": 0, "optional": True, "version": 1,
                                  "ids": {str(i): n for n, i in sorted(BIOMES[layer].items(), key=lambda t: t[1])}}
        counts = np.bincount(biome_ids[layer][~sea].ravel(), minlength=256)
        biome_names = {i: n for n, i in BIOMES[layer].items()}
        print(f"  {layer}: " + ", ".join(f"{biome_names[k]} {counts[k] * 100 / max(1, (~sea).sum()):.1f} %"
                                          for k in range(1, 256) if counts[k]))
    (out / "world.json").write_text(json.dumps(meta, indent=1))
    (out / "ground_legend.json").write_text(json.dumps(legend, indent=1))

    # The preview: ground ids, then the four channels in grey.
    k = max(1, max(nx, ny) // 768)
    palette = np.array([c for _, _, c in GROUND], np.uint8)
    tiles = [palette[np.minimum(ground[::k, ::k], len(GROUND) - 1)]]
    for n in names:
        g = (np.clip(world[n][::k, ::k], 0, 1) * 255).astype(np.uint8)
        tiles.append(np.repeat(g[..., None], 3, -1))
    gap = np.full((tiles[0].shape[0], 8, 3), 255, np.uint8)
    row = np.concatenate(sum(([t, gap] for t in tiles), [])[:-1], 1)
    Image.fromarray(row).save(out / "controls_preview.png")
    landish = ~sea
    print("written:", out / "raster" / "control_0.png", "and ground.png;",
          " ".join(f"{n} {world[n][landish].mean():.2f}" for n in names), "over land")


if __name__ == "__main__":
    main()
