#!/usr/bin/env python3
"""Append the selected environment visuals to the existing scene catalogue.

Uses the same SCM2 mesh chains, native 8+21 depth impostors and cluster sidecars
as the shipped models. Keeps the base catalogue intact and publishes manifest
last. Repeated runs replace this extension instead of growing the texture array.
Sources and licenses remain in assets/models; generated assets remain outside git.

    python3 tools/simplify_models.py --only rock_face_01 rock_face_02 moss_01 fern_02 shrub_02 shrub_04
    python3 tools/prepare_environment_models.py
"""
import argparse
import copy
import json
import math
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

import bake_model_impostors as bake
import hemisphere_impostor as hemi
import mesh_lod
import prepare_scene_models as scene

ROOT = Path(__file__).resolve().parents[1]
CONFIG = ROOT / "content/config/environment_models.json"
HEADER = ROOT / "src/game/world/environment_models.gen.hpp"


def canonical_face(v, ind):
    """Exposed scan normal -> horizontal -Y, in a rigid upright cliff frame."""
    p = v[ind.reshape(-1, 3), :3].astype(float)
    normals = np.cross(p[:, 1] - p[:, 0], p[:, 2] - p[:, 0])
    horizontal = normals[:, :2].sum(axis=0)
    if np.linalg.norm(horizontal) < 1e-5:
        # A closed rock has no average normal. Its thin principal axis is the face.
        _, basis = np.linalg.eigh(np.cov(v[:, :2].T))
        horizontal = basis[:, 0]
    angle = -math.pi / 2 - math.atan2(horizontal[1], horizontal[0])
    c, s = math.cos(angle), math.sin(angle)
    rotation = np.array([[c, -s], [s, c]])
    v[:, :2] = v[:, :2] @ rotation.T
    v[:, 3:5] = v[:, 3:5] @ rotation.T
    # These scans lean uphill by 33–45 degrees. A yaw-only frame places their
    # upper face inside steep terrain. Remove the scan's average lean before
    # baking; mesh, normals, impostors and support then use the same frame.
    magnitude = float(np.linalg.norm(normals[:, :2].sum(axis=0)))
    pitch = math.atan2(float(normals[:, 2].sum()), magnitude) if magnitude > 1e-5 else 0.0
    c, s = math.cos(pitch), math.sin(pitch)
    upright = np.array([[c, -s], [s, c]])
    v[:, 1:3] = v[:, 1:3] @ upright.T
    v[:, 4:6] = v[:, 4:6] @ upright.T
    v[:, 2] -= v[:, 2].min()
    low = v[:, 2] <= max(float(v[:, 2].max()) * .1, 1e-4)
    v[:, :2] -= v[low, :2].mean(axis=0)
    return angle, pitch


def normals_for(asset_dir):
    doc = json.loads((asset_dir / (asset_dir.name + ".gltf")).read_text())
    # bake.variants allocates one layer per used material, in primitive encounter order.
    used = []
    for node in doc["nodes"]:
        if "mesh" in node:
            for primitive in doc["meshes"][node["mesh"]]["primitives"]:
                material = primitive.get("material")
                if material not in used:
                    used.append(material)
    images = []
    for mat in used:
        material = doc["materials"][mat] if mat is not None else {}
        ref = material.get("normalTexture")
        if ref:
            uri = doc["images"][doc["textures"][ref["index"]]["source"]]["uri"]
            normal = bake.load_image(asset_dir / uri).copy()
            strength = float(ref.get("scale", 1))
            n = normal[..., :3].astype(float) / 127.5 - 1
            n[..., :2] *= strength
            n /= np.maximum(np.linalg.norm(n, axis=-1, keepdims=True), 1e-8)
            normal[..., :3] = np.rint(np.clip(n * .5 + .5, 0, 1) * 255).astype(np.uint8)
        else:
            normal = np.full((bake.SIZE, bake.SIZE, 4), (128, 128, 255, 255), np.uint8)
        images.append(normal)
    return images


def recolour(image, spec):
    """A model's colour texture moved: saturation, a tint and a gain. The scans
    of some rocks are all brown; the same stone as a grey one is a different rock."""
    if spec.get("leaves_only") and (image[..., 3] >= 250).mean() > 0.8:
        return image          # an opaque texture is bark or stone: left alone
    rgb = image[..., :3].astype(float) / 255.0
    if spec.get("hue"):
        # a rotation of the colour about the grey axis (YIQ): green leaves to
        # amber, to pink, to violet
        a = np.radians(spec["hue"])
        c, s = np.cos(a), np.sin(a)
        to_yiq = np.array([[0.299, 0.587, 0.114], [0.596, -0.274, -0.322], [0.211, -0.523, 0.312]])
        turn = np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
        rgb = np.clip(rgb @ (np.linalg.inv(to_yiq) @ turn @ to_yiq).T, 0.0, 1.0)
    luma = (rgb * np.array([0.2126, 0.7152, 0.0722])).sum(axis=-1, keepdims=True)
    rgb = luma + (rgb - luma) * spec.get("saturation", 1.0)
    rgb = rgb * np.array(spec.get("tint", [1, 1, 1])) * spec.get("gain", 1.0)
    out = image.copy()
    out[..., :3] = np.clip(rgb * 255.0 + 0.5, 0, 255).astype(np.uint8)
    return out


