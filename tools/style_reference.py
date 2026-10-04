#!/usr/bin/env python3
"""What a folder of style references is made of, as numbers a look can use.

For every picture and for the folder as a whole:
  - its palette: the dominant colours (k-means in Lab-ish space), by share
  - its value structure: the luminance histogram, how much is near black,
    near white, and how stepped (posterised) the values are
  - its shadows and lights: the mean hue and saturation of the darkest and
    brightest fifth - the "cool shadow, warm light" a grade should lean to
  - its saturation: mean and how much sits in strongly saturated colour
And gradient ramps: colour against value, darks to lights, which
tools/stylize_textures.py can map a texture's luminance onto.

The tool is generic (engine side of the split); what a game keeps of it is
its own (content/config/style/reference.json).

    python3 tools/style_reference.py doc/references/artstyle --out content/config/style/reference.json
"""
import argparse
import colorsys
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load(path, side=256):
    image = Image.open(path).convert("RGB")
    image.thumbnail((side, side))
    return np.asarray(image, dtype=np.float32) / 255.0


def luminance(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def kmeans(points, k, iterations=12, seed=1):
    rng = np.random.default_rng(seed)
    centres = points[rng.choice(len(points), size=k, replace=False)]
    for _ in range(iterations):
        d = ((points[:, None, :] - centres[None, :, :]) ** 2).sum(axis=2)
        label = d.argmin(axis=1)
        for c in range(k):
            members = points[label == c]
            if len(members):
                centres[c] = members.mean(axis=0)
    share = np.bincount(label, minlength=k) / len(points)
    order = np.argsort(-share)
    return centres[order], share[order]


def hue_sat(rgb):
    h, l, s = colorsys.rgb_to_hls(*[float(v) for v in rgb])
    return h, s


def ramp(pixels, steps=8):
    """Mean colour at each step of value, darks to lights."""
    lum = luminance(pixels)
    edges = np.quantile(lum, np.linspace(0, 1, steps + 1))
    out = []
    for i in range(steps):
        sel = (lum >= edges[i]) & (lum <= edges[i + 1])
        out.append(pixels[sel].mean(axis=0).tolist() if sel.any() else [0, 0, 0])
    return out


def analyse(pixels):
    flat = pixels.reshape(-1, 3)
    lum = luminance(flat)
    dark = flat[lum <= np.quantile(lum, 0.2)].mean(axis=0)
    light = flat[lum >= np.quantile(lum, 0.8)].mean(axis=0)
    mx, mn = flat.max(axis=1), flat.min(axis=1)
    sat = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0)
    hist, _ = np.histogram(lum, bins=32, range=(0, 1))
    hist = hist / hist.sum()
    # How stepped the values are: share of mass in the few strongest bins.
    stepped = float(np.sort(hist)[-6:].sum())
    centres, share = kmeans(flat, 8)
    return {
        "palette": [{"rgb": [round(float(v), 4) for v in c], "share": round(float(s), 4)} for c, s in zip(centres, share)],
        "shadow": {"rgb": [round(float(v), 4) for v in dark], "hue": round(hue_sat(dark)[0], 4), "saturation": round(hue_sat(dark)[1], 4)},
        "light": {"rgb": [round(float(v), 4) for v in light], "hue": round(hue_sat(light)[0], 4), "saturation": round(hue_sat(light)[1], 4)},
        "saturation": {"mean": round(float(sat.mean()), 4), "strong": round(float((sat > 0.6).mean()), 4)},
        "value": {"mean": round(float(lum.mean()), 4), "black": round(float((lum < 0.08).mean()), 4),
                  "white": round(float((lum > 0.9).mean()), 4), "stepped": round(stepped, 4)},
        "ramp": [[round(float(v), 4) for v in c] for c in ramp(flat)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder")
    parser.add_argument("--out")
    args = parser.parse_args()
    pictures, every = {}, []
    for path in sorted(Path(args.folder).iterdir()):
        if path.name.startswith("."):
            continue
        try:
            pixels = load(path)
        except Exception as error:   # avif and friends PIL cannot read
            print("skip", path.name, error, file=sys.stderr)
            continue
        pictures[path.name] = analyse(pixels)
        every.append(pixels.reshape(-1, 3))
    whole = analyse(np.concatenate(every).reshape(-1, 1, 3))
    summary = {"pictures": len(pictures), "folder": whole, "each": pictures}
    text = json.dumps(summary, indent=1)
    if args.out:
        Path(args.out).write_text(text + "\n")
    f = whole
    print(f"{len(pictures)} pictures")
    print("shadow  hue %.2f sat %.2f rgb %s" % (f["shadow"]["hue"], f["shadow"]["saturation"], f["shadow"]["rgb"]))
    print("light   hue %.2f sat %.2f rgb %s" % (f["light"]["hue"], f["light"]["saturation"], f["light"]["rgb"]))
    print("saturation mean %.2f strong %.2f" % (f["saturation"]["mean"], f["saturation"]["strong"]))
    print("value mean %.2f black %.2f white %.2f stepped %.2f" % (f["value"]["mean"], f["value"]["black"], f["value"]["white"], f["value"]["stepped"]))
    print("palette:", [(p["rgb"], p["share"]) for p in f["palette"]])
    return 0


if __name__ == "__main__":
    sys.exit(main())
