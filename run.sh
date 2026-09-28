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
#   ./run.sh engine-editor   engine modules GUI with GPU viewport (no game)

set -euo pipefail
cd "$(dirname "$0")"

mode="${1:-game}"
shift || true

# Reuse a GPU build when present: a second directory would rebuild DXC from scratch.
# CAMPFIRE_ENGINE_EDITOR_GPU=OFF keeps an independent, DXC-free CPU editor.
if [ "$mode" = engine-editor ]; then
    EDITOR_GPU="${CAMPFIRE_ENGINE_EDITOR_GPU:-ON}"
    DEFAULT_BUILD="cmake-build-engine-editor"
    if [ "$EDITOR_GPU" = ON ] && [ -f cmake-build-relwithdebinfo/CMakeCache.txt ] &&
            grep -q '^ASR_BUILD_CLIENT:BOOL=ON$' cmake-build-relwithdebinfo/CMakeCache.txt; then
        DEFAULT_BUILD="cmake-build-relwithdebinfo"
    fi
    BUILD_DIR="${CAMPFIRE_ENGINE_EDITOR_BUILD_DIR:-$DEFAULT_BUILD}"
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        GENERATOR=()
        command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
        SOURCES=()
        for dep in entt sdl3 sdl3_image sdl3_shadercross; do
            source_dir="$PWD/cmake-build-relwithdebinfo/_deps/$dep-src"
            if [ -d "$source_dir" ]; then
                upper="$(printf '%s' "$dep" | tr '[:lower:]' '[:upper:]')"
                SOURCES+=("-DFETCHCONTENT_SOURCE_DIR_$upper=$source_dir")
            fi
        done
        nice cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" "${SOURCES[@]}" \
            -DASR_BUILD_CLIENT=OFF -DASR_BUILD_ENGINE_EDITOR=ON -DASR_BUILD_TESTS=OFF \
            -DASR_ENGINE_EDITOR_GPU="$EDITOR_GPU"
    elif ! grep -q "^ASR_ENGINE_EDITOR_GPU:BOOL=$EDITOR_GPU$" "$BUILD_DIR/CMakeCache.txt" ||
            ! grep -q '^ASR_BUILD_ENGINE_EDITOR:BOOL=ON$' "$BUILD_DIR/CMakeCache.txt"; then
        nice cmake -S . -B "$BUILD_DIR" -DASR_BUILD_ENGINE_EDITOR=ON -DASR_ENGINE_EDITOR_GPU="$EDITOR_GPU"
    fi
    nice cmake --build "$BUILD_DIR" -j 2 --target campfire_engine_editor
    exec "$BUILD_DIR/campfire_engine_editor" "$@"
fi

BUILD_DIR="${CAMPFIRE_BUILD_DIR:-${ASR_BUILD_DIR:-cmake-build-relwithdebinfo}}"

# RelWithDebInfo, not Debug: an unoptimised simulation is about ten times slower,
# which puts the 10x game speed out of reach.
if [ ! -f "$BUILD_DIR/build.ninja" ] && [ ! -f "$BUILD_DIR/Makefile" ]; then
    GENERATOR=()
    command -v ninja >/dev/null 2>&1 && GENERATOR=(-G Ninja)
    echo "configuring $BUILD_DIR (first run fetches SDL3, this takes a few minutes)"
    nice cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" -DCMAKE_BUILD_TYPE=RelWithDebInfo
fi

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
        sed -n '2,13p' "$0" >&2
        exit 2
        ;;
esac
