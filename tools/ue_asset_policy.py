"""CPU-only transfer limits matching AncientSettlement/Tools/Editor/cut_assets.py.
These are rejection limits, never a request to load or simplify a huge original.
UE's cut tool rewrites source LOD0; HiRes and automatic Nanite fallback are not
interchangeable with that prepared source. UE/Oodle compression is not PNG/GPU compression.
"""
from pathlib import Path
import struct

TRIANGLE_LIMITS = {"CommonTree_1": 10000, "Pine_1": 10000, "Bush_Common": 3000,
                   "Rock_Medium_1": 2500, "Mushroom_Common": 300,
                   "Deadwood_Log": 2000, "Deadwood_Stump": 1500, "Deadwood_Branch": 600, "Grass_1": 800}
SCENE_ROLES = tuple(role for role in TRIANGLE_LIMITS if role != "Grass_1")
VISUAL_PREFIXES = {"CommonTree_1": "CommonTree_1/", "Pine_1": "Pine_1/",
                   "Bush_Common": "Bush_Common/", "Rock_Medium_1": "Rock_Boulder/",
                   "Mushroom_Common": "Forest_Mushroom/", "Deadwood_Log": "Deadwood_Log/",
                   "Deadwood_Stump": "Deadwood_Stump/", "Deadwood_Branch": "Deadwood_Branch/"}
MAX_TEXTURE_EDGE = 2048
MAX_TEXTURES = 48
MAX_EXPORT_BYTES = 256 * 1024 * 1024


def check_leaf_materials(role, materials, used):
    if role not in ("CommonTree_1", "Pine_1"):
        return
    leaves = {i for i, material in enumerate(materials) if material and
              float(material.get("scalars", {}).get("IsLeaves", 0)) > .5}
    if not leaves or not leaves.intersection(used):
        raise ValueError(f"{role}: foliage material lost; check section-to-material mapping")


def check_triangles(role, count):
    if role not in TRIANGLE_LIMITS or not 0 < count <= TRIANGLE_LIMITS[role]:
        raise ValueError(f"{role}: {count} triangles; transfer only the already-cut LOD0, limit {TRIANGLE_LIMITS.get(role, 0)}")


def check_texture_size(width, height):
    if not 0 < width <= MAX_TEXTURE_EDGE or not 0 < height <= MAX_TEXTURE_EDGE:
        raise ValueError(f"Texture {width}x{height} is not a prepared <=2K texture")


def check_png(path):
    with Path(path).open("rb") as stream:
        header = stream.read(26)
    if len(header) != 26 or header[:8] != b"\x89PNG\r\n\x1a\n" or header[12:16] != b"IHDR":
        raise ValueError("Invalid exported PNG")
    check_texture_size(*struct.unpack_from(">II", header, 16))
    if header[24] != 8:
        raise ValueError("Exported texture is not the cut tool's 8-bit source")


def check_export_bytes(root, additional=0, replacing=None):
    total = sum(p.stat().st_size for p in Path(root).rglob("*") if p.is_file())
    if replacing is not None and Path(replacing).is_file():
        total -= Path(replacing).stat().st_size
    if total + additional > MAX_EXPORT_BYTES:
        raise ValueError("UE transfer exceeds the 256 MiB staging budget; do not export raw pack contents")
    return total


def selected_textures(material):
    """Only channels consumed by our material, not all inherited parameters.
    Explicit bindings resolve ambiguity; guessing among layered atlases is unsafe.
    """
    values = {k: v for k, v in material["textures"].items() if v and v != "None"}
    if material["asset"].startswith("/Game/Megaplant_Static/Materials/"):
        return {"albedo": values["Albedo"], "normal": values["PackedNormal"]}
    if material["asset"].startswith("/Game/Light_Foliage/Materials/"):
        return {"colour_mask": values["Mask"], "normal": values["Normal"]}
    if material["asset"].startswith("/Game/MWLandscapeAutoMaterial/Materials/Plants/"):
        result = {"albedo": values["MW_TextureBaseColor"], "normal": values["MW_TextureNormal"]}
        if material.get("switches", {}).get("MW_[x]_UseSeparateOpacityTexture", False):
            result["opacity"] = values["MW_TextureOpacity"]
        return result
    aliases = {"albedo": {"albedo", "basecolor", "basecolour", "diffuse", "diffusetexture", "albedotexture", "basecolortexture"},
               "normal": {"normal", "normalmap", "normaltexture"},
               "opacity": {"opacity", "opacitymask", "opacitytexture"}}
    result = {}
    for channel, names in aliases.items():
        explicit = material.get("bindings", {}).get(channel)
        if explicit is not None:
            result[channel] = values[explicit]
            continue
        matches = {value for name, value in values.items()
                   if "".join(c for c in name.lower() if c.isalnum()) in names}
        if len(matches) == 1:
            result[channel] = matches.pop()
        elif matches or channel == "albedo":
            raise ValueError(f"{material['asset']}: explicit {channel} binding required")
    return result

