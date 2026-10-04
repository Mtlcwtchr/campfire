"""Any source mesh made to sit in the painted world (fantasy_80s_art_asset_style_spec
§5-§6): simplified to its silhouette, optionally reduced to painted masses,
weathered, its textures painted towards the references, and written where the
model pipeline reads it. The engine's tool - which presets a game uses is the
game's.

    blender -b -P tools/blender/stylize_mesh.py -- SOURCE.glb --asset rock_painted_01 \\
        [--triangles 4000] [--mass 0.12] [--planar 10] [--weather 0.5] \\
        [--rescan rock_face_03,cliff_side] [--preset megalith_painted] [--size 1024]

SOURCE may be .glb/.gltf, .obj or .fbx. --rescan replaces the source's own
textures with box-projected world scans (what a photogrammetry scan needs
when its colour will not match the world); otherwise the source's material
is baked as it is. --preset runs tools/stylize_textures.py on the result.
"""
import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import asr_blender as ab  # noqa: E402

import bpy  # noqa: E402


def load(source):
    suffix = Path(source).suffix.lower()
    if suffix in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=source)
    elif suffix == ".obj":
        bpy.ops.wm.obj_import(filepath=source)
    elif suffix == ".fbx":
        bpy.ops.import_scene.fbx(filepath=source)
    else:
        sys.exit("unsupported source " + source)
    meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
    if not meshes:
        sys.exit("no mesh in " + source)
    return meshes


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("--asset", required=True)
    parser.add_argument("--triangles", type=int, default=0)
    parser.add_argument("--mass", type=float, default=0.0, help="voxel size of the painted-mass remesh, metres")
    parser.add_argument("--planar", type=float, default=0.0, help="dissolve to broad planes under this angle")
    parser.add_argument("--weather", type=float, default=0.0)
    parser.add_argument("--rescan", default="")
    parser.add_argument("--preset", default="")
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--license", default="CC0-1.0")
    parser.add_argument("--author", default="")
    args = parser.parse_args(ab.arguments())
    ab.reset()
    meshes = load(args.source)
    obj = ab.join(meshes, args.asset) if len(meshes) > 1 else meshes[0]
    obj.name = args.asset
    ab.select_only(obj)
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=True)
    if args.mass > 0:
        ab.painted_mass(obj, voxel=args.mass, smooth=4, planar=args.planar)
    elif args.planar > 0:
        d = obj.modifiers.new("planes", "DECIMATE")
        d.decimate_type = "DISSOLVE"
        d.angle_limit = args.planar * 3.14159265 / 180.0
        ab.apply_all(obj)
    if args.weather > 0:
        ab.weather(obj, seed=1, cracks=args.weather, erosion=args.weather * 0.7, chips=args.weather * 0.8)
    if args.triangles:
        ab.reduce(obj, args.triangles)
    ab.ground(obj)
    if args.rescan:
        scans = [ab.scan(n) for n in args.rescan.split(",") if n]
        obj.data.materials.clear()
        obj.data.materials.append(ab.stone_material(args.asset + "_m", scans, 2.0, (1, 1, 1), ab.scan("mossy_rock")))
    folder = ab.OUT / args.asset
    maps = ab.bake_textures(obj, folder / "textures", args.asset, args.size)
    if args.preset:
        for kind in ("albedo", "normal"):
            subprocess.run(["python3", str(ab.ROOT / "tools" / "stylize_textures.py"), "--preset", args.preset,
                            "--kind", kind, str(maps[kind]), str(maps[kind])], check=False)
    ab.export(args.asset, [obj], f"tools/blender/stylize_mesh.py from {Path(args.source).name}",
              license=args.license, author=args.author or "unknown", tags=["stylized"], category="rock",
              extra={"stylized_from": str(args.source)})
    print(args.asset + " -> " + str(folder))


if __name__ == "__main__":
    main()
