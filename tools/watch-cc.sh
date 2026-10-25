#!/bin/sh
# Compiler wrapper handed to the decomp's Makefile as CC/CXX: compiles for the
# watch instead of the host. WATCH_SDK and WATCH_TARGET come from build-game.sh.
# Drops -march=native, which the PC Makefile adds and clang rejects for arm.
set -eu
for a in "$@"; do
    shift
    [ "$a" = "-march=native" ] || set -- "$@" "$a"
done
exec xcrun -sdk "$WATCH_SDK" clang -target "$WATCH_TARGET" -Wno-everything "$@"
