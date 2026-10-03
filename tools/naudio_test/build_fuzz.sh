#!/usr/bin/env bash
# build_fuzz.sh <game repo> [oracle dir]: builds audio_fuzz, the equivalence
# fuzzer for the audio interpreters (audio_fuzz.cpp), against the working
# copy's src/platform and an oracle copy of naudio_hle.*, aspmain_hle.* and
# audio_hle_diff.* (default: those files at git HEAD, extracted to a temporary
# directory). The game repository only supplies the N64ModernRuntime headers.
#
#   OUT=...      the binary (default tools/naudio_test/audio_fuzz)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}
oracle=${2:-}
nmr=$game/lib/N64ModernRuntime
plat=$here/../../src/platform
work=${TMPDIR:-/tmp}/audio_fuzz_build.$$
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT
if [ -z "$oracle" ]; then
    oracle=$work/oracle; mkdir -p "$oracle"
    for f in naudio_hle.cpp naudio_hle.h aspmain_hle.cpp aspmain_hle.h audio_hle_diff.cpp audio_hle_diff.h; do
        git -C "$here/../.." show "HEAD:src/platform/$f" > "$oracle/$f"
    done
fi
inc=(-I"$nmr/librecomp/include" -I"$nmr/librecomp/include/librecomp" -I"$nmr/N64Recomp/include" -I"$nmr/ultramodern/include" -I"$nmr/thirdparty/concurrentqueue")
flags=(-std=c++20 -O2 -fno-strict-aliasing -w ${FUZZ_CXXFLAGS:-})
# The oracle in its own namespace (every recomp3ds:: name becomes recomp3ds_oracle::).
for f in naudio_hle aspmain_hle audio_hle_diff; do
    g++ "${flags[@]}" -Drecomp3ds=recomp3ds_oracle -I"$oracle" "${inc[@]}" -c "$oracle/$f.cpp" -o "$work/oracle_$f.o"
done
g++ "${flags[@]}" -I"$plat" "${inc[@]}" \
    -DORACLE_NAUDIO_H="\"$oracle/naudio_hle.h\"" -DORACLE_ASPMAIN_H="\"$oracle/aspmain_hle.h\"" \
    "$here/audio_fuzz.cpp" "$plat/naudio_hle.cpp" "$plat/aspmain_hle.cpp" "$plat/audio_hle_diff.cpp" \
    "$work"/oracle_*.o "$nmr/librecomp/src/rsp.cpp" \
    ${FUZZ_CXXFLAGS:-} -o "${OUT:-$here/audio_fuzz}"
echo "built ${OUT:-$here/audio_fuzz} (oracle: $oracle)"
