#!/usr/bin/env bash
# A fixed set of views for judging the look: on foot in a wood, on foot in a
# meadow (with and against the sun), a third-person vista over the country, a
# river, and from the air. One client launch for all of them (--shot-list).
#
#   tools/look_shots.sh OUTDIR [WORLD] [SEED] [-- extra client args]
#
# WORLD is a size name (average by default: 262 km, raised in ~2 s; giant is
# the 2000 km reference and takes minutes). Places come from the landmarks the
# client prints, so the same seed gives the same pictures; they are cached per
# world and seed, so a second run is a single launch.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:?output directory}"; shift
world="${1:-average}"; [ $# -gt 0 ] && shift
seed="${1:-11}"; [ $# -gt 0 ] && shift
[ "${1:-}" = "--" ] && shift
extra=("$@")
mkdir -p "$out"
bin="${CAMPFIRE_BUILD_DIR:-cmake-build-relwithdebinfo}/campfire_client"
size="${LOOK_SIZE:-1600x900}"
export ASR_TERRAIN_CACHE_DIR="${ASR_TERRAIN_CACHE_DIR:-/tmp/campfire-look-cache}"
mkdir -p "$ASR_TERRAIN_CACHE_DIR"

cache="$ASR_TERRAIN_CACHE_DIR/landmarks-$world-$seed.txt"
if [ ! -s "$cache" ]; then
    "$bin" --explore --world "$world" --seed "$seed" --headless 64x64 --shot-frame 1 \
        --shot "$out/.probe.png" 2>/dev/null | grep '^landmarks:' > "$cache" || true
    rm -f "$out/.probe.png"
fi
landmarks=$(cat "$cache")
place() {   # place NAME -> "X Y"
    echo "$landmarks" | tr '[' '\n' | grep "^$1 " | head -1 | sed -E 's/.* ([0-9-]+),([0-9-]+)\].*/\1 \2/'
}
read -r fx fy <<<"$(place 'temperate forest')"
read -r ox oy <<<"$(place 'meadow')"
[ -n "${ox:-}" ] || read -r ox oy <<<"$(place 'open country')"
read -r rx ry <<<"$(place 'the big river')"
echo "forest $fx,$fy  meadow $ox,$oy  river $rx,$ry"

# PATH eye X Y ABOVE YAW PITCH | PATH orbit X Y ZOOM YAW PITCH
list="$out/shots.txt"
{
    echo "$out/forest-foot.png eye $fx $fy 1.7 0.8 0.04"
    echo "$out/forest-look.png eye $fx $fy 1.7 2.4 -0.06"
    echo "$out/meadow-foot.png eye $ox $oy 1.7 0.8 0.03"
    echo "$out/meadow-sun.png eye $ox $oy 1.7 3.9 0.05"
    echo "$out/vista.png orbit $fx $fy 9 0.8 0.22"
    echo "$out/river.png orbit $rx $ry 14 2.0 0.30"
    echo "$out/aerial.png orbit $fx $fy 1.2 0.8 0.55"
    # LOOK_GRAPHICS="a.json b.json": the two on-foot views again with each
    # (lighting A/B in the same launch), named after the file.
    for g in ${LOOK_GRAPHICS:-}; do
        n=$(basename "$g" .json)
        echo "$out/forest-foot.$n.png eye $fx $fy 1.7 0.8 0.04 $g"
        echo "$out/meadow-foot.$n.png eye $ox $oy 1.7 0.8 0.03 $g"
    done
} > "$list"
# A watchdog: a run that has not finished in LOOK_TIMEOUT seconds (300) is
# stopped rather than left loading the machine.
perl -e 'alarm shift; exec @ARGV' "${LOOK_TIMEOUT:-300}" \
    nice -n 5 "$bin" --explore --world "$world" --seed "$seed" --headless "$size" --clean \
    --shot-list "$list" ${extra[@]+"${extra[@]}"} > "$out/run.log" 2>&1 || true
grep -E '^wrote' "$out/run.log" | cut -c1-150 || echo "no shots - see $out/run.log"
python3 tools/look_metrics.py "$out"/*.png --json "$out/metrics.json" | grep -E '^/|  all|  sky|  ground'

