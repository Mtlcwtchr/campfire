#!/usr/bin/env python3
"""Numbers for a picture, for comparing looks without eyes on the screen.

    python3 tools/look_metrics.py shot.png [more.png ...] [--json out.json]

Per image and per horizontal band (sky / horizon / middle / ground):
mean linear-ish luminance, contrast (luma std), saturation, hue histogram of
saturated pixels (green / yellow-brown / blue share), clipped and crushed
fractions, edge density (fine detail), and a "vegetation" share (green-dominant
pixels). Not a judge of beauty: a way to see what changed between two shots.
"""
import json
import sys

import numpy as np
from PIL import Image


def rgb_to_hsv(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    mx = rgb.max(-1)
    mn = rgb.min(-1)
    d = mx - mn + 1e-6
    h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) * 60.0
    s = np.where(mx > 1e-4, (mx - mn) / (mx + 1e-6), 0.0)
    return h, s, mx


def band_stats(px):
    luma = 0.2126 * px[..., 0] + 0.7152 * px[..., 1] + 0.0722 * px[..., 2]
    h, s, v = rgb_to_hsv(px)
    sat = s > 0.15
    total = max(1, sat.sum())
    green = (sat & (h >= 70) & (h < 170)).sum() / total
    yellow = (sat & (h >= 30) & (h < 70)).sum() / total
    blue = (sat & (h >= 170) & (h < 260)).sum() / total
    gy, gx = np.gradient(luma)
    edges = float((np.hypot(gx, gy) > 0.04).mean())
    veg = float(((px[..., 1] > px[..., 0] * 1.02) & (px[..., 1] > px[..., 2] * 1.05)).mean())
    return {
        "luma": round(float(luma.mean()), 4),
        "contrast": round(float(luma.std()), 4),
        "sat": round(float(s.mean()), 4),
        "rgb": [round(float(c), 3) for c in px.reshape(-1, 3).mean(0)],
        "green": round(float(green), 3), "yellow": round(float(yellow), 3), "blue": round(float(blue), 3),
        "clip": round(float((v > 0.98).mean()), 4), "crush": round(float((v < 0.02).mean()), 4),
        "edges": round(edges, 4), "veg": round(veg, 4),
    }


def stats(path):
    px = np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
    h = px.shape[0]
    bands = {"sky": (0, h // 4), "horizon": (h // 4, h // 2), "middle": (h // 2, 3 * h // 4), "ground": (3 * h // 4, h)}
    out = {"image": path, "all": band_stats(px)}
    for name, (a, b) in bands.items():
        out[name] = band_stats(px[a:b])
    return out


def ascii_view(path, cols=96, rows=30):
    """A coarse map of what is where: one character per cell, by colour class.

    ' ' black/crushed  '.' dark  '#' dark green (canopy)  'g' green (grass)
    'y' yellow-green/dry  'b' brown (bark, soil)  'r' grey (rock, haze)
    '~' blue-grey (water)  's' sky blue  'w' bright/white  'o' orange/warm
    """
    img = Image.open(path).convert("RGB").resize((cols, rows), Image.BOX)
    px = np.asarray(img).astype(np.float32) / 255.0
    h, s, v = rgb_to_hsv(px)
    lines = []
    for y in range(rows):
        line = ""
        for x in range(cols):
            hh, ss, vv = h[y, x], s[y, x], v[y, x]
            if vv < 0.04: c = " "
            elif vv > 0.85 and ss < 0.2: c = "w"
            elif ss < 0.12: c = "." if vv < 0.18 else "r"
            elif 70 <= hh < 170: c = "#" if vv < 0.28 else "g"
            elif 45 <= hh < 70: c = "y"
            elif 15 <= hh < 45: c = "b" if vv < 0.5 else "o"
            elif 170 <= hh < 260: c = "s" if vv > 0.45 else "~"
            else: c = "o" if vv > 0.3 else "b"
            line += c
        lines.append(line)
    return "\n".join(lines)


def main(argv):
    paths = [a for a in argv if not a.startswith("--")]
    json_out = None
    if "--json" in argv:
        json_out = argv[argv.index("--json") + 1]
        paths = [p for p in paths if p != json_out]
    if "--ascii" in argv:
        for p in paths:
            print(p)
            print(ascii_view(p))
        return
    results = [stats(p) for p in paths]
    keys = ["luma", "contrast", "sat", "green", "yellow", "blue", "clip", "crush", "edges", "veg"]
    for r in results:
        print(r["image"])
        print("  %-8s " % "band" + " ".join("%8s" % k for k in keys) + "   rgb")
        for band in ("all", "sky", "horizon", "middle", "ground"):
            b = r[band]
            print("  %-8s " % band + " ".join("%8.3f" % b[k] for k in keys) + "   " + str(b["rgb"]))
    if json_out:
        with open(json_out, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main(sys.argv[1:])
