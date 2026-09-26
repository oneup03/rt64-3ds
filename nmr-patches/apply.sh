#!/usr/bin/env bash
# Apply the runtime patch series to a game's N64ModernRuntime checkout.
#
#   nmr-patches/apply.sh <path/to/lib/N64ModernRuntime>
#
# The nested N64Recomp submodule gets n64recomp/*.patch, the runtime itself
# nmr/*.patch, each as commits on a local branch "3ds-patched" so the game
# repository's submodule pointer stays on the fork's own commit. Re-running
# on an already patched tree is a no-op.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
nmr=${1:?path to N64ModernRuntime}
apply_series() {
    local repo=$1 dir=$2
    if git -C "$repo" log --oneline -50 2>/dev/null | grep -q "3DS patch series applied"; then
        echo "already patched: $repo"; return
    fi
    git -C "$repo" checkout -q -B 3ds-patched
    git -C "$repo" am -q --3way "$dir"/*.patch
    git -C "$repo" commit -q --allow-empty -m "3DS patch series applied (rt64-3ds)"
    echo "patched: $repo ($(ls "$dir"/*.patch | wc -l) patches)"
}
apply_series "$nmr/N64Recomp" "$here/n64recomp"
apply_series "$nmr" "$here/nmr"
