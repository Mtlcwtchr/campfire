"""This game's megaliths, for the 1960s-80s fantasy look (doc/references/artstyle):
the monumental stone that stands in a Frazetta, Dean, Moebius or Pennington
landscape - weathered, overgrown, too big for whoever raised it.

Game content built on the engine's Blender toolkit (asr_blender.py). Each
archetype is an asset of a few variants (one mesh node each), written to
assets/generated/simplified_models/megalith_<kind>/ for
tools/prepare_environment_models.py; the painted feature layer's recipes
(content/config/environment/recipes/megaliths.json) set them in the world.

    blender -b -P tools/blender/megaliths.py -- --kind menhir --variants 4 --seed 1
    blender -b -P tools/blender/megaliths.py -- --all
    blender -b -P tools/blender/megaliths.py -- --kind colossus_head --preview /tmp/head.png
"""
import argparse
import math
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import asr_blender as ab  # noqa: E402

import bpy  # noqa: E402
from mathutils import Vector  # noqa: E402

GREY = [ab.scan("rock_face_03"), ab.scan("cliff_side")]
DARK = [ab.scan("cliff_side"), ab.scan("rocks_ground_05")]
WARM = [ab.scan("rock_face_03"), ab.scan("red_laterite_soil_stones")]
MOSS = ab.scan("mossy_rock")


def stone(obj, scans, seed, metres=2.0, tint=(1, 1, 1), moss=True, weathering=(0.35, 0.25, 0.3)):
    ab.weather(obj, seed=seed, cracks=weathering[0], erosion=weathering[1], chips=weathering[2])
    obj.data.materials.clear()
    obj.data.materials.append(ab.stone_material(obj.name + "_m", scans, metres, tint, MOSS if moss else None))
    return obj


# --- archetypes ---------------------------------------------------------------
# Each makes one variant from a random generator and returns the object, its
# foot on z = 0. Sizes are metres: these are meant to dwarf a person.

def menhir(rng, seed):
    h = rng.uniform(4.5, 9.0)
    o = ab.block("menhir", (rng.uniform(1.6, 2.6), rng.uniform(1.0, 1.6), h), (0, 0, h / 2),
                 (rng.uniform(-0.08, 0.08), rng.uniform(-0.08, 0.08), rng.uniform(0, math.pi)),
                 taper=rng.uniform(0.15, 0.4), round_edges=0.25, subdivisions=4)
    return ab.ground(stone(o, GREY, seed, weathering=(0.4, 0.45, 0.35)), sink=0.6)


def fallen_menhir(rng, seed):
    h = rng.uniform(4.0, 7.5)
    o = ab.block("fallen", (rng.uniform(1.0, 1.6), rng.uniform(0.6, 1.0), h), (0, 0, 0),
                 (math.pi / 2 + rng.uniform(-0.15, 0.15), 0, rng.uniform(0, math.pi)), taper=0.3, round_edges=0.2)
    return ab.ground(stone(o, GREY, seed, weathering=(0.45, 0.3, 0.45)), sink=0.35)


def lintel(rng, seed):
    w = rng.uniform(4.0, 6.0)
    o = ab.block("lintel", (w, rng.uniform(1.0, 1.4), rng.uniform(0.8, 1.1)), (0, 0, 0), round_edges=0.18)
    return ab.ground(stone(o, GREY, seed, weathering=(0.35, 0.2, 0.4)))


def capstone(rng, seed):
    o = ab.block("capstone", (rng.uniform(4.0, 6.5), rng.uniform(3.0, 4.5), rng.uniform(0.7, 1.1)), (0, 0, 0),
                 (0, 0, rng.uniform(0, math.pi)), round_edges=0.3)
    return ab.ground(stone(o, DARK, seed, weathering=(0.5, 0.2, 0.5)))


def cyclopean_block(rng, seed):
    """An irregular fitted block of a giant's wall: polygonal faces, not a brick."""
    o = ab.block("cyclopean", (rng.uniform(2.5, 4.5), rng.uniform(2.0, 3.0), rng.uniform(2.0, 3.5)), (0, 0, 0),
                 (0, 0, rng.uniform(-0.2, 0.2)), round_edges=0.08)
    ab.painted_mass(o, voxel=0.18, smooth=2, planar=12)
    return ab.ground(stone(o, WARM, seed, weathering=(0.25, 0.2, 0.25)))


