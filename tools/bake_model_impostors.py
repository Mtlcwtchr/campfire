#!/usr/bin/env python3
"""bake_model_impostors - impostors for the simplified source models, in the
engine's catalogue format, with the native rasterizer.

For every variant (one mesh per plant) of every model in
assets/generated/simplified_models/<asset>/ this writes, under
<asset>/impostors/v<k>/:

    colour-N.png, normal-N.png, z-N.png    N = 0..7 the horizontal ring
                                           (catalogue `impostor` range),
                                           N = 8..28 the 21-view hemisphere
                                           (rings 8-8-4-1, `hemisphere_impostor`)
    impostor.json                          frame: width, height, side, centre_z,
                                           layout, encodings - the manifest fields
    sheet.png                              all 29 colour views, for looking at

exactly as tools/prepare_ue_assets.py and tools/bake_impostor_depth.py would
for a catalogue model: the variant brought to the engine's frame (glTF Y-up to
right-handed Z-up, base at z = 0, centred on its trunk axis), textures at the
catalogue's 256, the shared frame width = 2.04 * max horizontal radius and
height = 1.02 * top, ring views at view * tau / 8 with eye_sign -1, hemisphere
views from hemisphere_impostor.basis about (0, 0, height / 2) in a square of
side hypot(width, height), depth as rg16-view-b-coverage-a-v1.

The rasterizer is tools/impostor_bake.cpp (byte-identical to the Python one on
the ring views; --parity proves it on the shipped catalogue). One process per
model, `nice`d; the whole set is about a minute.

    python3 tools/bake_model_impostors.py               # all simplified models
    python3 tools/bake_model_impostors.py --only fir_tree_01
    python3 tools/bake_model_impostors.py --parity      # native vs Python
"""
import argparse
import json
import math
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hemisphere_impostor as hemi  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MODELS = ROOT / "assets/generated/simplified_models"
CATALOGUE = ROOT / "assets/generated/scene_models"
SIZE = 256                      # prepare_scene_models.SIZE
DEPTH_ENCODING = "rg16-view-b-coverage-a-v1"
COMPONENT = {5121: np.uint8, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}


def find_binary(explicit):
    if explicit:
        return Path(explicit)
    for build in sorted(ROOT.glob("cmake-build-*")) + [ROOT / "build"]:
        if (build / "impostor_bake").exists():
            return build / "impostor_bake"
    sys.exit("impostor_bake not built: cmake --build <build dir> --target impostor_bake")


# ------------------------------------------------------------------ native call

def run_native(binary, vertices, indices, layers, jobs):
    """jobs: dicts with kind, view, views, width, height, frame, angle, eye_sign, origin, basis."""
    with tempfile.TemporaryDirectory() as tmp:
        src, dst = Path(tmp) / "jobs.bin", Path(tmp) / "out.bin"
        v = np.ascontiguousarray(vertices, dtype="<f4")
        ind = np.ascontiguousarray(indices, dtype="<u4")
        with src.open("wb") as f:
            f.write(b"IMPB" + struct.pack("<6I", 1, SIZE, len(v), len(ind), len(layers), len(jobs)))
            f.write(v.tobytes())
            f.write(ind.tobytes())
            for layer in layers:
                f.write(np.ascontiguousarray(layer, dtype=np.uint8).tobytes())
            for j in jobs:
                f.write(struct.pack("<4I", j["kind"], j["view"], j["views"], 0))
                f.write(struct.pack("<5d", j["width"], j["height"], j["frame"], j["angle"], j["eye_sign"]))
                f.write(struct.pack("<3d", *j.get("origin", (0, 0, 0))))
                f.write(struct.pack("<9d", *np.asarray(j.get("basis", np.eye(3)), dtype=float).reshape(-1)))
        done = subprocess.run(["nice", "-n", "15", str(binary), str(src), str(dst)], capture_output=True, text=True)
        if done.returncode != 0:
            raise RuntimeError(done.stderr.strip())
        raw = np.fromfile(dst, dtype=np.uint8)
    per = SIZE * SIZE * 4
    if raw.size != per * 3 * len(jobs):
        raise RuntimeError("impostor_bake wrote %d bytes, expected %d" % (raw.size, per * 3 * len(jobs)))
    images = raw.reshape(len(jobs), 3, SIZE, SIZE, 4)
    return [(images[i, 0], images[i, 1], images[i, 2]) for i in range(len(jobs))]


