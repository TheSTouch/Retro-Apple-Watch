#!/bin/sh
# Builds Super Mario 64 as a static library for the watch app:
#   build/watch/watchsimulator/libsm64.a   Apple Watch simulator (arm64)
#   build/watch/watchos/libsm64.a          real watches (arm64 + arm64_32)
#
# Usage: tools/build-game.sh [eu|us|jp] [simulator|device|all]
#
# Needs your own ROM at external/sm64-port/baserom.<version>.z64 (see README),
# plus: brew install make aarch64-elf-binutils
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SM64=$ROOT/external/sm64-port
VERSION=${1:-eu}
WHICH=${2:-simulator}
JOBS=$(sysctl -n hw.ncpu)

fail() {
    echo "build-game: $*" >&2
    exit 1
}

command -v gmake >/dev/null || fail "GNU make 4 is needed (macOS make is 3.81): brew install make"
command -v aarch64-elf-as >/dev/null || fail "GNU as is needed for a sound sequence: brew install aarch64-elf-binutils"

rom=$SM64/baserom.$VERSION.z64
[ -f "$rom" ] || fail "no ROM at $rom (bring your own, see README)"
expected=$(cut -d' ' -f1 "$SM64/sm64.$VERSION.sha1")
actual=$(shasum -a 1 "$rom" | cut -d' ' -f1)
[ "$expected" = "$actual" ] || fail "$rom is not an unmodified $VERSION ROM (SHA-1 $actual)"

# Builds the game for one SDK/architecture and prints the library path.
build_one() {
    sdk=$1
    target=$2
    base=build/watch-$sdk-${target%%-*}
    export WATCH_SDK="$sdk" WATCH_TARGET="$target"
    set -- VERSION="$VERSION" TARGET_LINUX=0 ENABLE_OPENGL=0 \
        CC="$ROOT/tools/watch-cc.sh" CXX="$ROOT/tools/watch-cc.sh" LD="$ROOT/tools/watch-ar.sh" \
        AS=aarch64-elf-as OBJCOPY=aarch64-elf-objcopy BUILD_DIR_BASE="$base"

    echo "build-game: $target" >&2
    (cd "$SM64" && gmake "$@" -j"$JOBS" >&2)

    # The port's game glue is compiled with exactly the flags the game used
    # (the last line: the Makefile prints a build-options banner first).
    cflags=$(cd "$SM64" && gmake "$@" --no-print-directory --eval='watch-flags-%: ; @echo $($*)' watch-flags-CFLAGS | tail -n 1)
    objdir=$SM64/$base/${VERSION}_pc/port
    mkdir -p "$objdir"
    for src in "$ROOT"/port/game/*.c; do
        # shellcheck disable=SC2086
        (cd "$SM64" && "$ROOT/tools/watch-cc.sh" $cflags -Isrc/pc -I"$ROOT/port/include" -c "$src" \
            -o "$objdir/$(basename "$src" .c).o")
    done
    lib=$SM64/$base/${VERSION}_pc/sm64.$VERSION.a
    xcrun ar rs "$lib" "$objdir"/*.o 2>/dev/null
    echo "$lib"
}

if [ "$WHICH" = simulator ] || [ "$WHICH" = all ]; then
    lib=$(build_one watchsimulator arm64-apple-watchos10.0-simulator)
    mkdir -p "$ROOT/build/watch/watchsimulator"
    cp "$lib" "$ROOT/build/watch/watchsimulator/libsm64.a"
    echo "build-game: wrote build/watch/watchsimulator/libsm64.a"
fi

if [ "$WHICH" = device ] || [ "$WHICH" = all ]; then
    lib64=$(build_one watchos arm64-apple-watchos10.0)
    lib32=$(build_one watchos arm64_32-apple-watchos10.0)
    mkdir -p "$ROOT/build/watch/watchos"
    xcrun lipo -create "$lib64" "$lib32" -output "$ROOT/build/watch/watchos/libsm64.a"
    echo "build-game: wrote build/watch/watchos/libsm64.a"
fi
