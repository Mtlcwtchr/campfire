"""Export extra foliage from the AncientSettlement UE project: glTF geometry + colour/opacity textures.

Read-only - loads assets, never saves a package. Run as a Python commandlet with
the engine's glTF exporter enabled for this run only (the .uproject is not
touched):

    nice -n 15 "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \\
        ~/Documents/UProjects/AncientSettlement/AncientSettlement.uproject \\
        -run=pythonscript -script="$PWD/tools/export_ue_foliage.py" \\
        -EnablePlugins=GLTFExporter -unattended -nosplash -NoSound -nullrhi -stdout

Reads content/config/ue_foliage_sources.json and writes, per asset,
assets/models/ue_foliage/<asset_id>/raw/:

    <mesh>.gltf + .bin   UE's glTF exporter, source model (not the Nanite
                         fallback), no material baking - one glTF material per
                         UE material, named after it
    textures/*.png       only the colour / opacity / colour-mask textures the
                         materials bind (TextureExporterPNG)
    export.json          per mesh: package path, triangles, and per material
                         its parameters, blend mode, two-sidedness and - for
                         Light Foliage - the base-colour graph

tools/prepare_ue_foliage.py turns that into the layout tools/fetch_models.py
writes for Poly Haven. AS_UE_FOLIAGE_ONLY="a,b" restricts to some asset ids.
"""
import json
import os
import re
import sys
import traceback
from pathlib import Path

import unreal

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
CONFIG = ROOT / "content/config/ue_foliage_sources.json"
OUT = Path(os.environ.get("AS_UE_FOLIAGE_OUT", str(ROOT / "assets/models/ue_foliage")))
ONLY = {a for a in os.environ.get("AS_UE_FOLIAGE_ONLY", "").split(",") if a}

COLOUR = re.compile(r"(albedo|basecolou?r|base_colou?r|diffuse|colou?r|opacity|alpha|_b-o$|_b$|_bc$|_d$|_mask(_\d+)?$|^mask$)",
                    re.IGNORECASE)
NOT_COLOUR = re.compile(r"(winter|billboard|impostor|normal|noise|wind|pivot|rough|ort$|_n$|_nt$|spec|gloss|cavity|bump|"
                        r"displace|translucen|_ao$|subsurface|sss)", re.IGNORECASE)


def log(message):
    unreal.log_warning("AS_FOLIAGE " + message)


def meshes_for(entry):
    if "meshes" in entry:
        return list(entry["meshes"])
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    found = []
    for data in registry.get_assets_by_path(entry["folder"], recursive=True):
        if str(data.asset_class_path.asset_name) != "StaticMesh":
            continue
        name = str(data.asset_name)
        if entry.get("include") and not any(word in name for word in entry["include"]):
            continue
        if any(word in name for word in entry.get("exclude", [])):
            continue
        found.append(str(data.package_name))
    return sorted(found)


def material_entry(material):
    lib = unreal.MaterialEditingLibrary
    base = material
    chain = [material.get_path_name()]
    while isinstance(base, unreal.MaterialInstance):
        base = base.get_editor_property("parent")
        chain.append(base.get_path_name())
    entry = {"asset": material.get_path_name(), "name": material.get_name(), "parents": chain[1:],
             "textures": {}, "scalars": {}, "vectors": {}, "switches": {}}
    try:
        entry["blend_mode"] = str(material.get_blend_mode())
    except Exception:
        entry["blend_mode"] = str(base.get_editor_property("blend_mode"))
    try:
        entry["two_sided"] = bool(base.get_editor_property("two_sided"))
    except Exception:
        entry["two_sided"] = None
    is_instance = isinstance(material, unreal.MaterialInstance)
    prefix = "get_material_instance_" if is_instance else "get_material_default_"
    for kind in ("texture", "scalar", "vector"):
        for name in getattr(lib, "get_" + kind + "_parameter_names")(material):
            value = getattr(lib, prefix + kind + "_parameter_value")(material, name)
            if kind == "vector":
                entry["vectors"][str(name)] = [value.r, value.g, value.b, value.a]
            elif kind == "texture":
                entry["textures"][str(name)] = value.get_path_name() if value else None
            else:
                entry["scalars"][str(name)] = float(value)
    if is_instance:
        for name in lib.get_static_switch_parameter_names(material):
            entry["switches"][str(name)] = bool(lib.get_material_instance_static_switch_parameter_value(material, name))
    if any(p.startswith("/Game/Light_Foliage/") for p in chain):
        from ue_material_graph import describe
        entry["graph"] = describe(material)
    return entry


