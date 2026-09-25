#!/usr/bin/env python3
"""How much of a picture is pixel-scale noise rather than shape.

The ripple on the terrain is a claim about frequency, so it has to be measured
as one. Looking at a screenshot and saying "still rippling" cannot tell a fix
from a placebo, and a plain diff between two shots cannot either - two shots
differ wherever anything moved.

What this reports is the energy left after a 3x3 high pass, divided by the
local contrast over the same neighbourhood. Real ground detail - a material
border, a groove, the lit side of a slope - carries its energy over several
pixels, so a high pass keeps only its edges. Sampling noise lives entirely at
the pixel, so the high pass keeps all of it. Dividing by local contrast makes
the number comparable between a bright shot and a dark one.

Bright pixels can be dropped with --sky, because sky is smooth by construction
and would only dilute the answer. A rectangle can be given with --crop to ask
about one part of the frame.

    python3 tools/terrain_shimmer.py a.png b.png --sky 0.72
"""
import sys

import numpy as np
from PIL import Image


def luma(path):
    rgb = np.asarray(Image.open(path).convert("RGB")).astype(np.float64) / 255.0
    # The eye reads the shimmer as brightness, not as hue.
    return rgb @ np.array([0.2126, 0.7152, 0.0722])


def blur3(a):
    p = np.pad(a, 1, mode="edge")
    out = np.zeros_like(a)
    for dy in range(3):
        for dx in range(3):
            out += p[dy:dy + a.shape[0], dx:dx + a.shape[1]]
    return out / 9.0


def shimmer(path, sky=0.0, crop=None):
    y = luma(path)
    if crop:
        x0, y0, x1, y1 = crop
        y = y[y0:y1, x0:x1]
    smooth = blur3(y)
    high = y - smooth
    # Local contrast over the same neighbourhood as the high pass.
    contrast = np.sqrt(np.maximum(blur3(y * y) - smooth * smooth, 0.0))
    mask = smooth < sky if sky > 0 else np.ones(y.shape, dtype=bool)
    if mask.sum() < 1000:
        raise SystemExit("%s: mask kept only %d pixels" % (path, int(mask.sum())))
    hi = float(np.sqrt(np.mean(high[mask] ** 2)))
    lo = float(np.sqrt(np.mean(contrast[mask] ** 2)))
    return hi, lo, hi / max(lo, 1e-9), int(mask.sum())


def main(argv):
    sky = 0.0
    crop = None
    paths = []
    it = iter(argv)
    for arg in it:
        if arg == "--sky":
            sky = float(next(it))
        elif arg == "--crop":
            crop = [int(v) for v in next(it).split(",")]
        else:
            paths.append(arg)
    print("%-46s %9s %9s %8s %9s" % ("file", "high", "contrast", "ratio", "pixels"))
    for path in paths:
        hi, lo, ratio, kept = shimmer(path, sky, crop)
        print("%-46s %9.5f %9.5f %8.4f %9d" % (path[-46:], hi, lo, ratio, kept))


if __name__ == "__main__":
    main(sys.argv[1:])

