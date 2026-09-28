#!/usr/bin/env python3
"""Validate and activate a prepared UE bundle, retaining the previous resources.
Run scene_model_clusters on STAGE/scene_models BEFORE installing. This script
never touches the UE project, game configs, shaders or unrelated asset groups.
"""
import argparse
from datetime import datetime
import json
from pathlib import Path
import shutil
import struct
import tempfile

from PIL import Image
from prepare_ue_assets import ROLES, TERRAIN, source_file
from ue_asset_policy import check_triangles, check_png, check_export_bytes


def validate(stage):
    check_export_bytes(stage)
    provenance = json.loads((stage / "provenance.json").read_text())
    if provenance.get("geometry_source") != "catalogue_verified_cut_lod0":
        raise ValueError("Bundle is not a verified cut-LOD0 export")
    if not provenance.get("section_materials_verified"):
        raise ValueError("Bundle predates the section-material/foliage fix")
    models = stage / "scene_models"
    manifest = json.loads((models / "manifest.json").read_text())
    if [m["name"] for m in manifest["models"]] != ROLES or len(manifest.get("grass", [])) != 6:
        raise ValueError("Incomplete UE model/grass catalogue")
    for row in manifest["models"]:
        if row["name"] in ("CommonTree_1", "Pine_1") and row.get("foliage_triangles", 0) <= 0:
            raise ValueError("Tree has no verified foliage triangles: " + row["name"])
        mesh = source_file(models, row["mesh"])
        data = mesh.read_bytes()
        if len(data) < 12 or data[:4] != b"SCM2":
            raise ValueError("Not a prepared scene mesh: " + str(mesh))
        _, vertices, levels = struct.unpack_from("<4sII", data)
        if not vertices or not 1 <= levels <= 8 or len(data) < 12 + levels * 4:
            raise ValueError("Invalid SCM2 header")
        counts = struct.unpack_from(f"<{levels}I", data, 12)
        check_triangles(row["name"], counts[0] // 3)
        if any(not n or n % 3 for n in counts) or len(data) != 12 + levels * 4 + vertices * 48 + sum(counts) * 4:
            raise ValueError("Invalid SCM2 payload size")
        indices = struct.unpack_from(f"<{sum(counts)}I", data, 12 + levels * 4 + vertices * 48)
        if any(i >= vertices for i in indices):
            raise ValueError("SCM2 index out of range")
        sidecar = mesh.with_suffix(".clusters")
        data = sidecar.read_bytes() if sidecar.is_file() else b""
        if len(data) < 64 or data[:4] != b"SCC6":
            raise ValueError("Run scene_model_clusters before installation: " + str(sidecar))
        header = struct.unpack_from("<4s13I2f", data)
        ni, nc, cards, card_levels, crown_vertices, crown_indices, crown_clusters = header[6:13]
        expected = 64 + ni * 16 + nc * 44 + cards * 4 + card_levels * 8 + crown_vertices * 32 + crown_indices * 4 + crown_clusters * 44
        if header[1] != vertices or header[2] != counts[0] // 3 or len(data) != expected or not nc + crown_clusters:
            raise ValueError("SCC6 does not describe its source mesh")
        if any(i >= vertices for (i,) in struct.iter_unpack("<I", data[64:64 + ni * 4])):
            raise ValueError("SCC6 index out of range")
    for name in manifest["colours"] + manifest["normals"] + manifest["grass"]:
        check_png(source_file(models, name))
        with Image.open(source_file(models, name)) as image:
            image.verify()
    for _, stem, _ in TERRAIN:
        for kind in ("albedo", "normal", "properties"):
            for step in (1, 2, 4, 8, 16):
                suffix = "" if step == 1 else "@" + str(step)
                with Image.open(source_file(stage / "terrain", stem + "_" + kind + suffix + ".png")) as image:
                    if image.size != (1024 // step, 1024 // step):
                        raise ValueError("Invalid terrain mip size")
                    image.verify()
    if not (stage / "provenance.json").is_file():
        raise ValueError("Missing source provenance")


def install(stage, assets):
    validate(stage)
    if not assets.is_dir():
        raise ValueError("Expected existing game assets directory")
    source_directories = [(stage / "scene_models", assets / "generated/scene_models")]
    source_directories += [(stage / "terrain" / str(Path(stem).parent), assets / "terrain" / str(Path(stem).parent))
                           for _, stem, _ in TERRAIN]
    backup = assets / "retired" / ("before-ue-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    backup.mkdir(parents=True)
    replaced = []
    with tempfile.TemporaryDirectory(prefix=".ue-install-", dir=assets) as temporary:
        prepared = []
        for index, (source, target) in enumerate(source_directories):
            copied = Path(temporary) / str(index)
            shutil.copytree(source, copied)
            if index == 0:
                shutil.copy2(stage / "provenance.json", copied / "provenance.json")
                (copied / ".ue-imported").write_text("AncientSettlement; regenerate with prepare_ue_assets.py\n")
            prepared.append((copied, target, backup / target.relative_to(assets)))
        try:
            for copied, target, old in prepared:
                old.parent.mkdir(parents=True, exist_ok=True)
                target.parent.mkdir(parents=True, exist_ok=True)
                had_old = target.exists()
                if had_old:
                    target.rename(old)
                replaced.append((target, old, had_old))
                copied.rename(target)
        except BaseException:
            for target, old, had_old in reversed(replaced):
                if target.exists():
                    shutil.rmtree(target)
                if had_old:
                    old.rename(target)
            raise
    return backup


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--assets", type=Path, default=Path(__file__).resolve().parents[1] / "assets")
    args = parser.parse_args()
    print("Previous resources retained in", install(args.stage.resolve(), args.assets.resolve()))

