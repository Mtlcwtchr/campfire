"""Read Nanite assembly metadata without copying high-resolution trunk geometry."""
import json
import unreal
from ue_asset_inventory import OUT

TREES = {
    'CommonTree_1': '/Game/Megaplant_Library/Tree_European_Beech/Tree_European_Beech_01/Tree_European_Beech_01_A',
    'Pine_1': '/Game/Megaplant_Library/Tree_Norway_Spruce/Tree_Norway_Spruce_01/Tree_Norway_Spruce_01_A',
}


def inspect():
    if (OUT / 'export-assembly').exists():
        import importlib
        import ue_assembly_format
        import ue_assembly_export
        importlib.reload(ue_assembly_format)
        importlib.reload(ue_assembly_export).export_parts(OUT)
        return
    rows = []
    for role, path in TREES.items():
        mesh = unreal.load_asset(path)
        settings = mesh.get_editor_property('nanite_settings')
        assembly = settings.get_editor_property('nanite_assembly_data')
        parts = assembly.get_editor_property('parts')
        text = assembly.export_text()
        (OUT / (role + '-assembly.txt')).write_text(text)
        row = {'role': role, 'asset': path, 'parts': [], 'assembly_text_bytes': len(text),
               'bone_methods': {name: str(getattr(mesh, name).__doc__) for name in dir(mesh) if 'bone' in name or 'pose' in name}}
        for part in parts:
            row['parts'].append({'text': part.export_text()})
        rows.append(row)
        mesh = settings = assembly = parts = None
        unreal.SystemLibrary.collect_garbage()
    (OUT / 'assembly-inspect.json').write_text(json.dumps(rows, indent=2))
    unreal.log_warning('AS_ASSEMBLY inspection written')

