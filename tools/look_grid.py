#!/usr/bin/env python3
"""A picture as a small text grid, for reading a shot without a screen.

    python3 tools/look_grid.py shot.png [--cols 48] [--rows 18]

Each cell prints luminance as a character ramp and a colour letter for its
mean hue: G green, Y yellow/olive, O orange/brown, R red, B blue, C cyan,
M magenta, '.' grey (unsaturated). Below that, per row: mean luma, luma std
inside the row (texture), and the share of green cells.
"""
import argparse

import numpy as np
from PIL import Image

RAMP = " .:-=+*#%@"


def hue_letter(rgb):
    r, g, b = rgb
    mx, mn = max(rgb), min(rgb)
    if mx < 1e-3 or (mx - mn) / (mx + 1e-6) < 0.12:
        return "."
    d = mx - mn
    if mx == r:
        h = ((g - b) / d) % 6
    elif mx == g:
        h = (b - r) / d + 2
    else:
        h = (r - g) / d + 4
    h *= 60
    for top, letter in ((15, "R"), (45, "O"), (70, "Y"), (165, "G"), (200, "C"), (260, "B"), (330, "M"), (361, "R")):
        if h < top:
            return letter
    return "R"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--cols", type=int, default=48)
    ap.add_argument("--rows", type=int, default=18)
    a = ap.parse_args()
    px = np.asarray(Image.open(a.image).convert("RGB"), dtype=np.float32) / 255.0
    h, w, _ = px.shape
    luma_img = 0.2126 * px[..., 0] + 0.7152 * px[..., 1] + 0.0722 * px[..., 2]
    for r in range(a.rows):
        y0, y1 = r * h // a.rows, (r + 1) * h // a.rows
        line, letters, greens = "", "", 0
        for c in range(a.cols):
            x0, x1 = c * w // a.cols, (c + 1) * w // a.cols
            cell = px[y0:y1, x0:x1].reshape(-1, 3).mean(0)
            l = float(0.2126 * cell[0] + 0.7152 * cell[1] + 0.0722 * cell[2])
            line += RAMP[min(len(RAMP) - 1, int(l * len(RAMP)))]
            letter = hue_letter(cell)
            letters += letter
            greens += letter == "G"
        band = luma_img[y0:y1]
        print(f"{line}  {letters}  l={band.mean():.2f} t={band.std():.3f} g={greens / a.cols:.2f}")


if __name__ == "__main__":
    main()

