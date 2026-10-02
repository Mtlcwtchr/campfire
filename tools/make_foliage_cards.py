#!/usr/bin/env python3
"""Ground flora cards beyond the six imported grass views: the meadow flowers,
ferns, reeds, dry and short grass a painted countryside is made of.

    python3 tools/make_foliage_cards.py [--out assets/generated/foliage_cards] [--preview]

Each card is a 256x256 RGBA picture of a clump standing on the bottom edge,
drawn the way the imported views are used: a camera-facing quad, roots at the
bottom, alpha-tested. Colours are albedo. Green parts are tinted by the
renderer with the climate's plant colour (relative to a temperate meadow);
anything that is not green - petals, seed heads, dry stems, cattails - keeps
its own colour (foliage.hlsl, foliageCardAlbedo).

Deterministic: the same script draws the same cards byte for byte. Order is
the layer order after the six imported views (cards.json), which the shaders
know by index (foliage_field.hlsli, kFoliageCard*).
"""
import argparse
import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

SIZE = 256
SS = 3                      # supersampling
W = SIZE * SS

# Temperate leaf albedo the renderer's tint is relative to (foliage.hlsl).
LEAF_DARK = (0.10, 0.17, 0.035)
LEAF_MID = (0.17, 0.27, 0.05)
LEAF_LIGHT = (0.27, 0.37, 0.08)
LEAF_YELLOW = (0.30, 0.34, 0.07)


