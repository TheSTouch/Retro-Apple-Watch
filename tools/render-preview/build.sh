#!/bin/sh
# Builds the demo core for the Mac into build/render-preview.
set -eu
cd "$(dirname "$0")/../.."

SM64=external/sm64-port
mkdir -p build
clang -O2 -std=gnu11 -Wall -Wno-unused-function \
    -DF3DEX_GBI_2E -D_LANGUAGE_C -DNON_MATCHING -DAVOID_UB \
    -I"$SM64/include" -I"$SM64/src" -I"$SM64/src/pc" -Iport/include \
    "$SM64/src/pc/gfx/gfx_pc.c" "$SM64/src/pc/gfx/gfx_cc.c" \
    port/src/gfx_soft.c port/src/wm_watch.c port/src/audio_watch.c port/src/input_watch.c \
    port/src/demo_scene.c tools/render-preview/main.c \
    -o build/render-preview
echo "built build/render-preview"
