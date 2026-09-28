#!/usr/bin/env python3
"""fetch_terrain - brings down the source textures the terrain is made of.

What it is for: the material library is a list of asset ids in
content/config/terrain_materials.json, and this turns those ids into files on
disk. Nothing here knows a filename. Poly Haven publishes an endpoint that says
what files an asset actually has -

    https://api.polyhaven.com/files/{asset_id}

- and it is asked every time, so an asset gaining a map or changing a naming
convention is not a thing anybody has to notice. Written-down filenames rot;
this cannot.

The maps taken are the ones a terrain surface needs: Diffuse, Normal GL,
Displacement, Roughness, AO, and the Mask where the asset has one. Not Metal:
ground is not a metal and a metal map on it is a channel of zeros costing a
quarter of the memory.

Resolution is one flag. The sources may as well be kept at 4K - they are
sources - while what the game loads is built from them at 2K by
tools/pack_terrain.py.

    python3 tools/fetch_terrain.py                  # 2K, everything missing
    python3 tools/fetch_terrain.py --res 4k
    python3 tools/fetch_terrain.py --only grass_lush soil_base
    python3 tools/fetch_terrain.py --water          # the OpenGameArt water set
    python3 tools/fetch_terrain.py --check          # say what is missing, take nothing

Every download is checked against the md5 the API gives, and a file already on
disk with the right md5 is not fetched again - so this is safe to re-run and
safe to interrupt.

Beside each asset it writes source.json: where the picture came from, its
licence, who has to be credited, and how big a piece of the world it is a
picture of. That last one is not decoration - it is what says whether the
material's world_scale is honest, and tools/pack_terrain.py complains when the
two disagree.
"""
import argparse
import hashlib
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MATERIALS = ROOT / "content/config/terrain_materials.json"
WATER = ROOT / "content/config/water_textures.json"
TERRAIN = ROOT / "assets/terrain"

API = "https://api.polyhaven.com"
AGENT = "asr-terrain-fetch/1 (+local build tool)"

# What a ground surface needs, and what it is called here. Poly Haven's own
# names on the left; ours on the right, because "nor_gl" is not a thing anybody
# should have to remember downstream.
WANTED = {
    "Diffuse": "diffuse",
    "nor_gl": "normal",
    "Displacement": "displacement",
    "Rough": "rough",
    "AO": "ao",
    "Mask": "mask",          # only some assets have one
}
OPTIONAL = {"Mask"}


def get(url, binary=False):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(request, timeout=120) as answer:
        raw = answer.read()
    return raw if binary else json.loads(raw)


