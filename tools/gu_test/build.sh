#!/usr/bin/env bash
# build.sh <game repo> [bodies.c]: builds gu_test (run it as
# `gu_test <cases> <rom.z64>`: the boot segment's constants come from the
# ROM) from the game's recompiled
# matrix helpers (extracted from its RecompiledFuncs, which must have been
# generated WITHOUT the helpers on the config's ignored list; or a file of
# their bodies saved earlier) and src/platform/gu_native.cpp.
#   OUT=...   the binary (default tools/gu_test/gu_test)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?game repo}
bodies=${2:-}
nmr=$game/lib/N64ModernRuntime
out=${OUT:-$here/gu_test}
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
if [ -z "$bodies" ]; then
    bodies=$work/bodies.c
    python3 - "$game" "$bodies" <<'PY'
import sys, re, glob
want = {'guMtxL2F','guMtxF2L','guMtxIdentF','guMtxIdent','guMtxCatF','guMtxXFMF','guMtxXFML','guNormalize','guRotateF','guTranslateF','guScaleF','sinf_recomp','cosf_recomp','sqrtf_recomp'}
out = []
for f in sorted(glob.glob(sys.argv[1] + '/RecompiledFuncs/*.c')):
    cur=None; body=[]
    for l in open(f):
        m = re.match(r'RECOMP_FUNC void (\w+)\(', l)
        if m: cur=m.group(1); body=[]
        if cur: body.append(l)
        if cur and l.startswith(';}'):
            if cur in want: out.append(''.join(body))
            cur=None
missing = want - {re.match(r'RECOMP_FUNC void (\w+)', b).group(1) for b in out}
if missing: sys.exit('bodies missing from RecompiledFuncs (generated with the helpers ignored?): ' + ' '.join(sorted(missing)))
open(sys.argv[2],'w').write(''.join(out))
PY
fi
# The reference bodies: the helpers renamed ref_*, the renamed libultra sinf,
# cosf and sqrtf kept (the host functions call them too). Empty trace macros.
printf '#define TRACE_ENTRY()\n#define TRACE_RETURN()\n' > "$work/trace.h"
sed -E 's/\b(guMtxL2F|guMtxF2L|guMtxIdentF|guMtxIdent|guMtxCatF|guMtxXFMF|guMtxXFML|guNormalize|guRotateF|guTranslateF|guScaleF)\b/ref_\1/g' "$bodies" > "$work/ref.c"
{ printf '#include "recomp.h"\n#include "trace.h"\n'; for f in guMtxL2F guMtxF2L guMtxIdentF guMtxIdent guMtxCatF guMtxXFMF guMtxXFML guNormalize guRotateF guTranslateF guScaleF; do printf 'void ref_%s(uint8_t* rdram, recomp_context* ctx);\n' "$f"; done; for f in sinf cosf sqrtf; do printf 'void %s_recomp(uint8_t* rdram, recomp_context* ctx);\n' "$f"; done; cat "$work/ref.c"; } > "$work/ref_all.c"
cc -std=gnu11 -O2 -fno-strict-aliasing -ffp-contract=off -w -I"$work" -I"$nmr/N64Recomp/include" -c "$work/ref_all.c" -o "$work/ref.o"
c++ -std=c++20 -O2 -ffp-contract=off -Wall -I"$nmr/N64Recomp/include" -I"$here/../../src/platform" -c "$here/../../src/platform/gu_native.cpp" -o "$work/native.o"
c++ -std=c++20 -O2 -ffp-contract=off -I"$nmr/N64Recomp/include" "$here/main.cpp" "$work/native.o" "$work/ref.o" -lm -o "$out"
echo "built $out"