def lerp(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def jitter(rng, colour, amount=0.12):
    k = 1.0 + rng.uniform(-amount, amount)
    return tuple(max(0.0, min(1.0, c * k)) for c in colour)


class Card:
    def __init__(self):
        self.img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
        self.draw = ImageDraw.Draw(self.img)

    def poly(self, points, colour, shade=1.0):
        c = tuple(int(max(0, min(255, round(v * shade * 255)))) for v in colour)
        self.draw.polygon([(x * W, (1 - y) * W) for x, y in points], fill=c + (255,))

    def ellipse(self, cx, cy, rx, ry, colour, shade=1.0):
        c = tuple(int(max(0, min(255, round(v * shade * 255)))) for v in colour)
        self.draw.ellipse([(cx - rx) * W, (1 - cy - ry) * W, (cx + rx) * W, (1 - cy + ry) * W], fill=c + (255,))

    def blade(self, x0, length, lean, curve, width, root, tip, y0=0.0, segments=10, occlusion=0.35):
        """A tapered blade along a bent curve. lean: radians off vertical;
        curve: extra bend towards the tip. Coordinates are card units (0..1)."""
        pts = []
        for i in range(segments + 1):
            t = i / segments
            a = lean + curve * t * t
            # integrate the direction: cheap closed form good enough here
            x = x0 + length * (math.sin(lean) * t + curve * t * t * t / 3.0 * math.cos(lean))
            y = y0 + length * (math.cos(lean) * t - curve * t * t * t / 3.0 * math.sin(lean) * 0.5)
            pts.append((x, y, a, t))
        for i in range(segments):
            (xa, ya, aa, ta), (xb, yb, ab, tb) = pts[i], pts[i + 1]
            wa = width * (1 - ta) ** 0.8 + 0.0015
            wb = width * (1 - tb) ** 0.8 + 0.0015
            na = (math.cos(aa), -math.sin(aa))
            nb = (math.cos(ab), -math.sin(ab))
            quad = [(xa - na[0] * wa, ya - na[1] * wa), (xa + na[0] * wa, ya + na[1] * wa),
                    (xb + nb[0] * wb, yb + nb[1] * wb), (xb - nb[0] * wb, yb - nb[1] * wb)]
            tm = (ta + tb) * 0.5
            height = (ya + yb) * 0.5
            shade = 1.0 - occlusion * (1.0 - min(1.0, height / 0.55))
            self.poly(quad, lerp(root, tip, tm), shade)
        x, y, a, _ = pts[-1]
        return x, y, a

    def result(self):
        img = self.img.resize((SIZE, SIZE), Image.LANCZOS)
        a = np.asarray(img).astype(np.float32) / 255.0
        # Hard-ish alpha: these are alpha-tested; keep a short soft edge.
        alpha = np.clip((a[..., 3] - 0.15) / 0.55, 0, 1)
        rgb = a[..., :3] / np.maximum(a[..., 3:4], 1e-4)
        rgb = np.where(a[..., 3:4] > 0.02, rgb, 0)
        rgb = bleed(rgb, a[..., 3] > 0.02)
        out = np.concatenate([np.clip(rgb, 0, 1), alpha[..., None]], -1)
        return Image.fromarray((out * 255 + 0.5).astype(np.uint8))


def bleed(rgb, known):
    """Push colour into the transparent texels (push-pull), so the mip chain of
    an alpha-tested card has no dark fringe."""
    levels = [(rgb * known[..., None], known.astype(np.float32))]
    while levels[-1][1].shape[0] > 1:
        c, w = levels[-1]
        h = c.shape[0] // 2
        c2 = c.reshape(h, 2, h, 2, 3).sum((1, 3))
        w2 = w.reshape(h, 2, h, 2).sum((1, 3))
        levels.append((c2, w2))
    fill = levels[-1][0] / np.maximum(levels[-1][1][..., None], 1e-6)
    for c, w in reversed(levels[:-1]):
        fill = np.repeat(np.repeat(fill, 2, 0), 2, 1)
        mean = c / np.maximum(w[..., None], 1e-6)
        fill = np.where(w[..., None] > 0, mean, fill)
    return np.where(known[..., None], rgb, fill)


def grass_tuft(card, rng, n, height, spread, colours, lean=0.35, width=0.012, curve=0.6, cx=0.5):
    for _ in range(n):
        x0 = cx + rng.normal(0, spread)
        if not 0.04 < x0 < 0.96:
            continue
        side = (x0 - cx) / max(spread, 1e-3)
        l = height * rng.uniform(0.45, 1.0)
        a = side * lean * 0.5 + rng.normal(0, lean * 0.45)
        c = rng.uniform(-curve, curve) + math.copysign(curve * 0.5, a)
        root, tip = colours[rng.integers(len(colours))]
        card.blade(x0, l, a, c, width * rng.uniform(0.7, 1.3), jitter(rng, root), jitter(rng, tip))


GREEN = [(LEAF_DARK, LEAF_LIGHT), (LEAF_DARK, LEAF_MID), (LEAF_MID, LEAF_YELLOW), (LEAF_DARK, LEAF_YELLOW)]


def short_grass(rng):
    card = Card()
    grass_tuft(card, rng, 260, 0.62, 0.20, GREEN, lean=0.5, width=0.010, curve=0.9)
    grass_tuft(card, rng, 60, 0.85, 0.12, GREEN, lean=0.3, width=0.011, curve=0.5)
    return card


def seed_grass(rng):
    card = Card()
    grass_tuft(card, rng, 130, 0.55, 0.20, GREEN, lean=0.45, width=0.011, curve=0.8)
    straw = (0.62, 0.55, 0.32)
    for _ in range(22):
        x0 = 0.5 + rng.normal(0, 0.15)
        x, y, a = card.blade(x0, rng.uniform(0.70, 0.97), rng.normal(0, 0.18), rng.uniform(-0.3, 0.3), 0.004,
                             LEAF_MID, (0.45, 0.45, 0.18))
        for k in range(7):   # a nodding panicle of seeds
            t = k / 7
            card.ellipse(x + math.sin(a) * t * 0.06 + rng.normal(0, 0.006), y - t * 0.07,
                         0.006, 0.012, jitter(rng, straw, 0.15))
    return card


def flowers(rng, petal, centre, kind):
    card = Card()
    grass_tuft(card, rng, 120, 0.50, 0.21, GREEN, lean=0.45, width=0.010, curve=0.8)
    heads = rng.integers(9, 15)
    for _ in range(heads):
        x0 = 0.5 + rng.normal(0, 0.17)
        if not 0.08 < x0 < 0.92:
            continue
        x, y, a = card.blade(x0, rng.uniform(0.42, 0.88), rng.normal(0, 0.15), rng.uniform(-0.2, 0.2), 0.0045,
                             LEAF_DARK, LEAF_MID)
        # a couple of stem leaves
        for _ in range(2):
            card.blade(x0 + rng.normal(0, 0.01), rng.uniform(0.12, 0.25), rng.choice([-1, 1]) * rng.uniform(0.5, 0.9),
                       0.4, 0.010, LEAF_DARK, LEAF_MID, y0=rng.uniform(0.05, 0.25))
        r = rng.uniform(0.022, 0.034)
        p = jitter(rng, petal, 0.08)
        if kind == "daisy":
            for k in range(10):
                t = k / 10 * 2 * math.pi
                card.ellipse(x + math.cos(t) * r * 0.75, y + math.sin(t) * r * 0.45, r * 0.42, r * 0.28, p)
            card.ellipse(x, y, r * 0.38, r * 0.30, centre)
        elif kind == "cup":
            card.ellipse(x, y, r * 0.95, r * 0.70, p)
            card.ellipse(x, y + r * 0.25, r * 0.65, r * 0.35, lerp(p, (1, 1, 0.8), 0.3))
            card.ellipse(x, y - r * 0.1, r * 0.25, r * 0.2, centre)
        elif kind == "spike":       # lupine / foxglove / lavender: a column of florets
            for k in range(9):
                t = k / 9
                card.ellipse(x + rng.normal(0, 0.004), y + t * 0.11 - 0.02, r * (0.62 - 0.4 * t), r * 0.42,
                             lerp(p, (1, 1, 1), 0.12 * (k % 2)))
        elif kind == "poppy":
            card.ellipse(x, y, r * 1.05, r * 0.85, p)
            card.ellipse(x - r * 0.3, y + r * 0.15, r * 0.6, r * 0.5, lerp(p, (1, 0.3, 0.2), 0.3))
            card.ellipse(x, y, r * 0.28, r * 0.25, centre)
    return card


def fern(rng):
    card = Card()
    fronds = rng.integers(7, 10)
    for i in range(fronds):
        side = (i / (fronds - 1)) * 2 - 1
        lean = side * 0.95 + rng.normal(0, 0.12)
        length = rng.uniform(0.62, 0.92) * (1 - 0.25 * abs(side))
        curve = -math.copysign(rng.uniform(0.6, 1.1), lean) * -1
        x0 = 0.5 + rng.normal(0, 0.02)
        segs = 22
        prev = None
        for k in range(segs + 1):
            t = k / segs
            a = lean + curve * t * t * 0.9
            x = x0 + length * (math.sin(lean) * t + curve * t ** 3 / 3 * math.cos(lean))
            y = length * (math.cos(lean) * t - curve * t ** 3 / 3 * math.sin(lean) * 0.6)
            if prev is not None and 0.12 < t:
                # leaflets on both sides, shortening to the tip
                size = 0.11 * (1 - t) ** 0.7 * (0.6 + 0.4 * math.sin(t * math.pi) ** 0.5) * length
                for s in (-1, 1):
                    card.blade(x, size, a + s * 1.2, -s * 0.5, 0.012 * (1 - t) + 0.004,
                               jitter(rng, LEAF_DARK, 0.08), jitter(rng, lerp(LEAF_MID, LEAF_LIGHT, 0.5), 0.1),
                               y0=y, segments=4, occlusion=0.25)
            prev = (x, y)
        card.blade(x0, length, lean, curve * 0.9, 0.006, LEAF_DARK, LEAF_MID, segments=16)
    return card


def dry_grass(rng):
    card = Card()
    straw = [((0.36, 0.30, 0.14), (0.70, 0.60, 0.36)), ((0.40, 0.34, 0.16), (0.78, 0.68, 0.42)),
             ((0.30, 0.27, 0.12), (0.62, 0.52, 0.30)), (LEAF_DARK, (0.55, 0.50, 0.25))]
    grass_tuft(card, rng, 200, 0.78, 0.18, straw, lean=0.40, width=0.010, curve=0.7)
    for _ in range(14):
        x, y, a = card.blade(0.5 + rng.normal(0, 0.14), rng.uniform(0.75, 0.98), rng.normal(0, 0.15), 0.2, 0.004,
                             (0.45, 0.38, 0.20), (0.74, 0.64, 0.40))
        card.ellipse(x, y - 0.02, 0.008, 0.035, (0.80, 0.70, 0.46))
    return card


def reeds(rng):
    card = Card()
    sedge = [(LEAF_DARK, LEAF_MID), ((0.12, 0.18, 0.06), (0.30, 0.36, 0.14))]
    grass_tuft(card, rng, 70, 0.98, 0.14, sedge, lean=0.18, width=0.012, curve=0.25)
    for _ in range(6):
        x0 = 0.5 + rng.normal(0, 0.12)
        x, y, a = card.blade(x0, rng.uniform(0.80, 0.96), rng.normal(0, 0.06), 0.05, 0.005, LEAF_DARK, (0.30, 0.32, 0.12))
        card.poly([(x - 0.012, y - 0.20), (x + 0.012, y - 0.20), (x + 0.011, y - 0.07), (x - 0.011, y - 0.07)],
                  (0.32, 0.20, 0.10))
    return card


def undergrowth(rng):
    card = Card()
    grass_tuft(card, rng, 60, 0.45, 0.22, GREEN, lean=0.5, width=0.010, curve=0.8)
    for _ in range(28):      # clover / wood sorrel / small broad leaves on stems
        x0 = 0.5 + rng.normal(0, 0.2)
        if not 0.06 < x0 < 0.94:
            continue
        x, y, a = card.blade(x0, rng.uniform(0.18, 0.55), rng.normal(0, 0.35), rng.uniform(-0.4, 0.4), 0.004,
                             LEAF_DARK, LEAF_MID)
        leaf = jitter(rng, lerp(LEAF_MID, LEAF_LIGHT, rng.uniform(0, 0.7)), 0.1)
        r = rng.uniform(0.022, 0.04)
        for k in range(3):
            t = k / 3 * 2 * math.pi + rng.uniform(0, 0.4)
            card.ellipse(x + math.cos(t) * r * 0.8, y + math.sin(t) * r * 0.5, r * 0.7, r * 0.5, leaf,
                         0.85 + 0.15 * math.sin(t))
    return card


# Layer order after the six imported views. Index = 6 + position.
CARDS = [
    ("short_grass", short_grass),
    ("seed_grass", seed_grass),
    ("flowers_white", lambda r: flowers(r, (0.93, 0.92, 0.86), (0.85, 0.66, 0.12), "daisy")),
    ("flowers_yellow", lambda r: flowers(r, (0.95, 0.78, 0.10), (0.70, 0.50, 0.05), "cup")),
    ("flowers_purple", lambda r: flowers(r, (0.48, 0.32, 0.72), (0.30, 0.20, 0.45), "spike")),
    ("fern", fern),
    ("dry_grass", dry_grass),
    ("reeds", reeds),
    ("undergrowth", undergrowth),
    ("flowers_red", lambda r: flowers(r, (0.80, 0.10, 0.06), (0.12, 0.08, 0.06), "poppy")),
]


def bled_meadow_views(out):
    """The six imported grass views with their empty texels filled from the
    blades (bleed): the renderer mips the card array, and the imported views'
    near-black surround would otherwise darken every far clump's edge."""
    source = Path(__file__).resolve().parents[1] / "assets/generated/scene_models"
    names = []
    for i in range(6):
        path = source / f"grass-view-{i}.png"
        if not path.is_file():
            return []
        a = np.asarray(Image.open(path).convert("RGBA")).astype(np.float32) / 255.0
        if a.shape[:2] != (SIZE, SIZE):
            return []
        rgb = bleed(a[..., :3], a[..., 3] > 0.05)
        img = np.concatenate([rgb, a[..., 3:4]], -1)
        name = f"meadow_view_{i}.png"
        Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)).save(out / name, optimize=True)
        names.append(name)
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(Path(__file__).resolve().parents[1] / "assets/generated/foliage_cards"))
    ap.add_argument("--preview", action="store_true")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    names = []
    images = []
    for i, (name, make) in enumerate(CARDS):
        rng = np.random.default_rng(1009 + i * 7919)
        img = make(rng).result()
        img.save(out / f"{name}.png", optimize=True)
        names.append(f"{name}.png")
        images.append(img)
        al = np.asarray(img)[..., 3] > 20
        print(f"{6 + i:2d} {name:16s} coverage {al.mean():.2f}")
    meadow = bled_meadow_views(out)
    print("meadow views bled:", len(meadow))
    (out / "cards.json").write_text(json.dumps({"first_layer": 6, "cards": names, "meadow": meadow}, indent=2) + "\n")
    if a.preview:
        sheet = Image.new("RGB", (SIZE * len(images), SIZE), (120, 140, 170))
        for i, img in enumerate(images):
            sheet.paste(img, (i * SIZE, 0), img)
        sheet.save(out / "preview.png")


if __name__ == "__main__":
    main()

