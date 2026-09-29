#!/usr/bin/env bash
# build.sh <game repo> [flags]: host build of the interpreter, the combiner
# planner and tev_test (host/citro3d.h stands in for citro3d).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}; shift || true
nmr=$game/lib/N64ModernRuntime
dkp=${DEVKITPRO:-$HOME/devkitpro}
g++ -std=c++20 -O2 -g -w "$@" \
    -I"$here/host" -I"$dkp/libctru/include" -I"$nmr/ultramodern/include" -I"$here/../../src/renderer" \
    "$here/tev_test.cpp" "$here/../../src/renderer/rt64_3ds_tev.cpp" "$here/../../src/renderer/rt64_3ds_dl.cpp" \
    -o "${OUT:-$here/tev_test}"
echo "built ${OUT:-$here/tev_test}"
