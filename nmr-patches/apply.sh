#!/usr/bin/env bash
# Apply the runtime patch series to a game's N64ModernRuntime checkout.
#
#   nmr-patches/apply.sh <path/to/lib/N64ModernRuntime>
#
# The nested N64Recomp submodule gets n64recomp/*.patch, the runtime itself
# nmr/*.patch, each as commits on a local branch "3ds-patched" so the game
# repository's submodule pointer stays on the fork's own commit. A patch
# whose subject is already a commit there is skipped, so re-running applies
# only what the series has gained since.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
nmr=${1:?path to N64ModernRuntime}
apply_series() {
    local repo=$1 dir=$2
    local subjects missing=() p subject
    subjects=$(git -C "$repo" log --format=%s -300 2>/dev/null || true)
    for p in "$dir"/*.patch; do
        subject=$(git -C "$repo" mailinfo /dev/null /dev/null < "$p" | sed -n 's/^Subject: //p')
        grep -qxF -- "$subject" <<< "$subjects" || missing+=("$p")
    done
    if [ ${#missing[@]} -eq 0 ]; then
        echo "already patched: $repo"; return
    fi
    # None applied yet: start the local branch at the fork's commit.
    if [ ${#missing[@]} -eq "$(ls "$dir"/*.patch | wc -l)" ]; then
        git -C "$repo" checkout -q -B 3ds-patched
    fi
    git -C "$repo" am -q --3way "${missing[@]}"
    echo "patched: $repo (${#missing[@]} patches)"
}
apply_series "$nmr/N64Recomp" "$here/n64recomp"
apply_series "$nmr" "$here/nmr"