def giant_stair(rng, seed):
    """A flight of steps for someone four times our size, half sunk and broken."""
    pieces = []
    steps = rng.randint(4, 7)
    rise, tread, width = rng.uniform(1.4, 1.9), rng.uniform(2.0, 2.6), rng.uniform(6.0, 9.0)
    for k in range(steps):
        if rng.random() < 0.12:
            continue   # a step gone
        pieces.append(ab.block(f"step{k}", (width * rng.uniform(0.9, 1.0), tread, rise * (k + 1)),
                               (rng.uniform(-0.3, 0.3), k * tread, rise * (k + 1) / 2), round_edges=0.06))
    o = ab.union(pieces[0], pieces[1:]) if len(pieces) > 1 else pieces[0]
    o.name = "stair"
    return ab.ground(stone(o, WARM, seed, metres=3.0, weathering=(0.5, 0.35, 0.45)), sink=0.5)


def limb(a, b, radius, stiffness=2.0):
    """A metaball capsule from a to b: a finger joint, a nose bridge."""
    a, b = Vector(a), Vector(b)
    return {"type": "CAPSULE", "co": (a + b) / 2, "radius": radius, "size": ((b - a).length / 2, 1, 1),
            "rotation": (b - a).to_track_quat("X", "Z"), "stiffness": stiffness}


def colossus_head(rng, seed):
    """A carved head half sunk in the ground - the Pennington colossus: one
    mass of skull and jaw with the brow, nose, lips and chin modelled out of
    it and the eye sockets and the mouth cut in, weathered as one stone."""
    s = rng.uniform(6.0, 10.0)
    f = -0.48 * s   # the plane of the face (towards -y)
    balls = [((0, 0, 0.55 * s), 0.62 * s, (0.92, 1.0, 1.15)),                     # cranium
             ((0, -0.15 * s, 0.05 * s), 0.48 * s, (0.85, 0.95, 0.9)),              # jaw and cheeks
             {"co": (0, f, 0.66 * s), "radius": 0.2 * s, "size": (1.7, 0.7, 0.45)},   # brow ridge
             limb((0, f + 0.02 * s, 0.6 * s), (0, f - 0.12 * s, 0.28 * s), 0.09 * s),  # nose bridge
             {"co": (0, f - 0.1 * s, 0.26 * s), "radius": 0.11 * s, "size": (1.4, 1.0, 0.8)},  # nostrils
             {"co": (0, f - 0.02 * s, 0.1 * s), "radius": 0.12 * s, "size": (1.6, 0.8, 0.5)},   # lips
             {"co": (0, f + 0.04 * s, -0.12 * s), "radius": 0.16 * s, "size": (1.2, 0.9, 0.8)},  # chin
             {"co": (0.48 * s, -0.05 * s, 0.38 * s), "radius": 0.14 * s, "size": (0.45, 0.9, 1.4)},  # ears
             {"co": (-0.48 * s, -0.05 * s, 0.38 * s), "radius": 0.14 * s, "size": (0.45, 0.9, 1.4)},
             # Carved in: the eye sockets under the brow, the line of the mouth.
             {"co": (0.2 * s, f + 0.05 * s, 0.5 * s), "radius": 0.11 * s, "size": (1.3, 1.0, 0.75), "negative": True},
             {"co": (-0.2 * s, f + 0.05 * s, 0.5 * s), "radius": 0.11 * s, "size": (1.3, 1.0, 0.75), "negative": True},
             {"co": (0, f - 0.06 * s, 0.1 * s), "radius": 0.06 * s, "size": (2.6, 1.0, 0.25), "negative": True}]
    o = ab.keep_largest(ab.metaball_mass("head", balls, resolution=0.03 * s))
    # A tilt and a turn of its own; facing roughly -y, the way the recipe
    # yaws it to look out over its ground.
    o.rotation_euler = (rng.uniform(-0.25, 0.05), rng.uniform(-0.25, 0.25), rng.uniform(-0.6, 0.6))
    ab.select_only(o)
    bpy.ops.object.transform_apply(rotation=True)
    ab.cut_below(o, rng.uniform(-0.05, 0.15) * s)
    o = ab.ground(o)
    return stone(o, WARM if rng.random() < 0.5 else GREY, seed, metres=3.0, weathering=(0.3, 0.5, 0.2))


