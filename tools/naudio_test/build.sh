#!/usr/bin/env bash
# build.sh <game repo> [extra flags]: builds naudio_test against the game's
# recompiled n_aspMain.cpp and its N64ModernRuntime headers. Pass
# -U__x86_64__ to force the scalar vector-unit path (what the 3DS runs).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}; shift || true
nmr=$game/lib/N64ModernRuntime
inc=$nmr/librecomp/include
if [ -n "${SISD:-}" ]; then
    # A copy of the headers with the SSE detection disabled: the scalar
    # vector-unit path, which is what the 3DS build runs.
    shim=${TMPDIR:-/tmp}/naudio_test_sisd_inc
    rm -rf "$shim"; mkdir -p "$shim"; cp -r "$inc/librecomp" "$shim/"
    sed -i 's/^#if defined(__x86_64__) || defined(_M_X64)$/#if 0/' "$shim/librecomp/rsp_vu.hpp"
    inc=$shim
fi
g++ -std=c++20 -O2 -fno-strict-aliasing -msse4.1 -w "$@" \
    -I"$inc" -I"$inc/librecomp" -I"$nmr/N64Recomp/include" -I"$nmr/ultramodern/include" -I"$nmr/thirdparty/concurrentqueue" \
    -I"$here/../../src/platform" \
    "$here/naudio_test.cpp" "$here/../../src/platform/naudio_hle.cpp" "$game/rsp/n_aspMain.cpp" "$nmr/librecomp/src/rsp.cpp" \
    -o "${OUT:-$here/naudio_test}"
echo "built ${OUT:-$here/naudio_test}"
