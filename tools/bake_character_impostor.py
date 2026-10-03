#!/usr/bin/env python3
"""Eight-view impostor of a packed character (tools/pack_character.py), in its rest pose.

    python3 tools/bake_character_impostor.py assets/generated/characters/knight

Writes impostor_colour_N.png and impostor_normal_N.png (N = 0..7, view N at
N * tau / 8 about the vertical, the ring layout of the scene catalogue) and an
"impostor" block in character.json: width, height, the frame's centre on the
ground. The character is drawn as this card beyond the distance where even its
coarsest level is a few pixels tall (CharacterPass). Rasterised with the scene
catalogue's own rasterizer (tools/prepare_scene_models.py) from level 2.
"""
import json
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))
import prepare_scene_models as psm   # noqa: E402

LEVEL = 2


def load(directory):
    meta = json.loads((directory / "character.json").read_text())
    data = (directory / "character.bin").read_bytes()
    assert data[:4] == b"CHR1"
    vertices, indices, joints, materials, levels = struct.unpack_from("<5I", data, 4)
    at = 24
    ranges = np.frombuffer(data, "<u4", levels * materials * 2, at).reshape(levels, materials, 2)
    at += ranges.nbytes
    record = np.dtype([("p", "<f4", 3), ("n", "<f4", 3), ("t", "<f4", 4), ("uv", "<f4", 2), ("j", "u1", 4), ("w", "u1", 4)])
    verts = np.frombuffer(data, record, vertices, at)
    at += verts.nbytes
    index = np.frombuffer(data, "<u4", indices, at)
    at += index.nbytes
    parents, local, inverse = [], [], []
    for _ in range(joints):
        parents.append(struct.unpack_from("<i", data, at)[0]); at += 4
        local.append(np.frombuffer(data, "<f4", 16, at).reshape(4, 4).astype(np.float64)); at += 64
        inverse.append(np.frombuffer(data, "<f4", 16, at).reshape(4, 4).astype(np.float64)); at += 64
        at += 32
    world = [None] * joints
    for k in range(joints):
        world[k] = local[k] if parents[k] < 0 else world[parents[k]] @ local[k]
    skin = np.einsum("jab,jbc->jac", np.array(world), np.array(inverse))
    w = verts["w"].astype(np.float64) / 255.0
    j = verts["j"].astype(np.int64)
    p = np.concatenate([verts["p"].astype(np.float64), np.ones((vertices, 1))], axis=1)
    n = verts["n"].astype(np.float64)
    pos = np.zeros((vertices, 3)); nor = np.zeros((vertices, 3))
    for c in range(4):
        m = skin[j[:, c]]
        pos += w[:, c:c + 1] * np.einsum("vab,vb->va", m, p)[:, :3]
        nor += w[:, c:c + 1] * np.einsum("vab,vb->va", m[:, :3, :3], n)
    nor /= np.maximum(np.linalg.norm(nor, axis=1, keepdims=True), 1e-8)
    return meta, verts, index, ranges, pos, nor


def main(directory):
    directory = Path(directory)
    meta, verts, index, ranges, pos, nor = load(directory)
    # The frame: centred on the feet, base on the ground.
    low = pos.min(axis=0)
    centre = (pos[:, :2].min(axis=0) + pos[:, :2].max(axis=0)) * 0.5
    local = pos - np.array([centre[0], centre[1], low[2]])
    radius = float(np.max(np.linalg.norm(local[:, :2], axis=1)))
    width, height = 2.04 * radius, 1.02 * float(local[:, 2].max())
    textures, tris, layer_of = [], [], []
    for layer, material in enumerate(meta["materials"]):
        image = Image.open(directory / material["albedo"]).convert("RGBA").resize((psm.SIZE, psm.SIZE), Image.LANCZOS)
        textures.append(np.asarray(image).copy())
        first, count = ranges[LEVEL, layer]
        t = index[first:first + count].reshape(-1, 3)
        tris.append(t)
        layer_of.append(np.full(len(t), layer))
    tris = np.concatenate(tris)
    layer_of = np.concatenate(layer_of)
    # The rasterizer reads the layer from a triangle's first vertex: give each
    # triangle its own three vertices.
    flat = tris.reshape(-1)
    columns = np.zeros((len(flat), 12))
    columns[:, 0:3] = local[flat]
    columns[:, 3:6] = nor[flat]
    columns[:, 6:8] = verts["uv"][flat]
    columns[:, 8:11] = 1.0
    columns[:, 11] = np.repeat(layer_of, 3)
    order = np.arange(len(flat), dtype=np.int64)
    for view in range(8):
        angle = view * 2 * np.pi / 8
        colour, normal = psm.rasterize(columns, order, textures, width, height, angle, eye_sign=-1)
        colour.save(directory / f"impostor_colour_{view}.png")
        normal.save(directory / f"impostor_normal_{view}.png")
        print("view", view, "done", flush=True)
    meta["impostor"] = {"width": width, "height": height, "views": 8,
                        "centre": [float(centre[0]), float(centre[1]), float(low[2])],
                        "colours": [f"impostor_colour_{v}.png" for v in range(8)],
                        "normals": [f"impostor_normal_{v}.png" for v in range(8)]}
    (directory / "character.json").write_text(json.dumps(meta, indent=1))
    print(json.dumps(meta["impostor"]))


if __name__ == "__main__":
    main(sys.argv[1])
