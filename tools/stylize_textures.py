#!/usr/bin/env python3
"""Force source textures through one look (doc/plan_procedural_environment_2026-10-03.md, part I).

Assets come from everywhere - photogrammetry, CC0 packs, retro kits - and
read as a mixed bag until they pass through the same treatment. The
operations are generic (the engine side of the split); the presets that pick
and tune them are the game's (content/config/style/texture_presets.json).

Operations, applied in the preset's order:
  microcontrast  pull fine detail towards a blurred copy: less "scanned"
  normal         weaken fine normal detail and keep the large forms
  palette        tint shadows and highlights by luminance, set saturation
  macro          broad patches of colour variation instead of per-pixel noise
  posterize      a light, partial quantisation: printed, not pixel art
  roughness      simplify roughness to a few broad regions
  variants       hue/value shifted copies for regional palettes

    python3 tools/stylize_textures.py --preset rock_painted in_albedo.png out_albedo.png
    python3 tools/stylize_textures.py --preset rock_painted --kind normal in_n.png out_n.png
    python3 tools/stylize_textures.py --list
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
PRESETS = ROOT / "content/config/style/texture_presets.json"


def load(path):
    image = Image.open(path)
    mode = "RGBA" if image.mode in ("RGBA", "LA", "P") else "RGB"
    return np.asarray(image.convert(mode), dtype=np.float32) / 255.0


def save(array, path):
    array = np.clip(array * 255.0 + 0.5, 0, 255).astype(np.uint8)
    Image.fromarray(array).save(path)


def blur(array, radius):
    """Separable Gaussian, wrapping at the edges: textures tile, so must their blur."""
    if radius <= 0:
        return array
    reach = int(np.ceil(radius * 3))
    weights = np.exp(-0.5 * (np.arange(-reach, reach + 1) / radius) ** 2)
    weights /= weights.sum()
    out = array
    for axis in (0, 1):
        acc = np.zeros_like(out)
        for offset, w in zip(range(-reach, reach + 1), weights):
            acc += np.roll(out, offset, axis=axis) * w
        out = acc
    return out


def luminance(rgb):
    return rgb[..., 0] * 0.2126 + rgb[..., 1] * 0.7152 + rgb[..., 2] * 0.0722


def smoothstep(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def op_microcontrast(img, p):
    """Keep the large forms; scale what is finer than `radius` pixels by `keep`."""
    rgb = img[..., :3]
    base = blur(rgb, p.get("radius", 3.0))
    img[..., :3] = base + (rgb - base) * p.get("keep", 0.5)
    return img


def op_normal(img, p):
    """Normals: fine detail weakened, large forms kept, then renormalised."""
    n = img[..., :3] * 2 - 1
    large = blur(n, p.get("radius", 8.0))
    fine = n - large
    n = large * p.get("large", 1.0) + fine * p.get("fine", 0.4)
    n[..., 2] = np.maximum(n[..., 2], 0.05)
    n /= np.linalg.norm(n, axis=2, keepdims=True) + 1e-8
    img[..., :3] = n * 0.5 + 0.5
    return img


def op_palette(img, p):
    """Warm highlights, cool shadows: each pulled towards its tint by luminance."""
    rgb = img[..., :3]
    lum = luminance(rgb)[..., None]
    shadow = np.array(p.get("shadow", [0.42, 0.45, 0.62]), dtype=np.float32)
    highlight = np.array(p.get("highlight", [1.0, 0.92, 0.74]), dtype=np.float32)
    lo, hi = p.get("split", [0.25, 0.7])
    ws = (1 - smoothstep(lo - 0.15, lo + 0.15, lum)) * p.get("shadow_strength", 0.35)
    wh = smoothstep(hi - 0.15, hi + 0.15, lum) * p.get("highlight_strength", 0.3)
    rgb = rgb * (1 - ws) + rgb * shadow * 1.6 * ws
    rgb = rgb * (1 - wh) + (rgb * highlight + highlight * 0.04) * wh
    grey = luminance(rgb)[..., None]
    rgb = grey + (rgb - grey) * p.get("saturation", 1.0)
    img[..., :3] = rgb
    return img


def op_macro(img, p):
    """Broad patches of value and warmth, `scale` pixels across, tileable."""
    h, w = img.shape[:2]
    rng = np.random.default_rng(p.get("seed", 1))
    cells = max(2, int(round(w / p.get("scale", 128.0))))
    grid = rng.random((cells, cells, 2)).astype(np.float32)
    # Bilinear upsampling of a periodic grid keeps the result tileable.
    ys = (np.arange(h) + 0.5) / h * cells
    xs = (np.arange(w) + 0.5) / w * cells
    y0 = np.floor(ys).astype(int) % cells
    x0 = np.floor(xs).astype(int) % cells
    fy = (ys - np.floor(ys))[:, None, None]
    fx = (xs - np.floor(xs))[None, :, None]
    fy = fy * fy * (3 - 2 * fy)
    fx = fx * fx * (3 - 2 * fx)
    g = lambda yy, xx: grid[yy][:, xx]
    field = (g(y0, x0) * (1 - fx) + g(y0, (x0 + 1) % cells) * fx) * (1 - fy) + \
            (g((y0 + 1) % cells, x0) * (1 - fx) + g((y0 + 1) % cells, (x0 + 1) % cells) * fx) * fy
    value = 1 + (field[..., 0] - 0.5) * 2 * p.get("value", 0.12)
    warm = (field[..., 1] - 0.5) * 2 * p.get("warmth", 0.06)
    rgb = img[..., :3] * value[..., None]
    rgb[..., 0] *= 1 + warm
    rgb[..., 2] *= 1 - warm
    img[..., :3] = rgb
    return img


def op_posterize(img, p):
    """A light partial quantisation: `levels` steps mixed in by `amount`."""
    levels = max(2, int(p.get("levels", 12)))
    rgb = img[..., :3]
    q = np.round(rgb * (levels - 1)) / (levels - 1)
    img[..., :3] = rgb + (q - rgb) * p.get("amount", 0.35)
    return img


def op_roughness(img, p):
    """Roughness as a few broad regions rather than full-frequency noise."""
    r = img[..., :1] if img.shape[2] == 1 else img[..., :3]
    r = blur(r, p.get("radius", 6.0))
    levels = max(2, int(p.get("levels", 5)))
    r = np.round(r * (levels - 1)) / (levels - 1)
    img[..., : r.shape[2]] = r * p.get("scale", 1.0) + p.get("bias", 0.0)
    return img


OPERATIONS = {
    "microcontrast": op_microcontrast, "normal": op_normal, "palette": op_palette,
    "macro": op_macro, "posterize": op_posterize, "roughness": op_roughness,
}
# Which operations make sense for which kind of map.
KINDS = {
    "albedo": {"microcontrast", "palette", "macro", "posterize"},
    "normal": {"normal"},
    "roughness": {"roughness"},
}


def variants(img, specs):
    """Regional copies: hue rotated (in turns) in YIQ, saturation and value scaled."""
    yiq = np.array([[0.299, 0.587, 0.114], [0.596, -0.274, -0.322], [0.211, -0.523, 0.312]], np.float32)
    inv = np.linalg.inv(yiq)
    out = []
    for spec in specs:
        angle = spec.get("hue", 0.0) * 2 * np.pi
        rot = np.array([[1, 0, 0], [0, np.cos(angle), -np.sin(angle)], [0, np.sin(angle), np.cos(angle)]], np.float32)
        s = spec.get("saturation", 1.0)
        m = inv @ np.diag([1, s, s]).astype(np.float32) @ rot @ yiq
        copy = img.copy()
        copy[..., :3] = (img[..., :3] @ m.T) * spec.get("value", 1.0)
        out.append((spec["name"], np.clip(copy, 0, 1)))
    return out


def stylize(img, preset, kind):
    allowed = KINDS[kind]
    for step in preset.get("steps", []):
        name = step["op"]
        if name not in OPERATIONS:
            raise ValueError("unknown operation " + name)
        if name in allowed:
            img = OPERATIONS[name](img, step)
    return np.clip(img, 0, 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", nargs="?")
    parser.add_argument("target", nargs="?")
    parser.add_argument("--preset")
    parser.add_argument("--presets", default=str(PRESETS))
    parser.add_argument("--kind", choices=sorted(KINDS), default="albedo")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    presets = json.loads(Path(args.presets).read_text()) if Path(args.presets).exists() else {"presets": {}}
    if args.list:
        for name, preset in presets["presets"].items():
            print(name + ": " + ", ".join(s["op"] for s in preset.get("steps", [])))
        return 0
    if not (args.source and args.target and args.preset):
        parser.error("source, target and --preset are needed")
    if args.preset not in presets["presets"]:
        parser.error("no preset " + args.preset + " in " + args.presets)
    preset = presets["presets"][args.preset]
    img = stylize(load(args.source), preset, args.kind)
    save(img, args.target)
    if args.kind == "albedo":
        target = Path(args.target)
        for name, copy in variants(img, preset.get("variants", [])):
            save(copy, target.with_name(target.stem + "_" + name + target.suffix))
    return 0


if __name__ == "__main__":
    sys.exit(main())
