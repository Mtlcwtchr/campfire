"""Export the used, already-cut nature LOD0 and terrain textures without rendering.
Uses the read-only inventory beside this script. Never saves a UE package.
AS_UE_OUT controls the staging directory. Run with -nullrhi -unattended.
The resulting export.json is published only when every required file succeeds.
"""
import json
import math
import os
from pathlib import Path
import struct
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ue_asset_inventory import OUT, inventory
from ue_asset_policy import (TRIANGLE_LIMITS, MAX_TEXTURE_EDGE, MAX_TEXTURES, MAX_EXPORT_BYTES,
                             check_triangles, check_texture_size, check_png, check_export_bytes,
                             selected_textures, check_leaf_materials)

Q = unreal.GeometryScript_MeshQueries
L = unreal.GeometryScript_List


def pick(value, kind):
    if isinstance(value, kind):
        return value
    return next(v for v in value if isinstance(v, kind))


def vectors(result, kind):
    if any(type(v) is bool and not v for v in result):
        raise ValueError("Source triangle has missing attributes")
    values = [v for v in result if isinstance(v, kind)]
    if len(values) != 3:
        raise ValueError("Unexpected GeometryScript attribute result: " + str(result))
    return values


def geometry(mesh, destination, role, material_count):
    gs = unreal.GeometryScript_AssetUtils
    # cut_assets.py destructively replaces source LOD0 with its final cut.
    # HiRes may still contain an original; RENDER_DATA is Nanite's *fallback*,
    # often much coarser than the already-budgeted surface used by Nanite.
    description = mesh.get_static_mesh_description(0)
    if description is None:
        raise RuntimeError("Prepared LOD0 is unavailable")
    check_triangles(role, description.get_triangle_count())
    req = unreal.GeometryScriptMeshReadLOD()
    req.set_editor_property("lod_type", unreal.GeometryScriptLODType.SOURCE_MODEL)
    req.set_editor_property("lod_index", 0)
    opts = unreal.GeometryScriptCopyMeshFromAssetOptions()
    opts.set_editor_property("apply_build_settings", True)
    dm = unreal.DynamicMesh()
    # The cut meshes can have identical imported slot names. Name-based
    # conversion then maps every face to bark; use section IDs explicitly.
    result = gs.copy_mesh_from_static_mesh_v2(mesh, dm, opts, req, True)
    dm = pick(result, unreal.DynamicMesh)
    if role in ("CommonTree_1", "Pine_1"):
        # Reserve most of the full-tree budget for the missing assembly crown.
        simplify = unreal.GeometryScriptSimplifyMeshOptions()
        simplify.set_editor_property("method", unreal.GeometryScriptRemoveMeshSimplificationType.STANDARD_QEM)
        dm = unreal.GeometryScript_MeshSimplification.apply_simplify_to_triangle_count(dm, 2400, simplify)
    section_result = gs.get_section_material_list_from_static_mesh(mesh, req)
    section_to_material = [int(index) for index in section_result[1]]
    if not section_to_material:
        raise ValueError("Source LOD0 has no section-material map")
    check_triangles(role, Q.get_num_triangle_i_ds(dm))
    triangles = L.convert_triangle_list_to_array(pick(Q.get_all_triangle_indices(dm, False), unreal.GeometryScriptTriangleList))
    xyz = L.convert_vector_list_to_array(pick(Q.get_all_vertex_positions(dm, False), unreal.GeometryScriptVectorList))
    vertex_bytes, indices, dedup = bytearray(), [], {}
    used_materials = set()
    section_counts = {}
    for tid, tri in enumerate(triangles):
        if min(tri.x, tri.y, tri.z) < 0:
            continue
        normals = vectors(Q.get_triangle_normals(dm, tid), unreal.Vector)
        uv = vectors(Q.get_triangle_u_vs(dm, 0, tid) if hasattr(Q, "get_triangle_u_vs") else Q.get_triangle_uvs(dm, 0, tid), unreal.Vector2D)
        material_result = unreal.GeometryScript_Materials.get_triangle_material_id(dm, tid)
        if tid == 0:
            unreal.log_warning(f"AS_MATERIAL_ID {role} first={material_result}")
        if isinstance(material_result, tuple) and any(type(v) is bool and not v for v in material_result):
            raise ValueError("Copied UE mesh has no valid material-ID attribute")
        material = next(v for v in material_result if type(v) is int) if isinstance(material_result, tuple) else material_result
        if not 0 <= material < len(section_to_material):
            raise ValueError("Triangle refers to an absent section")
        section_counts[material] = section_counts.get(material, 0) + 1
        material = section_to_material[material]
        if not 0 <= material < material_count:
            raise ValueError("Invalid source material slot")
        used_materials.add(material)
        for c in (0, 2, 1):  # UE left-handed centimetres -> right-handed Z-up metres
            p = xyz[(tri.x, tri.y, tri.z)[c]]
            n, t = normals[c], uv[c]
            values = (p.x * .01, -p.y * .01, p.z * .01, n.x, -n.y, n.z, t.x, t.y, 1., 1., 1., float(material))
            if not all(math.isfinite(v) for v in values):
                raise ValueError("Non-finite source attributes")
            packed = struct.pack("<12f", *values)
            if packed not in dedup:
                dedup[packed] = len(dedup)
                vertex_bytes.extend(packed)
            indices.append(dedup[packed])
    if not indices:
        raise ValueError("Empty source mesh")
    check_triangles(role, len(indices) // 3)
    unreal.log_warning(f"AS_SECTIONS {role} map={section_to_material} triangles={section_counts}")
    used_materials = sorted(used_materials)
    remap = {slot: index for index, slot in enumerate(used_materials)}
    for vertex in range(len(dedup)):
        offset = vertex * 48 + 44
        slot = int(struct.unpack_from("<f", vertex_bytes, offset)[0])
        struct.pack_into("<f", vertex_bytes, offset, float(remap[slot]))
    check_export_bytes(OUT, 12 + len(vertex_bytes) + len(indices) * 4, destination)
    with destination.open("wb") as f:
        f.write(struct.pack("<4sII", b"UMS1", len(dedup), len(indices)))
        f.write(vertex_bytes)
        f.write(struct.pack("<%dI" % len(indices), *indices))
    return len(indices) // 3, used_materials


def main():
    # This is staging only; never leave a previous success marker pointing at
    # files a failed rerun has only partially replaced.
    (OUT / "export.json").unlink(missing_ok=True)
    (OUT / "export.json.tmp").unlink(missing_ok=True)
    inventory()
    rows = json.loads((OUT / "inventory.json").read_text())
    terrain = json.loads((OUT / "terrain_inventory.json").read_text())
    assembly_path = OUT / "assembly-parts.json"
    if not assembly_path.exists():
        raise ValueError("Export the Nanite crown assembly parts before exporting complete trees")
    assembly_book = json.loads(assembly_path.read_text())
    textures = {}
    check_export_bytes(OUT)
    def texture(path):
        if path in textures:
            return textures[path]
        obj = unreal.load_asset(path)
        if not isinstance(obj, unreal.Texture2D):
            raise ValueError("Unsupported material texture: " + path)
        width, height = obj.blueprint_get_size_x(), obj.blueprint_get_size_y()
        check_texture_size(width, height)
        if len(textures) >= MAX_TEXTURES:
            raise ValueError("Too many textures; transfer only the channels used by the selected visuals")
        name = "textures/" + path.split("/")[-1].split(".")[0] + "_" + str(len(textures)) + ".png"
        dest = OUT / name
        dest.parent.mkdir(exist_ok=True)
        # Reserve the uncompressed 8-bit image size, not an optimistic JPEG size.
        check_export_bytes(OUT, width * height * 4 + 65536, dest)
        task = unreal.AssetExportTask()
        task.set_editor_property("object", obj)
        task.set_editor_property("filename", str(dest))
        task.set_editor_property("automated", True)
        task.set_editor_property("prompt", False)
        task.set_editor_property("replace_identical", True)
        task.set_editor_property("exporter", unreal.TextureExporterPNG())
        if not unreal.Exporter.run_asset_export_task(task) or not dest.is_file():
            raise RuntimeError("Texture export failed: " + path)
        try:
            check_png(dest)  # also checks the actual source dimensions/bit depth
            check_export_bytes(OUT)
        except Exception:
            dest.unlink(missing_ok=True)
            raise
        textures[path] = name
        return name
    for row in rows:
        row["mesh"] = row["role"] + ".ums"
        row["triangles"], used = geometry(unreal.load_asset(row["asset"]), OUT / row["mesh"], row["role"], len(row["materials"]))
        if row["role"] in assembly_book["roots"]:
            row["assembly"] = assembly_book["roots"][row["role"]]
            assembly_slots = {i for part in row["assembly"]["parts"] for i in part["material_remap"]}
            check_leaf_materials(row["role"], row["materials"], set(used) | assembly_slots)
        else:
            check_leaf_materials(row["role"], row["materials"], used)
            row["materials"] = [row["materials"][index] for index in used]
        for material in row["materials"]:
            if material is None:
                raise ValueError("A used triangle has no material")
            channels = selected_textures(material)
            material["files"] = {name: texture(path) for name, path in channels.items()}
            material["bindings"] = {name: name for name in channels}
            if material["asset"].startswith("/Game/Megaplant_Static/Materials/"):
                material["normal_encoding"] = "packed_rg_directx"
            if "colour_mask" in channels:
                material["colour_encoding"] = "light_foliage_tints"
        unreal.log_warning("AS_EXPORT " + row["role"] + " " + str(row["triangles"]) + " triangles")
    for maps in terrain.values():
        for channel, path in list(maps.items()):
            maps[channel] = {"asset": path, "file": texture(path)}
    result = {"version": 1, "source_project": "AncientSettlement", "models": rows, "terrain": terrain,
              "geometry_source": "catalogue_verified_cut_lod0",
              "section_materials_verified": True,
              "assembly_parts": assembly_book["parts"],
              "limits": {"triangles": TRIANGLE_LIMITS, "texture_edge": MAX_TEXTURE_EDGE,
                         "textures": MAX_TEXTURES, "export_bytes": MAX_EXPORT_BYTES},
              "normal_encoding": "directx", "vertex_colours": "UE wind/mask data is not used as albedo tint",
              "license": "Original pack licenses apply; these assets are not CC0. See UE project purchase/source records."}
    temp = OUT / "export.json.tmp"
    payload = (json.dumps(result, indent=2) + "\n").encode("utf-8")
    check_export_bytes(OUT, len(payload))
    temp.write_bytes(payload)
    os.replace(temp, OUT / "export.json")
    unreal.log_warning("AS_EXPORT complete " + str(OUT))


if __name__ == "__main__":
    main()

