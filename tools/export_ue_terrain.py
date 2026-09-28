"""Export extra ground surface textures from the AncientSettlement UE project.

Read-only: loads assets, never saves a package. Run as a Python commandlet:

    "/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd" \
        ~/Documents/UProjects/AncientSettlement/AncientSettlement.uproject \
        -run=pythonscript -script="$PWD/tools/export_ue_terrain.py" \
        -unattended -nosplash -NoSound -nullrhi -stdout

AS_UE_TERRAIN_OUT sets the staging directory (default /tmp/campfire_ue_terrain_maps).
Every texture is written as PNG (its source art, not the cooked mip).
tools/stage_ue_terrain.py turns them into assets/terrain/<group>/<id>/src.
"""
import json
import os
from pathlib import Path

import unreal

OUT = Path(os.environ.get("AS_UE_TERRAIN_OUT", "/tmp/campfire_ue_terrain_maps"))

# id -> {channel: asset}. Channels: diffuse, normal (DirectX), ao, rough,
# displacement, or a packed mask "ard" (AO, roughness, displacement) / "ord".
SETS = {
    "sand_dune": {
        "diffuse": "/Game/DesertSandPack/Textures/T_Sands_4K_Albedo",
        "normal": "/Game/DesertSandPack/Textures/T_Sands_4K_Normal",
        "ao": "/Game/DesertSandPack/Textures/T_Sands_4K_AO",
        "rough": "/Game/DesertSandPack/Textures/T_Sands_4K_Roughness",
        "displacement": "/Game/DesertSandPack/Textures/T_Sands_4K_Displacement",
    },
    "sand_shore": {
        "diffuse": "/Game/MWLandscapeAutoMaterial/Textures/Ground/TEX_MWAM_SandC_col",
        "normal": "/Game/MWLandscapeAutoMaterial/Textures/Ground/TEX_MWAM_SandC_nrm",
    },
    "sand_gravelly": {
        "diffuse": "/Game/Namaqualand/Textures/T_gravelly_sand_diff_4k",
        "normal": "/Game/Namaqualand/Textures/T_gravelly_sand_nor_dx_4k",
        "ard": "/Game/Namaqualand/Textures/T_gravelly_sand_ard_4k",
    },
    "cliff_dolomite": {
        "diffuse": "/Game/ApexNature/Dolomites/Surfaces/Cliff/T_APXN_DOL_Surface_Cliff_1x1_01_COL",
        "normal": "/Game/ApexNature/Dolomites/Surfaces/Cliff/T_APXN_DOL_Surface_Cliff_1x1_01_NRM",
        "ord": "/Game/ApexNature/Dolomites/Surfaces/Cliff/T_APXN_DOL_Surface_Cliff_1x1_01_ORD",
    },
    "cliff_mossy_rock": {
        "diffuse": "/Game/Realistic_MossyRock/Texture/Mossy_Rock_Base_Color",
        "normal": "/Game/Realistic_MossyRock/Texture/Mossy_Rock_Normal",
        "ao": "/Game/Realistic_MossyRock/Texture/Mossy_Rock_AO",
        "rough": "/Game/Realistic_MossyRock/Texture/Mossy_Rock_Roughness",
        "displacement": "/Game/Realistic_MossyRock/Texture/Mossy_Rock_Height",
    },
    "cliff_desert": {
        "diffuse": "/Game/Namaqualand/Textures/T_cliff_side_diff_4k",
        "normal": "/Game/Namaqualand/Textures/T_cliff_side_nor_dx_4k",
        "ard": "/Game/Namaqualand/Textures/T_cliff_side_ard_4k",
    },
}


def export(obj, dest):
    task = unreal.AssetExportTask()
    task.set_editor_property("object", obj)
    task.set_editor_property("filename", str(dest))
    task.set_editor_property("automated", True)
    task.set_editor_property("prompt", False)
    task.set_editor_property("replace_identical", True)
    task.set_editor_property("exporter", unreal.TextureExporterPNG())
    return unreal.Exporter.run_asset_export_task(task) and dest.is_file()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    report = {}
    for ident, channels in SETS.items():
        folder = OUT / ident
        folder.mkdir(parents=True, exist_ok=True)
        entry = {}
        for channel, path in channels.items():
            obj = unreal.load_asset(path)
            if obj is None:
                entry[channel] = {"asset": path, "ok": False}
                continue
            # The PNG exporter asserts on floating-point source art; skip it
            # rather than crash the commandlet (height is optional).
            compression = str(obj.get_editor_property("compression_settings"))
            if "HDR" in compression.upper():
                entry[channel] = {"asset": path, "ok": False, "skipped": compression}
                continue
            dest = folder / (channel + ".png")
            ok = export(obj, dest)
            size = None
            try:
                size = [obj.blueprint_get_size_x(), obj.blueprint_get_size_y()]
            except Exception:
                pass
            entry[channel] = {"asset": path, "ok": bool(ok), "size": size}
            unreal.log("terrain export %s %s %s" % (ident, channel, ok))
        report[ident] = entry
    (OUT / "terrain_export.json").write_text(json.dumps(report, indent=2))


main()

