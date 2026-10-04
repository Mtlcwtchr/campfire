"""Engine-side Blender toolkit: shapes made monumental and weathered, textured
from the scans the world already uses, and written where the model pipeline
reads them (doc/plan_procedural_environment_2026-10-03.md, part I).

Runs inside Blender (`blender -b -P <script> -- ...`); a game's generators
(tools/blender/megaliths.py) and the generic mesh stylizer
(tools/blender/stylize_mesh.py) build on it. Nothing here knows what a
megalith is: blocks, weathering, painted masses, bakes and export.

Output: assets/generated/simplified_models/<asset>/<asset>.gltf (+ .bin and
textures/), one mesh node per variant - the shape tools/prepare_environment_models.py
reads - with source.json and <asset>.meta.json beside it (asset_meta.hpp).
"""
import json
import math
import os
import random
from pathlib import Path

import bpy
import bmesh
from mathutils import Matrix, Vector, noise

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "assets" / "generated" / "simplified_models"
SCANS = ROOT / "assets" / "terrain" / "ph"


# --- scene ------------------------------------------------------------------

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 16
    scene.cycles.device = "CPU"
    return scene


def select_only(obj):
    for o in bpy.context.scene.objects:
        o.select_set(False)
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def apply_all(obj):
    select_only(obj)
    for m in list(obj.modifiers):
        bpy.ops.object.modifier_apply(modifier=m.name)


def join(objects, name):
    for o in bpy.context.scene.objects:
        o.select_set(False)
    for o in objects:
        o.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.join()
    obj = bpy.context.view_layer.objects.active
    obj.name = name
    return obj


# --- shapes -----------------------------------------------------------------

def block(name, size, location=(0, 0, 0), rotation=(0, 0, 0), subdivisions=3, taper=0.0, round_edges=0.12):
    """A dressed stone: a box `size` (x, y, z), its top tapered by `taper`
    (share of the base), its edges rounded - the raw material of every
    standing stone, lintel and wall block."""
    bpy.ops.mesh.primitive_cube_add(size=1, location=location, rotation=rotation)
    obj = bpy.context.active_object
    obj.name = name
    obj.scale = size
    bpy.ops.object.transform_apply(scale=True)
    if taper:
        mesh = obj.data
        top = max(v.co.z for v in mesh.vertices)
        bottom = min(v.co.z for v in mesh.vertices)
        for v in mesh.vertices:
            t = (v.co.z - bottom) / max(top - bottom, 1e-6)
            k = 1.0 - taper * t
            v.co.x *= k
            v.co.y *= k
    if round_edges > 0:
        bevel = obj.modifiers.new("round", "BEVEL")
        bevel.width = round_edges * min(size)
        bevel.segments = 3
    if subdivisions > 0:
        sub = obj.modifiers.new("subdivide", "SUBSURF")
        sub.levels = subdivisions
        sub.render_levels = subdivisions
        sub.subdivision_type = "SIMPLE"
    apply_all(obj)
    return obj


