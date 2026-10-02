#!/usr/bin/env python3
"""Palette summary of shots: overall luma/contrast, the ground's mean hue and
saturation (lower 40 %), and the top band's (sky) hue and saturation.

    python3 tools/look_palette.py shot.png [...]
"""
import colorsys
import os
import sys

import numpy as np
from PIL import Image

for path in sys.argv[1:]:
    a = np.asarray(Image.open(path).convert("RGB")).astype(float) / 255
    ground = a[int(a.shape[0] * 0.6):].reshape(-1, 3).mean(0)
    top = a[:int(a.shape[0] * 0.12)].reshape(-1, 3).mean(0)
    hg, sg, vg = colorsys.rgb_to_hsv(*ground)
    ht, st, vt = colorsys.rgb_to_hsv(*top)
    luma = 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]
    print(f"{os.path.basename(path):26s} luma {luma.mean():.3f} contrast {luma.std():.3f} | "
          f"ground hue {hg * 360:5.1f} sat {sg:.2f} val {vg:.2f} | top hue {ht * 360:5.1f} sat {st:.2f} val {vt:.2f}")

