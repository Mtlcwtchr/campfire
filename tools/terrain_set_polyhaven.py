#!/usr/bin/env python3
"""The Poly Haven ground set the renderer's sixteen layers are built from.

Adds (or refreshes) one catalogue entry per layer in
content/config/terrain_materials.json, then:

    python3 tools/terrain_set_polyhaven.py
    python3 tools/fetch_terrain.py --res 2k --only $(python3 tools/terrain_set_polyhaven.py --ids)
    python3 tools/pack_terrain.py --only $(python3 tools/terrain_set_polyhaven.py --ids)

Layer order is the shader's (assets/shaders/terrain_material.hlsli) and the
renderer's map list (src/game/render/world_renderer.cpp).
"""
import json
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOGUE = ROOT / "content/config/terrain_materials.json"

# (layer, Poly Haven asset, role)
SET = [
    (0, "leafy_grass", "grass: meadow ground under the grass cards"),
    (1, "dirt_floor", "dirt: bare earth"),
    (2, "red_sand", "sand: dunes (orange), desert only"),
    (3, "rocks_ground_05", "rock: stony ground"),
    (4, "brown_mud_02", "marsh: wet mud"),
    (5, "snow_02", "snow"),
    (6, "withered_grass", "grass: dry"),
    (7, "forest_leaves_02", "forest floor"),
    (8, "coast_sand_04", "sand: beach"),
    (9, "damp_beach_sand", "sand: waterline"),
    (10, "sand_01", "sand: plain dry"),
    (11, "sandy_gravel_02", "sand: stony"),
    (12, "rock_face_03", "cliff: grey rock face"),
    (13, "mossy_rock", "cliff: mossy, wet climates"),
    (14, "cliff_side", "cliff: warm, arid"),
    (15, "mud_cracked_dry_riverbed_002", "marsh: dried and cracked"),
]


def ids():
    return ["ph_" + asset for _, asset, _ in SET]


def main():
    if "--ids" in sys.argv:
        print(" ".join(ids()))
        return 0
    catalogue = json.loads(CATALOGUE.read_text())
    index = {m["id"]: i for i, m in enumerate(catalogue)}
    for layer, asset, role in SET:
        request = urllib.request.Request("https://api.polyhaven.com/info/" + asset,
                                         headers={"User-Agent": "asr-terrain-set/1"})
        info = json.loads(urllib.request.urlopen(request, timeout=60).read())
        metres = round(info["dimensions"][0] / 1000.0, 3)
        entry = {
            "id": "ph_" + asset, "group": "ph/" + asset,
            "_comment": "layer %d - %s" % (layer, role),
            "source": {"site": "polyhaven", "asset_id": asset, "license": "CC0",
                       "attribution_required": False},
            "world_scale": metres, "tint": [1.0, 1.0, 1.0], "saturation": 1.0,
            "normal_strength": 0.6, "ao_strength": 0.5, "roughness_multiplier": 1.0,
            "height_strength": 0.6, "slope_preference": 1.0 if role.startswith("cliff") else 0.0,
            "moisture_response": 0.8, "snow_compatibility": 0.7,
        }
        if entry["id"] in index:
            catalogue[index[entry["id"]]] = entry
        else:
            index[entry["id"]] = len(catalogue)
            catalogue.append(entry)
        print("  layer %2d  %-30s %6.2f m" % (layer, asset, metres))
    CATALOGUE.write_text(json.dumps(catalogue, indent=2, ensure_ascii=False) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())

