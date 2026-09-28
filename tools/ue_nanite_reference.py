"""Unreal editor script: export what Nanite's bake makes of a few reference meshes.

Run headless (no window, no rendering, nothing is saved back into the project):

    "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
        ~/Documents/UProjects/AncientSettlement/AncientSettlement.uproject \
        -run=pythonscript -script="$PWD/tools/ue_nanite_reference.py" \
        -unattended -nosplash -NoSound -nullrhi -stdout

Why the fallback: Nanite's fallback mesh is not a separate simplifier. The
builder takes a cut of the very cluster DAG it streams at runtime
(`FClusterDAG::FindCut`, NaniteBuilder.cpp BuildCoarseRepresentation) at a
target error, then tidies it. Setting the fallback to a relative error and
reading the render LOD back is therefore a faithful sample of the Nanite DAG
at that error - the same thing our own `cutAt` produces from our DAG.

Relative error is Nanite's own unit: percent of
sqrt(min(2 * surface area, bounds surface area)). tools/mesh_lab converts it
with the same formula, so both bakes are cut at the same absolute error.

Output: AS_NANITE_OUT (default <repo>/assets/generated/ue_reference):
  <name>.json            what was exported and the Nanite statistics
  <name>@source.uem      the mesh Nanite was built from
  <name>@e<error>.uem    the Nanite DAG cut at that relative error
.uem: b"UEM1", u32 vertices, u32 triangles, f32 xyz[vertices] (UE cm), u32 ijk[triangles]
"""
import json
import os
import struct
import traceback

import unreal

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.environ.get("AS_NANITE_OUT", os.path.join(REPO, "assets", "generated", "ue_reference"))

# name -> (asset, category). A dense scanned rock, two game-budget rocks, two
# grasses (alpha cards), two trees (the game's cut Megaplant copies).
ASSETS = {
    "quarry_rock_dense": ("/Game/Scene_QuarrySlate/Assets/MS/3D/Qua_Sla_Cluster_Rock_M_06/SM_Qua_Sla_Cluster_Rock_M_06", "rock"),
    "mossy_rock": ("/Game/Fab/Megascans/Mossy_Forest_Rock/Mossy_Forest_Rock_vimrfjsaw_Mid", "rock"),
    "kite_boulder": ("/Game/KiteDemo/Environments/Rocks/Medium_Boulder_001/Medium_Boulder_001", "rock"),
    "mwam_grass": ("/Game/MWLandscapeAutoMaterial/Meshes/Plants/SM_MWAM_GrassA", "grass"),
    "wild_grass": ("/Game/Fab/Megascans/Wild_Grass_5848d16a/vczndjqja_tier_2/StaticMeshes/SM_vczndjqja_VarA", "grass"),
    "beech": ("/Game/Megaplant_Static/Tree_European_Beech/SM_Tree_European_Beech_01_A", "tree"),
    "spruce": ("/Game/Megaplant_Static/Tree_Norway_Spruce/SM_Tree_Norway_Spruce_01_A", "tree"),
}
ONLY = [n for n in os.environ.get("AS_NANITE_ONLY", "").split(",") if n]
# Percent, Nanite's unit. 0 is the source itself.
ERRORS = [0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0]

SMS = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem) or unreal.StaticMeshEditorSubsystem()
GS = unreal.GeometryScript_AssetUtils
Q = unreal.GeometryScript_MeshQueries
L = unreal.GeometryScript_List


def read(mesh, lod_type, lod_index=0):
    req = unreal.GeometryScriptMeshReadLOD()
    req.set_editor_property("lod_type", lod_type)
    req.set_editor_property("lod_index", lod_index)
    opts = unreal.GeometryScriptCopyMeshFromAssetOptions()
    opts.set_editor_property("apply_build_settings", False)
    dm = unreal.DynamicMesh()
    res = GS.copy_mesh_from_static_mesh_v2(mesh, dm, opts, req, False)
    return res[0] if isinstance(res, tuple) and isinstance(res[0], unreal.DynamicMesh) else dm