def jobs_for(width, height):
    side = math.hypot(width, height)
    jobs = [{"kind": 0, "view": view, "views": 8, "width": width, "height": height, "frame": width,
             "angle": view * math.tau / 8, "eye_sign": -1.0} for view in range(8)]
    jobs += [{"kind": 1, "view": view, "views": hemi.VIEW_COUNT, "width": side, "height": side, "frame": side,
              "angle": 0.0, "eye_sign": 1.0, "origin": (0.0, 0.0, height * .5), "basis": hemi.basis(view)}
             for view in range(hemi.VIEW_COUNT)]
    return jobs, side


# ------------------------------------------------------------------ glTF -> engine frame

def accessor(doc, blob, index):
    acc = doc["accessors"][index]
    view = doc["bufferViews"][acc["bufferView"]]
    dtype = np.dtype(COMPONENT[acc["componentType"]])
    n = WIDTH[acc["type"]]
    offset = view.get("byteOffset", 0) + acc.get("byteOffset", 0)
    stride = view.get("byteStride", dtype.itemsize * n)
    return np.ndarray((acc["count"], n), dtype=dtype, buffer=blob, offset=offset, strides=(stride, dtype.itemsize)).copy()


def node_matrix(node):
    if "matrix" in node:
        return np.asarray(node["matrix"], dtype=float).reshape(4, 4).T
    t = np.asarray(node.get("translation", [0, 0, 0]), dtype=float)
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * np.asarray(node.get("scale", [1, 1, 1]), dtype=float)
    m[:3, 3] = t
    return m


def load_image(path):
    with Image.open(path) as image:
        return np.asarray(image.convert("RGBA").resize((SIZE, SIZE), Image.Resampling.LANCZOS))


def variants(asset_dir):
    """Every mesh-carrying node as one variant, in the engine frame."""
    doc = json.loads((asset_dir / (asset_dir.name + ".gltf")).read_text())
    blob = (asset_dir / doc["buffers"][0]["uri"]).read_bytes()
    layers, layer_of = [], {}

    def layer(material_index):
        if material_index in layer_of:
            return layer_of[material_index]
        material = doc["materials"][material_index] if material_index is not None else {}
        pbr = material.get("pbrMetallicRoughness", {})
        ref = pbr.get("baseColorTexture")
        if ref is None:
            image = np.full((SIZE, SIZE, 4), 255, dtype=np.uint8)
        else:
            uri = doc["images"][doc["textures"][ref["index"]]["source"]]["uri"]
            image = load_image(asset_dir / uri).copy()
            if material.get("alphaMode", "OPAQUE") == "OPAQUE":
                image[..., 3] = 255   # the engine has no alpha on opaque surfaces
        factor = np.asarray(pbr.get("baseColorFactor", [1, 1, 1, 1]), dtype=float)
        if not np.allclose(factor[:3], 1):
            image = image.copy()
            image[..., :3] = np.clip(image[..., :3] * factor[:3], 0, 255).astype(np.uint8)
        layer_of[material_index] = len(layers)
        layers.append(image)
        return layer_of[material_index]

    out = []
    for node in doc["nodes"]:
        if "mesh" not in node:
            continue
        m = node_matrix(node)
        verts, inds, count = [], [], 0
        for prim in doc["meshes"][node["mesh"]]["primitives"]:
            a = prim["attributes"]
            p = accessor(doc, blob, a["POSITION"]).astype(float)
            n = accessor(doc, blob, a["NORMAL"]).astype(float)
            uv = accessor(doc, blob, a["TEXCOORD_0"]).astype(float)
            mat = prim.get("material")
            tex = (doc["materials"][mat].get("pbrMetallicRoughness", {}).get("baseColorTexture", {})
                   if mat is not None else {})
            xf = tex.get("extensions", {}).get("KHR_texture_transform")
            if xf:
                if xf.get("rotation"):
                    c, s = math.cos(xf["rotation"]), math.sin(xf["rotation"])
                    uv = uv @ np.array([[c, -s], [s, c]])
                uv = uv * np.asarray(xf.get("scale", [1, 1])) + np.asarray(xf.get("offset", [0, 0]))
            p = p @ m[:3, :3].T + m[:3, 3]
            n = n @ np.linalg.inv(m[:3, :3])
            n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-12)
            # glTF Y-up -> right-handed Z-up, as prepare_scene_models does.
            p = p[:, [0, 2, 1]] * np.array([1, -1, 1])
            n = n[:, [0, 2, 1]] * np.array([1, -1, 1])
            verts.append(np.column_stack((p, n, uv, np.ones((len(p), 3)), np.full(len(p), layer(mat)))))
            inds.append(accessor(doc, blob, prim["indices"]).reshape(-1).astype(np.uint32) + count)
            count += len(p)
        v = np.concatenate(verts).astype("<f4")
        ind = np.concatenate(inds).astype("<u4")
        # Base at z = 0, trunk axis at the origin: the centroid of the lowest
        # tenth of the plant, not the bounding box (a leaning tree's box centre
        # is in the air beside it).
        v[:, 2] -= v[:, 2].min()
        low = v[:, 2] <= max(v[:, 2].max() * 0.1, 1e-4)
        v[:, :2] -= v[low, :2].mean(axis=0)
        width = float(np.linalg.norm(v[:, :2], axis=1).max() * 2.04)
        height = float(v[:, 2].max() * 1.02)
        out.append((doc["meshes"][node["mesh"]].get("name", str(node["mesh"])), v, ind, width, height))
    return out, layers


