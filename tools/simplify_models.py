#!/usr/bin/env python3
"""simplify_models - the downloaded source models reduced to import budgets.

Reads content/config/scene_model_sources.json (the same list tools/fetch_models.py
fetches), runs the native reducer `model_simplify` (tools/model_simplify.cpp,
meshoptimizer) on every model and writes

    assets/generated/simplified_models/<asset_id>/<asset_id>.gltf  (+ .bin, textures/)
    assets/generated/simplified_models/report.json

Budgets are per variant (one mesh per plant in the source file) and per role,
overridable per model with "tris" in the config. They are the import limits the
engine already uses for its own roles (tools/ue_asset_policy.py), except trees:
a Poly Haven crown is thirty thousand separate leaves, or eight hundred thousand
twig cards for the fir, and below these numbers the crown has to be thinned so
hard that it stops matching its own silhouette (see D168 for the measurements).

Leaves: Poly Haven ships the leaf colour as a JPEG and the cut-out as a separate
map the glTF never references, so the glTF draws every leaf as a solid card.
Here the two are merged into one RGBA PNG and the material becomes an alpha-cut
(MASK) one - the same for the pine's twig cards, which the source marks opaque.
Other textures are hard-linked, not copied.

    python3 tools/simplify_models.py                       # everything
    python3 tools/simplify_models.py --only fern_02 fir_tree_01
    python3 tools/simplify_models.py --role tree_conifer --tris 120000
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "content/config/scene_model_sources.json"
UE_CONFIG = ROOT / "content/config/ue_foliage_sources.json"
SOURCES = ROOT / "assets/models/polyhaven"
UE_SOURCES = ROOT / "assets/models/ue_foliage"   # tools/prepare_ue_foliage.py
OUT = ROOT / "assets/generated/simplified_models"

# role: (triangles per variant, solid share of the budget, max leaf growth)
BUDGETS = {
    "tree_broadleaf": (30000, 0.30, 4.0),
    "tree_conifer":   (60000, 0.30, 6.0),
    "sapling":        (8000,  0.25, 4.0),
    "shrub":          (3000,  0.30, 3.0),
    "shrub_large":    (12000, 0.30, 4.0),
    "undergrowth":    (1500,  0.30, 3.0),
    "ground_cover":   (400,   0.30, 3.0),
    "flower":         (800,   0.30, 3.0),
    "grass":          (800,   0.30, 3.0),
    "deadwood":       (2000,  1.00, 1.0),
    "mushroom":       (600,   1.00, 1.0),
    "rock":           (2500,  1.00, 1.0),
    "cliff":          (6000,  1.00, 1.0),
}
LEAFY = ("leaf", "leaves", "twig", "needle", "grass", "blade", "frond")


def find_binary(explicit):
    if explicit:
        return Path(explicit)
    for build in sorted(ROOT.glob("cmake-build-*")) + [ROOT / "build"]:
        candidate = build / "model_simplify"
        if candidate.exists():
            return candidate
    sys.exit("model_simplify not built: cmake --build <build dir> --target model_simplify")


def link(src, dst):
    if dst.exists():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def alpha_for(colour):
    """The cut-out map beside a colour map, by Poly Haven's naming."""
    for word in ("alpha", "opacity"):
        candidate = colour.with_name(colour.name.replace("_diff_", "_%s_" % word)).with_suffix(".png")
        if candidate.exists() and candidate != colour:
            return candidate
    return None


def finish_materials(gltf_path, source_dir):
    """Textures beside the output; leaf colour + cut-out -> RGBA and MASK."""
    doc = json.loads(gltf_path.read_text())
    out_dir = gltf_path.parent
    images = doc.get("images", [])
    textures = doc.get("textures", [])
    cut = {}
    for material in doc.get("materials", []):
        ref = material.get("pbrMetallicRoughness", {}).get("baseColorTexture")
        if ref is None:
            continue
        image = textures[ref["index"]]["source"]
        colour = source_dir / images[image]["uri"]
        alpha = alpha_for(colour)
        leafy = material.get("alphaMode", "OPAQUE") != "OPAQUE" or \
            any(w in material.get("name", "").lower() for w in LEAFY)
        if alpha is None or not leafy:
            continue
        if image not in cut:
            rgba_rel = "textures/" + colour.stem + "_rgba.png"
            target = out_dir / rgba_rel
            if not target.exists() or target.stat().st_mtime < max(colour.stat().st_mtime, alpha.stat().st_mtime):
                target.parent.mkdir(parents=True, exist_ok=True)
                rgb = Image.open(colour).convert("RGB")
                a = Image.open(alpha).convert("L")
                if a.size != rgb.size:
                    a = a.resize(rgb.size, Image.BILINEAR)
                rgb.putalpha(a)
                rgb.save(target, compress_level=6)
            cut[image] = rgba_rel
        material["alphaMode"] = "MASK"
        material["alphaCutoff"] = 0.5
        material["doubleSided"] = True
    for i, image in enumerate(images):
        if i in cut:
            image["uri"] = cut[i]
            image["mimeType"] = "image/png"
        else:
            link(source_dir / image["uri"], out_dir / image["uri"])
    gltf_path.write_text(json.dumps(doc, indent=1))
    return len(cut)


