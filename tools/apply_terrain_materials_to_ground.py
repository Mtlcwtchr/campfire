#!/usr/bin/env python3
"""Bridge new terrain library materials into legacy ground textures.

Current renderer on `master` samples `assets/sprites/ground/<name>.png` (+ @2/@4/@8/@16).
This script composes those files from `assets/terrain/<group>/<id>_{albedo,properties}.png`
by taking RGB from albedo and height from properties.B into alpha.
"""

from __future__ import annotations

from pathlib import Path
from typing import Dict

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
TERRAIN = ROOT / "assets" / "terrain"
GROUND = ROOT / "assets" / "sprites" / "ground"
MIP_STEPS = [1, 2, 4, 8, 16]

# Legacy slots -> new terrain material ids.
MATERIAL_MAP: Dict[str, str] = {
    "grass": "grass_lush",
    "dirt": "soil_base",
    "sand": "sand_dry",
    "rock": "rock_ground",
    "marsh": "mud_wet",
    "snow": "snow_clean",
    "grass_wild": "grass_sparse",
    "rock_smooth": "river_pebbles",
}


def material_group(material_id: str) -> str:
    # Resolve group from packed metadata instead of hardcoding folder names.
    for packed in TERRAIN.glob("**/packed.json"):
        text = packed.read_text(encoding="utf-8")
        if f'"id": "{material_id}"' in text:
            return str(packed.parent.relative_to(TERRAIN))
    raise FileNotFoundError(f"packed.json for '{material_id}' not found under {TERRAIN}")


def open_rgba(material_id: str, group: str, step: int) -> Image.Image:
    suffix = "" if step == 1 else f"@{step}"
    albedo = TERRAIN / group / f"{material_id}_albedo{suffix}.png"
    props = TERRAIN / group / f"{material_id}_properties{suffix}.png"
    if not albedo.exists() or not props.exists():
        raise FileNotFoundError(f"missing input maps for {material_id}{suffix}")

    rgb = Image.open(albedo).convert("RGB")
    prop = Image.open(props).convert("RGBA")
    alpha = prop.split()[2]  # B channel stores height
    out = Image.merge("RGBA", (*rgb.split(), alpha))
    return out


def main() -> int:
    GROUND.mkdir(parents=True, exist_ok=True)

    resolved = {legacy: material_group(new_id) for legacy, new_id in MATERIAL_MAP.items()}
    for legacy, material_id in MATERIAL_MAP.items():
        group = resolved[legacy]
        for step in MIP_STEPS:
            out_name = f"{legacy}.png" if step == 1 else f"{legacy}@{step}.png"
            out_path = GROUND / out_name
            image = open_rgba(material_id, group, step)
            image.save(out_path, optimize=True)
        print(f"{legacy:12s} <- {material_id:16s} ({group})")

    print(f"wrote legacy ground textures to {GROUND}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

