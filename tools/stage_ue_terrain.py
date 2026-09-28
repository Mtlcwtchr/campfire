#!/usr/bin/env python3
"""Stage the UE terrain exports as ordinary material sources.

tools/export_ue_terrain.py writes the UE project's source art to a staging
directory (default /tmp/campfire_ue_terrain_maps/<id>/<channel>.png). This
turns each set into assets/terrain/<group>/src/<id>_{diffuse,normal,ao,rough,
displacement}.png - the layout tools/pack_terrain.py already packs - and adds
the set to content/config/terrain_materials.json.

  * normals are converted from UE's DirectX convention to OpenGL (green up),
    and Z is rebuilt from XY (UE normal maps are often stored two-channel);
  * packed masks are split: "ard" = AO / roughness / displacement,
    "ord" = occlusion / roughness / displacement;
  * everything is resampled to --size (default 2048) once, here, so the repo
    does not carry 8K sources the runtime never reads.

    python3 tools/stage_ue_terrain.py [--source /tmp/campfire_ue_terrain_maps]
    python3 tools/pack_terrain.py --only sand_dune sand_shore ...
"""
import argparse
import json
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CATALOGUE = ROOT / "content/config/terrain_materials.json"
TERRAIN = ROOT / "assets/terrain"

# id: (group, metres one texture turn covers, comment, material response)
SETS = {
    "sand_dune": ("sand/dune", 3.2, "Wind-rippled dune sand: open desert, dune fields.",
                  dict(normal_strength=0.45, slope_preference=0.0, moisture_response=0.6)),
    "sand_shore": ("sand/shore", 2.0, "Coarse beach sand above the waterline.",
                   dict(normal_strength=0.4, slope_preference=0.0, moisture_response=1.0)),
    "sand_gravelly": ("sand/gravelly", 2.6, "Gravelly semi-desert sand, dry washes.",
                      dict(normal_strength=0.5, slope_preference=0.2, moisture_response=0.5)),
    "cliff_dolomite": ("cliff/dolomite", 6.0, "Pale limestone cliff face.",
                       dict(normal_strength=0.7, slope_preference=1.0, moisture_response=0.4)),
    "cliff_mossy_rock": ("cliff/mossy_rock", 4.0, "Moss-covered rock on wet, shaded slopes.",
                         dict(normal_strength=0.65, slope_preference=1.0, moisture_response=0.8)),
    "cliff_desert": ("cliff/desert", 4.5, "Warm weathered cliff side of arid country.",
                     dict(normal_strength=0.7, slope_preference=1.0, moisture_response=0.3)),
}


def load(path, size):
    image = Image.open(path)
    raw = np.asarray(image)
    if raw.dtype == np.uint16:
        values = raw.astype(np.float32) / 65535.0
    else:
        values = raw.astype(np.float32) / 255.0
    if values.ndim == 2:
        values = values[..., None]
    channels = []
    for c in range(values.shape[2]):
        plane = Image.fromarray(values[..., c].astype(np.float32))
        if plane.size != (size, size):
            plane = plane.resize((size, size), Image.LANCZOS)
        channels.append(np.asarray(plane))
    return np.clip(np.stack(channels, axis=2), 0.0, 1.0)


def save(array, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    if array.ndim == 3 and array.shape[2] == 1:
        array = array[..., 0]
    Image.fromarray((np.clip(array, 0, 1) * 255.0 + 0.5).astype(np.uint8)).save(path)


def normal_gl(values):
    xy = values[..., :2] * 2.0 - 1.0
    xy[..., 1] = -xy[..., 1]                      # DirectX -> OpenGL
    z = np.sqrt(np.clip(1.0 - (xy * xy).sum(axis=2), 0.0, 1.0))
    n = np.concatenate([xy, z[..., None]], axis=2)
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-6)
    return n * 0.5 + 0.5


def stage(ident, source, size):
    group, scale, comment, response = SETS[ident]
    folder = source / ident
    src = TERRAIN / group / "src"
    written = {}
    diffuse = load(folder / "diffuse.png", size)[..., :3]
    save(diffuse, src / (ident + "_diffuse.png")); written["diffuse"] = ident + "_diffuse.png"
    normal = normal_gl(load(folder / "normal.png", size))
    save(normal, src / (ident + "_normal.png")); written["normal"] = ident + "_normal.png"
    for packed in ("ard", "ord"):
        if (folder / (packed + ".png")).exists():
            mask = load(folder / (packed + ".png"), size)
            for index, kind in enumerate(("ao", "rough", "displacement")):
                save(mask[..., index], src / ("%s_%s.png" % (ident, kind)))
                written[kind] = "%s_%s.png" % (ident, kind)
    for kind in ("ao", "rough", "displacement"):
        if kind not in written and (folder / (kind + ".png")).exists():
            save(load(folder / (kind + ".png"), size)[..., :1], src / ("%s_%s.png" % (ident, kind)))
            written[kind] = "%s_%s.png" % (ident, kind)
    report = json.loads((source / "terrain_export.json").read_text()).get(ident, {})
    (src / "source.json").write_text(json.dumps({
        "site": "ue-export",
        "project": "AncientSettlement",
        "license": "Original asset-pack licence (not CC0)",
        "assets": {k: v.get("asset") for k, v in report.items()},
        "original_sizes": {k: v.get("size") for k, v in report.items()},
        "runtime_world_scale": scale,
        "resolution": "%dpx" % size,
        "files": written,
    }, indent=2) + "\n")
    entry = {
        "id": ident, "group": group, "_comment": comment,
        "source": {"site": "ue-export", "project": "AncientSettlement",
                   "license": "Original asset-pack licence (not CC0)",
                   "package": report.get("diffuse", {}).get("asset")},
        "world_scale": scale, "tint": [1.0, 1.0, 1.0], "saturation": 0.9,
        "normal_strength": response["normal_strength"], "ao_strength": 0.4,
        "roughness_multiplier": 1.0, "height_strength": 0.5,
        "slope_preference": response["slope_preference"],
        "moisture_response": response["moisture_response"], "snow_compatibility": 0.6,
    }
    return entry, sorted(written)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=Path("/tmp/campfire_ue_terrain_maps"))
    parser.add_argument("--size", type=int, default=2048)
    args = parser.parse_args()
    catalogue = json.loads(CATALOGUE.read_text())
    known = {m["id"]: i for i, m in enumerate(catalogue)}
    for ident in SETS:
        if not (args.source / ident / "diffuse.png").exists():
            print("  %-18s not exported" % ident)
            continue
        entry, files = stage(ident, args.source, args.size)
        if ident in known:
            catalogue[known[ident]] = entry
        else:
            catalogue.append(entry)
        print("  %-18s -> %s  %s" % (ident, entry["group"], ", ".join(files)))
    CATALOGUE.write_text(json.dumps(catalogue, indent=2, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

