#!/usr/bin/env python3
"""pack_terrain - turns the downloaded source maps into what the game loads.

Five source pictures a material is three on the card:

    NAME_albedo.png       RGB  colour                       sRGB
    NAME_normal.png       RGB  normal, OpenGL convention     linear
    NAME_properties.png   R    ambient occlusion            linear
                          G    roughness
                          B    height, stretched to its own range
                          A    the asset's mask, or white

Three reads instead of five, and the last one is the point: occlusion,
roughness and height are one channel each and travelling separately they cost
three textures, three samplers and three sets of filtering to say what fits in
one. The alpha is a spare that a few assets fill and the rest do not, which is
why it is white when it is missing rather than absent - a channel that is
sometimes there is a shader with a branch in it.

Height is stretched to fill its own range before packing. A displacement map
that only ever uses the middle third of its numbers is a map with a third of
the precision it could have, and once packed into eight bits that is visible as
banding where two materials compete for the surface.

The mip chain is built here and not by the driver, for the same reason the
water's is (D146): a normal map filtered down by averaging is a normal map that
is no longer unit length, and water or ground lit by one of those is lit wrong
at every distance but the nearest. Normals are averaged and then re-normalised;
everything else is averaged plainly.

    python3 tools/pack_terrain.py                 # everything that has sources
    python3 tools/pack_terrain.py --only grass_lush
    python3 tools/pack_terrain.py --size 1024     # smaller runtime maps

The sources are never touched. They are sources.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
MATERIALS = ROOT / "content/config/terrain_materials.json"
WATER = ROOT / "content/config/water_textures.json"
TERRAIN = ROOT / "assets/terrain"

# The same ladder the ground and the water already use, so one loader reads
# every array in the game: name.png, then name@2 through name@16.
STEPS = [2, 4, 8, 16]


def scaled(array):
    """Whatever depth it came at, as nought to one."""
    if array.dtype == np.uint16:
        return array.astype(np.float32) / 65535.0
    if array.dtype in (np.float32, np.float64):
        return array.astype(np.float32)
    return array.astype(np.float32) / 255.0


def resized(array, size):
    if array.shape[0] == size and array.shape[1] == size:
        return array
    if array.ndim == 2:
        # A float image, so the resample is done on the real numbers rather
        # than on eight-bit ones - which matters for height, where the whole
        # point of the sixteen-bit source is the precision.
        return np.asarray(Image.fromarray(array.astype(np.float32)).resize(
                (size, size), Image.LANCZOS))
    return np.stack([resized(array[..., c], size) for c in range(array.shape[2])], axis=2)


def read(path, size, grey=False):
    """One source map, as floats.

    Not through PIL's own convert(), and this is worth saying plainly because
    it silently threw away every single-channel map the first time: Poly Haven
    ships occlusion, roughness and displacement as sixteen-bit PNGs, and
    `Image.convert("L")` on a sixteen-bit image does not scale it down, it
    *clips* it - so every one of them came out solid white and the packed
    properties texture was three channels of ones. It looked like a working
    pipeline right up until the ground was lit by it.
    """
    if not path or not path.exists():
        return None
    image = Image.open(path)
    raw = np.asarray(image)
    if grey:
        plane = raw if raw.ndim == 2 else raw.mean(axis=2)
        return resized(scaled(plane), size)
    if raw.ndim == 2:                       # a one-channel picture asked for as colour
        raw = np.stack([raw] * 3, axis=2)
    return resized(scaled(raw[..., :3]), size)


def find(folder, material_id, name):
    for ext in ("png", "jpg", "jpeg", "exr"):
        candidate = folder / ("%s_%s.%s" % (material_id, name, ext))
        if candidate.exists():
            return candidate
    return None


def stretched(plane):
    """A channel pushed out to fill nought to one.

    Only where there is something to stretch: a map that already spans the
    range is left alone, and a flat one is left flat rather than being turned
    into noise by dividing by nearly nothing.
    """
    low, high = float(plane.min()), float(plane.max())
    if high - low < 0.02:
        return plane, (low, high)
    return (plane - low) / (high - low), (low, high)


def smaller(image, normal):
    """One step down the chain, by fours."""
    h, w = image.shape[:2]
    small = image.reshape(h // 2, 2, w // 2, 2, image.shape[2]).mean(axis=(1, 3))
    if normal:
        # Back to unit length. Averaging four directions gives a shorter vector
        # pointing the average way, and it is the length that has to be put
        # back, not the direction.
        v = small * 2.0 - 1.0
        length = np.sqrt((v * v).sum(axis=2, keepdims=True)).clip(1e-6)
        small = (v / length) * 0.5 + 0.5
    return small


def write(array, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.clip(array, 0, 1) * 255.0 + 0.5).astype(np.uint8)).save(path)


def chain(array, base, normal=False):
    write(array, base)
    level = array
    for step in STEPS:
        level = smaller(level, normal)
        write(level, base.with_name("%s@%d%s" % (base.stem, step, base.suffix)))


def pack(material, size, quiet):
    folder = TERRAIN / material["group"]
    src = folder / "src"
    if not src.exists():
        return None
    ident = material["id"]

    albedo = read(find(src, ident, "diffuse"), size)
    normal = read(find(src, ident, "normal"), size)
    if albedo is None or normal is None:
        print("  %-24s no sources yet - run tools/fetch_terrain.py" % ident)
        return None

    ao = read(find(src, ident, "ao"), size, grey=True)
    rough = read(find(src, ident, "rough"), size, grey=True)
    height = read(find(src, ident, "displacement"), size, grey=True)
    mask = read(find(src, ident, "mask"), size, grey=True)

    ones = np.ones((size, size), np.float32)
    ao = ones if ao is None else ao
    rough = ones * 0.8 if rough is None else rough
    height, span = (ones * 0.5, (0.5, 0.5)) if height is None else stretched(height)
    mask = ones if mask is None else mask

    properties = np.stack([ao, rough, height, mask], axis=2)

    chain(albedo, folder / (ident + "_albedo.png"))
    chain(normal, folder / (ident + "_normal.png"), normal=True)
    chain(properties, folder / (ident + "_properties.png"))

    note = src / "source.json"
    meta = json.loads(note.read_text()) if note.exists() else {}
    real = meta.get("original_physical_metres")
    packed = {
        "id": ident,
        "runtime_size": size,
        "runtime_files": {
            "albedo": ident + "_albedo.png",
            "normal": ident + "_normal.png",
            "properties": ident + "_properties.png",
        },
        "properties_channels": {"r": "ao", "g": "roughness", "b": "height", "a": "mask"},
        "mip_steps": STEPS,
        "height_range_before_stretch": [round(span[0], 4), round(span[1], 4)],
        "had": {
            "ao": find(src, ident, "ao") is not None,
            "rough": find(src, ident, "rough") is not None,
            "displacement": find(src, ident, "displacement") is not None,
            "mask": find(src, ident, "mask") is not None,
        },
        "source": meta,
        "world_scale": material.get("world_scale"),
    }
    (folder / "packed.json").write_text(json.dumps(packed, indent=2, ensure_ascii=False) + "\n")

    if not quiet:
        missing = [k for k, v in packed["had"].items() if not v]
        note = ("  (stood in for %s)" % ", ".join(missing)) if missing else ""
        scale = ""
        if real and material.get("world_scale"):
            scale = "   %.1f m of world, laid at %.1f" % (real[0], material["world_scale"])
        print("  %-24s %dx%d%s%s" % (ident, size, size, scale, note))
    return packed


def pack_water(entry, size, quiet):
    folder = TERRAIN / entry["group"]
    src = folder / "src" / entry["file"]
    if not src.exists():
        return None
    ident = entry["id"]
    if entry["kind"] == "normal":
        chain(read(src, size), folder / (ident + "_normal.png"), normal=True)
        kept = {"normal": ident + "_normal.png"}
    else:
        # Foam is kept as one channel and never as colour. What is wanted out of
        # the photograph is where the foam is, not what colour that day's sea
        # was, so the brightest channel becomes a mask and the rest is dropped.
        rgb = read(src, size)
        mask = rgb.max(axis=2)
        mask, _ = stretched(mask)
        chain(np.stack([mask] * 3, axis=2), folder / (ident + "_mask.png"))
        kept = {"mask": ident + "_mask.png"}
    (folder / "packed.json").write_text(json.dumps({
        "id": ident, "runtime_size": size, "runtime_files": kept, "mip_steps": STEPS,
        "source": json.loads((folder / "src" / "source.json").read_text())
        if (folder / "src" / "source.json").exists() else {},
    }, indent=2, ensure_ascii=False) + "\n")
    if not quiet:
        print("  %-24s %dx%d  %s" % (ident, size, size, entry["kind"]))
    return kept


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--size", type=int, default=2048, help="runtime map size (default 2048)")
    ap.add_argument("--only", nargs="*", metavar="ID")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    materials = json.loads(MATERIALS.read_text())
    water = json.loads(WATER.read_text())
    if args.only:
        wanted = set(args.only)
        materials = [m for m in materials if m["id"] in wanted]
        water = [w for w in water if w["id"] in wanted]

    print("packing at %d into %s" % (args.size, TERRAIN.relative_to(ROOT)))
    done = 0
    for m in materials:
        if pack(m, args.size, args.quiet):
            done += 1
    for w in water:
        if pack_water(w, args.size, args.quiet):
            done += 1
    total = len(materials) + len(water)
    print("%d of %d packed%s" % (done, total,
                                 "" if done == total else " - the rest have no sources yet"))
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())

