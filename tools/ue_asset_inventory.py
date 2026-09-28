"""Read-only UE commandlet inventory for the nature/terrain migration.
Run with UnrealEditor-Cmd -run=pythonscript -script=<this file> -nullrhi -unattended.
Only writes AS_UE_OUT (default assets/source/ancient_settlement), never saves assets.
"""
import json
import os
from pathlib import Path
import sys

import unreal

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ue_asset_policy import VISUAL_PREFIXES, check_triangles, check_texture_size

OUT = Path(os.environ.get("AS_UE_OUT", str(Path(__file__).resolve().parents[1] / "assets/source/ancient_settlement")))
ASSETS = {
    "CommonTree_1": "/Game/Megaplant_Static/Tree_European_Beech/SM_Tree_European_Beech_01_A",
    "Pine_1": "/Game/Megaplant_Static/Tree_Norway_Spruce/SM_Tree_Norway_Spruce_01_A",
    "Bush_Common": "/Game/Light_Foliage/Meshes/SM_Bush_01",
    "Rock_Medium_1": "/Game/Fab/Megascans/Mossy_Forest_Rock/Mossy_Forest_Rock_vimrfjsaw_Mid",
    "Mushroom_Common": "/Game/Fab/Megascans/Bolete_Mushrooms/qdzrT_LOD0_TIER2_000",
    "Deadwood_Log": "/Game/Fab/Megascans/Large_Fallen_Tree/Large_Fallen_Tree_wd1mbcvbw_Mid",
    "Deadwood_Stump": "/Game/Fab/Megascans/Old_Tree_Stump/Old_Tree_Stump_wd0qcaobw_Mid",
    "Deadwood_Branch": "/Game/Namaqualand/Meshes/General/NN/SM_bark_debris_01_a_NN",
    "Grass_1": "/Game/MWLandscapeAutoMaterial/Meshes/Plants/SM_MWAM_GrassA",
}


