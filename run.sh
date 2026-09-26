#!/usr/bin/env bash
# Launch Campfire (or any other target) in its current state, from a terminal.
# Mirrors the CLion run configurations in .idea/runConfigurations.
#
#   ./run.sh                 the game
#   ./run.sh game 42 180     the game on seed 42, 180-tile map
#   ./run.sh explore        the 3D world explorer
#   ./run.sh headless 200    a 200-day headless run with a report
#   ./run.sh explain 5000    dump the demand table and every person at tick 5000
#   ./run.sh validate        check the content tree
#   ./run.sh test            the test suite

set -euo pipefail
cd "$(dirname "$0")"

BUILD_DIR="${CAMPFIRE_BUILD_DIR:-${ASR_BUILD_DIR:-cmake-build-relwithdebinfo}}"

# RelWithDebInfo, not Debug: an unoptimised simulation is about ten times slower,
# which puts the 10x game speed out of reach.
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
    GENERATOR=()
    command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
    echo "configuring $BUILD_DIR (first run fetches SDL3, this takes a few minutes)"
    nice cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi

mode="${1:-game}"
shift || true

case "$mode" in
    game)
        nice cmake --build "$BUILD_DIR" -j 2 --target asr_client
        exec "$BUILD_DIR/campfire_client" --seed "${1:-11}" --map "${2:-140}" --pop "${3:-10}"
        ;;
    explore)
        nice cmake --build "$BUILD_DIR" -j 2 --target asr_client
        exec "$BUILD_DIR/campfire_client" --explore "$@"
        ;;
    headless)
        nice cmake --build "$BUILD_DIR" -j 2 --target sim_runner
        exec "$BUILD_DIR/sim_runner" --days "${1:-200}" --seed "${2:-11}" --map "${3:-140}" \
             --status-every 720
        ;;
    explain)
        nice cmake --build "$BUILD_DIR" -j 2 --target sim_runner
        exec "$BUILD_DIR/sim_runner" --days 400 --seed "${2:-11}" --map 140 --quiet \
             --explain "${1:-5000}"
        ;;
    validate)
        nice cmake --build "$BUILD_DIR" -j 2 --target content_validator
        exec "$BUILD_DIR/content_validator"
        ;;
    test)
        nice cmake --build "$BUILD_DIR" -j 2 --target asr_tests
        exec "$BUILD_DIR/asr_tests"
        ;;
    *)
        echo "unknown mode: $mode" >&2
        sed -n '2,12p' "$0" >&2
        exit 2
        ;;
esac
