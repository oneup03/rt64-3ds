#!/usr/bin/env bash
# build.sh <game repo> [flags]: host build of the interpreter + dl_test.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}; shift || true
nmr=$game/lib/N64ModernRuntime
g++ -std=c++20 -O2 -g -fno-omit-frame-pointer -w "$@" \
    -I"$nmr/ultramodern/include" -I"$here/../../src/renderer" \
    "$here/dl_test.cpp" "$here/../../src/renderer/rt64_3ds_dl.cpp" \
    -o "${OUT:-$here/dl_test}"
echo "built ${OUT:-$here/dl_test}"
