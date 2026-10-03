#!/usr/bin/env python3
"""Builds the Fairytale world's authoring package from its painted heightmap.

    python3 tools/build_fairytale_package.py [--out DIR] [--preview-only]

The picture (doc/references/campfireWorld/Fairytale/*.png, 1254 px grey, black
sea) is turned into a 2 x 2 region world of 262 km: the sea and the coast are
made here (the picture's coast is a cliff of blobs), the grey is smoothed and
mapped to metres, and from the ground the control layers are derived:

  height.png      u16        metres, -200 .. 2600
  water.png       flags      lake bit: the caldera lake
  control_0.png   rgba8      moisture_bias, forest_bias, mountain_strength, erosion_strength
  control_1.png   rgba8      river_strength (main rivers, from the drainage of the height), spare
  ground.png      categorical  terrain categories (content/config/terrain/categories.json)

Then:  worldtool import <out> worlds/Fairytale/source
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "doc/references/campfireWorld/Fairytale/7b6f6f70-e293-4fb6-82c5-6832be26f036.png"
N, SAMPLE = 1024, 256            # samples a side, metres a sample: 262 144 m
LOW_M, HIGH_M, PEAK_M = -200.0, 2700.0, 2300.0
K = PEAK_M / 2400.0              # the thresholds below were tuned for a 2400 m peak
rng = np.random.default_rng(20261002)


def smooth(t):
    t = np.clip(t, 0, 1)
    return t * t * (3 - 2 * t)


def noise(scale_samples, seed):
    """Smooth 0..1 noise with features about `scale_samples` across."""
    r = np.random.default_rng(seed)
    a = ndi.gaussian_filter(r.standard_normal((N, N)), scale_samples, mode="wrap")
    a = (a - a.mean()) / (a.std() + 1e-9)
    return np.clip(0.5 + a * 0.25, 0, 1)


def load_ground():
    g = np.asarray(Image.open(SRC).convert("L").resize((N, N), Image.LANCZOS), np.float32)
    raw = g >= 10
    labels, n = ndi.label(~raw)
    border = set(np.unique(np.concatenate((labels[0], labels[-1], labels[:, 0], labels[:, -1]))))
    border.discard(0)
    sea = np.isin(labels, list(border))
    land = ~sea
    # The caldera lake is painted dark grey (about 30..55), not black: a dark
    # patch inside the land, away from the sea, big enough to be water.
    dark_labels, dn = ndi.label(land & (ndi.gaussian_filter(g, 1.0) < 60))
    lake = np.zeros_like(land)
    for k in range(1, dn + 1):
        m = dark_labels == k
        if m.sum() >= 250 and not (ndi.binary_dilation(m, iterations=6) & sea).any():
            lake |= m
    lake = ndi.binary_closing(lake, iterations=2)
    # The picture's coast is blobs of round stamps: round it off.
    soft = ndi.gaussian_filter(land.astype(np.float32), 5.0)
    land = soft > 0.5
    land |= lake
    return g, land, lake


def build_height(g, land, lake):
    # Lake bed takes the rim's grey so the smoothing does not dig a pit.
    rim = ndi.distance_transform_edt(lake, return_distances=False, return_indices=True)
    filled = np.where(lake, g[tuple(rim)], g)
    filled = np.where(land, filled, 0)
    gs = ndi.gaussian_filter(filled, 2.0)
    u = np.clip((gs - 60.0) / 195.0, 0, 1)
    hland = 15.0 + (PEAK_M - 15.0) * u ** 1.5
    d_in = ndi.distance_transform_edt(land) * SAMPLE
    # How the land climbs from the sea differs along the coast: here a cliff
    # (the full height within a kilometre), there a plain that rises over many
    # kilometres. The picture's own coast is a cliff everywhere, which read as a
    # thin beach outline and a wall behind it.
    plainness = smooth((noise(40, 901) - 0.50) / 0.16)
    reach = 1000.0 + 11000.0 * plainness ** 1.2
    ramp = smooth(d_in / reach)
    ramp = ramp ** (1.0 + 0.8 * plainness)
    h = 8.0 + (hland - 8.0) * ramp
    # the cliffs get a little ledge at the foot and a ragged top
    cliff = (1 - plainness) * smooth(d_in / 300.0) * (1 - smooth(d_in / 1400.0))
    h += cliff * 18.0 * noise(6, 902)
    h = np.maximum(h, 6.0 + 10.0 * smooth(d_in / 600.0))
    # Ledges and cliff faces in the mountains: in places the slope climbs in
    # steps of ~55 m with near-vertical faces, in others it stays smooth.
    mountain = smooth((h - 450.0) / 500.0)
    faces = smooth((noise(18, 950) - 0.42) / 0.22)
    q = h / 55.0
    f = q - np.floor(q)
    stairs = (np.floor(q) + smooth((f - 0.30) / 0.40)) * 55.0
    h = h + (stairs - h) * 0.55 * mountain * faces
    d_out = ndi.distance_transform_edt(~land) * SAMPLE
    hsea = -(8.0 + 192.0 * smooth(d_out / 14000.0))
    h = np.where(land, h, hsea)
    # The caldera lake: a bowl below the lowest part of its rim.
    if lake.any():
        shore = ndi.binary_dilation(lake, iterations=2) & ~lake
        level = np.percentile(h[shore], 8) if shore.any() else 100.0
        d_lake = ndi.distance_transform_edt(lake) * SAMPLE
        bed = level - 4.0 - 38.0 * smooth(d_lake / 1800.0)
        h = np.where(lake, bed, h)
    return h.astype(np.float32), d_in, d_out


def slope_of(h):
    gy, gx = np.gradient(h, SAMPLE)
    return np.hypot(gx, gy)


def main_rivers(h, land, lake, slope):
    """Main rivers by the drainage of the height: D8 flow accumulation over the
    filled ground, the biggest courses kept, drawn with a strength that grows
    downstream. Painted onto the 256 m grid; the generator adds water there."""
    from heapq import heappush, heappop
    n = N
    filled = h.copy()
    filled[~land] = -1e3
    # priority-flood fill so every land cell drains to the sea or a lake
    res = np.full((n, n), np.inf, np.float32)
    seen = np.zeros((n, n), bool)
    heap = []
    edge = ~land | lake
    sy, sx = np.nonzero(edge & ndi.binary_dilation(land & ~lake, iterations=1))
    for y, x in zip(*np.nonzero(edge)):
        res[y, x] = filled[y, x]; seen[y, x] = True; heappush(heap, (float(res[y, x]), int(y), int(x)))
    nb = [(-1, 0), (1, 0), (0, -1), (0, 1), (-1, -1), (-1, 1), (1, -1), (1, 1)]
    order = []
    while heap:
        z, y, x = heappop(heap)
        order.append((y, x))
        for dy, dx in nb:
            yy, xx = y + dy, x + dx
            if 0 <= yy < n and 0 <= xx < n and not seen[yy, xx]:
                seen[yy, xx] = True
                res[yy, xx] = max(float(filled[yy, xx]), z + 0.01)
                heappush(heap, (float(res[yy, xx]), yy, xx))
    flow = np.where(land & ~lake, 1.0, 0.0).astype(np.float64)
    down = {}
    for y, x in reversed(order):
        if not land[y, x] or lake[y, x]:
            continue
        best, bd = None, 0.0
        for dy, dx in nb:
            yy, xx = y + dy, x + dx
            if 0 <= yy < n and 0 <= xx < n:
                drop = (res[y, x] - res[yy, xx]) / (1.414 if dy and dx else 1.0)
                if drop > bd:
                    bd, best = drop, (yy, xx)
        if best is not None:
            flow[best] += flow[y, x]
            down[(y, x)] = best
    return flow, down, res


def paint_rivers(flow, down, land, lake, threshold_cells=900, min_len=60):
    n = N
    strength = np.zeros((n, n), np.float32)
    big = flow >= threshold_cells
    # keep only courses that reach the sea or a lake and are long
    cells = np.argwhere(big)
    for y, x in cells:
        s = np.log2(flow[y, x] / threshold_cells)          # 0 at the head
        v = 0.25 + 0.75 * min(1.0, s / 4.0)
        strength[y, x] = v
    # the lake feeds out: continue the course from the lake rim downhill is the generator's job
    strength = ndi.maximum_filter(strength, size=3)
    strength = ndi.gaussian_filter(strength, 0.8)
    strength[~land] = 0
    keep, kn = ndi.label(strength > 0.15)
    for k in range(1, kn + 1):
        if (keep == k).sum() < 40:
            strength[keep == k] = 0
    return np.clip(strength * 1.6, 0, 1)


def save_u16(path, arr):
    Image.fromarray(arr.astype(np.uint16)).save(path)


CATEGORIES = json.loads((ROOT / "content/config/terrain/categories.json").read_text())["categories"]
IDS = {c["name"]: c["id"] for c in CATEGORIES}
DECORS = json.loads((ROOT / "content/config/terrain/decor_biomes.json").read_text())["decor_biomes"]
DECOR = {d["name"]: d["id"] for d in DECORS}
# What each category asks of the control maps: moisture, forest (categories.json controls).
CONTROLS = {c["id"]: c.get("controls", {}) for c in CATEGORIES}


def classify(h, land, lake, d_in, slope, labels, river):
    n = N
    ground = np.zeros((n, n), np.uint8)
    hs = ndi.gaussian_filter(h, 1.0)
    sl = ndi.gaussian_filter(slope, 1.5)
    sizes = ndi.sum(np.ones_like(labels), labels, range(1, labels.max() + 1))
    rank = np.argsort(-sizes)
    continent = np.isin(labels, [rank[0] + 1, rank[1] + 1])
    island = land & ~continent & ~lake

    # --- measures -------------------------------------------------------
    low = 1 - smooth((hs - 15) / 150.0)
    hollow = smooth((ndi.gaussian_filter(h, 12) - h) / 25.0)
    flat = 1 - smooth(sl / 0.08)
    wet = np.clip(0.5 * low + 0.3 * hollow + 0.4 * flat * low + 0.12 * (noise(14, 11) - 0.5), 0, 1)
    flat_wide = 1 - smooth((ndi.gaussian_filter(slope, 3.0) - 0.04) / 0.10)
    plateau = flat_wide * smooth((hs - 420 * K) / (200.0 * K)) * (1 - smooth((hs - 1900 * K) / (250.0 * K))) * smooth(d_in / 5000.0)
    valley = flat * smooth((ndi.gaussian_filter(h, 10) - h) / 6.0) * (1 - smooth((hs - 700 * K) / (250.0 * K)))
    open_flat = flat * (1 - low)

    rock = land & ~lake & ((sl > 0.24 * K) | (hs > 1800 * K)) & continent
    rock = ndi.binary_opening(rock, iterations=1)
    near_rock = ndi.distance_transform_edt(~rock) * SAMPLE
    near_river = ndi.distance_transform_edt(river < 0.2) * SAMPLE

    # --- the desert: the south of the central massif, in the rain shadow --------
    yy, xx = np.mgrid[0:n, 0:n] / float(n)
    warp = (noise(40, 31) - 0.5) * 0.10
    dd = np.hypot((xx - 0.45 + warp) * 1.0, (yy - 0.70 + warp * 0.8) * 1.2)
    desert = land & continent & ~rock & (dd < 0.135) & (sl < 0.2) & (hs > 120) & (hs < 1250 * K) & (wet < 0.7)
    steppe = land & continent & ~rock & ~desert & (dd < 0.185) & (sl < 0.22) & (wet < 0.6)

    # --- the realistic country: what most of the world is -----------------------
    # Temperate wood (fertile), open valley shrubland (atrod_valley) and sparse
    # wood on the drier hills, mixed by broad noise.
    nA, nB = noise(34, 101), noise(30, 102)
    base = np.full((n, n), IDS["fertile"], np.uint8)
    base[(nA > 0.56) & (open_flat > 0.10)] = IDS["atrod_valley"]
    base[(nB > 0.64) & (wet < 0.45)] = IDS["sparse"]
    ground[:] = base

    # --- the fairy territories: one of each, each on its own ground --------------
    # Soft-edged discs round a centre (in samples), the rim torn by noise, so a
    # territory is a place with a border and not a texture laid over the world.
    def territory(cx, cy, radius, seed):
        rim = (noise(18, seed) - 0.5) * 0.55 + (noise(7, seed + 1) - 0.5) * 0.18
        return np.hypot(xx * n - cx, yy * n - cy) < radius * (1.0 + rim)

    lake_cx, lake_cy = (np.nonzero(lake)[1].mean(), np.nonzero(lake)[0].mean()) if lake.any() else (655.0, 335.0)
    flowers = territory(lake_cx, lake_cy, 78, 300) & ~rock                      # the flower valley round the lake
    magic_wood = territory(lake_cx, lake_cy, 175, 310) & ~flowers & ~rock       # bright wood up the slopes
    amber = territory(105, 545, 125, 320) & ~rock
    dark = territory(480, 185, 105, 330) & ~rock
    silver = territory(585, 520, 100, 340) & ~rock
    bog = territory(635, 700, 105, 350) & ~rock
    ground[silver & (open_flat > 0.15)] = IDS["silver_meadow"]
    ground[amber] = IDS["amber_grove"]
    ground[dark] = IDS["dark_wood"]
    ground[magic_wood & ((sl > 0.05 * K) | (hs > 450 * K))] = IDS["blossom_grove"]
    ground[magic_wood & (ground == base) & (noise(25, 360) > 0.50)] = IDS["blossom_grove"]
    ground[flowers] = IDS["glade"]

    # --- the bog territory: moss and pools, fen by the rivers, peat on the flat --
    inland = d_in > 2500
    wetv = np.clip(np.maximum(wet, 0.9 * valley) + 0.10 * (noise(18, 12) - 0.5), 0, 1)
    ground[bog] = IDS["moor_marsh"]
    ground[bog & (near_river < 1800)] = IDS["river_fen"]
    ground[bog & (flat > 0.55) & (near_river > 900) & inland & (hs < 520)] = IDS["peat_plain"]

    ground[steppe] = IDS["savanna"]
    ground[desert] = IDS["golden_desert"]

    # --- mountains and their conifer skirts -------------------------------------
    skirt = continent & ~rock & ~desert & ~steppe & (hs > 850 * K) & (hs <= 1700 * K) & (sl < 0.30 * K) & (sl > 0.10 * K)
    ground[skirt] = IDS["conifer"]
    ground[rock] = IDS["rock"]

    # --- the islands and the coasts ------------------------------------------
    ground[island] = IDS["isle_green"]
    ground[island & (d_in < 900)] = IDS["pearl_coast"]
    ground[continent & land & (d_in < 500) & ~rock] = IDS["beach"]
    ground[~land] = 0
    # --- decor: cliffs and scree on the mountain, boulders along the rivers -----
    decor = np.zeros((n, n), np.uint8)
    cliffy = (rock | (skirt & (sl > 0.14 * K))) & ~lake
    decor[cliffy] = DECOR["alpine_cliffs"]
    banks = (near_river < 640) & land & ~lake & ~rock
    decor[banks] = DECOR["river_stones"]
    return ground, dict(wet=wet, rock=rock, desert=desert, island=island, hs=hs, sl=sl, decor=decor, near_river=near_river)


def bog_pools(h, ground, lake, land):
    """Wide shallow pools between the mossy islands of the bogs: blobs of noise
    in the marsh categories, a kilometre or two across (the generator keeps
    water in basins a macro cell of 512 m can see), a few metres deep, held as
    lakes. Each pool is ringed by a shelf, so the water is shallow at the edge."""
    marsh = np.isin(ground, [IDS["moor_marsh"], IDS["peat_plain"], IDS["river_fen"]])
    big = noise(4.2, 960)          # pools ~1 km across
    wide = noise(9.0, 961)         # where the bog is pitted at all
    pool = marsh & (big > 0.57) & (wide > 0.40)
    pool = ndi.binary_opening(pool, structure=np.ones((3, 3)))
    labels, count = ndi.label(pool)
    for k in range(1, count + 1):
        if (labels == k).sum() < 10:
            pool[labels == k] = False
    if pool.any():
        depth = ndi.distance_transform_edt(pool)
        h = h - 3.5 * smooth(depth / 3.0) * pool
        lake = lake | pool
    return h, ground, lake


def control_maps(ground, extra, h, land):
    n = N
    moist = np.full((n, n), 0.5, np.float32)
    forest = np.full((n, n), 0.5, np.float32)
    for cid, c in CONTROLS.items():
        m = ground == cid
        if not m.any():
            continue
        # The imported heights are high and steep, so the rain shadow dries
        # everything: the maps lean wetter and greener than the categories ask,
        # except where dry is the point (the desert and its steppe).
        dry = cid in (IDS["golden_desert"], IDS["savanna"])
        if "moisture" in c:
            moist[m] = c["moisture"] if dry else min(1.0, c["moisture"] + 0.22)
        if "forest" in c:
            forest[m] = c["forest"] if dry else min(1.0, c["forest"] + 0.15)
    moist = ndi.gaussian_filter(moist, 4.0) + 0.06 * (noise(20, 7) - 0.5)
    forest = ndi.gaussian_filter(forest, 3.0) + 0.10 * (noise(14, 8) - 0.5)
    mountain = ndi.gaussian_filter(extra["rock"].astype(np.float32), 6.0) * smooth((extra["hs"] - 700) / 1000.0) * 1.2
    erosion = np.where(extra["rock"], 0.65, np.where(extra["wet"] > 0.55, 0.25, 0.4)).astype(np.float32)
    erosion = ndi.gaussian_filter(erosion, 5.0)
    rgba = np.stack([np.clip(moist, 0, 1), np.clip(forest, 0, 1), np.clip(mountain, 0, 1), np.clip(erosion, 0, 1)], -1)
    rgba[~land] = (0.5, 0.0, 0.0, 0.5)
    return (rgba * 255 + 0.5).astype(np.uint8)


def world_json(extra_rasters):
    ids = {str(c["id"]): c["name"] for c in CATEGORIES}
    return {
        "format": "campfire.world-authoring", "version": 1, "stage": "water",
        "world": {"width_m": float(N * SAMPLE), "height_m": float(N * SAMPLE), "sample_m": SAMPLE, "chunk_m": 32768},
        "origin_m": [0.0, 0.0], "size_m": [float(N * SAMPLE), float(N * SAMPLE)], "vectors": {},
        "rasters": {
            "height": {"file": "raster/height.png", "type": "u16", "kind": "height", "min_m": LOW_M, "max_m": HIGH_M,
                       "default_m": -60.0, "interpolation": "bicubic", "optional": False, "version": 1},
            "water": {"file": "raster/water.png", "type": "u8", "kind": "flags", "default": 0, "bits": {"lake": 0},
                      "optional": True, "version": 1},
            "control_0": {"file": "raster/control_0.png", "type": "rgba8", "kind": "control", "version": 1, "channels": {
                "R": {"name": "moisture_bias", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["climate", "ecology"]},
                "G": {"name": "forest_bias", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["ecology", "vegetation"]},
                "B": {"name": "mountain_strength", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True, "affects": ["terrain", "drainage"]},
                "A": {"name": "erosion_strength", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["terrain"]}}},
            "control_1": {"file": "raster/control_1.png", "type": "rgba8", "kind": "control", "version": 1, "channels": {
                "R": {"name": "river_strength", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True, "affects": ["drainage"]},
                "G": {"name": "lake_mask", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True},
                "B": {"name": "temperature_bias", "range": [0, 1], "default": 0.5, "interpolation": "bilinear", "optional": True, "affects": ["climate"]},
                "A": {"name": "reserved_a", "range": [0, 1], "default": 0, "interpolation": "bilinear", "optional": True}}},
            "ground": {"file": "raster/ground.png", "type": "u8", "kind": "categorical", "default": 0, "optional": True,
                       "version": 1, "ids": ids},
            "decor": {"file": "raster/decor.png", "type": "u8", "kind": "categorical", "default": 0, "optional": True,
                      "version": 1, "ids": {str(d["id"]): d["name"] for d in DECORS}},
        },
    }


def preview(ground, river, h, land):
    palette = {0: (20, 40, 80), 4: (150, 150, 150), 7: (60, 90, 80), 12: (40, 120, 200), 15: (200, 180, 90), 18: (230, 220, 180)}
    colours = {IDS["fertile"]: (70, 110, 70), IDS["sparse"]: (150, 150, 100), IDS["atrod_valley"]: (175, 175, 95),
               IDS["glade"]: (255, 240, 120), IDS["silver_meadow"]: (190, 220, 200), IDS["amber_grove"]: (230, 130, 40),
               IDS["blossom_grove"]: (240, 160, 200), IDS["moor_marsh"]: (110, 140, 90), IDS["peat_plain"]: (110, 80, 50),
               IDS["river_fen"]: (70, 150, 120), IDS["dark_wood"]: (20, 70, 40), IDS["pearl_coast"]: (255, 245, 220),
               IDS["isle_green"]: (110, 200, 110), IDS["golden_desert"]: (235, 190, 90)}
    palette.update(colours)
    img = np.zeros((N, N, 3), np.uint8)
    for k, c in palette.items():
        img[ground == k] = c
    shade = np.clip(0.75 + 0.25 * ndi.gaussian_filter(np.gradient(h, axis=1) * -0.01, 1.0), 0.5, 1.2)
    img = np.clip(img * shade[..., None], 0, 255).astype(np.uint8)
    img[river > 0.15] = (30, 90, 255)
    im = Image.fromarray(img).resize((N * 2 // 1, N * 2 // 1), Image.NEAREST) if False else Image.fromarray(img)
    return label_map(im, ground)


def label_map(im, ground):
    """The map with a 50 km grid and the name of every biome at its widest place."""
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    km = 1000.0 / SAMPLE                       # samples per km
    for k in range(0, 263, 50):
        x = k * km
        d.line([(x, 0), (x, N)], fill=(255, 255, 255, 60), width=1)
        d.line([(0, x), (N, x)], fill=(255, 255, 255, 60), width=1)
        d.text((x + 2, 2), "%d km" % k, fill=(255, 255, 255))
        d.text((2, x + 2), "%d km" % k, fill=(255, 255, 255))
    for cat in CATEGORIES:
        if cat["id"] < 28 and cat["name"] not in ("rock", "conifer", "beach"):
            continue
        m = ground == cat["id"]
        if m.sum() < 400:
            continue
        dist = ndi.distance_transform_edt(m)
        y, x = np.unravel_index(dist.argmax(), dist.shape)
        d.text((x - 24, y - 5), cat["name"], fill=(0, 0, 0))
        d.text((x - 25, y - 6), cat["name"], fill=(255, 255, 255))
    return im


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, default=ROOT / "doc/references/campfireWorld/Fairytale_Authoring_Package")
    ap.add_argument("--preview", type=Path, default=None)
    ap.add_argument("--river-cells", type=int, default=1800)
    args = ap.parse_args()
    g, land, lake = load_ground()
    h, d_in, d_out = build_height(g, land, lake)
    slope = slope_of(ndi.gaussian_filter(h, 1.0))
    labels, count = ndi.label(land)
    flow, down, res = main_rivers(h, land, lake, slope)
    river = paint_rivers(flow, down, land, lake, args.river_cells)
    ground, extra = classify(h, land, lake, d_in, slope, labels, river)
    # A lake across a region's border would be cut straight there (each region
    # holds its own water): keep them 3 km clear of the borders (every 512 samples).
    border = np.zeros((N, N), bool)
    for k in range(512, N, 512):
        border[:, max(0, k - 12):k + 12] = True
        border[max(0, k - 12):k + 12, :] = True
    lake &= ~border
    # What lies under the water is the ground round it, not a category of its own.
    nearest = ndi.distance_transform_edt(lake, return_distances=False, return_indices=True)
    ground = np.where(lake, ground[tuple(nearest)], ground)
    ctrl = control_maps(ground, extra, h, land)
    print("land %.2f  lake px %d  max h %.0f  river px %d" % (land.mean(), lake.sum(), h.max(), (river > 0.15).sum()))
    for name, cid in IDS.items():
        share = (ground == cid).sum() / max(1, land.sum())
        if share > 0.001:
            print("  %-16s %5.1f%%" % (name, share * 100))
    out = args.out
    (out / "raster").mkdir(parents=True, exist_ok=True)
    save_u16(out / "raster/height.png", np.clip((h - LOW_M) / (HIGH_M - LOW_M), 0, 1) * 65535 + 0.5)
    Image.fromarray((lake.astype(np.uint8))).save(out / "raster/water.png")
    Image.fromarray(ctrl, "RGBA").save(out / "raster/control_0.png")
    rv = np.zeros((N, N, 4), np.uint8)
    rv[..., 0] = np.clip(river * 255 + 0.5, 0, 255).astype(np.uint8)
    rv[..., 1] = lake.astype(np.uint8) * 255      # the lakes again, to read and paint in a picture editor
    # temperature_bias: colder up the mountains' flank and in the north, warmer
    # in the south and on the resort islands (0.5 = as the climate makes it)
    yy2 = np.linspace(0, 1, N)[:, None]
    warm = 0.5 + 0.10 * (yy2 - 0.5) + 0.08 * extra["island"] - 0.10 * smooth((h - 900.0) / 900.0)
    rv[..., 2] = np.clip(warm * 255 + 0.5, 0, 255).astype(np.uint8)
    rv[..., 3] = 255                              # opaque: an image editor shows it instead of nothing
    Image.fromarray(rv, "RGBA").save(out / "raster/control_1.png")
    hydro = np.zeros((N, N, 3), np.uint8)
    shade = np.clip(0.35 + 0.65 * (h - h.min()) / (h.max() - h.min()), 0, 1)
    hydro[...] = (shade[..., None] * 90).astype(np.uint8)
    hydro[~land] = (10, 20, 45)
    hydro[lake] = (80, 200, 255)
    hydro[river > 0.05] = (30, 110, 255)
    Image.fromarray(hydro).save(out / "preview_hydrology.png")
    Image.fromarray(ground).save(out / "raster/ground.png")
    Image.fromarray(extra["decor"]).save(out / "raster/decor.png")
    (out / "world.json").write_text(json.dumps(world_json(None), indent=2) + "\n")
    preview(ground, river, h, land).save(out / "preview_biomes.png")
    if args.preview:
        preview(ground, river, h, land).save(args.preview)
    print("wrote", out)


if __name__ == "__main__":
    main()
