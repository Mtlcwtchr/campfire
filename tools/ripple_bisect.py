#!/usr/bin/env python3
"""Which shader term is the ripple. Measured, one term at a time.

The shaders compile at run time, so a term can be switched off by editing the
HLSL and taking another shot - no rebuild. The map camera at a fixed frame
settles to a bit-identical picture, so any change in the number is the term and
nothing else.

Each entry is a literal replacement in one file. The originals are restored
afterwards, including on failure.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path("/Users/antonslauta/CLionProjects/untitled")
SHADERS = ROOT / "assets/shaders"

CASES = [
    ("baseline", []),
    # Proves the harness: if tinting the ground does not move the number, the
    # edit is not reaching the pixels being measured and nothing below means
    # anything. This caught a first attempt aimed at a view that was 56% water.
    ("sanity-red", [("terrain.hlsl", "float3 colour = ground.colour;",
                     "float3 colour = ground.colour * float3(1.0, 0.2, 0.2);")]),
    ("bendTear=0", [("terrain.hlsl", "* 0.018;", "* 0.0;")]),
    ("relief=0", [("terrain.hlsl", "relief.cut *= rockCover;",
                   "relief.cut = 0; relief.edge = 0;")]),
    ("borderTear=0", [("terrain.hlsl", "const float intrusion = (broad",
                       "const float intrusion = 0.0 * (broad")]),
    ("macroColour=0", [("terrain.hlsl", "macroNoise(input.worldXY) - 0.5) * 0.06",
                        "macroNoise(input.worldXY) - 0.5) * 0.0")]),
    ("roughNoise=0", [("terrain.hlsl", "(roughNoise - 0.5) * 0.06",
                       "(roughNoise - 0.5) * 0.0")]),
    ("reliefNormal=0", [("terrain.hlsl",
                         "shadingNormal = reliefNormal(shadingNormal, worldPos, relief.cut, 0.10);",
                         "")]),
    ("erosionNormal=0", [("terrain.hlsl",
                          "shadingNormal = reliefNormal(shadingNormal,worldPos,erosion.depth*erosionSupport,0.65);",
                          "")]),
    ("screeNormal=0", [("terrain.hlsl",
                        "shadingNormal = reliefNormal(shadingNormal, worldPos, scree.y + gravel.y, 0.35);",
                        "")]),
    ("clutterNormal=0", [("terrain.hlsl",
                          "shadingNormal = reliefNormal(shadingNormal, worldPos, clutterHeight, 0.20);",
                          "")]),
    ("matNormal=geom", [("terrain.hlsl", "float3 shadingNormal = ground.normal;",
                         "float3 shadingNormal = normal;")]),
    ("albedoFlat", [("terrain.hlsl", "float3 colour = ground.colour;",
                     "float3 colour = float3(0.45, 0.42, 0.36);")]),
    ("sand=0", [("terrain.hlsl", "sandCover = sandSupport * (0.40 + 0.60 * patch) *",
                 "sandCover = 0.0 * (0.40 + 0.60 * patch) *")]),
    ("erosionCut=0", [("terrain.hlsl",
                       "groundColour *= 1.0-erosion.cut*erosionSupport*0.12;", "")]),
    # The floor of the measurement: no ground shading at all. Whatever is left
    # is sky, water and the edges of the frame, and no shader change below can
    # ever go under it.
    ("flatShade", [("terrain_pages.hlsl", "float4 colour = TerrainPS(input);",
                    "float4 colour = float4(0.45, 0.42, 0.36, 1.0);")]),
    ("normalGeom", [("terrain_pages.hlsl",
                     "input.normal = staged ? normal : normalize(float3(-shape.xy, 1.0));",
                     "input.normal = normal;")]),
    ("normalShape", [("terrain_pages.hlsl",
                      "input.normal = staged ? normal : normalize(float3(-shape.xy, 1.0));",
                      "input.normal = normalize(float3(-shape.xy, 1.0));")]),
    ("tileBlend=const", [("terrain_material.hlsli",
                          "const float blend = smoothstep(0.15, 0.85, noiseAt(p.xy / 9.0 + float(layer) * 17.0));",
                          "const float blend = 0.5;")]),
    ("matFine=0", [("terrain_material.hlsli",
                    "s.normal = rnmBlend(s.normal, quietNormal(fine.normal, normalStrength * 0.65 * fineDetail));",
                    "")]),
]


def apply(edits):
    touched = []
    for name, old, new in edits:
        path = SHADERS / name
        text = path.read_text()
        if text.count(old) != 1:
            raise SystemExit("%s: %r appears %d times" % (name, old, text.count(old)))
        shutil.copy(path, str(path) + ".bisect-backup")
        touched.append(path)
        path.write_text(text.replace(old, new))
    return touched


def restore(touched):
    for path in touched:
        backup = Path(str(path) + ".bisect-backup")
        if backup.exists():
            shutil.move(str(backup), str(path))


def shot(tag):
    out = "/tmp/bisect_%s.png" % tag.replace("=", "").replace("/", "")
    subprocess.run(["/tmp/shot.sh", out], check=True)
    result = subprocess.run([sys.executable, str(ROOT / "tools/terrain_shimmer.py"),
                             out, "--sky", "0.72"],
                            capture_output=True, text=True, check=True)
    return result.stdout.strip().splitlines()[-1]


def main():
    wanted = sys.argv[1:]
    for tag, edits in CASES:
        if wanted and tag not in wanted:
            continue
        touched = []
        try:
            touched = apply(edits)
            print("%-18s %s" % (tag, shot(tag)), flush=True)
        finally:
            restore(touched)


if __name__ == "__main__":
    main()

