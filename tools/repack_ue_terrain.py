#!/usr/bin/env python3
"""Re-pack the installed UE terrain materials at a higher runtime resolution.

prepare_ue_assets.py packed the six MW Landscape surfaces at 1024 px, half of
the 2048 px originals in the UE project (and in the export under
assets/source/ancient_settlement). This rewrites only their albedo and normal
chains (name.png + name@2..@16) at --size (default 2048); the neutral
properties maps are left as they are. Every file is written to a temporary
name first and moved into place only when all of them succeeded.

    python3 tools/repack_ue_terrain.py [--size 2048] [--source assets/source/ancient_settlement]
"""
import argparse
import json
import shutil
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import pack_terrain  # noqa: E402
from prepare_ue_assets import TERRAIN, source_file  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--size", type=int, default=2048)
    parser.add_argument("--source", type=Path, default=ROOT / "assets/source/ancient_settlement")
    args = parser.parse_args()
    if args.size not in (512, 1024, 2048, 4096):
        raise SystemExit("size must be 512, 1024, 2048 or 4096")
    export = json.loads((args.source / "export.json").read_text())
    target = ROOT / "assets/terrain"
    with tempfile.TemporaryDirectory(dir=str(target)) as temporary:
        stage = Path(temporary)
        written = []
        for name, stem, _roughness in TERRAIN:
            maps = export["terrain"][name]
            albedo = pack_terrain.read(source_file(args.source, maps["albedo"]["file"]), args.size)
            normal = pack_terrain.read(source_file(args.source, maps["normal"]["file"]), args.size)
            normal[..., 1] = 1. - normal[..., 1]  # DirectX to OpenGL green, as prepare_ue_assets does
            for kind, values in (("albedo", albedo), ("normal", normal)):
                base = stage / (stem + "_" + kind + ".png")
                base.parent.mkdir(parents=True, exist_ok=True)
                pack_terrain.chain(values, base, normal=kind == "normal")
                written.append(stem + "_" + kind)
            print(name, "->", stem, args.size, flush=True)
        for file in stage.rglob("*.png"):
            destination = target / file.relative_to(stage)
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(file), str(destination))
    print("re-packed", len(written), "maps at", args.size)


main()

