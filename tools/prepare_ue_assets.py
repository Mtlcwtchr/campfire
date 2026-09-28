#!/usr/bin/env python3
"""Prepare an exported UE nature/terrain set in a NEW staging directory.
Never replaces live assets. Requires numpy/Pillow from scene_models_requirements.
Unsupported/ambiguous material bindings fail rather than silently producing grey trees.
"""
import argparse
import json
import math
import re
from pathlib import Path
import struct
import tempfile

import numpy as np
from PIL import Image

import mesh_lod
import pack_terrain
import prepare_scene_models as scene
from ue_asset_policy import (TRIANGLE_LIMITS, SCENE_ROLES, MAX_TEXTURES, check_triangles,
                             check_png, check_export_bytes, check_leaf_materials)

ROLES = list(SCENE_ROLES)
TERRAIN = [("Grass", "grass/lush/grass_lush", .85), ("Dirt", "soil/base/soil_base", .8),
           ("SandA", "sand/dry/sand_dry", .9), ("Rock", "stone/rock/rock_ground", .75),
           ("Dirt", "soil/mud_wet/mud_wet", .55), ("Snow", "snow/clean/snow_clean", .6)]


def source_file(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise ValueError("Missing/unsafe source file: " + str(name))
    return path


def load_mesh(path, role=None):
    with path.open("rb") as stream:
        header = stream.read(12)
    if len(header) < 12:
        raise ValueError("Truncated UMS1 mesh")
    magic, nv, ni = struct.unpack("<4sII", header)
    limit = TRIANGLE_LIMITS[role] if role else max(TRIANGLE_LIMITS.values())
    if magic != b"UMS1" or not nv or not ni or ni % 3 or nv > ni or ni > limit * 3:
        raise ValueError("Invalid/beyond-budget UMS1 header")
    if path.stat().st_size != 12 + nv * 48 + ni * 4:
        raise ValueError("UMS1 size does not match its header")
    data = path.read_bytes()
    v = np.frombuffer(data, dtype="<f4", count=nv * 12, offset=12).reshape(nv, 12).copy()
    indices = np.frombuffer(data, dtype="<u4", count=ni, offset=12 + nv * 48).copy()
    if not np.isfinite(v).all() or np.any(indices >= nv) or np.any(v[:, 11] != np.floor(v[:, 11])):
        raise ValueError("Invalid UMS1 attributes/indices")
    return v, indices


def binding(material, channel, optional=False):
    # Explicit bindings, if supplied, take precedence; otherwise accept only
    # unambiguous conventional parameter names. Never guess among atlas layers.
    aliases = {"albedo": {"albedo", "basecolor", "diffuse", "diffusetexture", "albedotexture", "basecolortexture"},
               "normal": {"normal", "normalmap", "normaltexture"},
               "opacity": {"opacity", "opacitymask", "opacitytexture"}}
    files = material["files"]
    explicit = material.get("bindings", {}).get(channel)
    if explicit is not None:
        return files[explicit]
    found = {file for key, file in files.items()
             if "".join(c for c in key.lower() if c.isalnum()) in aliases[channel]}
    if len(found) == 1:
        return found.pop()
    if not found and optional:
        return None
    raise ValueError(f"{material['asset']}: explicit {channel} binding required; parameters: {list(files)}")


def normal_gl(image):
    data = np.array(image.convert("RGB"))
    data[..., 1] = 255 - data[..., 1]  # UE DirectX -> renderer's OpenGL convention
    return Image.fromarray(data)


def packed_normal_gl(image):
    # Megaplant's B contains AO/translucency, NOT the normal's Z component.
    xy = np.asarray(image.convert("RGB"), dtype=np.float32)[..., :2] / 255. * 2. - 1.
    xy[..., 1] *= -1.
    z = np.sqrt(np.maximum(0., 1. - np.sum(xy * xy, axis=-1)))
    n = np.dstack((xy, z))
    n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-8)
    return Image.fromarray(np.rint(np.clip(n * .5 + .5, 0, 1) * 255).astype(np.uint8))


def light_foliage_colour(material, image):
    # M_Master_Foliage: lerp(base,Tint_2,R), then Tint_3/G, then Tint_1/B.
    # Camera-dependent fake shadows/fades are left to our renderer.
    constants = [n["constant"] for n in material["graph"]["nodes"].values()
                 if n["class"] == "MaterialExpressionConstant3Vector"]
    if len(constants) != 1:
        raise ValueError("Unexpected Light Foliage base-colour graph")
    base = np.array([float(re.search(r"\b" + c + r":\s*([-+0-9.eE]+)", constants[0]).group(1)) for c in "rgb"])
    mask = np.asarray(image.convert("RGB"), dtype=np.float32) / 255.
    colour = np.broadcast_to(base, mask.shape).copy()
    for channel, name in enumerate(("Tint_2", "Tint_3", "Tint_1")):
        t = mask[..., channel:channel + 1]
        colour = colour * (1. - t) + np.asarray(material["vectors"][name][:3]) * t
    colour = np.clip(colour, 0, 1)
    colour = np.where(colour <= .0031308, colour * 12.92, 1.055 * colour ** (1. / 2.4) - .055)
    alpha = mask.sum(axis=-1)
    if not material.get("switches", {}).get("is Shrub", False):
        alpha *= 6.
    rgba = np.dstack((colour, np.clip(alpha, 0, 1)))
    return Image.fromarray(np.rint(rgba * 255).astype(np.uint8))


