"""Pixel comparison of two shot directories: share of differing pixels, max and mean |delta|."""
import sys
from pathlib import Path

import numpy as np
from PIL import Image

a, b = Path(sys.argv[1]), Path(sys.argv[2])
threshold = int(sys.argv[3]) if len(sys.argv) > 3 else 0
worst = 0.0
for pa in sorted(a.glob("*.png")):
    pb = b / pa.name
    if not pb.exists():
        print("%-18s missing in %s" % (pa.name, b))
        continue
    x = np.asarray(Image.open(pa).convert("RGB")).astype(int)
    y = np.asarray(Image.open(pb).convert("RGB")).astype(int)
    if x.shape != y.shape:
        print("%-18s size differs" % pa.name)
        continue
    d = np.abs(x - y).max(axis=-1)
    share = float((d > threshold).mean())
    worst = max(worst, share)
    print("%-18s differing %.4f%%  max |d| %3d  mean |d| %.4f  pixels >8: %.4f%%" % (
        pa.name, 100 * share, d.max(), d.mean(), 100 * float((d > 8).mean())))
print("worst share %.4f%%" % (100 * worst))