def inventory():
    OUT.mkdir(parents=True, exist_ok=True)
    report_path = os.environ.get("AS_UE_CATALOG_REPORT")
    report = json.loads(Path(report_path).read_text()) if report_path else None
    visuals = []
    if report is None:
        catalog = unreal.load_asset("/Game/AncientSettlement/Catalog/DA_ExploreDecor")
        if catalog is None:
            raise RuntimeError("Cannot verify the active UE decoration catalogue")
        visuals = catalog.get_editor_property("visuals")
    api = {}
    for clsname in ("GeometryScript_MeshQueries", "GeometryScript_Materials", "GeometryScript_VertexColors", "MaterialEditingLibrary", "GLTFExporter", "TextureExporterPNG"):
        cls = getattr(unreal, clsname, None)
        api[clsname] = {n: str(getattr(cls, n).__doc__) for n in dir(cls)
                        if any(s in n for s in ("triangle", "parameter", "export", "color"))} if cls else None
    (OUT / "api.json").write_text(json.dumps(api, indent=2))
    rows = []
    for role, path in ASSETS.items():
        mesh = unreal.load_asset(path)
        if not isinstance(mesh, unreal.StaticMesh):
            raise RuntimeError("Missing static mesh: " + path)
        matches = [v for v in visuals if role in VISUAL_PREFIXES and
                   str(v.get_editor_property("id")).startswith(VISUAL_PREFIXES[role]) and
                   v.get_editor_property("mesh") and
                   v.get_editor_property("mesh").get_path_name().split(".")[0] == path]
        recorded = [] if report is None else [r for r in report if r.get("mesh") == path and
                    any(vid == "terrain_grass" if role == "Grass_1" else
                        vid.startswith(VISUAL_PREFIXES[role]) for vid in r.get("ids", []))]
        if report is not None and len(recorded) != 1:
            raise RuntimeError("Asset budget report has no unambiguous used visual: " + path)
        if report is None and role != "Grass_1" and not matches:
            raise RuntimeError("Selected mesh is not used by the requested UE role: " + path)
        source = mesh.get_static_mesh_description(0)
        if source is None:
            raise RuntimeError("Prepared LOD0 is unavailable: " + path)
        source_triangles = source.get_triangle_count()
        check_triangles(role, source_triangles)
        overrides = list(matches[0].get_editor_property("material_overrides") or []) if matches else []
        visual_id = str(matches[0].get_editor_property("id")) if matches else "terrain_grass"
        if recorded:
            if recorded[0].get("source_tris") != source_triangles:
                raise RuntimeError("Source LOD0 changed since the used-asset report: " + path)
            overrides = [unreal.load_asset(p) if p else None for p in recorded[0]["materials"]]
            if any(p and not mat for p, mat in zip(recorded[0]["materials"], overrides)):
                raise RuntimeError("A material from the used-asset report could not be loaded")
            visual_id = next(vid for vid in recorded[0]["ids"] if vid == "terrain_grass" or
                             (role in VISUAL_PREFIXES and vid.startswith(VISUAL_PREFIXES[role])))
        row = {"role": role, "asset": path, "prepared_lod0_triangles": source_triangles,
               "visual_id": visual_id, "catalogue_verification": report_path or "live DA_ExploreDecor",
               "nanite_triangles": mesh.get_num_nanite_triangles(), "materials": []}
        for index, slot in enumerate(mesh.get_editor_property("static_materials")):
            mat = overrides[index] if index < len(overrides) and overrides[index] else slot.get_editor_property("material_interface")
            if mat is None:
                row["materials"].append(None)
                continue
            entry = {"asset": mat.get_path_name(), "class": mat.get_class().get_name(), "textures": {}, "scalars": {}, "vectors": {}}
            lib = unreal.MaterialEditingLibrary
            for kind in ("texture", "scalar", "vector"):
                for name in getattr(lib, "get_" + kind + "_parameter_names")(mat):
                    prefix = "get_material_instance_" if isinstance(mat, unreal.MaterialInstance) else "get_material_default_"
                    value = getattr(lib, prefix + kind + "_parameter_value")(mat, name)
                    if kind == "vector":
                        entry["vectors"][str(name)] = [value.r, value.g, value.b, value.a]
                    else:
                        entry[kind + "s"][str(name)] = value.get_path_name() if isinstance(value, unreal.Object) else str(value)
            if role in ("Bush_Common", "Grass_1"):
                from ue_material_graph import describe
                entry["graph"] = describe(mat)
                entry["switches"] = {str(name): bool(lib.get_material_instance_static_switch_parameter_value(mat, name))
                                     for name in lib.get_static_switch_parameter_names(mat)}
            row["materials"].append(entry)
        rows.append(row)
        unreal.log_warning("AS_INVENTORY " + role + " " + str(row["nanite_triangles"]))
        (OUT / "inventory.json").write_text(json.dumps(rows, indent=2))
    terrain = {}
    for surface in ("Grass", "Dirt", "SandA", "Rock", "Snow"):
        terrain[surface] = {}
        for channel, suffix in (("albedo", "col"), ("normal", "nrm")):
            path = "/Game/MWLandscapeAutoMaterial/Textures/Ground/TEX_MWAM_" + surface + "_" + suffix
            texture = unreal.load_asset(path)
            if not isinstance(texture, unreal.Texture2D):
                raise RuntimeError("Missing terrain texture: " + path)
            check_texture_size(texture.blueprint_get_size_x(), texture.blueprint_get_size_y())
            terrain[surface][channel] = path
    (OUT / "terrain_inventory.json").write_text(json.dumps(terrain, indent=2))
    if (OUT / "inspect-assembly").exists():
        import importlib
        import ue_tree_assembly
        importlib.reload(ue_tree_assembly).inspect()
    unreal.log_warning("AS_INVENTORY complete")


if __name__ == "__main__":
    inventory()