def tuft_cluster(variants, side, spacing):
    """A cushion of real centimetre-sized moss shoots, keeping each shoot's scale."""
    vertices, indices, offset = [], [], 0
    rng = np.random.default_rng(1729)
    for y in range(side):
        for x in range(side):
            _, v, ind, _, _ = variants[(x + y * side) % len(variants)]
            v = v.copy()
            angle = float(rng.uniform(0, math.tau))
            c, s = math.cos(angle), math.sin(angle)
            rotation = np.array([[c, -s], [s, c]])
            v[:, :2] = v[:, :2] @ rotation.T
            v[:, 3:5] = v[:, 3:5] @ rotation.T
            v[:, :2] += np.array([x - (side - 1) * .5, y - (side - 1) * .5]) * spacing + rng.uniform(-.2, .2, 2) * spacing
            vertices.append(v)
            indices.append(ind + offset)
            offset += len(v)
    return "moss-cushion", np.concatenate(vertices), np.concatenate(indices), 0, 0


def prepare(catalogue, binary, clusters):
    config = json.loads(CONFIG.read_text())
    manifest = json.loads((catalogue / "manifest.json").read_text())
    base = config["base_models"]
    if [m["name"] for m in manifest["models"][:len(base)]] != base:
        raise ValueError("Base catalogue order disagrees with environment configuration")
    old_extension = manifest.get("environment")
    if old_extension:
        stop = old_extension["base_layers"]
        for key in ("colours", "normals"):
            manifest[key] = manifest[key][:stop]
        manifest["depth_atlas"]["layers"] = manifest["depth_atlas"]["layers"][:stop]
    elif len(manifest["models"]) != len(base):
        raise ValueError("Refusing to replace an unknown catalogue extension")
    manifest["models"] = manifest["models"][:len(base)]
    if "depth_atlas" not in manifest:
        raise ValueError("Bake the base catalogue first: tools/bake_impostor_depth.py")
    manifest["environment"] = {"version": 1, "base_layers": len(manifest["colours"]),
                               "config": str(CONFIG.relative_to(ROOT))}
    bounds = []
    with tempfile.TemporaryDirectory(prefix="environment-models-") as temporary:
        stage = Path(temporary)

        def layer(name, colour, normal, depth=None):
            index = len(manifest["colours"])
            for key, image in (("colours", colour), ("normals", normal)):
                file = "env-" + name + ("-colour.png" if key == "colours" else "-normal.png")
                Image.fromarray(image).save(stage / file)
                manifest[key].append(file)
            file = "env-" + name + "-depth.png"
            if depth is None:
                depth = np.zeros_like(colour)
            Image.fromarray(depth).save(stage / file)
            manifest["depth_atlas"]["layers"].append(file)
            return index

        for selection in config["models"]:
            name, asset = selection["name"], selection["asset"]
            folder = bake.MODELS / asset
            variants, colours = bake.variants(folder)
            if selection.get("recolour"):
                colours = [recolour(c, selection["recolour"]) for c in colours]
            variant, v, ind, _, _ = variants[selection["variant"]]
            if selection.get("cluster"):
                cluster = selection["cluster"]
                variant, v, ind, _, _ = tuft_cluster(variants, cluster["side"], cluster["spacing_m"])
            original_layers = v[:, 11].astype(int)
            normal_images = normals_for(folder)
            if len(normal_images) != len(colours):
                raise ValueError(asset + ": material layer mapping disagrees")
            angle, pitch = canonical_face(v, ind) if selection.get("canonical_face") else (0, 0)
            front = max(0.0, -float(v[:, 1].min())) if selection.get("canonical_face") else 0.0
            width = float(np.linalg.norm(v[:, :2], axis=1).max() * 2.04)
            height = float(v[:, 2].max() * 1.02)
            if min(width, height) <= 0:
                raise ValueError(name + ": empty model frame")
            # Bake the canonical frame, then remap mesh layers to the shared catalogue.
            jobs, side = bake.jobs_for(width, height)
            views = bake.run_native(binary, v, ind, colours, jobs)
            mapping = {}
            for source in sorted(set(original_layers)):
                mapping[source] = layer(name + "-material-" + str(source), colours[source], normal_images[source])
            v[:, 11] = np.array([mapping[i] for i in original_layers])
            grown, coarse = mesh_lod.build_levels(v, ind, scene.LOD_RATIOS)
            buffer = np.concatenate((v, grown)).astype("<f4") if len(grown) else v
            levels = [{"indices": ind, "triangles": len(ind) // 3, "cards": -1, "error": (0., 0., 0.)}] + coarse
            with (stage / (name + ".mesh")).open("wb") as output:
                output.write(struct.pack("<4sII", b"SCM2", len(buffer), len(levels)))
                output.write(struct.pack("<%dI" % len(levels), *[len(l["indices"]) for l in levels]))
                output.write(buffer.astype("<f4").tobytes())
                for level in levels:
                    output.write(level["indices"].astype("<u4").tobytes())
            first = len(manifest["colours"])
            for view, (colour, normal, depth) in enumerate(views):
                layer(name + "-view-" + str(view), colour, normal, depth)
            model = {"name": name, "source_asset": selection.get("url", "https://polyhaven.com/a/" + asset),
                     "source_variant": variant, "license": selection.get("license", "CC0"), "canonical_yaw": angle,
                     "canonical_pitch": pitch,
                     "mesh": name + ".mesh", "width": width, "height": height,
                     "vegetation": selection["kind"] != "cliff", "kind": selection["kind"],
                     "impostor": first, "depth_impostor": True,
                     "vertices": len(buffer), "triangles": len(ind) // 3,
                     "support_front_m": front,
                     "hemisphere_impostor": {"first": first + 8, "views": hemi.VIEW_COUNT,
                         "layout": hemi.LAYOUT, "side": side, "center_z": height * .5, "resolution": bake.SIZE},
                     "levels": [{"triangles": l["triangles"], "cards": l["cards"],
                         "error_m": l["error"][1], "error_max_m": l["error"][0], "error_rms_m": l["error"][2]}
                                for l in levels]}
            manifest["models"].append(model)
            bounds.append((name, width, height, front))
            bake.sheet(stage, [v[0] for v in views])
            (stage / "sheet.png").rename(stage / (name + "-sheet.png"))
            print(f"{name}: {len(ind)//3} triangles, {width:.2f} x {height:.2f} m, {len(levels)} LODs", flush=True)

        # The final atlas layer is the moss surface; the model shader finds it by
        # array size. No new GPU texture binding or compiled-in layer number.
        stem = ROOT / config["moss_surface"]
        colour = bake.load_image(Path(str(stem) + "_diffuse.png")).copy()
        mask = Path(str(stem) + "_mask.png")
        colour[..., 3] = bake.load_image(mask)[..., 0] if mask.exists() else 255
        normal = bake.load_image(Path(str(stem) + "_normal.png"))
        moss = layer("moss-surface", colour, normal)
        manifest["environment"]["moss_layer"] = moss
        if len(manifest["colours"]) > 1280:
            raise ValueError("Scene material atlas exceeds the 1280-layer budget")
        # Cluster only the additions, preserving the base catalogue's sidecars.
        additions = copy.deepcopy(manifest)
        additions["models"] = additions["models"][len(base):]
        additions["groves"] = []
        (stage / "manifest.json").write_text(json.dumps(additions, indent=2) + "\n")
        subprocess.run(["nice", "-n", "15", str(clusters), str(stage)], check=True)
        header = ["#pragma once", "// Generated by tools/prepare_environment_models.py; physical catalogue bounds.",
                  "#include <array>", "namespace world::decor {",
                  "struct EnvironmentModelBounds { double width, height, front; };",
                  "inline constexpr std::array<EnvironmentModelBounds, %d> kEnvironmentBounds{{" % len(bounds)]
        header += ["    {%.9f, %.9f, %.9f}, // %s" % (w, h, front, name) for name, w, h, front in bounds]
        header += ["}};", "} // namespace world::decor", ""]
        for file in stage.iterdir():
            if file.name != "manifest.json":
                shutil.copy2(file, catalogue / file.name)
        HEADER.write_text("\n".join(header))
        marker = catalogue / "manifest.json.tmp"
        marker.write_text(json.dumps(manifest, indent=2) + "\n")
        marker.replace(catalogue / "manifest.json")
    print(f"Published {len(manifest['models'])} models / {len(manifest['colours'])} layers")
    # The published chains are rebuilt for the solid props (seam-preserving
    # levels, their sidecars retired) and every model is turned to the
    # renderer's winding: tools/rebuild_prop_lods.py.
    subprocess.run([sys.executable, str(Path(__file__).resolve().parent / "rebuild_prop_lods.py"),
                    "--dir", str(catalogue)], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalogue", type=Path, default=bake.CATALOGUE)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--clusters", type=Path)
    args = parser.parse_args()
    binary = bake.find_binary(args.binary).resolve()
    clusters = args.clusters.resolve() if args.clusters else binary.parent / "scene_model_clusters"
    prepare(args.catalogue.resolve(), binary, clusters)