def colossus_hand(rng, seed):
    """A stone hand rising out of the ground, fingers curled - palm, joints and
    thumb as one soft mass."""
    s = rng.uniform(4.0, 7.0)
    balls = [{"co": (0, 0, 0.45 * s), "radius": 0.42 * s, "size": (1.2, 0.5, 1.25)},     # palm
             limb((0, 0.05 * s, -0.3 * s), (0, 0.02 * s, 0.25 * s), 0.3 * s)]            # wrist into the ground
    for k, (dx, curl, length) in enumerate(((-0.36, 0.75, 0.9), (-0.12, 0.5, 1.0), (0.12, 0.4, 0.95), (0.34, 0.6, 0.75))):
        joint = Vector((dx * s, -0.02 * s, 0.72 * s))
        # A knuckle sunk in the palm, so the finger grows out of it.
        balls.append({"co": joint, "radius": 0.15 * s, "size": (1.0, 1.0, 1.0)})
        a = joint + Vector((0, -0.12 * s * curl, 0.38 * s * length))
        b = a + Vector((0, -0.3 * s * curl, 0.3 * s * length * (1 - curl * 0.6)))
        c = b + Vector((0, -0.22 * s * curl, 0.12 * s * length * (1 - curl)))
        balls += [limb(joint, a, 0.14 * s), limb(a, b, 0.125 * s), limb(b, c, 0.11 * s)]
    t0 = Vector((0.42 * s, -0.08 * s, 0.35 * s))
    t1 = t0 + Vector((0.18 * s, -0.25 * s, 0.3 * s))
    balls += [limb(t0, t1, 0.14 * s), limb(t1, t1 + Vector((0.02 * s, -0.22 * s, 0.18 * s)), 0.11 * s)]
    o = ab.keep_largest(ab.metaball_mass("hand", balls, resolution=0.025 * s, threshold=0.45))
    o.rotation_euler = (rng.uniform(-0.3, 0.3), rng.uniform(-0.2, 0.2), rng.uniform(-0.6, 0.6))
    ab.select_only(o)
    bpy.ops.object.transform_apply(rotation=True)
    ab.cut_below(o, 0.0)
    o = ab.ground(o)
    return stone(o, GREY, seed, metres=2.5, weathering=(0.35, 0.4, 0.25))


def needle(rng, seed):
    """A basalt needle: a tall columnar spire, the Reynisdrangar silhouette."""
    h = rng.uniform(18.0, 40.0)
    columns = []
    for k in range(rng.randint(5, 9)):
        r = rng.uniform(0.8, 1.6)
        hk = h * rng.uniform(0.55, 1.0)
        bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=r, depth=hk,
                                            location=(rng.uniform(-1.6, 1.6), rng.uniform(-1.6, 1.6), hk / 2))
        c = bpy.context.active_object
        # Rings up the column, so the spire can taper and lean along it.
        ab.select_only(c)
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.subdivide(number_cuts=int(hk / 1.5))
        bpy.ops.object.mode_set(mode="OBJECT")
        columns.append(c)
    o = ab.union(columns[0], columns[1:])
    o.name = "needle"
    lean = rng.uniform(-2.5, 2.5)
    for v in o.data.vertices:
        t = max(0.0, v.co.z / h)
        v.co.x *= 1.0 - 0.55 * t
        v.co.y *= 1.0 - 0.55 * t
        v.co.x += t * t * lean
    return ab.ground(stone(o, DARK, seed, metres=4.0, tint=(0.42, 0.42, 0.46), moss=True,
                           weathering=(0.2, 0.6, 0.2)), sink=1.0)


def portal_tower(rng, seed):
    """The tall gate-tower on its hill (the Dean/Hildebrandt doorway): a
    block with an arch cut through it."""
    w, d, h = rng.uniform(5.0, 7.0), rng.uniform(4.0, 5.5), rng.uniform(14.0, 22.0)
    o = ab.block("tower", (w, d, h), (0, 0, h / 2), taper=0.08, round_edges=0.05)
    arch_h = h * rng.uniform(0.45, 0.6)
    bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=w * 0.22, depth=d * 3, location=(0, 0, arch_h),
                                        rotation=(math.pi / 2, 0, 0))
    top = bpy.context.active_object
    cut = ab.block("cut", (w * 0.44, d * 3, arch_h), (0, 0, arch_h / 2), round_edges=0.0, subdivisions=0)
    cutter = ab.union(cut, [top])
    m = o.modifiers.new("arch", "BOOLEAN")
    m.operation = "DIFFERENCE"
    m.object = cutter
    m.solver = "EXACT"
    ab.apply_all(o)
    bpy.data.objects.remove(cutter, do_unlink=True)
    return ab.ground(stone(o, GREY, seed, metres=3.0, weathering=(0.3, 0.35, 0.3)))


ARCHETYPES = {
    "menhir": (menhir, 3000), "fallen_menhir": (fallen_menhir, 3000), "lintel": (lintel, 2500),
    "capstone": (capstone, 3000), "cyclopean_block": (cyclopean_block, 2500), "giant_stair": (giant_stair, 6000),
    "colossus_head": (colossus_head, 9000), "colossus_hand": (colossus_hand, 8000), "needle": (needle, 6000),
    "portal_tower": (portal_tower, 6000),
}


