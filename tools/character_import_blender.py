"""Blender (headless) half of the character import: FBX in, glTF binary out.

    /Applications/Blender.app/Contents/MacOS/Blender -b --factory-startup \
        --python tools/character_import_blender.py -- SOURCE.fbx OUT.glb [TEXTURE_SIDE]

The source is the artist's file as downloaded (Fab vault cache); nothing is
written next to it. What this does and no more:
  - imports the FBX in metres, skin and skeleton kept;
  - shrinks every embedded texture to TEXTURE_SIDE (default 1024) - a hero
    seen at a few hundred pixels does not carry 4K maps;
  - exports one .glb with the skin, the bind pose and no animation (the
    engine poses the skeleton itself, see src/game/render/character_model.hpp);
  - prints the materials and which maps each one uses, for the packer.
tools/pack_character.py turns the .glb into the engine's own files.
"""
import json
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:]
source, output = argv[0], argv[1]
side = int(argv[2]) if len(argv) > 2 else 1024

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=source, use_anim=False, ignore_leaf_bones=True,
                         automatic_bone_orientation=False, global_scale=1.0)

# Units: Auto-Rig Pro exports for UE in centimetres; whatever the file says,
# the hero must come out about 1.8-2.0 m tall.
meshes = [o for o in bpy.context.scene.objects if o.type == "MESH"]
low = [min((o.matrix_world @ v.co).z for v in o.data.vertices) for o in meshes if o.data.vertices]
high = [max((o.matrix_world @ v.co).z for v in o.data.vertices) for o in meshes if o.data.vertices]
height = max(high) - min(low)
scale = 1.0
if height > 20:
    scale = 0.01
elif height < 0.2:
    scale = 100.0
if scale != 1.0:
    for o in bpy.context.scene.objects:
        if o.parent is None:
            o.scale = [s * scale for s in o.scale]
            o.location = [l * scale for l in o.location]
    bpy.context.view_layer.update()
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)

for image in bpy.data.images:
    if image.size[0] > side or image.size[1] > side:
        w, h = image.size
        k = side / max(w, h)
        image.scale(max(1, int(w * k)), max(1, int(h * k)))
        image.pack()

report = {"height_m": height * scale, "scale_applied": scale, "materials": {}}
for m in bpy.data.materials:
    maps = {}
    if m.use_nodes:
        for node in m.node_tree.nodes:
            if node.type == "TEX_IMAGE" and node.image:
                links = [l.to_socket.name for l in node.outputs[0].links]
                maps[node.image.name] = {"size": list(node.image.size), "to": links}
    report["materials"][m.name] = maps

bpy.ops.export_scene.gltf(filepath=output, export_format="GLB", export_skins=True,
                          export_animations=False, export_morph=False,
                          export_image_format="AUTO", export_yup=False,
                          export_apply=False)
print("CHARACTER_IMPORT " + json.dumps(report))