def capsule(name, start, end, radius, segments=16):
    """A rounded limb between two points: a finger, a nose, a fold of cloth."""
    a, b = Vector(start), Vector(end)
    length = (b - a).length
    bpy.ops.mesh.primitive_cylinder_add(vertices=segments, radius=radius, depth=length,
                                        location=(a + b) / 2)
    obj = bpy.context.active_object
    obj.name = name
    obj.rotation_mode = "QUATERNION"
    obj.rotation_quaternion = (b - a).to_track_quat("Z", "Y")
    bpy.ops.object.transform_apply(location=False, rotation=True, scale=False)
    for end_point in (a, b):
        bpy.ops.mesh.primitive_uv_sphere_add(radius=radius, location=end_point, segments=segments, ring_count=segments // 2)
        bpy.context.active_object.name = name + "_cap"
    return obj


def metaball_mass(name, balls, resolution=0.08, threshold=0.6):
    """A soft organic mass from metaballs, as a mesh: heads, hands, eroded
    knolls, the swelling stone of a Roger Dean landscape. Each element is
    (centre, radius, scale_xyz) for an ellipsoid, or a dict with type
    (BALL, ELLIPSOID, CAPSULE), co, radius, size (x, y, z), rotation
    (quaternion), stiffness and negative - a negative element carves (an eye
    socket, the line of a mouth)."""
    data = bpy.data.metaballs.new(name)
    data.resolution = resolution
    data.render_resolution = resolution
    data.threshold = threshold
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    for ball in balls:
        if isinstance(ball, dict):
            e = data.elements.new(type=ball.get("type", "ELLIPSOID"))
            e.co = ball["co"]
            e.radius = ball["radius"]
            size = ball.get("size", (1, 1, 1))
            e.size_x, e.size_y, e.size_z = size
            if "rotation" in ball:
                e.rotation = ball["rotation"]
            e.stiffness = ball.get("stiffness", 2.0)
            e.use_negative = ball.get("negative", False)
            continue
        centre, radius, scale = ball
        e = data.elements.new(type="ELLIPSOID")
        e.co = centre
        e.radius = radius
        e.size_x, e.size_y, e.size_z = scale
    select_only(obj)
    bpy.ops.object.convert(target="MESH")
    mesh_obj = bpy.context.active_object
    mesh_obj.name = name
    return mesh_obj


def keep_largest(obj):
    """Only the largest connected piece: what a metaball field left floating
    beside a shape is not part of it."""
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.verts.ensure_lookup_table()
    seen, pieces = set(), []
    for v in bm.verts:
        if v.index in seen:
            continue
        stack, piece = [v], []
        seen.add(v.index)
        while stack:
            u = stack.pop()
            piece.append(u)
            for e in u.link_edges:
                w = e.other_vert(u)
                if w.index not in seen:
                    seen.add(w.index)
                    stack.append(w)
        pieces.append(piece)
    if len(pieces) > 1:
        largest = max(pieces, key=len)
        keep = {v.index for v in largest}
        bmesh.ops.delete(bm, geom=[v for v in bm.verts if v.index not in keep], context="VERTS")
        bm.to_mesh(obj.data)
        obj.data.update()
    bm.free()
    return obj


def union(target, others):
    """Boolean union of `others` into `target`, which keeps its name."""
    for o in others:
        m = target.modifiers.new("union", "BOOLEAN")
        m.operation = "UNION"
        m.object = o
        m.solver = "EXACT"
        apply_all(target)
        bpy.data.objects.remove(o, do_unlink=True)
    return target


def cut_below(obj, z):
    """Everything under `z` removed: the part of a colossus the ground has."""
    select_only(obj)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.mesh.bisect(plane_co=(0, 0, z), plane_no=(0, 0, 1), clear_inner=True, use_fill=True)
    bpy.ops.object.mode_set(mode="OBJECT")
    return obj


# --- weathering -------------------------------------------------------------

def weather(obj, seed=0, cracks=0.35, erosion=0.25, chips=0.3, scale=1.0):
    """Age on a stone: broad lumps, cracks along a Voronoi web, rain-cut runnels
    down its faces, chipped edges. Strengths are shares of the stone's own
    smallest dimension, so a pebble and a monolith weather alike."""
    random.seed(seed)
    mesh = obj.data
    dims = obj.dimensions
    size = max(min(dims.x, dims.y, dims.z), 0.05) * scale
    offset = Vector((random.uniform(-500, 500), random.uniform(-500, 500), random.uniform(-500, 500)))
    mesh.calc_loop_triangles()
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.normal_update()
    for v in bm.verts:
        p = v.co / max(size, 1e-3) + offset
        # Broad lumps first: the silhouette of a stone nobody squared.
        lump = noise.noise(p * 0.35) * 0.22 + noise.noise(p * 0.9) * 0.08
        # Voronoi edges: the distance between the two nearest cells, small on a crack.
        d = noise.voronoi(p * 1.8, distance_metric="DISTANCE", exponent=2.5)[0]
        crack = -math.exp(-max(d[1] - d[0], 0.0) * 14.0) * cracks * 0.22
        # Runnels: grooves running down the faces, strongest on steep sides.
        steep = 1.0 - abs(v.normal.z)
        runnel = -max(0.0, noise.noise(Vector((p.x * 3.0, p.y * 3.0, p.z * 0.3)))) * erosion * 0.14 * steep
        chip = -max(0.0, noise.noise(p * 2.6) - 0.15) * chips * 0.28
        v.co += v.normal * (lump + crack + runnel + chip) * size
    bm.to_mesh(mesh)
    bm.free()
    mesh.update()
    return obj


def painted_mass(obj, voxel=0.15, smooth=6, planar=0.0):
    """A shape reduced to its painted masses: voxel-remeshed, relaxed, and
    optionally cut into a few broad planes - fine scan detail gone, the
    silhouette kept. What a Frazetta rock or a Dean arch is drawn as."""
    select_only(obj)
    remesh = obj.modifiers.new("mass", "REMESH")
    remesh.mode = "VOXEL"
    remesh.voxel_size = voxel
    s = obj.modifiers.new("relax", "SMOOTH")
    s.iterations = smooth
    s.factor = 0.6
    apply_all(obj)
    if planar > 0:
        d = obj.modifiers.new("planes", "DECIMATE")
        d.decimate_type = "DISSOLVE"
        d.angle_limit = math.radians(planar)
        apply_all(obj)
    return obj


def reduce(obj, triangles):
    """Down to a triangle budget, keeping the outline."""
    obj.data.calc_loop_triangles()
    have = len(obj.data.loop_triangles)
    if have > triangles:
        d = obj.modifiers.new("budget", "DECIMATE")
        d.ratio = triangles / have
        apply_all(obj)
    return obj


def ground(obj, sink=0.0):
    """Stand it on z = 0 (its lowest point), then sink it by `sink` metres."""
    lowest = min((obj.matrix_world @ v.co).z for v in obj.data.vertices)
    obj.location.z -= lowest + sink
    select_only(obj)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    return obj


# --- surface ----------------------------------------------------------------

def scan(name):
    """A world scan's maps (assets/terrain/ph/<name>), largest first available."""
    folder = SCANS / name
    found = {}
    for kind in ("albedo", "normal", "properties"):
        for suffix in ("", "@2", "@4"):
            f = folder / f"ph_{name}_{kind}{suffix}.png"
            if f.exists():
                found[kind] = f
                break
    return found


def stone_material(name, scans, metres=2.0, tint=(1.0, 1.0, 1.0), moss_scan=None):
    """Box-projected scans (no UVs needed to look right), with moss on the
    upward faces from the moss scan, mixed by the geometry's own normal."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    coord = nodes.new("ShaderNodeTexCoord")
    mapping = nodes.new("ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = (1.0 / metres,) * 3
    links.new(coord.outputs["Object"], mapping.inputs["Vector"])

    def boxed(path, colour=True):
        t = nodes.new("ShaderNodeTexImage")
        t.image = bpy.data.images.load(str(path), check_existing=True)
        t.image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
        t.projection = "BOX"
        t.projection_blend = 0.3
        links.new(mapping.outputs["Vector"], t.inputs["Vector"])
        return t

    albedo = boxed(scans[0]["albedo"])
    colour = albedo.outputs["Color"]
    if len(scans) > 1:
        # A second scan in broad patches: a stone is never one photograph.
        second = boxed(scans[1]["albedo"])
        mask = nodes.new("ShaderNodeTexNoise")
        mask.inputs["Scale"].default_value = 0.35
        ramp = nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].position = 0.45
        ramp.color_ramp.elements[1].position = 0.6
        links.new(mask.outputs["Fac"], ramp.inputs["Fac"])
        mix = nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        links.new(ramp.outputs["Color"], mix.inputs["Factor"])
        links.new(colour, mix.inputs["A"])
        links.new(second.outputs["Color"], mix.inputs["B"])
        colour = mix.outputs["Result"]
    tinted = nodes.new("ShaderNodeMix")
    tinted.data_type = "RGBA"
    tinted.blend_type = "MULTIPLY"
    tinted.inputs["Factor"].default_value = 1.0
    tinted.inputs["B"].default_value = (*tint, 1.0)
    links.new(colour, tinted.inputs["A"])
    colour = tinted.outputs["Result"]
    if moss_scan:
        moss = boxed(moss_scan["albedo"])
        geometry = nodes.new("ShaderNodeNewGeometry")
        sep = nodes.new("ShaderNodeSeparateXYZ")
        links.new(geometry.outputs["Normal"], sep.inputs["Vector"])
        ramp = nodes.new("ShaderNodeValToRGB")
        ramp.color_ramp.elements[0].position = 0.55
        ramp.color_ramp.elements[1].position = 0.85
        links.new(sep.outputs["Z"], ramp.inputs["Fac"])
        mix = nodes.new("ShaderNodeMix")
        mix.data_type = "RGBA"
        links.new(ramp.outputs["Color"], mix.inputs["Factor"])
        links.new(colour, mix.inputs["A"])
        links.new(moss.outputs["Color"], mix.inputs["B"])
        colour = mix.outputs["Result"]
    links.new(colour, bsdf.inputs["Base Color"])
    if "normal" in scans[0]:
        nmap = nodes.new("ShaderNodeNormalMap")
        links.new(boxed(scans[0]["normal"], colour=False).outputs["Color"], nmap.inputs["Color"])
        nmap.inputs["Strength"].default_value = 0.6
        links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    bsdf.inputs["Roughness"].default_value = 0.85
    return mat


def bake_textures(obj, folder, stem, size=1024):
    """The procedural material baked into the object's own UVs: albedo and a
    tangent normal map, written as PNG, the material replaced by the baked one."""
    select_only(obj)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.02)
    bpy.ops.object.mode_set(mode="OBJECT")
    source = obj.active_material
    nodes = source.node_tree.nodes
    images = {}
    for kind, bake_type, colour in (("albedo", "DIFFUSE", True), ("normal", "NORMAL", False)):
        image = bpy.data.images.new(f"{stem}_{kind}", size, size)
        image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
        target = nodes.new("ShaderNodeTexImage")
        target.image = image
        nodes.active = target
        if bake_type == "DIFFUSE":
            bpy.ops.object.bake(type="DIFFUSE", pass_filter={"COLOR"}, margin=8)
        else:
            bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", margin=8)
        folder.mkdir(parents=True, exist_ok=True)
        path = folder / f"{stem}_{kind}.png"
        image.filepath_raw = str(path)
        image.file_format = "PNG"
        image.save()
        images[kind] = path
        nodes.remove(target)
    baked = bpy.data.materials.new(stem)
    baked.use_nodes = True
    n, l = baked.node_tree.nodes, baked.node_tree.links
    bsdf = n.get("Principled BSDF")
    t = n.new("ShaderNodeTexImage")
    t.image = bpy.data.images.load(str(images["albedo"]))
    l.new(t.outputs["Color"], bsdf.inputs["Base Color"])
    nt = n.new("ShaderNodeTexImage")
    nt.image = bpy.data.images.load(str(images["normal"]))
    nt.image.colorspace_settings.name = "Non-Color"
    nm = n.new("ShaderNodeNormalMap")
    l.new(nt.outputs["Color"], nm.inputs["Color"])
    l.new(nm.outputs["Normal"], bsdf.inputs["Normal"])
    bsdf.inputs["Roughness"].default_value = 0.85
    obj.data.materials.clear()
    obj.data.materials.append(baked)
    return images


# --- export -----------------------------------------------------------------

def export(asset, variants, source, license="proprietary-owned", author="generated", tags=(), category="rock",
           extra=None):
    """Every variant one mesh node, written as assets/generated/simplified_models/
    <asset>/<asset>.gltf with its source.json and asset record."""
    folder = OUT / asset
    folder.mkdir(parents=True, exist_ok=True)
    for o in bpy.context.scene.objects:
        o.select_set(o in variants)
    bpy.context.view_layer.objects.active = variants[0]
    bpy.ops.export_scene.gltf(filepath=str(folder / (asset + ".gltf")), export_format="GLTF_SEPARATE",
                              use_selection=True, export_texture_dir="textures", export_yup=True,
                              export_apply=True, export_normals=True, export_materials="EXPORT")
    dims = [max(abs(v) for o in variants for v in o.dimensions)]
    record = {"site": "procedural", "asset_id": asset, "name": asset, "license": license,
              "authors": [author], "role": category, "source": source, "variants": len(variants),
              "dimensions_metres": [round(float(d), 3) for d in variants[0].dimensions]}
    if extra:
        record.update(extra)
    (folder / "source.json").write_text(json.dumps(record, indent=2) + "\n")
    meta = {"name": asset, "source": source, "author": author, "license": license,
            "modifications": ["generated in Blender by " + source.split(" ")[0]], "category": category,
            "tags": list(tags)}
    (folder / (asset + ".meta.json")).write_text(json.dumps(meta, indent=2) + "\n")
    return folder


def arguments():
    """The script's own arguments: what follows `--` on Blender's command line."""
    import sys
    return sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
