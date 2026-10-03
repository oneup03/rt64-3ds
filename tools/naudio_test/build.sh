#!/usr/bin/env bash
# build.sh <game repo> [extra flags]: builds naudio_test against the game's
# recompiled audio microcode and its N64ModernRuntime headers.
#
#   HLE=naudio (default)  the n_aspMain family: rsp/n_aspMain.cpp, n_aspMain()
#   HLE=aspmain           libaudio's ABI 1 aspMain: rsp/aspMain.cpp, aspMain()
#   UCODE_SRC=..., UCODE_FUNC=...   another file or function name
#   SISD=1                the scalar vector-unit path (what the 3DS runs)
#   OUT=...               the binary (default tools/naudio_test/naudio_test)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}; shift || true
nmr=$game/lib/N64ModernRuntime
inc=$nmr/librecomp/include
case "${HLE:-naudio}" in
    naudio)  family=0; src_default=$game/rsp/n_aspMain.cpp; func_default=n_aspMain ;;
    aspmain) family=1; src_default=$game/rsp/aspMain.cpp;   func_default=aspMain ;;
    *) echo "HLE must be naudio or aspmain" >&2; exit 2 ;;
esac
src=${UCODE_SRC:-$src_default}
func=${UCODE_FUNC:-$func_default}
if [ -n "${SISD:-}" ]; then
    # A copy of the headers with the SSE detection disabled: the scalar
    # vector-unit path, which is what the 3DS build runs.
    shim=${TMPDIR:-/tmp}/naudio_test_sisd_inc
    rm -rf "$shim"; mkdir -p "$shim"; cp -r "$inc/librecomp" "$shim/"
    sed -i 's/^#if defined(__x86_64__) || defined(_M_X64)$/#if 0/' "$shim/librecomp/rsp_vu.hpp"
    inc=$shim
fi
plat=$here/../../src/platform
g++ -std=c++20 -O2 -fno-strict-aliasing -msse4.1 -w "$@" \
    -DHLE_FAMILY=$family -DUCODE_FUNC=$func \
    -I"$inc" -I"$inc/librecomp" -I"$nmr/N64Recomp/include" -I"$nmr/ultramodern/include" -I"$nmr/thirdparty/concurrentqueue" \
    -I"$plat" \
    "$here/naudio_test.cpp" "$plat/naudio_hle.cpp" "$plat/aspmain_hle.cpp" "$plat/audio_hle_diff.cpp" "$src" "$nmr/librecomp/src/rsp.cpp" \
    -o "${OUT:-$here/naudio_test}"
echo "built ${OUT:-$here/naudio_test} ($func from $src)"
