#!/bin/sh
# Compiler wrapper handed to the decomp's Makefile as CC/CXX: compiles for the
# watch instead of the host. WATCH_SDK and WATCH_TARGET come from build-game.sh.
# - Drops -march=native, which the PC Makefile adds and clang rejects for arm.
# - Turns off fortify, whose memcpy/memset macros clash with libultra's os_libc.h.
# - Defines TARGET_WATCH, which the port's patches (patches/) key on.
# - Compiles the PC-only backends (SDL, OpenGL, ALSA, DirectX, keyboard, the PC
#   main) as empty files: they need libraries watchOS lacks, and the port
#   replaces them. watch-ar.sh leaves their objects out of the library anyway.
set -eu
for a in "$@"; do
    shift
    case "$a" in
        -march=native) ;;
        src/pc/pc_main.c | src/pc/configfile.c | src/pc/audio/* | src/pc/controller/* | \
        src/pc/gfx/gfx_opengl.c | src/pc/gfx/gfx_glx.c | src/pc/gfx/gfx_sdl2.c | src/pc/gfx/gfx_dummy.c | \
        src/pc/gfx/*.cpp)
            set -- "$@" -x c /dev/null ;;
        *) set -- "$@" "$a" ;;
    esac
done
exec xcrun -sdk "$WATCH_SDK" clang -target "$WATCH_TARGET" -Wno-everything -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DTARGET_WATCH "$@"
