#!/usr/bin/env python3
"""The engine half of the character import: a skinned .glb in, the engine's files out.

    python3 tools/pack_character.py assets/generated/characters/knight/knight.glb \
        assets/generated/characters/knight

Writes, beside each other:
  character.bin   - vertices, indices (every level of detail), skeleton
  character.json  - materials and their texture layers, levels, bounds, facing
  albedo_N.png / normal_N.png / surface_N.png - one layer of each per material
                    (surface: R roughness, G metallic, B unused, A 255)

character.bin, little endian:
  "CHR1", u32 vertices, u32 indices, u32 joints, u32 materials, u32 levels
  levels x materials x (u32 first index, u32 count)        - index ranges
  vertices x { f32 position[3], normal[3], tangent[4], uv[2], u8 joints[4], u8 weights[4] }
  indices  x u32
  joints   x { i32 parent, f32 rest_local[16], f32 inverse_bind[16], char name[32] }
Matrices are row-major, column vectors (p' = M p), z up, metres.

Levels: 0 is the source; each further one is meshoptimizer's quadric collapse
over the skinned rest pose, per material, borders locked so the parts of the
armour stay closed. The vertex buffer is shared - only indices change - so a
level costs no more skinning than the vertices it actually references.
"""
import io
import json
import struct
import sys
from pathlib import Path

import meshoptimizer as mo
import numpy as np
from PIL import Image

LEVELS = [1.0, 0.45, 0.18, 0.06]   # of each material's triangles
SIDE = 1024

COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def read_glb(path):
    data = Path(path).read_bytes()
    assert data[:4] == b"glTF", "not a glb"
    length = struct.unpack_from("<I", data, 12)[0]
    doc = json.loads(data[20:20 + length])
    at = 20 + length
    blob_length = struct.unpack_from("<I", data, at)[0]
    blob = data[at + 8:at + 8 + blob_length]
    return doc, blob


def accessor(doc, blob, index):
    a = doc["accessors"][index]
    view = doc["bufferViews"][a["bufferView"]]
    kind = COMPONENT[a["componentType"]]
    width = WIDTH[a["type"]]
    start = view.get("byteOffset", 0) + a.get("byteOffset", 0)
    stride = view.get("byteStride", 0)
    item = np.dtype(kind).itemsize * width
    count = a["count"]
    if stride and stride != item:
        raw = np.frombuffer(blob, np.uint8, count * stride, start).reshape(count, stride)[:, :item]
        out = np.frombuffer(raw.tobytes(), kind).reshape(count, width)
    else:
        out = np.frombuffer(blob, kind, count * width, start).reshape(count, width)
    if a.get("normalized") and kind != np.float32:
        out = out.astype(np.float32) / float(np.iinfo(kind).max)
    return out