def prepare(source, output):
    if output.exists():
        raise ValueError("Staging destination already exists; do not overwrite live resources")
    check_export_bytes(source)
    export = json.loads((source / "export.json").read_text())
    if export.get("geometry_source") != "catalogue_verified_cut_lod0":
        raise ValueError("Only a catalogue-verified, already-cut UE export may be installed")
    if not export.get("section_materials_verified"):
        raise ValueError("Re-export with verified section materials; old exports can lose foliage")
    rows = {row["role"]: row for row in export["models"]}
    if set(rows) != set(ROLES + ["Grass_1"]):
        raise ValueError("Export must contain the full scene catalogue and Grass_1")
    texture_files = {source_file(source, name) for row in rows.values() for material in row["materials"]
                     for name in material["files"].values()}
    texture_files.update(source_file(source, channel["file"]) for maps in export["terrain"].values() for channel in maps.values())
    if len(texture_files) > MAX_TEXTURES:
        raise ValueError("Too many source textures for the selected game visuals")
    for path in texture_files:
        check_png(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".ue-prepare-", dir=output.parent) as temporary:
        stage = Path(temporary)
        models = stage / "scene_models"
        models.mkdir()
        manifest = {"version": 2, "views": 8, "models": [], "groves": [], "colours": [], "normals": [],
                    "source": "AncientSettlement UE export", "license": export["license"], "grass": []}
        images, layers = [], {}
        def add_layer(key, colour, normal):
            if key not in layers:
                n = len(images)
                colour.save(models / f"colour-{n}.png")
                normal.save(models / f"normal-{n}.png")
                images.append(np.array(colour.convert("RGBA")))
                manifest["colours"].append(f"colour-{n}.png")
                manifest["normals"].append(f"normal-{n}.png")
                layers[key] = n
            return layers[key]
        def read_image(name):
            with Image.open(source_file(source, name)) as image:
                return image.convert("RGBA").resize((scene.SIZE, scene.SIZE), Image.Resampling.LANCZOS)
        for role in ROLES + ["Grass_1"]:
            row = rows[role]
            v, ind = load_mesh(source_file(source, row["mesh"]), role)
            check_triangles(role, len(ind) // 3)
            source_materials = v[ind[::3], 11].astype(int)
            if not row.get("assembly"):
                check_leaf_materials(role, row["materials"], set(source_materials.tolist()))
            leaf_slots = [i for i, m in enumerate(row["materials"])
                          if float(m.get("scalars", {}).get("IsLeaves", 0)) > .5]
            leaf_triangles = int(np.isin(source_materials, leaf_slots).sum())
            slots = []
            for material in row["materials"]:
                masked_colour = material.get("colour_encoding") == "light_foliage_tints"
                colour_file = material["files"]["colour_mask"] if masked_colour else binding(material, "albedo")
                normal_file = binding(material, "normal", True)
                opacity_file = binding(material, "opacity", True)
                colour = read_image(colour_file)
                if masked_colour:
                    colour = light_foliage_colour(material, colour)
                if opacity_file:
                    opacity = read_image(opacity_file).getchannel("R")
                    colour.putalpha(opacity)
                decode_normal = packed_normal_gl if material.get("normal_encoding") == "packed_rg_directx" else normal_gl
                normal = decode_normal(read_image(normal_file)).convert("RGBA") if normal_file else Image.new("RGBA", (scene.SIZE, scene.SIZE), (128, 128, 255, 255))
                slots.append(add_layer((material["asset"], colour_file, normal_file, opacity_file), colour, normal))
            material_ids = v[:, 11].astype(int)
            if np.any(material_ids < 0) or np.any(material_ids >= len(slots)):
                raise ValueError("Mesh refers to an absent material")
            v[:, 11] = np.asarray(slots)[material_ids]
            assembly_stats = None
            if row.get("assembly"):
                from assemble_tree_crown import add_crown
                v, ind, leaf_triangles, assembly_stats = add_crown(row, export["assembly_parts"], source,
                    v, ind, slots, images, add_layer, load_mesh, scene, TRIANGLE_LIMITS[role])
            # Source dimensions are metres, NOT the old stylized model's scale.
            v[:, 2] -= v[:, 2].min()
            if role == "Grass_1":
                width = float(np.linalg.norm(v[:, :2], axis=1).max() * 2.04)
                height = float(v[:, 2].max() * 1.02)
                if min(width, height) <= 0:
                    raise ValueError("Grass has no spatial extent")
                for view in range(6):
                    colour, _ = scene.rasterize(v, ind, images, width, height, view * math.tau / 6)
                    name = f"grass-view-{view}.png"
                    colour.save(models / name)
                    manifest["grass"].append(name)
                continue
            grown, coarse = mesh_lod.build_levels(v, ind, scene.LOD_RATIOS)
            buffer = np.concatenate((v, grown)).astype("<f4") if len(grown) else v
            width = float(np.linalg.norm(buffer[:, :2], axis=1).max() * 2.04)
            height = float(buffer[:, 2].max() * 1.02)
            if min(width, height) <= 0:
                raise ValueError("Mesh has no spatial extent")
            levels = [{"indices": ind, "triangles": len(ind) // 3, "cards": -1, "error": (0., 0., 0.)}] + coarse
            with (models / (role + ".mesh")).open("wb") as f:
                f.write(struct.pack("<4sII", b"SCM2", len(buffer), len(levels)))
                f.write(struct.pack("<%dI" % len(levels), *[len(level["indices"]) for level in levels]))
                f.write(buffer.astype("<f4").tobytes())
                for level in levels:
                    f.write(level["indices"].astype("<u4").tobytes())
            first = len(images)
            for view in range(8):
                colour, normal = scene.rasterize(v, ind, images, width, height, view * math.tau / 8)
                add_layer((role, view), colour, normal)
            manifest["models"].append({"name": role, "source_asset": row["asset"], "mesh": role + ".mesh",
                "width": width, "height": height, "vegetation": role in ROLES[:3], "impostor": first,
                "vertices": len(buffer), "triangles": len(ind) // 3,
                "foliage_triangles": leaf_triangles,
                "assembly": assembly_stats,
                "levels": [{"triangles": l["triangles"], "cards": l["cards"], "error_m": l["error"][1],
                            "error_max_m": l["error"][0], "error_rms_m": l["error"][2]} for l in levels]})
            if role in ROLES[:2]:
                grove_width, grove_height, grove_first = width + 24., height * 1.12, len(images)
                positions = [(x * 7.5 + (y % 2) * 1.3, y * 7.5, .86 + ((x + 2 * y) % 5) * .06)
                             for y in (-1, 0, 1) for x in (-1, 0, 1)]
                for view in range(8):
                    angle = view * math.tau / 8
                    colour = Image.new("RGBA", (scene.SIZE, scene.SIZE))
                    normal = Image.new("RGBA", (scene.SIZE, scene.SIZE), (128, 128, 255, 0))
                    tree = Image.fromarray(images[first + view])
                    with Image.open(models / f"normal-{first + view}.png") as im:
                        tree_normal = im.convert("RGBA")
                    tree_normal.putalpha(tree.getchannel("A"))
                    for x, y, factor in sorted(positions, key=lambda p: p[0] * math.sin(angle) - p[1] * math.cos(angle)):
                        w, h = max(1, round(scene.SIZE * width * factor / grove_width)), max(1, round(scene.SIZE * height * factor / grove_height))
                        at = (round(scene.SIZE * (.5 + (x * math.cos(angle) + y * math.sin(angle)) / grove_width)) - w // 2, scene.SIZE - h)
                        colour.alpha_composite(tree.resize((w, h), Image.Resampling.LANCZOS), at)
                        normal.alpha_composite(tree_normal.resize((w, h), Image.Resampling.BILINEAR), at)
                    add_layer((role, "grove", view), colour, normal)
                manifest["groves"].append({"model": len(manifest["models"]) - 1, "impostor": grove_first,
                                          "width": grove_width, "height": grove_height, "trees": 9})
            print(role, len(ind) // 3, "source triangles", flush=True)
        (models / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (models / "LICENSE.txt").write_text(export["license"] + "\nSource: AncientSettlement UE project.\n")
        for name, stem, roughness in TERRAIN:
            maps = export["terrain"][name]
            albedo = pack_terrain.read(source_file(source, maps["albedo"]["file"]), 1024)
            normal = pack_terrain.read(source_file(source, maps["normal"]["file"]), 1024)
            normal[..., 1] = 1. - normal[..., 1]
            # MW ships no AO/height maps. Keep explicit neutral channels instead
            # of inventing scanned physical properties from colour brightness.
            properties = np.empty((1024, 1024, 4), np.float32)
            properties[:] = (1., roughness, .5, 1.)
            for kind, values in (("albedo", albedo), ("normal", normal), ("properties", properties)):
                pack_terrain.chain(values, stage / "terrain" / (stem + "_" + kind + ".png"), normal=kind == "normal")
        (stage / "provenance.json").write_text(json.dumps(export, indent=2) + "\n")
        check_export_bytes(stage)
        stage.rename(output)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(prepare(args.source.resolve(), args.output.resolve()))

