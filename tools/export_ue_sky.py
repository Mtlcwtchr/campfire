"""Export the sky source textures from the AncientSettlement UE project.

Read-only: loads assets, never saves a package. Run as a Python commandlet:

    "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
        ~/Documents/UProjects/AncientSettlement/AncientSettlement.uproject \
        -run=pythonscript -script="$PWD/tools/export_ue_sky.py" \
        -unattended -nosplash -NoSound -nullrhi -stdout

AS_UE_SKY_OUT sets the staging directory (default /tmp/campfire_ue_sky).
Cubemaps are written as long-lat .hdr, 2D textures as .png. tools/prepare_sky.py
turns them into the runtime sky assets.
"""
import json
import os
from pathlib import Path

import unreal

OUT = Path(os.environ.get("AS_UE_SKY_OUT", "/tmp/campfire_ue_sky"))
CANDIDATES = [
    "/Game/Namaqualand/HDRIs/goegap_8k",
    "/Game/Namaqualand/HDRIs/klippad_dawn_1_8k",
    "/Game/KiteDemo/LevelContent/HDRI/HDRI_Epic_Courtyard_Daylight",
    "/Game/GoodSky/Resource/Textures/T_GoodSky_clouds_sphere",
    "/Game/GoodSky/Resource/Textures/T_GoodSky_noise_smooth",
    "/Game/MWLandscapeAutoMaterial/Textures/FX/TEX_MW_CloudWeatherA",
]


def export(obj, dest, exporter):
    task = unreal.AssetExportTask()
    task.set_editor_property("object", obj)
    task.set_editor_property("filename", str(dest))
    task.set_editor_property("automated", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("replace_identical", True)
    if exporter is not None:
        task.set_editor_property("exporter", exporter)
    return unreal.Exporter.run_asset_export_task(task) and dest.is_file()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    report = []
    for path in CANDIDATES:
        obj = unreal.load_asset(path)
        entry = {"asset": path, "class": obj.get_class().get_name() if obj else None}
        if obj is None:
            report.append(entry)
            continue
        name = path.split("/")[-1]
        if isinstance(obj, unreal.TextureCube):
            dest = OUT / (name + ".hdr")
            ok = export(obj, dest, None)
        else:
            dest = OUT / (name + ".png")
            ok = export(obj, dest, unreal.TextureExporterPNG())
            if not ok:
                dest = OUT / (name + ".hdr")
                ok = export(obj, dest, None)
        entry.update({"file": dest.name if ok else None, "ok": bool(ok)})
        try:
            entry["size"] = [obj.blueprint_get_size_x(), obj.blueprint_get_size_y()]
        except Exception:
            pass
        report.append(entry)
        unreal.log("sky export " + json.dumps(entry))
    (OUT / "sky_export.json").write_text(json.dumps(report, indent=2))


main()

