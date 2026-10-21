#!/usr/bin/env bash
# build.sh: builds reverb_match, which checks dsp_reverb_match.cpp (OpenDSP's
# reverb settings for a game's effect) against OpenDSP's own firmware reverb
# (third_party/OpenDSP/firmware/reverb.c, its host build).
#
#   tools/reverb_match/build.sh && tools/reverb_match/reverb_match <log.txt>...
#
# Each distinct effect the log's "dsp-reverb: effect" lines name is matched;
# the game's decay and loudness are printed beside those of the model and of
# the firmware at the chosen settings.
#   OUT=...   the binary (default tools/reverb_match/reverb_match)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$here/../..
fw=$root/third_party/OpenDSP/firmware
out=${OUT:-$here/reverb_match}
obj=$(mktemp -d)
trap 'rm -rf "$obj"' EXIT
cc -O2 -w -I"$fw" -c "$fw/reverb.c" -o "$obj/reverb.o"
c++ -std=c++20 -O2 -Wall -I"$root/src/platform" -I"$fw" "$here/main.cpp" "$root/src/platform/dsp_reverb_match.cpp" "$obj/reverb.o" -o "$out"
echo "built $out"
