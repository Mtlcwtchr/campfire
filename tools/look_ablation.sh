#!/usr/bin/env bash
# What each graphics feature costs: the same scripted bench with one thing
# turned off at a time, mean milliseconds per segment side by side.
#
#   tools/look_ablation.sh OUTDIR [X,Y] [WORLD] [SEED]
#
# Variants are graphics.json files written into OUTDIR; env-only switches
# (grass) go through the environment. Frames per view: ABL_FRAMES (90).
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:?output directory}"; at="${2:-219904,21760}"; world="${3:-average}"; seed="${4:-11}"
frames="${ABL_FRAMES:-90}"
size="${ABL_SIZE:-1920x1080}"
bin="${CAMPFIRE_BUILD_DIR:-cmake-build-relwithdebinfo}/campfire_client"
export ASR_TERRAIN_CACHE_DIR="${ASR_TERRAIN_CACHE_DIR:-/tmp/campfire-look-cache}"
mkdir -p "$out"
variant() {   # variant NAME JSON-OVERRIDES [ENV=...]
    local name="$1" json="$2"; shift 2
    python3 - "$out/$name.json" "$json" <<'EOF'
import json, sys
base = {"quality": 2, "antialiasing": 2, "shadows": True, "forestProxies": True, "farForest": True,
        "farTrees": True, "clouds": True, "cloudQuality": 1, "grade": True, "fog": True}
base.update(json.loads(sys.argv[2]))
json.dump(base, open(sys.argv[1], "w"))
EOF
    env "$@" "$bin" --explore --world "$world" --seed "$seed" --at "$at" --headless "$size" --clean \
        --graphics-file "$out/$name.json" --bench "$frames" --bench-load 400 \
        --bench-json "$out/$name.bench.json" >/dev/null 2>&1 || true
    python3 - "$name" "$out/$name.bench.json" <<'EOF'
import json, sys
try:
    rows = json.load(open(sys.argv[2]))
except Exception:
    print("%-14s failed" % sys.argv[1]); sys.exit()
print("%-14s " % sys.argv[1] + " ".join("%s=%6.2f" % (r["segment"], r["mean"]) for r in rows if r["segment"] != "load"))
EOF
}
variant base '{}'
variant no-aa '{"antialiasing": 0}'
variant no-clouds '{"clouds": false}'
variant no-shadows '{"shadows": false}'
variant no-grade '{"grade": false}'
variant no-fog '{"fog": false}'
variant no-fartrees '{"farTrees": false}'
variant no-forest '{"forestProxies": false, "farForest": false}'
variant no-grass '{}' ASR_GRASS_DISABLED=1