def first(value):
    return value[0] if isinstance(value, tuple) else value


def pick(value, kind):
    """Out-parameters come back as a tuple whose order the binding chooses."""
    if isinstance(value, kind):
        return value
    return next(v for v in value if isinstance(v, kind))


def write_uem(dm, path):
    positions = pick(Q.get_all_vertex_positions(dm, True), unreal.GeometryScriptVectorList)
    triangles = pick(Q.get_all_triangle_indices(dm, True), unreal.GeometryScriptTriangleList)
    xyz = L.convert_vector_list_to_array(positions)
    ijk = L.convert_triangle_list_to_array(triangles)
    with open(path, "wb") as f:
        f.write(b"UEM1")
        f.write(struct.pack("<II", len(xyz), len(ijk)))
        f.write(struct.pack("<%df" % (3 * len(xyz)), *[c for v in xyz for c in (v.x, v.y, v.z)]))
        f.write(struct.pack("<%dI" % (3 * len(ijk)), *[c for t in ijk for c in (t.x, t.y, t.z)]))
    return len(ijk)


def has_hi_res(mesh):
    req = unreal.GeometryScriptMeshReadLOD()
    req.set_editor_property("lod_type", unreal.GeometryScriptLODType.HI_RES_SOURCE_MODEL)
    req.set_editor_property("lod_index", 0)
    return bool(first(GS.check_static_mesh_has_available_lod(mesh, req)))


def export(name, asset, category):
    mesh = unreal.EditorAssetLibrary.load_asset(asset)
    if mesh is None:
        unreal.log_warning(f"nanite_reference: {asset} not found")
        return None
    source_type = (unreal.GeometryScriptLODType.HI_RES_SOURCE_MODEL if has_hi_res(mesh)
                   else unreal.GeometryScriptLODType.SOURCE_MODEL)
    row = {"name": name, "asset": asset, "category": category,
           "nanite_triangles": mesh.get_num_nanite_triangles(), "cuts": []}
    ns = SMS.get_nanite_settings(mesh)
    row["shape_preservation"] = str(ns.get_editor_property("shape_preservation"))
    row["source_triangles"] = write_uem(read(mesh, source_type), os.path.join(OUT, f"{name}@source.uem"))
    unreal.log_warning(f"nanite_reference: {name} source {row['source_triangles']} tris")
    for error in ERRORS:
        ns.set_editor_property("enabled", True)
        ns.set_editor_property("fallback_target", unreal.NaniteFallbackTarget.RELATIVE_ERROR)
        ns.set_editor_property("fallback_relative_error", error)
        ns.set_editor_property("fallback_percent_triangles", 1.0)
        SMS.set_nanite_settings(mesh, ns, True)
        file = f"{name}@e{error:g}.uem"
        tris = write_uem(read(mesh, unreal.GeometryScriptLODType.RENDER_DATA), os.path.join(OUT, file))
        row["cuts"].append({"relative_error": error, "triangles": tris, "file": file})
        unreal.log_warning(f"nanite_reference: {name} e={error:g}% -> {tris} tris")
    # Never saved: the asset reloads from disk unchanged next time.
    with open(os.path.join(OUT, f"{name}.json"), "w") as f:
        json.dump(row, f, indent=1)
    return row


def main():
    os.makedirs(OUT, exist_ok=True)
    summary = []
    for name, (asset, category) in ASSETS.items():
        if ONLY and name not in ONLY:
            continue
        try:
            row = export(name, asset, category)
            if row:
                summary.append(row)
        except Exception:
            unreal.log_error(f"nanite_reference: {name}: {traceback.format_exc()}")
    with open(os.path.join(OUT, "index.json"), "w") as f:
        json.dump(summary, f, indent=1)


main()
