"""List large ground-like textures in the UE project (read-only, no loading).

Uses Asset Registry tags ("Dimensions"), so nothing is loaded or saved:
    UnrealEditor-Cmd AncientSettlement.uproject -run=pythonscript -script=<this> -unattended -nullrhi
Writes AS_UE_OUT/terrain_sources.json (default /tmp/campfire_ue_terrain).
"""
import json
import os
from pathlib import Path

import unreal

OUT = Path(os.environ.get("AS_UE_OUT", "/tmp/campfire_ue_terrain"))
WORDS = ("grass", "soil", "dirt", "mud", "sand", "rock", "stone", "gravel", "snow", "ground", "moss", "forest",
         "earth", "cliff", "pebble", "clay", "leaves", "floor")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    rows = []
    for data in registry.get_assets_by_path("/Game", recursive=True):
        if str(data.asset_class_path.asset_name) != "Texture2D":
            continue
        name = str(data.asset_name)
        low = name.lower()
        if not any(word in low for word in WORDS):
            continue
        dims = str(data.get_tag_value("Dimensions") or "")
        try:
            w, h = (int(v) for v in dims.lower().split("x")[:2])
        except Exception:
            continue
        rows.append({"asset": str(data.package_name), "name": name, "width": w, "height": h,
                     "compression": str(data.get_tag_value("CompressionSettings") or ""),
                     "srgb": str(data.get_tag_value("SRGB") or "")})
    rows.sort(key=lambda r: (-r["width"] * r["height"], r["asset"]))
    (OUT / "terrain_sources.json").write_text(json.dumps(rows, indent=1))
    unreal.log("terrain sources: %d, >=4k: %d" % (len(rows), sum(1 for r in rows if max(r["width"], r["height"]) >= 4096)))


main()