def build(kind, variants, seed, bake=True, size=1024, preview=None):
    ab.reset()
    # A fresh folder: textures of an earlier run must not ride along.
    import shutil
    shutil.rmtree(ab.OUT / ("megalith_" + kind), ignore_errors=True)
    make, budget = ARCHETYPES[kind]
    made = []
    for v in range(variants):
        rng = random.Random(seed * 1000 + v)
        obj = make(rng, seed * 1000 + v)
        obj.name = f"{kind}_{v}"
        ab.reduce(obj, budget)
        obj.location.x = v * 30.0   # apart while baking; the pipeline frames each node
        ab.select_only(obj)
        bpy.ops.object.transform_apply(location=True)
        made.append(obj)
    asset = "megalith_" + kind
    if bake:
        import subprocess
        for obj in made:
            maps = ab.bake_textures(obj, ab.OUT / asset / "textures", obj.name, size)
            # Painted towards the references (tools/stylize_textures.py, preset
            # megalith_painted): outside Blender, whose Python has no PIL.
            for kind in ("albedo", "normal"):
                subprocess.run(["python3", str(ab.ROOT / "tools" / "stylize_textures.py"), "--preset", "megalith_painted",
                                "--kind", kind, str(maps[kind]), str(maps[kind])], check=False)
    # Each variant back to the origin: one node a variant, foot at z = 0.
    for obj in made:
        centre = sum((v.co for v in obj.data.vertices), Vector()) / max(len(obj.data.vertices), 1)
        for v in obj.data.vertices:
            v.co.x -= centre.x
            v.co.y -= centre.y
    folder = ab.export(asset, made, f"tools/blender/megaliths.py --kind {kind} --seed {seed}",
                       tags=["megalith", kind], category="megalith")
    if preview:
        render_preview(made, preview)
    print(f"{asset}: {len(made)} variant(s) -> {folder}")
    return folder


def render_preview(objects, path):
    """A quick look: the variants side by side under a low warm sun, framed to
    fit, seen three-quarters from the front (-y, where a face looks)."""
    scene = bpy.context.scene
    x = 0.0
    tallest = 0.0
    for o in objects:
        w = max(o.dimensions.x, o.dimensions.y)
        o.location = (x + w / 2, 0, 0)
        x += w * 1.25 + 2.0
        tallest = max(tallest, o.dimensions.z)
    engines = {e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items}
    scene.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in engines else "BLENDER_EEVEE"
    bpy.ops.object.light_add(type="SUN", rotation=(math.radians(50), 0, math.radians(-35)))
    sun = bpy.context.active_object.data
    sun.energy = 4.0
    sun.color = (1.0, 0.9, 0.75)
    world = bpy.data.worlds.new("sky")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.32, 0.45, 0.75, 1)
    scene.world = world
    bpy.ops.mesh.primitive_plane_add(size=1000, location=(x / 2, 0, 0))
    centre = Vector((x / 2, 0, tallest / 2))
    direction = Vector((0.35, -1.0, 0.35)).normalized()
    bpy.ops.object.camera_add(location=centre + direction * (x + tallest) * 2)
    cam = bpy.context.active_object
    cam.rotation_mode = "QUATERNION"
    cam.rotation_quaternion = (-direction).to_track_quat("-Z", "Y")
    cam.data.type = "ORTHO"
    aspect = 1280 / 540
    cam.data.ortho_scale = max(x * 1.05, tallest * 1.25 * aspect)
    scene.camera = cam
    scene.render.resolution_x, scene.render.resolution_y = 1280, 540
    scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=sorted(ARCHETYPES))
    parser.add_argument("--all", action="store_true")
    parser.add_argument("--variants", type=int, default=4)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--size", type=int, default=1024)
    parser.add_argument("--no-bake", action="store_true")
    parser.add_argument("--preview")
    args = parser.parse_args(ab.arguments())
    kinds = sorted(ARCHETYPES) if args.all else [args.kind]
    if not kinds or kinds == [None]:
        parser.error("--kind or --all")
    for kind in kinds:
        preview = None
        if args.preview:
            p = Path(args.preview)
            preview = p if len(kinds) == 1 else p.with_name(p.stem + "_" + kind + p.suffix)
        build(kind, args.variants, args.seed, bake=not args.no_bake, size=args.size, preview=preview)


if __name__ == "__main__":
    main()
