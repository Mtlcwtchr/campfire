#!/usr/bin/env python3
"""What the ripple actually looks like, as numbers.

A screenshot cannot be looked at from here, so the pattern is characterised
instead: where the pixel-scale energy is strongest, and what its autocorrelation
says about the shape. Lag-one correlations separate the possibilities that
otherwise get argued about -

  near -1 along both axes   a checkerboard, so a dither or a per-pixel hash
  near  0                   white noise, so unfiltered sampling
  near +0.5 then falling    real detail a few pixels wide

and a 2x2 block pattern - the signature of a screen-space derivative taken
across a quad - shows as a strong lag-two correlation with a weak lag-one.
"""
import sys

import numpy as np
from PIL import Image


def highpass(path, box):
    a = np.asarray(Image.open(path).convert("RGB")).astype(float) / 255.0
    y0, y1, x0, x1 = box
    y = (a @ np.array([0.2126, 0.7152, 0.0722]))[y0:y1, x0:x1]
    q = np.pad(y, 1, mode="edge")
    s = sum(q[i:i + y.shape[0], j:j + y.shape[1]] for i in range(3) for j in range(3)) / 9.0
    return y, y - s


def main(path, box=(150, 750, 100, 1180), win=64):
    y, h = highpass(path, box)
    best = None
    for i in range(0, y.shape[0] - win, 32):
        for j in range(0, y.shape[1] - win, 32):
            e = float((h[i:i + win, j:j + win] ** 2).mean())
            if best is None or e > best[0]:
                best = (e, i, j)
    print("%s hottest %.4f at %d,%d  whole %.5f" %
          (path, np.sqrt(best[0]), best[1], best[2], np.sqrt((h ** 2).mean())))
    c = h[best[1]:best[1] + win, best[2]:best[2] + win]
    f = np.fft.rfft2(c - c.mean())
    ac = np.fft.irfft2(np.abs(f) ** 2, s=c.shape)
    ac = ac / ac[0, 0]
    print("  autocorr x lags 1..4 %s" % np.round(ac[0, 1:5], 3))
    print("  autocorr y lags 1..4 %s" % np.round(ac[1:5, 0], 3))
    print("  autocorr diagonal    %.3f" % ac[1, 1])
    symbols = np.array(list(" .:-=+*#%@"))
    sub = c[:22, :76]
    scaled = np.clip((sub - sub.min()) / (sub.max() - sub.min() + 1e-9) * 9, 0, 9).astype(int)
    for row in scaled:
        print("  " + "".join(symbols[row]))


if __name__ == "__main__":
    for arg in sys.argv[1:] or ["/tmp/bisect_baseline.png"]:
        main(arg)

