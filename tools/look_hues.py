#!/usr/bin/env python3
"""Hue histogram of the lower part of shots (saturated pixels only).

    python3 tools/look_hues.py shot.png [...] [--from 0.6]
"""
import argparse
import colorsys
import os

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("images", nargs="+")
ap.add_argument("--from", dest="start", type=float, default=0.6)
a = ap.parse_args()
bins = [0, 20, 30, 40, 50, 60, 70, 80, 90, 110, 140, 180, 220, 260, 360]
print(" " * 26 + " ".join(f"{b:>4d}" for b in bins[:-1]))
for path in a.images:
    px = np.asarray(Image.open(path).convert("RGB")).astype(float) / 255
    g = px[int(px.shape[0] * a.start):].reshape(-1, 3)[::7]
    hsv = np.array([colorsys.rgb_to_hsv(*p) for p in g])
    sel = hsv[:, 1] > 0.2
    hist, _ = np.histogram(hsv[sel, 0] * 360, bins=bins)
    share = hist / max(1, hist.sum())
    print(f"{os.path.basename(path):26s}" + " ".join(f"{s:4.2f}" for s in share))

