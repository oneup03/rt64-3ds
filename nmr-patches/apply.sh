#!/usr/bin/env bash
# Apply the runtime patch series to a game's N64ModernRuntime checkout.
#
#   nmr-patches/apply.sh <path/to/lib/N64ModernRuntime>
#
# The nested N64Recomp submodule gets n64recomp/*.patch, the runtime itself
# nmr/*.patch, each as commits on a local branch "3ds-patched" so the game
# repository's submodule pointer stays on the upstream commit (those commits
# exist only here, so a pointer to them could not be fetched by CI or another
# clone). A patch whose subject is already a commit there is skipped, so
# re-running applies only what the series has gained since; a patch that
# changed after it was applied is reported (the subject still matches), and
# the checkout is then regenerated from its base. After a game-level
# `git submodule update` the checkout is back at the base: run this again.
# Never run `git submodule update` inside the runtime: it would check its
# N64Recomp out at the unpatched pointer.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
nmr=${1:?path to N64ModernRuntime}
[ -e "$nmr/N64Recomp/.git" ] || { echo "N64Recomp is not checked out: git -C '$nmr' submodule update --init" >&2; exit 1; }
# git am needs a committer identity; a fresh clone or a CI job may have none.
ident=()
git config user.email >/dev/null 2>&1 || ident=(-c user.name="rt64-3ds apply.sh" -c user.email="rt64-3ds@localhost")
apply_series() {
    local repo=$1 dir=$2
    local subjects missing=() stale=() p subject sha
    subjects=$(git -C "$repo" log --format=%s -300 2>/dev/null || true)
    for p in "$dir"/*.patch; do
        subject=$(git -C "$repo" mailinfo /dev/null /dev/null < "$p" | sed -n 's/^Subject: //p')
        if grep -qxF -- "$subject" <<< "$subjects"; then
            # Applied: still the same patch? (The file may have been edited.)
            sha=$(git -C "$repo" log --format=%H -1 --fixed-strings --grep="$subject" 2>/dev/null || true)
            if [ -n "$sha" ] && [ "$(git -C "$repo" show --format= "$sha" | git patch-id --stable | cut -d' ' -f1)" != "$(git patch-id --stable < "$p" | cut -d' ' -f1)" ]; then
                stale+=("$(basename "$p")")
            fi
        else
            missing+=("$p")
        fi
    done
    if [ ${#stale[@]} -ne 0 ]; then
        echo "WARNING: applied in $repo but changed since: ${stale[*]}" >&2
        echo "         regenerate: reset the checkout to the game's recorded commit (git -C <game> submodule update lib/N64ModernRuntime) and run this again" >&2
    fi
    if [ ${#missing[@]} -eq 0 ]; then
        echo "already patched: $repo"; return
    fi
    # None applied yet: start the local branch at the checkout's commit.
    if [ ${#missing[@]} -eq "$(ls "$dir"/*.patch | wc -l)" ]; then
        git -C "$repo" checkout -q -B 3ds-patched
    fi
    if ! git -C "$repo" "${ident[@]}" am -q --3way "${missing[@]}"; then
        echo "a patch did not apply in $repo: fix it up and 'git -C \"$repo\" am --continue', or 'git -C \"$repo\" am --abort'" >&2
        exit 1
    fi
    echo "patched: $repo (${#missing[@]} patches)"
}
apply_series "$nmr/N64Recomp" "$here/n64recomp"
apply_series "$nmr" "$here/nmr"