def contact_sheet(out_dir, rep):
    """preview.png: one row per variant - side, front, top silhouettes of the
    original against the result (yellow both, red lost, blue added)."""
    from PIL import ImageDraw
    previews = out_dir / "preview"
    rows = []
    for i, mesh in enumerate(rep["meshes"]):
        ppm = previews / ("mesh_%d.ppm" % i)
        if not ppm.exists():
            continue
        img = Image.open(ppm).convert("RGB")
        ImageDraw.Draw(img).text((4, 2), "%s  %d->%d  IoU %s" % (
            mesh["mesh"][:20], mesh["triangles_in"], mesh["triangles_out"],
            "/".join("%.2f" % v for v in mesh["silhouette_iou"])), fill=(240, 240, 240))
        rows.append(img)
    if rows:
        sheet = Image.new("RGB", (max(r.width for r in rows), sum(r.height for r in rows)))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.height
        sheet.save(out_dir / "preview.png")
    shutil.rmtree(previews, ignore_errors=True)


def simplify(binary, model, args):
    asset = model["asset_id"]
    source_dir = (UE_SOURCES if model.get("source") == "ue" else SOURCES) / asset
    meta_path = source_dir / "source.json"
    if not meta_path.exists():
        print("  %-24s not fetched - run tools/%s" % (asset, "export_ue_foliage.py + prepare_ue_foliage.py"
                                                    if model.get("source") == "ue" else "fetch_models.py"))
        return None
    meta = json.loads(meta_path.read_text())
    tris, share, growth = BUDGETS[model["role"]]
    tris = args.tris or model.get("tris", tris)
    out_dir = OUT / asset
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)
    gltf = out_dir / (asset + ".gltf")
    report = out_dir / "simplify.json"
    cmd = [str(binary), "--in", str(source_dir / meta["gltf"]), "--out", str(gltf), "--tris", str(tris),
           "--solid-share", str(model.get("solid_share", share)),
           "--max-growth", str(model.get("max_growth", growth)), "--report", str(report),
           "--preview", str(out_dir / "preview"), "--res", "192"]
    print("  %-24s %-14s budget %6d / variant" % (asset, model["role"], tris), flush=True)
    started = time.time()
    done = subprocess.run(["nice", "-n", "15"] + cmd, capture_output=True, text=True)
    if done.returncode != 0:
        print(done.stdout + done.stderr)
        return None
    if args.verbose:
        print(done.stdout, end="")
    cut = finish_materials(gltf, source_dir)
    rep = json.loads(report.read_text())
    contact_sheet(out_dir, rep)
    ious = [min(m["silhouette_iou"]) for m in rep["meshes"]]
    row = {"asset_id": asset, "role": model["role"], "budget": tris, "variants": len(rep["meshes"]),
           "triangles_in": rep["triangles_in"], "triangles_out": rep["triangles_out"],
           "max_per_variant": max(m["triangles_out"] for m in rep["meshes"]),
           "worst_silhouette_iou": round(min(ious), 3), "mean_silhouette_iou": round(sum(ious) / len(ious), 3),
           "max_growth_used": round(max(m["growth"] for m in rep["meshes"]), 2),
           "alpha_cut_materials": cut, "seconds": round(time.time() - started, 1),
           "gltf": str(gltf.relative_to(ROOT)), "license": meta.get("license"),
           "source": meta.get("asset_url")}
    print("      %10d -> %7d tris (%d variants, <= %d each)  silhouette IoU worst %.2f mean %.2f  "
          "growth %.1f  alpha-cut %d  %.1fs" % (
              row["triangles_in"], row["triangles_out"], row["variants"], row["max_per_variant"],
              row["worst_silhouette_iou"], row["mean_silhouette_iou"], row["max_growth_used"], cut,
              row["seconds"]), flush=True)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", nargs="*", metavar="ASSET")
    ap.add_argument("--role", nargs="*")
    ap.add_argument("--tris", type=int, help="override the budget per variant")
    ap.add_argument("--binary", help="path to model_simplify")
    ap.add_argument("--verbose", action="store_true", help="per-variant lines from the reducer")
    args = ap.parse_args()

    binary = find_binary(args.binary)
    models = json.loads(CONFIG.read_text())["models"]
    if UE_CONFIG.exists():
        models += [dict(m, source="ue") for m in json.loads(UE_CONFIG.read_text())["assets"]]
    if args.only:
        models = [m for m in models if m["asset_id"] in set(args.only)]
    if args.role:
        models = [m for m in models if m["role"] in set(args.role)]
    if not models:
        print("nothing matches")
        return 2

    print("simplifying %d models into %s" % (len(models), OUT.relative_to(ROOT)))
    OUT.mkdir(parents=True, exist_ok=True)
    summary = OUT / "report.json"
    rows = {r["asset_id"]: r for r in json.loads(summary.read_text())["models"]} if summary.exists() else {}
    failed = 0
    for model in models:
        row = simplify(binary, model, args)
        if row is None:
            failed += 1
        else:
            rows[row["asset_id"]] = row
    summary.write_text(json.dumps({"models": sorted(rows.values(), key=lambda r: (r["role"], r["asset_id"]))},
                                  indent=1) + "\n")
    total_in = sum(r["triangles_in"] for r in rows.values())
    total_out = sum(r["triangles_out"] for r in rows.values())
    print("all: %d -> %d triangles; report %s" % (total_in, total_out, summary.relative_to(ROOT)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