def digest(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def take(url, into, md5, size, quiet=False):
    """Fetch unless it is already here and right."""
    if into.exists() and (md5 is None or digest(into) == md5):
        return False
    into.parent.mkdir(parents=True, exist_ok=True)
    if not quiet:
        print("      %-14s %6.1f MB" % (into.name, (size or 0) / 1e6), end="", flush=True)
    raw = get(url, binary=True)
    if md5 is not None and hashlib.md5(raw).hexdigest() != md5:
        raise RuntimeError("%s came down wrong - the md5 does not match" % into.name)
    into.write_bytes(raw)
    if not quiet:
        print("  ok")
    return True


def pick(files, kind, res, prefer=("png", "jpg")):
    """One file out of the API's map -> resolution -> format tree."""
    by_res = files.get(kind)
    if not by_res:
        return None
    at = by_res.get(res)
    if at is None:
        # Some assets do not have every size. Take the nearest one above.
        ladder = ["1k", "2k", "4k", "8k"]
        after = [r for r in ladder if r in by_res and ladder.index(r) >= ladder.index(res)]
        if not after:
            return None
        at = by_res[after[0]]
    for fmt in prefer:
        if fmt in at:
            return at[fmt]
    return None


def polyhaven(material, res, check, quiet):
    asset = material["source"]["asset_id"]
    into = TERRAIN / material["group"] / "src"
    print("  %-24s %s" % (material["id"], asset))

    try:
        files = get("%s/files/%s" % (API, asset))
    except urllib.error.HTTPError as bad:
        print("      the api says %s - is the asset id right?" % bad.code)
        return False
    info = {}
    try:
        info = get("%s/info/%s" % (API, asset))
    except urllib.error.HTTPError:
        pass

    missing, got = [], 0
    chosen = {}
    for kind, name in WANTED.items():
        entry = pick(files, kind, res)
        if entry is None:
            if kind not in OPTIONAL:
                missing.append(kind)
            continue
        target = into / ("%s_%s.%s" % (material["id"], name, entry["url"].rsplit(".", 1)[-1]))
        chosen[name] = target.name
        if check:
            if not target.exists():
                missing.append(kind + " (not here yet)")
            continue
        if take(entry["url"], target, entry.get("md5"), entry.get("size"), quiet):
            got += 1

    if missing:
        print("      missing: %s" % ", ".join(missing))
    if check:
        return not missing

    # Poly Haven gives the real size of the thing photographed, in millimetres.
    # It is the only honest answer to "how many metres is one turn of this",
    # and world_scale in the library is checked against it.
    size = info.get("dimensions")
    metres = [round(v / 1000.0, 3) for v in size] if size else None
    (into / "source.json").write_text(json.dumps({
        "site": "polyhaven",
        "asset_id": asset,
        "asset_url": "https://polyhaven.com/a/%s" % asset,
        "license": info.get("license", material["source"].get("license", "CC0")),
        "authors": sorted(info.get("authors", {}).keys()) or None,
        "attribution_required": bool(material["source"].get("attribution_required")),
        "original_physical_metres": metres,
        "runtime_world_scale": material.get("world_scale"),
        "resolution": res,
        "files": chosen,
    }, indent=2, ensure_ascii=False) + "\n")
    if got == 0 and not quiet:
        print("      already here")
    return True


def opengameart(entry, check, quiet):
    into = TERRAIN / entry["group"] / "src"
    target = into / entry["file"]
    print("  %-24s %s" % (entry["id"], entry["file"]))
    if check:
        if not target.exists():
            print("      not here yet")
            return False
        return True
    if not target.exists():
        take(entry["source"]["url"], target, None, None, quiet)
    else:
        print("      already here")
    (into / "source.json").write_text(json.dumps({
        "site": "opengameart",
        "pack": entry["source"]["pack"],
        "pack_url": entry["source"]["pack_url"],
        "author": entry["source"]["author"],
        "license": entry["source"]["license"],
        "attribution_required": True,
        "kind": entry["kind"],
        "used_for": entry.get("used_for"),
        "files": {entry["kind"]: entry["file"]},
    }, indent=2, ensure_ascii=False) + "\n")
    return True


def credits(materials, water):
    """The licence page, written from the same files the downloader reads.

    Poly Haven's CC0 asks for nothing, and is listed anyway: a licence audit
    that only lists what is legally required is an audit that cannot tell "we
    checked and it is free" from "we did not check".
    """
    lines = ["# Credits and licences", "",
             "Written by `tools/fetch_terrain.py` from `content/config/*.json`.",
             "Do not edit by hand - edit the source entry and run the fetcher.", ""]
    need = [w for w in water if w["source"].get("attribution_required")]
    if need:
        lines += ["## Required attribution", ""]
        packs = {}
        for w in need:
            packs.setdefault((w["source"]["pack"], w["source"]["pack_url"],
                              w["source"]["author"], w["source"]["license"]), []).append(w["file"])
        for (pack, url, author, lic), files in sorted(packs.items()):
            lines += ["**%s**" % pack,
                      "",
                      "- Author: %s" % author,
                      "- License: %s" % lic,
                      "- Source: %s" % url,
                      "- Files used: %s" % ", ".join(sorted(files)),
                      ""]
    lines += ["## Public domain (CC0, no attribution required)", "",
              "Terrain surfaces from Poly Haven (<https://polyhaven.com>):", ""]
    for m in sorted((m for m in materials if m["source"].get("site") == "polyhaven"), key=lambda x: x["id"]):
        lines.append("- `%s` - %s" % (m["id"], m["source"]["asset_id"]))
    lines.append("")
    imported = [m for m in materials if m["source"].get("site") == "unreal"]
    if imported:
        lines += ["## Imported AncientSettlement resources (not CC0)", "",
                  "Original asset-pack licences apply. No redistribution rights are granted by this repository.",
                  "Terrain: MW Landscape Auto Material. Nature: Megaplant, Light Foliage, Fab/Megascans and Namaqualand.",
                  "Per-asset UE package paths and material conversions are recorded in",
                  "`assets/generated/scene_models/provenance.json`.", ""]
        for m in sorted(imported, key=lambda x: x["id"]):
            lines.append("- `%s` - `%s`" % (m["id"], m["source"]["package"]))
        scene = ROOT / "assets/generated/scene_models"
        if (scene / ".ue-imported").exists():
            for model in json.loads((scene / "provenance.json").read_text())["models"]:
                lines.append("- `%s` - `%s`" % (model["role"], model["asset"]))
        lines.append("")
    (ROOT / "CREDITS.md").write_text("\n".join(lines))
    print("wrote CREDITS.md")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--res", default="2k", choices=["1k", "2k", "4k", "8k"],
                    help="which size to take from Poly Haven (default 2k)")
    ap.add_argument("--only", nargs="*", metavar="ID", help="just these material ids")
    ap.add_argument("--water", action="store_true", help="the OpenGameArt water set as well")
    ap.add_argument("--check", action="store_true", help="say what is missing and take nothing")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    materials = [m for m in json.loads(MATERIALS.read_text())
                 if m.get("source", {}).get("site") == "polyhaven"]
    water = json.loads(WATER.read_text())
    if args.only:
        wanted = set(args.only)
        materials = [m for m in materials if m["id"] in wanted]
        water = [w for w in water if w["id"] in wanted]
        if not materials and not water:
            print("no material called that")
            return 2

    print("%s %d terrain materials at %s into %s" %
          ("checking" if args.check else "fetching", len(materials), args.res,
           TERRAIN.relative_to(ROOT)))
    ok = True
    for m in materials:
        try:
            ok &= polyhaven(m, args.res, args.check, args.quiet)
        except Exception as bad:               # one bad asset must not stop the rest
            print("      %s" % bad)
            ok = False
    if args.water or args.only:
        print("water:")
        for w in water:
            try:
                ok &= opengameart(w, args.check, args.quiet)
            except Exception as bad:
                print("      %s" % bad)
                ok = False
    if not args.check:
        credits(json.loads(MATERIALS.read_text()), json.loads(WATER.read_text()))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

