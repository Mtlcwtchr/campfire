#!/usr/bin/env python3
"""fetch_models - brings down the source scene models (trees, shrubs, ground
plants, deadwood, rocks) listed in content/config/scene_model_sources.json.

Same contract as tools/fetch_terrain.py: the config holds Poly Haven asset ids
and nothing else; the files an asset has are asked of

    https://api.polyhaven.com/files/{asset_id}

every time. The glTF flavour is taken (the .gltf, its .bin and the textures it
references), because that is what tools/prepare_scene_models.py reads.

    python3 tools/fetch_models.py                    # everything missing
    python3 tools/fetch_models.py --only fern_02 moss_01
    python3 tools/fetch_models.py --role tree_broadleaf sapling
    python3 tools/fetch_models.py --skip-heavy       # leave the multi-million-triangle trees
    python3 tools/fetch_models.py --check            # say what is missing, take nothing

Downloads are streamed to disk (some .bin files are hundreds of MB), checked
against the md5 the API gives, written as .part and renamed only when right -
so this is safe to interrupt and re-run, and a file already here with the right
md5 is not fetched again. One file at a time: this is a background chore, not a
benchmark.

Beside each asset it writes source.json: licence, authors, real-world size in
metres, triangle count and the files taken. The downloads themselves are not in
git (see .gitignore); this script and the config are how they come back.
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CONFIG = ROOT / "content/config/scene_model_sources.json"
INTO = ROOT / "assets/models/polyhaven"

API = "https://api.polyhaven.com"
AGENT = "asr-model-fetch/1 (+local build tool)"
CHUNK = 1 << 20


def get_json(url):
    request = urllib.request.Request(url, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(request, timeout=60) as answer:
        return json.loads(answer.read())


def digest(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(CHUNK), b""):
            h.update(block)
    return h.hexdigest()


def take(url, into, md5, size, quiet=False, attempts=3):
    """Stream one file to disk unless it is already here and right."""
    if into.exists() and (md5 is None or digest(into) == md5):
        return 0
    into.parent.mkdir(parents=True, exist_ok=True)
    part = into.with_name(into.name + ".part")
    for attempt in range(1, attempts + 1):
        try:
            if not quiet:
                print("      %-48s %7.1f MB" % (into.name[:48], (size or 0) / 1e6), end="", flush=True)
            h = hashlib.md5()
            request = urllib.request.Request(url, headers={"User-Agent": AGENT})
            with urllib.request.urlopen(request, timeout=120) as answer, open(part, "wb") as out:
                for block in iter(lambda: answer.read(CHUNK), b""):
                    h.update(block)
                    out.write(block)
            if md5 is not None and h.hexdigest() != md5:
                raise RuntimeError("md5 mismatch")
            part.replace(into)
            if not quiet:
                print("  ok")
            return size or into.stat().st_size
        except (urllib.error.URLError, TimeoutError, ConnectionError, RuntimeError) as bad:
            if not quiet:
                print("  %s (try %d/%d)" % (bad, attempt, attempts))
            part.unlink(missing_ok=True)
            if attempt == attempts:
                raise
            time.sleep(2 * attempt)
    return 0


def pick_gltf(files, res):
    """The glTF entry at `res`, or the nearest size above it."""
    by_res = files.get("gltf") or {}
    ladder = ["1k", "2k", "4k", "8k"]
    for r in ladder[ladder.index(res):] + ladder[:ladder.index(res)][::-1]:
        entry = by_res.get(r, {}).get("gltf")
        if entry:
            return r, entry
    return None, None


def fetch(model, default_res, check, quiet):
    asset = model["asset_id"]
    res = model.get("res", default_res)
    into = INTO / asset
    print("  %-24s %-15s %s" % (asset, model.get("role", "?"), res))
    try:
        files = get_json("%s/files/%s" % (API, asset))
    except urllib.error.HTTPError as bad:
        print("      the api says %s - is the asset id right?" % bad.code)
        return False, 0
    got_res, entry = pick_gltf(files, res)
    if entry is None:
        print("      no glTF for this asset")
        return False, 0

    wanted = [(into / entry["url"].rsplit("/", 1)[-1], entry)]
    for rel, inc in sorted(entry.get("include", {}).items()):
        wanted.append((into / rel, inc))
    # The glTF's leaf colour maps are JPEGs with no alpha; the cut-out is a
    # separate map the glTF does not reference. tools/simplify_models.py
    # merges the two into the RGBA a leaf card needs.
    for key, by_res in sorted(files.items()):
        if "alpha" not in key.lower() and "opacity" not in key.lower():
            continue
        at = by_res.get(got_res) or next((by_res[r] for r in ("1k", "2k", "4k", "8k") if r in by_res), {})
        png = at.get("png")
        if png:
            wanted.append((into / "textures" / png["url"].rsplit("/", 1)[-1], png))

    if check:
        missing = [p.name for p, _ in wanted if not p.exists()]
        if missing:
            print("      missing: %s" % ", ".join(missing[:6]) + (" ..." if len(missing) > 6 else ""))
        return not missing, 0

    taken = 0
    for path, e in wanted:
        taken += take(e["url"], path, e.get("md5"), e.get("size"), quiet)

    info = {}
    try:
        info = get_json("%s/info/%s" % (API, asset))
    except urllib.error.HTTPError:
        pass
    size = info.get("dimensions")
    (into / "source.json").write_text(json.dumps({
        "site": "polyhaven",
        "asset_id": asset,
        "asset_url": "https://polyhaven.com/a/%s" % asset,
        "name": info.get("name"),
        "license": "CC0",
        "authors": sorted(info.get("authors", {}).keys()) or None,
        "role": model.get("role"),
        "biomes": model.get("biomes"),
        "heavy": bool(model.get("heavy")),
        "dimensions_metres": [round(v / 1000.0, 3) for v in size] if size else None,
        "polycount": info.get("polycount"),
        "category": info.get("category"),
        "resolution": got_res,
        "gltf": wanted[0][0].name,
        "files": [str(p.relative_to(into)) for p, _ in wanted],
        "files_hash": info.get("files_hash"),
    }, indent=2, ensure_ascii=False) + "\n")
    if taken == 0 and not quiet:
        print("      already here")
    return True, taken


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", nargs="*", metavar="ASSET", help="just these asset ids")
    ap.add_argument("--role", nargs="*", help="just these roles (tree_broadleaf, shrub, ...)")
    ap.add_argument("--res", choices=["1k", "2k", "4k", "8k"], help="override every entry's resolution")
    ap.add_argument("--skip-heavy", action="store_true", help="leave the entries marked heavy")
    ap.add_argument("--check", action="store_true", help="say what is missing and take nothing")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    config = json.loads(CONFIG.read_text())
    models = config["models"]
    if args.only:
        models = [m for m in models if m["asset_id"] in set(args.only)]
    if args.role:
        models = [m for m in models if m.get("role") in set(args.role)]
    if args.skip_heavy:
        models = [m for m in models if not m.get("heavy")]
    if args.res:
        models = [dict(m, res=args.res) for m in models]
    if not models:
        print("nothing matches")
        return 2

    print("%s %d models into %s" % ("checking" if args.check else "fetching", len(models),
                                     INTO.relative_to(ROOT)))
    ok, total = True, 0
    for m in models:
        try:
            good, taken = fetch(m, config.get("default_res", "2k"), args.check, args.quiet)
            ok &= good
            total += taken
        except Exception as bad:                # one bad asset must not stop the rest
            print("      %s" % bad)
            ok = False
    if not args.check:
        print("took %.1f MB" % (total / 1e6))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

