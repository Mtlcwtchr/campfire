#!/usr/bin/env python3
"""prepare_ue_foliage - the raw UE foliage export (tools/export_ue_foliage.py)
in the source layout tools/fetch_models.py writes for Poly Haven, so that
tools/simplify_models.py and tools/bake_model_impostors.py take it unchanged.

For each asset in content/config/ue_foliage_sources.json:

    assets/models/ue_foliage/<asset>/raw/...      (from the UE commandlet)
 -> assets/models/ue_foliage/<asset>/<asset>.gltf + .bin
    assets/models/ue_foliage/<asset>/textures/*_rgba.png
    assets/models/ue_foliage/<asset>/source.json

One node and one mesh per exported UE mesh (a variant), one primitive per UE
material slot; POSITION / NORMAL / TEXCOORD_0 only. Materials are rebuilt from
the exported parameters, not from the glTF exporter (which ran without
baking): the base colour is an RGBA PNG -

- Light Foliage: colour from the material's mask and tints, decoded by
  prepare_ue_assets.light_foliage_colour (the same code the catalogue bush
  uses), alpha from the mask;
- everything else: the albedo texture, alpha from an opacity texture if the
  material binds one, else from the albedo's own alpha (Megascans B-O).

Masked or translucent UE materials become glTF MASK at 0.5; opaque ones get
alpha 255.

    python3 tools/prepare_ue_foliage.py [--only lf_bush ms_wild_grass]
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "content/config/ue_foliage_sources.json"
OUT = ROOT / "assets/models/ue_foliage"
MAX_EDGE = 2048
LICENSE = "Original UE asset-pack licence (AncientSettlement project); not CC0, not redistributable"

OPACITY = re.compile(r"(opacity|alpha|_o$)", re.IGNORECASE)
COMPONENT = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def accessor(doc, blob, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    dtype = np.dtype(COMPONENT[acc["componentType"]])
    n = WIDTH[acc["type"]]
    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", dtype.itemsize * n)
    return np.ndarray((acc["count"], n), dtype=dtype, buffer=blob, offset=offset,
                      strides=(stride, dtype.itemsize)).copy()


def load_png(path):
    if path.suffix.lower() == ".exr":
        os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
        import cv2
        pixels = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if pixels is None:
            raise ValueError("Cannot read HDR texture: " + str(path))
        pixels = pixels[..., [2, 1, 0]]
        image = Image.fromarray(np.rint(np.clip(pixels, 0, 1) * 255).astype(np.uint8))
        if max(image.size) > MAX_EDGE:
            ratio = MAX_EDGE / max(image.size)
            image = image.resize((round(image.width * ratio), round(image.height * ratio)), Image.Resampling.LANCZOS)
        return image
    with Image.open(path) as image:
        image.load()
        if max(image.size) > MAX_EDGE:
            scale = MAX_EDGE / max(image.size)
            image = image.resize((max(1, round(image.width * scale)), max(1, round(image.height * scale))),
                                 Image.Resampling.LANCZOS)
        return image.copy()


def masked(material):
    mode = str(material.get("blend_mode", "")).upper()
    files = material.get("files", {})
    # 3D Garden Plants leave the blend mode opaque and cut with a dithered
    # opacity texture; a bound opacity map is what makes a leaf a cut-out.
    has_opacity = any(OPACITY.search(k) or OPACITY.search(Path(v["file"]).stem) for k, v in files.items())
    return "MASKED" in mode or "TRANSLUCENT" in mode or has_opacity


def is_impostor(material):
    return material is not None and ("Impostor" in material["name"] or
                                     any("Impostor" in p for p in material.get("parents", [])))


def colour_texture(material, raw):
    """RGBA PIL image for a UE material, or None if it binds no colour."""
    files = material.get("files", {})
    if not files:
        return None
    if "graph" in material:
        from prepare_ue_assets import light_foliage_colour
        mask = next((v for k, v in files.items() if k.lower() == "mask"), None)
        if mask is None:
            return None
        return light_foliage_colour(material, load_png(raw / mask["file"]))
    if "B-O" in files:
        # Megascans 3D plant atlas: colour, opacity in alpha.
        return load_png(raw / files["B-O"]["file"]).convert("RGBA")
    if any("EuropeanHornbeam" in p or p.endswith("MA_Foliage.MA_Foliage") for p in material.get("parents", [])):
        # MA_Foliage (European Hornbeam): Albedo + Mask, opacity in Mask.R.
        albedo, mask = files.get("Albedo"), files.get("Mask")
        if albedo is None:
            return None
        rgba = load_png(raw / albedo["file"]).convert("RGBA")
        if mask is not None and masked(material):
            rgba.putalpha(load_png(raw / mask["file"]).convert("RGBA").getchannel("R").resize(rgba.size))
        else:
            rgba.putalpha(255)
        return rgba
    opacity = [v for k, v in files.items() if OPACITY.search(k) or OPACITY.search(Path(v["file"]).stem)]
    albedo = [v for k, v in files.items() if v not in opacity and not v.get("normal_map")]
    if not albedo:
        return None
    # Prefer the texture whose name says it is a base colour.
    albedo.sort(key=lambda v: 0 if re.search(r"(albedo|basecolou?r|diffuse|_b-o|_b$)", Path(v["file"]).stem,
                                             re.IGNORECASE) else 1)
    colour = load_png(raw / albedo[0]["file"])
    rgba = colour.convert("RGBA")
    if opacity:
        alpha = load_png(raw / opacity[0]["file"]).convert("L").resize(rgba.size, Image.Resampling.BILINEAR)
        rgba.putalpha(alpha)
    elif colour.mode not in ("RGBA", "LA"):
        rgba.putalpha(255)
    if not masked(material):
        rgba.putalpha(255)
    return rgba


def prepare(entry):
    asset = entry["asset_id"]
    raw = OUT / asset / "raw"
    export = json.loads((raw / "export.json").read_text())
    if not export["meshes"]:
        raise ValueError("%s: nothing exported (%s)" % (asset, "; ".join(export["errors"])[:200]))
    into = OUT / asset
    (into / "textures").mkdir(parents=True, exist_ok=True)
    bin_data = bytearray()
    views, accessors, meshes, nodes, materials, images, textures = [], [], [], [], [], [], []
    material_index = {}

    def add_view(array, target):
        while len(bin_data) % 4:
            bin_data.append(0)
        offset = len(bin_data)
        raw_bytes = np.ascontiguousarray(array).tobytes()
        bin_data.extend(raw_bytes)
        views.append({"buffer": 0, "byteOffset": offset, "byteLength": len(raw_bytes), "target": target})
        return len(views) - 1

    def add_accessor(array, component, kind, minmax=False):
        a = {"bufferView": add_view(array, 34963 if kind == "SCALAR" else 34962), "componentType": component,
             "count": int(len(array)), "type": kind}
        if minmax:
            a["min"] = [float(v) for v in array.min(axis=0)]
            a["max"] = [float(v) for v in array.max(axis=0)]
        accessors.append(a)
        return len(accessors) - 1

    def material_for(material):
        key = material["asset"] if material else None
        if key in material_index:
            return material_index[key]
        out = {"name": material["name"] if material else "none",
               "pbrMetallicRoughness": {"metallicFactor": 0.0, "roughnessFactor": 0.9}, "doubleSided": True}
        rgba = colour_texture(material, raw) if material else None
        stem = (re.sub(r"[^A-Za-z0-9_]+", "_", material["name"]) + "_" +
                hashlib.sha256(key.encode()).hexdigest()[:8]) if material else "none"
        if rgba is not None:
            name = "textures/%s_rgba.png" % stem
            rgba.save(into / name, compress_level=6)
            images.append({"uri": name, "mimeType": "image/png"})
            textures.append({"source": len(images) - 1})
            out["pbrMetallicRoughness"]["baseColorTexture"] = {"index": len(textures) - 1}
            if masked(material):
                out["alphaMode"] = "MASK"
                out["alphaCutoff"] = 0.5
        normals = [v for v in material.get("files", {}).values() if v.get("normal_map")] if material else []
        if normals:
            pixels = np.array(load_png(raw / normals[0]["file"]).convert("RGB"))
            pixels[..., 1] = 255 - pixels[..., 1]  # Unreal DirectX -> glTF OpenGL
            name = "textures/%s_normal.png" % stem
            Image.fromarray(pixels).save(into / name)
            images.append({"uri": name, "mimeType": "image/png"})
            textures.append({"source": len(images) - 1})
            out["normalTexture"] = {"index": len(textures) - 1}
        materials.append(out)
        material_index[key] = len(materials) - 1
        return material_index[key]

    variants = []
    for row in export["meshes"]:
        doc = json.loads((raw / row["gltf"]).read_text())
        blob = (raw / doc["buffers"][0]["uri"]).read_bytes()
        prims = doc["meshes"][0]["primitives"]
        out_prims, tris = [], 0
        for p in prims:
            slot = p.get("material")
            material = row["materials"][slot] if slot is not None and slot < len(row["materials"]) else None
            if is_impostor(material):
                continue   # UE's own far-distance card; we bake our own
            a = p["attributes"]
            pos = accessor(doc, blob, a["POSITION"]).astype("<f4")
            nrm = accessor(doc, blob, a["NORMAL"]).astype("<f4") if "NORMAL" in a else np.tile([0, 1, 0], (len(pos), 1)).astype("<f4")
            uv = accessor(doc, blob, a["TEXCOORD_0"]).astype("<f4") if "TEXCOORD_0" in a else np.zeros((len(pos), 2), "<f4")
            ind = accessor(doc, blob, p["indices"]).reshape(-1).astype("<u4")
            tris += len(ind) // 3
            out_prims.append({"attributes": {"POSITION": add_accessor(pos, 5126, "VEC3", True),
                                             "NORMAL": add_accessor(nrm, 5126, "VEC3"),
                                             "TEXCOORD_0": add_accessor(uv, 5126, "VEC2")},
                              "indices": add_accessor(ind, 5125, "SCALAR"), "mode": 4,
                              "material": material_for(material)})
        name = row["mesh"].split("/")[-1]
        meshes.append({"name": name, "primitives": out_prims})
        nodes.append({"name": name, "mesh": len(meshes) - 1})
        variants.append({"mesh": row["mesh"], "triangles": tris})

    gltf = {"asset": {"version": "2.0", "generator": "asr prepare_ue_foliage"},
            "scene": 0, "scenes": [{"nodes": list(range(len(nodes)))}], "nodes": nodes, "meshes": meshes,
            "materials": materials, "images": images, "textures": textures,
            "accessors": accessors, "bufferViews": views,
            "buffers": [{"uri": asset + ".bin", "byteLength": len(bin_data)}]}
    (into / (asset + ".bin")).write_bytes(bytes(bin_data))
    (into / (asset + ".gltf")).write_text(json.dumps(gltf, indent=1))
    (into / "source.json").write_text(json.dumps({
        "site": "unreal", "asset_id": asset, "asset_url": entry.get("source_url"), "name": asset,
        "license": LICENSE, "authors": None, "role": entry["role"], "biomes": entry.get("biomes"),
        "heavy": False, "gltf": asset + ".gltf", "variants": variants,
        "packages": [v["mesh"] for v in variants], "export_errors": export["errors"]}, indent=2) + "\n")
    return len(variants), sum(v["triangles"] for v in variants), len(images)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", nargs="*")
    args = ap.parse_args()
    entries = json.loads(CONFIG.read_text())["assets"]
    if args.only:
        entries = [e for e in entries if e["asset_id"] in set(args.only)]
    failed = 0
    for entry in entries:
        if not (OUT / entry["asset_id"] / "raw" / "export.json").exists():
            print("  %-22s not exported yet" % entry["asset_id"])
            failed += 1
            continue
        try:
            n, tris, textures = prepare(entry)
            print("  %-22s %-14s %2d variants  %8d tris  %d textures" % (
                entry["asset_id"], entry["role"], n, tris, textures))
        except Exception as error:
            print("  %-22s FAILED %s" % (entry["asset_id"], error))
            failed += 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
