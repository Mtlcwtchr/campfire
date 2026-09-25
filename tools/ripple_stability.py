#!/usr/bin/env python3
"""Does the ground stay still when the camera moves?

The difference between detail and shimmer is not how fine it is - it is
whether it belongs to the ground. Real detail translates with the world when
the camera slides sideways; shimmer is a function of where the pixel centres
happened to land, so it stays stuck to the screen and changes.

Two shots from positions a known distance apart are compared after shifting one
back onto the other. The shift is searched at sub-pixel resolution in the
Fourier domain, so a scale that is not a whole number of pixels cannot be
mistaken for instability - that mistake is easy to make and it turns every
correctly filtered picture into evidence of a bug.

What is reported is the residual at the best shift, against the contrast of the
picture. Zero means the ground translated exactly.
"""
import sys

import numpy as np
from PIL import Image


def luma(path, box):
    a = np.asarray(Image.open(path).convert("RGB")).astype(float) / 255.0
    y0, y1, x0, x1 = box
    return (a @ np.array([0.2126, 0.7152, 0.0722]))[y0:y1, x0:x1]


def shifted(image, dx, dy):
    rows, cols = image.shape
    fy = np.fft.fftfreq(rows)[:, None]
    fx = np.fft.fftfreq(cols)[None, :]
    phase = np.exp(-2j * np.pi * (fx * dx + fy * dy))
    return np.real(np.fft.ifft2(np.fft.fft2(image) * phase))


def residual(a, b, dx, dy, guard=24):
    moved = shifted(b, dx, dy)
    a2 = a[guard:-guard, guard:-guard]
    b2 = moved[guard:-guard, guard:-guard]
    return float(np.sqrt(np.mean((a2 - b2) ** 2)))


def main(first, second, box=(200, 712, 200, 1080)):
    a, b = luma(first, box), luma(second, box)
    # Coarse integer search, then a sub-pixel refinement around it.
    best = min(((residual(a, b, dx, 0.0), dx) for dx in range(-60, 61)))
    coarse = best[1]
    fine = min(((residual(a, b, coarse + step / 20.0, 0.0), coarse + step / 20.0)
                for step in range(-20, 21)))
    contrast = float(np.sqrt(np.var(a)))
    print("%s vs %s" % (first, second))
    print("  best whole-pixel shift %d  residual %.5f" % (coarse, best[0]))
    print("  best sub-pixel shift %.2f  residual %.5f" % (fine[1], fine[0]))
    print("  picture contrast %.5f  residual/contrast %.3f" % (contrast, fine[0] / contrast))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])