# ------------------------------------------------------------------ bake

def sheet(into, colours, cell=128):
    """sheet.png: the ring (top row) and the hemisphere rings 8 / 8 / 4+1 over
    a mid grey, for looking at, not for the engine."""
    rows = [list(range(0, 8)), list(range(8, 16)), list(range(16, 24)), list(range(24, 29))]
    canvas = Image.new("RGB", (cell * 8, cell * len(rows)), (96, 100, 104))
    for r, views in enumerate(rows):
        for c, i in enumerate(views):
            tile = Image.fromarray(colours[i]).resize((cell, cell), Image.Resampling.LANCZOS)
            canvas.paste(tile, (c * cell, r * cell), tile)
    canvas.save(into / "sheet.png")


def bake_asset(binary, asset_dir):
    started = time.time()
    found, layers = variants(asset_dir)
    target = asset_dir / "impostors"
    rows = []
    for k, (name, v, ind, width, height) in enumerate(found):
        if min(width, height) <= 0:
            raise ValueError("%s/%s has no extent" % (asset_dir.name, name))
        jobs, side = jobs_for(width, height)
        images = run_native(binary, v, ind, layers, jobs)
        into = target / ("v%d" % k)
        into.mkdir(parents=True, exist_ok=True)
        coverage = []
        for i, (colour, normal, depth) in enumerate(images):
            Image.fromarray(colour).save(into / ("colour-%d.png" % i))
            Image.fromarray(normal).save(into / ("normal-%d.png" % i))
            Image.fromarray(depth).save(into / ("z-%d.png" % i))
            coverage.append(float((colour[..., 3] > 0).mean()))
        meta = {"variant": name, "width": width, "height": height, "triangles": int(len(ind) // 3),
                "impostor": {"first": 0, "views": 8},
                "hemisphere_impostor": {"first": 8, "views": hemi.VIEW_COUNT, "layout": hemi.LAYOUT,
                                        "side": side, "center_z": height * .5, "resolution": SIZE},
                "depth_encoding": DEPTH_ENCODING, "coverage": [round(c, 4) for c in coverage]}
        (into / "impostor.json").write_text(json.dumps(meta, indent=1) + "\n")
        sheet(into, [c for c, _, _ in images])
        rows.append(meta)
    return rows, time.time() - started


def parity(binary):
    """Native vs prepare_scene_models.rasterize on the shipped catalogue."""
    import prepare_scene_models as scene
    manifest = json.loads((CATALOGUE / "manifest.json").read_text())
    images = []
    for name in manifest["colours"]:
        with Image.open(CATALOGUE / name) as image:
            images.append(np.asarray(image.convert("RGBA").resize((SIZE, SIZE))))
    worst = 0
    # A leafy tree, a bush and a rock: alpha-cut cards, mixed, opaque. The
    # Python side is one interpreter step per triangle, so a sample, not all.
    for model in [m for m in manifest["models"] if m["name"] in ("CommonTree_1", "Bush_Common", "Rock_Medium_1")]:
        data = (CATALOGUE / model["mesh"]).read_bytes()
        _, nv, levels = struct.unpack_from("<4sII", data)
        counts = struct.unpack_from("<%dI" % levels, data, 12)
        off = 12 + 4 * levels
        v = np.frombuffer(data, dtype="<f4", count=nv * 12, offset=off).reshape(-1, 12)
        ind = np.frombuffer(data, dtype="<u4", count=counts[0], offset=off + nv * 48)
        width, height = float(model["width"]), float(model["height"])
        jobs, side = jobs_for(width, height)
        picked = [0, 3, 8 + 9, 8 + 20]          # two ring views, a 45-degree view and the top
        native = run_native(binary, v, ind, images, [jobs[i] for i in picked])
        for (colour_n, normal_n, depth_n), i in zip(native, picked):
            j = jobs[i]
            if j["kind"] == 0:
                colour, normal, depth = scene.rasterize(v, ind, images, width, height, j["angle"],
                                                        return_depth=True, eye_sign=-1)
            else:
                colour, normal, depth = scene.rasterize(v, ind, images, side, side, 0, return_depth=True,
                                                        view_basis=j["basis"], origin=j["origin"])
            colour, normal = np.asarray(colour), np.asarray(normal)
            covered = (colour[..., 3] > 0) | (colour_n[..., 3] > 0)
            diff_c = int(np.abs(colour.astype(int) - colour_n).max())
            diff_n = int(np.abs(normal.astype(int) - normal_n).max())
            mism = float((np.any(colour != colour_n, axis=-1) & covered).sum() / max(covered.sum(), 1))
            valid = (colour[..., 3] > 0) & np.isfinite(depth)
            code_py = np.rint(np.clip(np.where(valid, depth / j["frame"] + .5, .5), 0, 1) * 65535).astype(int)
            code_n = depth_n[..., 0].astype(int) * 256 + depth_n[..., 1]
            diff_z = int(np.abs(code_py - code_n)[valid].max()) if valid.any() else 0
            worst = max(worst, mism)
            print("  %-16s %s view %2d  pixels differing %.4f%%  max|colour| %3d  max|normal| %3d  max|depth code| %d" % (
                model["name"], "ring" if j["kind"] == 0 else "hemi", j["view"], 100 * mism, diff_c, diff_n, diff_z))
    print("worst share of differing covered pixels: %.4f%%" % (100 * worst))
    return worst


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", nargs="*", metavar="ASSET")
    ap.add_argument("--binary")
    ap.add_argument("--parity", action="store_true", help="compare with the Python rasterizer and stop")
    args = ap.parse_args()
    binary = find_binary(args.binary)
    if args.parity:
        return 0 if parity(binary) < 0.001 else 1
    report_path = MODELS / "report.json"
    report = json.loads(report_path.read_text())
    assets = [r["asset_id"] for r in report["models"]]
    if args.only:
        assets = [a for a in assets if a in set(args.only)]
    total_views = 0
    started = time.time()
    for row in report["models"]:
        if row["asset_id"] not in assets:
            continue
        rows, seconds = bake_asset(binary, MODELS / row["asset_id"])
        total_views += len(rows) * (8 + hemi.VIEW_COUNT)
        row["impostors"] = {"variants": len(rows), "views": 8 + hemi.VIEW_COUNT, "resolution": SIZE,
                            "layout": hemi.LAYOUT, "seconds": round(seconds, 2),
                            "min_coverage": round(min(min(r["coverage"]) for r in rows), 4)}
        print("  %-24s %2d variants x 29 views  %.1fs" % (row["asset_id"], len(rows), seconds), flush=True)
    report_path.write_text(json.dumps(report, indent=1) + "\n")
    print("baked %d views in %.0fs" % (total_views, time.time() - started))
    return 0


if __name__ == "__main__":
    sys.exit(main())

