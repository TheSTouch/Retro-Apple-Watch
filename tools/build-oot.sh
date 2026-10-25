#!/bin/sh
# Builds Ocarina of Time (Ship of Harkinian) for the watch app:
#   build/watch/watchsimulator/liboot.a   Apple Watch simulator (arm64)
#   build/watch/watchos/liboot.a          real watches (arm64)
#   build/oot/assets/oot.o2r, soh.o2r     the game's data, bundled with the app
#
# Usage: tools/build-oot.sh [simulator|device|all|mac]
#
# "mac" builds the game for the Mac with the watch's renderer instead, into
# build/oot/mac/soh/soh-macos, to check rendering and speed without a watch:
#   cd build/oot/mac/soh && WATCH_PREVIEW_DIR=/tmp/frames WATCH_PREVIEW_FRAMES=600 ./soh-macos
# (see port/oot/wm_watch_lus.cpp for the WATCH_PREVIEW_* variables).
#
# Needs your own ROM in external/Shipwright/roms/ (see README), plus:
#   brew install cmake ninja
# and for "mac" also Ship of Harkinian's Mac dependencies:
#   brew install sdl2 sdl2_net libpng glew tinyxml2 nlohmann-json libzip opusfile libvorbis
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SOH=$ROOT/external/Shipwright
PORT=$ROOT/port
WHICH=${1:-simulator}

fail() {
    echo "build-oot: $*" >&2
    exit 1
}

command -v cmake >/dev/null || fail "CMake is needed: brew install cmake"
command -v ninja >/dev/null || fail "Ninja is needed: brew install ninja"
[ -f "$SOH/CMakeLists.txt" ] || fail "external/Shipwright is empty: git submodule update --init --recursive"

# The port's changes to Ship of Harkinian and libultraship (patches/oot),
# applied in place once. The submodule is set to ignore=dirty.
apply_patch() {
    dir=$1
    patch=$2
    git -C "$dir" apply --reverse --check "$patch" 2>/dev/null && return 0
    git -C "$dir" apply "$patch" || fail "could not apply patches/oot/$(basename "$patch")"
}
apply_patch "$SOH" "$ROOT/patches/oot/shipwright.patch"
apply_patch "$SOH/libultraship" "$ROOT/patches/oot/libultraship.patch"

# The game's data: Ship of Harkinian's asset tools, built for the Mac, turn
# the ROM into oot.o2r and pack soh.o2r.
assets=$ROOT/build/oot/assets
if [ ! -f "$assets/oot.o2r" ] || [ ! -f "$assets/soh.o2r" ]; then
    rom=
    for f in "$SOH"/roms/*.z64 "$SOH"/roms/*.n64 "$SOH"/roms/*.v64; do
        [ -f "$f" ] && rom=$f
    done
    [ -n "$rom" ] || fail "no ROM in external/Shipwright/roms/ (bring your own, see README)"
    tools=$ROOT/build/oot/tools
    echo "build-oot: extracting the game's data from your ROM" >&2
    cmake -S "$SOH" -B "$tools" -GNinja -DCMAKE_BUILD_TYPE=Release -DSOH_TOOLS_ONLY=ON >&2
    cmake --build "$tools" --target ExtractAssets >&2
    mkdir -p "$assets"
    cp "$tools"/soh/*.o2r "$assets/"
    echo "build-oot: wrote build/oot/assets/$(cd "$assets" && echo *.o2r)" >&2
fi

# The watch's backends for libultraship, compiled into it (port/oot).
backends="$PORT/oot/gfx_soft_lus.cpp;$PORT/oot/wm_watch_lus.cpp;$PORT/oot/platform_watch_lus.cpp;$PORT/src/input_watch.c"
includes="$PORT/oot;$PORT/include"

# Builds the game for one SDK and architecture, and merges it and all its
# libraries into one static library at $3.
build_one() {
    sdk=$1
    arch=$2
    out=$3
    dir=$ROOT/build/oot/$sdk
    echo "build-oot: $sdk $arch" >&2
    cmake -S "$SOH" -B "$dir" -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=watchOS -DCMAKE_OSX_SYSROOT="$sdk" -DCMAKE_OSX_ARCHITECTURES="$arch" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=10.0 \
        -DWATCH_PORT_CMAKE="$PORT/oot/cmake/watchos.cmake" \
        -DLUS_WATCH_PORT_SOURCES="$PORT/oot/game_main_oot.cpp;$backends" \
        -DLUS_WATCH_PORT_INCLUDES="$includes" >&2
    cmake --build "$dir" >&2
    mkdir -p "$(dirname "$out")"
    rm -f "$out"
    # shellcheck disable=SC2046
    xcrun libtool -static -no_warning_for_no_symbols -o "$out" \
        $(find "$dir" -name '*.a' -not -path '*/CMakeFiles/*' | sort)
}

if [ "$WHICH" = simulator ] || [ "$WHICH" = all ]; then
    build_one watchsimulator arm64 "$ROOT/build/watch/watchsimulator/liboot.a"
    echo "build-oot: wrote build/watch/watchsimulator/liboot.a" >&2
fi

if [ "$WHICH" = device ] || [ "$WHICH" = all ]; then
    build_one watchos arm64 "$ROOT/build/watch/watchos/liboot.a"
    echo "build-oot: wrote build/watch/watchos/liboot.a" >&2
fi

if [ "$WHICH" = mac ]; then
    dir=$ROOT/build/oot/mac
    cmake -S "$SOH" -B "$dir" -GNinja -DCMAKE_BUILD_TYPE=Release \
        -DLUS_WATCH_PORT_SOURCES="$backends" -DLUS_WATCH_PORT_INCLUDES="$includes" >&2
    cmake --build "$dir" >&2
    cp "$assets"/*.o2r "$dir/soh/"
    echo "build-oot: wrote build/oot/mac/soh/soh-macos" >&2
fi