def export_texture(path, into, done):
    if path in done:
        return done[path]
    texture = unreal.load_asset(path)
    if not isinstance(texture, unreal.Texture2D):
        done[path] = None
        return None
    name = "textures/" + path.split("/")[-1].split(".")[0] + ".png"
    dest = into / name
    dest.parent.mkdir(parents=True, exist_ok=True)
    task = unreal.AssetExportTask()
    task.set_editor_property("object", texture)
    task.set_editor_property("filename", str(dest))
    task.set_editor_property("automated", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("replace_identical", True)
    task.set_editor_property("exporter", unreal.TextureExporterPNG())
    if not unreal.Exporter.run_asset_export_task(task) or not dest.is_file():
        raise RuntimeError("texture export failed: " + path)
    done[path] = {"file": name, "width": texture.blueprint_get_size_x(), "height": texture.blueprint_get_size_y(),
                  "srgb": bool(texture.get_editor_property("srgb"))}
    return done[path]


def gltf_options():
    options = unreal.GLTFExportOptions()
    wanted = {
        "bake_material_inputs": unreal.GLTFMaterialBakeMode.DISABLED,
        "export_source_model": True,
        "export_vertex_colors": False,
        "use_mesh_quantization": False,
        "texture_image_format": unreal.GLTFTextureImageFormat.NONE,
        "export_proxy_materials": False,
        "export_lightmaps": False,
        "include_copyright_notice": False,
        "export_texture_transforms": False,
    }
    for key, value in wanted.items():
        try:
            options.set_editor_property(key, value)
        except Exception as error:
            log("option %s not set: %s" % (key, error))
    return options


def megascans_atlas(path):
    """/Game/Fab/Megascans/<Pack>/...: the pack's 3D-plant atlas T_<id>_2K_B-O
    (colour + opacity in alpha). The project's MI_AS_* materials bind the
    billboard atlas as Albedo, which does not match the mesh UVs."""
    parts = path.split("/")
    if len(parts) < 5 or parts[1:4] != ["Game", "Fab", "Megascans"]:
        return None
    pack = "/".join(parts[:5])
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    found = []
    for data in registry.get_assets_by_path(pack, recursive=True):
        name = str(data.asset_name)
        if str(data.asset_class_path.asset_name) == "Texture2D" and name.endswith("_B-O") and "Billboard" not in name:
            found.append(str(data.package_name) + "." + name)
    return sorted(found, key=len)[0] if found else None


def export_mesh(path, into, options, textures):
    mesh = unreal.load_asset(path)
    if not isinstance(mesh, unreal.StaticMesh):
        raise RuntimeError("not a static mesh: " + path)
    name = path.split("/")[-1]
    dest = into / (name + ".gltf")
    result = unreal.GLTFExporter.export_to_gltf(mesh, str(dest), options, set())
    ok = result[0] if isinstance(result, tuple) else bool(result)
    if not ok or not dest.is_file():
        raise RuntimeError("glTF export failed: " + path)
    description = mesh.get_static_mesh_description(0)
    row = {"mesh": path, "gltf": dest.name,
           "source_triangles": description.get_triangle_count() if description else None,
           "nanite_triangles": mesh.get_num_nanite_triangles(), "materials": []}
    for slot in mesh.get_editor_property("static_materials"):
        material = slot.get_editor_property("material_interface")
        if material is None:
            row["materials"].append(None)
            continue
        entry = material_entry(material)
        entry["slot"] = str(slot.get_editor_property("material_slot_name"))
        for param, tex in entry["textures"].items():
            if not tex:
                continue
            short = tex.split(".")[-1]
            if NOT_COLOUR.search(param) or NOT_COLOUR.search(short):
                continue
            if COLOUR.search(param) or COLOUR.search(short):
                info = export_texture(tex, into, textures)
                if info:
                    entry.setdefault("files", {})[param] = info
        atlas = megascans_atlas(path)
        if atlas:
            info = export_texture(atlas, into, textures)
            if info:
                entry.setdefault("files", {})["B-O"] = info
        row["materials"].append(entry)
    return row


def main():
    config = json.loads(CONFIG.read_text())
    options = gltf_options()
    summary = {}
    for entry in config["assets"]:
        asset = entry["asset_id"]
        if ONLY and asset not in ONLY:
            continue
        into = OUT / asset / "raw"
        into.mkdir(parents=True, exist_ok=True)
        rows, textures, errors = [], {}, []
        try:
            paths = meshes_for(entry)
        except Exception as error:
            paths, errors = [], [str(error)]
        for path in paths:
            try:
                rows.append(export_mesh(path, into, options, textures))
                log("%s %s ok" % (asset, path))
            except Exception as error:
                errors.append("%s: %s" % (path, error))
                log("%s %s FAILED %s" % (asset, path, traceback.format_exc()))
        (into / "export.json").write_text(json.dumps({"asset_id": asset, "source": entry, "meshes": rows,
                                                      "errors": errors}, indent=1) + "\n")
        summary[asset] = {"meshes": len(rows), "errors": len(errors), "textures": len([t for t in textures.values() if t])}
        log("%s: %d meshes, %d errors" % (asset, len(rows), len(errors)))
    (OUT / "export_summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    log("complete " + json.dumps(summary))


if __name__ == "__main__":
    main()