def node_local(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=np.float64).reshape(4, 4).T   # glTF is column-major
    t = np.array(node.get("translation", [0, 0, 0]), dtype=np.float64)
    x, y, z, w = node.get("rotation", [0, 0, 0, 1])
    s = np.array(node.get("scale", [1, 1, 1]), dtype=np.float64)
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                  [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                  [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * s
    m[:3, 3] = t
    return m


def image_of(doc, blob, texture):
    if texture is None:
        return None
    source = doc["textures"][texture["index"]]["source"]
    view = doc["bufferViews"][doc["images"][source]["bufferView"]]
    start = view.get("byteOffset", 0)
    return Image.open(io.BytesIO(blob[start:start + view["byteLength"]]))


def main(source, out_dir):
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    doc, blob = read_glb(source)
    nodes = doc["nodes"]
    parent_of = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent_of[c] = i

    def world(i):
        m = node_local(nodes[i])
        while i in parent_of:
            i = parent_of[i]
            m = node_local(nodes[i]) @ m
        return m

    assert len(doc.get("skins", [])) >= 1, "no skin"
    skin = doc["skins"][0]
    joint_nodes = skin["joints"]
    joint_index = {n: k for k, n in enumerate(joint_nodes)}
    inverse_bind = accessor(doc, blob, skin["inverseBindMatrices"]).astype(np.float64).reshape(-1, 4, 4).transpose(0, 2, 1)
    # Rest local transforms relative to the parent *joint*; anything between
    # (the armature object) folds into the child, so the skeleton is closed.
    parents, rest_local = [], []
    for k, n in enumerate(joint_nodes):
        p = parent_of.get(n)
        while p is not None and p not in joint_index:
            p = parent_of.get(p)
        parents.append(joint_index[p] if p is not None else -1)
        rest_local.append(world(n) if p is None else np.linalg.inv(world(p)) @ world(n))
    rest_world = [None] * len(joint_nodes)
    for k in range(len(joint_nodes)):   # glTF lists parents before children
        rest_world[k] = rest_local[k] if parents[k] < 0 else rest_world[parents[k]] @ rest_local[k]
    assert len(joint_nodes) <= 255

    # Every skinned primitive into one vertex buffer.
    vertices, rest_positions, per_material = [], [], {}
    base = 0
    for i, n in enumerate(nodes):
        if "mesh" not in n or n.get("skin") is None:
            continue
        for prim in doc["meshes"][n["mesh"]]["primitives"]:
            at = prim["attributes"]
            p = accessor(doc, blob, at["POSITION"]).astype(np.float32)
            count = len(p)
            normal = accessor(doc, blob, at["NORMAL"]).astype(np.float32) if "NORMAL" in at else np.tile([0, 0, 1], (count, 1)).astype(np.float32)
            tangent = accessor(doc, blob, at["TANGENT"]).astype(np.float32) if "TANGENT" in at else np.tile([1, 0, 0, 1], (count, 1)).astype(np.float32)
            uv = accessor(doc, blob, at["TEXCOORD_0"]).astype(np.float32) if "TEXCOORD_0" in at else np.zeros((count, 2), np.float32)
            # Up to two sets of four influences; the four strongest are kept.
            sets_j, sets_w = [], []
            for s in range(4):
                if f"JOINTS_{s}" in at:
                    sets_j.append(accessor(doc, blob, at[f"JOINTS_{s}"]).astype(np.int64))
                    sets_w.append(accessor(doc, blob, at[f"WEIGHTS_{s}"]).astype(np.float32))
            joints = np.concatenate(sets_j, axis=1)
            weights = np.concatenate(sets_w, axis=1)
            order = np.argsort(-weights, axis=1)[:, :4]
            joints = np.take_along_axis(joints, order, 1)
            weights = np.take_along_axis(weights, order, 1)
            total = weights.sum(axis=1, keepdims=True)
            weights = np.where(total > 0, weights / np.maximum(total, 1e-8), np.array([[1, 0, 0, 0]], np.float32))
            joints = np.where(weights > 0, joints, 0)
            # Weights in bytes that still sum to exactly 255.
            wb = np.floor(weights * 255 + 0.5).astype(np.int64)
            wb[:, 0] += 255 - wb.sum(axis=1)
            # The skinned rest pose, for bounds and the simplifier.
            skinning = np.einsum("jab,jbc->jac", np.array(rest_world), inverse_bind)
            homo = np.concatenate([p.astype(np.float64), np.ones((count, 1))], axis=1)
            rest = np.zeros((count, 3))
            for c in range(4):
                rest += weights[:, c:c + 1] * np.einsum("vab,vb->va", skinning[joints[:, c]], homo)[:, :3]
            rest_positions.append(rest.astype(np.float32))
            vertices.append((p, normal, tangent, uv, joints.astype(np.uint8), np.clip(wb, 0, 255).astype(np.uint8)))
            idx = accessor(doc, blob, prim["indices"]).astype(np.uint32).reshape(-1) + base
            per_material.setdefault(prim.get("material", 0), []).append(idx)
            base += count
    rest_all = np.concatenate(rest_positions)
    vertex_count = len(rest_all)

    # Levels: per material, so a material's range stays one draw.
    materials = sorted(per_material)
    ranges, indices = [], []
    cursor = 0
    level_triangles = []
    for level, share in enumerate(LEVELS):
        tris = 0
        row = []
        for m in materials:
            src = np.concatenate(per_material[m]).astype(np.uint32)
            if level == 0:
                got = src
            else:
                dst = np.zeros_like(src)
                target = max(3, int(len(src) * share) // 3 * 3)
                n = mo.simplify(dst, src, rest_all, target_index_count=target, target_error=0.02,
                                options=mo.SIMPLIFY_LOCK_BORDER)
                got = dst[:n]
                if n < 3:
                    got = src[:0]
            got = np.ascontiguousarray(got, dtype=np.uint32)
            row.append((cursor, len(got)))
            indices.append(got)
            cursor += len(got)
            tris += len(got) // 3
        ranges.append(row)
        level_triangles.append(tris)

    # Facing: from the heel to the toe of the rest skeleton, on the ground.
    names = [nodes[n].get("name", f"joint{k}") for k, n in enumerate(joint_nodes)]
    def joint_pos(name):
        return rest_world[names.index(name)][:3, 3] if name in names else None
    forward = [0.0, -1.0]
    for heel, toe in (("foot_l", "ball_l"), ("foot_r", "ball_r")):
        a, b = joint_pos(heel), joint_pos(toe)
        if a is not None and b is not None:
            d = (b - a)[:2]
            if np.linalg.norm(d) > 1e-4:
                forward = list(d / np.linalg.norm(d))
                break

    # Textures: albedo (alpha = coverage), normal, surface (R roughness, G metallic).
    material_out = []
    for layer, m in enumerate(materials):
        mat = doc["materials"][m] if m < len(doc.get("materials", [])) else {}
        pbr = mat.get("pbrMetallicRoughness", {})
        colour = image_of(doc, blob, pbr.get("baseColorTexture"))
        normal = image_of(doc, blob, mat.get("normalTexture"))
        surface = image_of(doc, blob, pbr.get("metallicRoughnessTexture"))
        factor = pbr.get("baseColorFactor", [1, 1, 1, 1])
        a = (colour.convert("RGBA") if colour else Image.new("RGBA", (4, 4), tuple(int(c * 255) for c in factor))).resize((SIDE, SIDE), Image.LANCZOS)
        a.save(out / f"albedo_{layer}.png")
        nm = (normal.convert("RGB") if normal else Image.new("RGB", (4, 4), (128, 128, 255))).resize((SIDE, SIDE), Image.LANCZOS)
        nm.convert("RGBA").save(out / f"normal_{layer}.png")
        if surface:
            s = np.asarray(surface.convert("RGB").resize((SIDE, SIDE), Image.LANCZOS))
            rough, metal = s[:, :, 1], s[:, :, 2]   # glTF: G roughness, B metallic
        else:
            rough = np.full((SIDE, SIDE), int(255 * pbr.get("roughnessFactor", 1.0)), np.uint8)
            metal = np.full((SIDE, SIDE), int(255 * pbr.get("metallicFactor", 0.0)), np.uint8)
        packed = np.stack([rough, metal, np.zeros_like(rough), np.full_like(rough, 255)], axis=2).astype(np.uint8)
        Image.fromarray(packed, "RGBA").save(out / f"surface_{layer}.png")
        material_out.append({"name": mat.get("name", f"material{m}"),
                             "alpha": mat.get("alphaMode", "OPAQUE"),
                             "cutoff": mat.get("alphaCutoff", 0.5),
                             "double_sided": mat.get("doubleSided", False),
                             "albedo": f"albedo_{layer}.png", "normal": f"normal_{layer}.png",
                             "surface": f"surface_{layer}.png"})

    with open(out / "character.bin", "wb") as f:
        all_indices = np.concatenate(indices).astype(np.uint32)
        f.write(b"CHR1")
        f.write(struct.pack("<5I", vertex_count, len(all_indices), len(joint_nodes), len(materials), len(LEVELS)))
        for row in ranges:
            for first, count in row:
                f.write(struct.pack("<2I", first, count))
        record = np.dtype([("p", "<f4", 3), ("n", "<f4", 3), ("t", "<f4", 4), ("uv", "<f4", 2),
                           ("j", "u1", 4), ("w", "u1", 4)])
        packed = np.zeros(vertex_count, record)
        at = 0
        for p, n, t, uv, j, w in vertices:
            k = len(p)
            packed["p"][at:at + k] = p; packed["n"][at:at + k] = n; packed["t"][at:at + k] = t
            packed["uv"][at:at + k] = uv; packed["j"][at:at + k] = j; packed["w"][at:at + k] = w
            at += k
        f.write(packed.tobytes())
        f.write(all_indices.tobytes())
        for k in range(len(joint_nodes)):
            f.write(struct.pack("<i", parents[k]))
            f.write(rest_local[k].astype("<f4").tobytes())     # row-major
            f.write(inverse_bind[k].astype("<f4").tobytes())
            f.write(names[k].encode()[:31].ljust(32, b"\0"))

    low, high = rest_all.min(axis=0), rest_all.max(axis=0)
    report = {"version": 1, "vertices": vertex_count, "joints": names, "materials": material_out,
              "levels": [{"triangles": t, "share": s} for t, s in zip(level_triangles, LEVELS)],
              "bounds": {"min": low.tolist(), "max": high.tolist()}, "forward": forward,
              "source": str(source)}
    (out / "character.json").write_text(json.dumps(report, indent=1))
    print(json.dumps({k: report[k] for k in ("vertices", "levels", "bounds", "forward")}))
    print("joints", len(names), "materials", [m["name"] + ":" + m["alpha"] for m in material_out])


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
