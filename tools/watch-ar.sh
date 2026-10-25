#!/bin/sh
# Linker stand-in handed to the decomp's Makefile as LD. Instead of linking a
# PC executable, it packs the game's objects into a static library at
# "<output>.a", leaving out the PC-only platform code the port replaces.
set -eu
out=""
objs=""
while [ $# -gt 0 ]; do
    case "$1" in
        -o) out="$2"; shift 2 ;;
        *.o)
            case "$1" in
                */src/pc/pc_main.o | */src/pc/configfile.o | */src/pc/controller/* | */src/pc/audio/*) ;;
                */src/pc/gfx/gfx_opengl.o | */src/pc/gfx/gfx_direct3d* | */src/pc/gfx/gfx_dxgi.o) ;;
                */src/pc/gfx/gfx_glx.o | */src/pc/gfx/gfx_sdl2.o | */src/pc/gfx/gfx_dummy.o) ;;
                *) objs="$objs $1" ;;
            esac
            shift ;;
        *) shift ;;
    esac
done
[ -n "$out" ] || { echo "watch-ar.sh: no -o given" >&2; exit 1; }
rm -f "$out.a"
# shellcheck disable=SC2086
xcrun ar rcs "$out.a" $objs
echo "watch-ar.sh: wrote $out.a"
