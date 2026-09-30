#!/usr/bin/env bash
# build.sh <game repo>: builds mod_bake against the game's N64Recomp and the
# libraries its host-tools build made (make -C platform/3ds host-tools).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}; shift || true
nmr=$game/lib/N64ModernRuntime
nr=$nmr/N64Recomp
host=${HOST:-$game/build-host}
out=${OUT:-$here/mod_bake}
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
echo '#define MINIZ_EXPORT' > "$obj/miniz_export.h"   # made by miniz's CMake otherwise
for c in miniz miniz_tdef miniz_tinfl miniz_zip; do
    gcc -O2 -w -c "$nmr/thirdparty/miniz/$c.c" -I"$nmr/thirdparty/miniz" -I"$obj" -o "$obj/$c.o"
done
g++ -std=c++20 -O2 -w "$@" -I"$nr/include" -I"$nr/lib/rabbitizer/include" -I"$nr/lib/rabbitizer/cplusplus/include" -I"$nr/lib/fmt/include" \
    -I"$nr/lib/tomlplusplus/include" -I"$nr/lib/ELFIO" -I"$nmr/thirdparty/json" -I"$nmr/thirdparty/miniz" -I"$obj" \
    "$here/mod_bake.cpp" "$nr/src/config.cpp" "$obj"/*.o \
    "$host/libN64Recomp.a" "$host/libSymbolLists.a" "$host/librabbitizer.a" "$host/lib/fmt/libfmt.a" \
    -o "$out"
echo "built $out"
