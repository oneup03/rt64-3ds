#!/usr/bin/env bash
# build.sh <game repo>: builds mod_bake against the game's N64Recomp and the
# libraries its host-tools build made (make -C platform/3ds host-tools).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}; shift || true
nr=$game/lib/N64ModernRuntime/N64Recomp
host=${HOST:-$game/build-host}
g++ -std=c++20 -O2 -w "$@" -I"$nr/include" -I"$nr/lib/rabbitizer/include" -I"$nr/lib/rabbitizer/cplusplus/include" -I"$nr/lib/fmt/include" -I"$nr/lib/tomlplusplus/include" \
    "$here/mod_bake.cpp" "$nr/src/config.cpp" "$host/libN64Recomp.a" "$host/libSymbolLists.a" "$host/librabbitizer.a" "$host/lib/fmt/libfmt.a" \
    -o "${OUT:-$here/mod_bake}"
echo "built ${OUT:-$here/mod_bake}"
