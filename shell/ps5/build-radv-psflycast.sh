#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# Copyright 2026 Press5elect
# SPDX-License-Identifier: GPL-2.0-or-later
#
# PSFlyCast: build the RADV archive PSFlyCast links, with Crux2's driver patches.
#
#   shell/ps5/build-radv-psflycast.sh
#   CRUX2_MESA_PATCHES=/path/to/crux2/ps5/patches/mesa-27 shell/ps5/build-radv-psflycast.sh
#   PSFLYCAST_RADV_REMAKE=1 shell/ps5/build-radv-psflycast.sh   (make the branch again first)
#
# The driver is PS5 Mesa at the upstream pin (1ba9874f478) with:
#   1. every patch of Crux2's ps5/patches/mesa-27, in order, the way Crux2's
#      ps5/tools/build-radv-crux2.sh applies them (a git format-patch with
#      `git am -3`, a plain diff with `git apply --index` and a commit); 0012
#      among them merges Mesa main (27.0-devel);
#   2. PSFlyCast's own two driver commits, cherry-picked on top:
#        56bec6e2a8c wsi/videoout: 119.88 Hz on a VRR display; a title may ask for 59.94 Hz
#        72a093bb459 wsi/videoout: a title may ask the output to leave its fixed rate
#
# That is branch psflycast-radv-27 of ../PS5_Mesa. Nothing is pushed.
#
# The first cherry-pick conflicts with Crux2's 0003 (the 120 Hz settle loop),
# and was resolved by hand once: the settle loop is kept, and a vblank period
# at the 48 Hz floor of a VRR window ends it and keeps 119.88 Hz. So the
# script builds the branch as it is when it is there. With
# PSFLYCAST_RADV_REMAKE=1, or with no branch yet, it makes the branch again
# and stops at a conflict for it to be resolved the same way.
#
# The build is PS5_Vulkan's tools/build-radv.sh release, at the branch's head.
# Its result is moved to PS5_Vulkan's .deps/native/radv-psflycast, which
# shell/ps5/build.sh and ps5-link.sh link when it is there. PS5_Vulkan's own
# release archive (.deps/native/radv-release) is put back as it was.
#
# Environment: PS5_VULKAN_DIR, PS5_MESA_FORK, CRUX2_MESA_PATCHES name other
# places; JOBS is passed on to the build (when build-radv.sh reads it).
set -euo pipefail
src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$src/../PS5_Vulkan}" && pwd)
mesa=$(cd -- "${PS5_MESA_FORK:-$src/../PS5_Mesa}" && pwd)
patches=${CRUX2_MESA_PATCHES:-/home/claude/press5elect/crux2/ps5/patches/mesa-27}
pin=1ba9874f478657ccf6da72ce55f0260164cc0bd0
picks=(56bec6e2a8cc21073872defb976aae19f340d627 72a093bb45936dccef266ef09621f50174d6234b)
branch=psflycast-radv-27
who=(-c user.name=PSFlyCast -c user.email=noreply@anthropic.com)

[[ -d $patches ]] && compgen -G "$patches/*.patch" > /dev/null ||
    { echo "build-radv-psflycast: no patches in $patches (set CRUX2_MESA_PATCHES)" >&2; exit 1; }

# The branch is made in the fork's working tree: that must hold no change of its own.
if [[ -n $(git -C "$mesa" status --porcelain --untracked-files=no) ]]; then
    echo "build-radv-psflycast: $mesa has uncommitted changes; commit or stash them first" >&2
    exit 1
fi

make_branch() {
    echo "==> PS5 Mesa ${pin:0:11} with $(ls "$patches"/*.patch | wc -l) Crux2 patches and ${#picks[@]} PSFlyCast commits"
    git -C "$mesa" cat-file -e "$pin^{commit}" 2>/dev/null || git -C "$mesa" fetch --quiet origin
    git -C "$mesa" checkout --quiet -B "$branch" "$pin"
    for patch in "$patches"/*.patch; do
        name=$(basename "$patch" .patch)
        if head -1 "$patch" | grep -q '^From [0-9a-f]\{40\}'; then
            # A git format-patch: its author, date and message are kept.
            git -C "$mesa" "${who[@]}" am --quiet -3 "$patch"
        else
            git -C "$mesa" apply --index "$patch"
            git -C "$mesa" "${who[@]}" commit --quiet -m "PSFlyCast (Crux2 patch): $name"
        fi
        echo "    $name"
    done
    for pick in "${picks[@]}"; do
        if ! git -C "$mesa" "${who[@]}" cherry-pick -x "$pick"; then
            echo "build-radv-psflycast: cherry-pick ${pick:0:11} conflicts; resolve it in $mesa" >&2
            echo "  (keep Crux2's settle loop and queue depth, and the VRR / 59.94 Hz paths)," >&2
            echo "  run git cherry-pick --continue, then this script again without PSFLYCAST_RADV_REMAKE" >&2
            exit 1
        fi
        echo "    cherry-pick $(git -C "$mesa" log -1 --format='%h %s' "$pick")"
    done
}

if [[ ${PSFLYCAST_RADV_REMAKE:-0} == 1 ]] || ! git -C "$mesa" rev-parse --verify --quiet "refs/heads/$branch" > /dev/null; then
    make_branch
fi

# The branch must be the pin plus the patches plus the two commits.
git -C "$mesa" merge-base --is-ancestor "$pin" "$branch" ||
    { echo "build-radv-psflycast: $branch is not built on ${pin:0:11}" >&2; exit 1; }
for pick in "${picks[@]}"; do
    git -C "$mesa" log --format=%B "$pin..$branch" | grep -q "cherry picked from commit $pick" ||
        { echo "build-radv-psflycast: $branch lacks ${pick:0:11}" >&2; exit 1; }
done
revision=$(git -C "$mesa" rev-parse "$branch")
echo "==> building $branch at ${revision:0:11}"

# PS5_Vulkan's release archive is held aside while its build script builds ours in its place.
native="$vulkan/.deps/native"
hold=""
if [[ -d "$native/radv-release" ]]; then
    hold=$(mktemp -d "$native/.radv-release-hold.XXXXXX")
    mv "$native/radv-release" "$hold/"
fi
restore() {
    if [[ -n "$hold" && -d "$hold/radv-release" ]]; then
        rm -rf "$native/radv-release"
        mv "$hold/radv-release" "$native/"
        rmdir "$hold"
    fi
}
trap restore EXIT
(cd "$vulkan" && PS5_MESA_FORK="$mesa" PS5_MESA_REVISION="$revision" JOBS="${JOBS:-}" bash tools/build-radv.sh release)
rm -rf "$native/radv-psflycast"
mv "$native/radv-release" "$native/radv-psflycast"
{
    echo
    echo "PSFlyCast: PS5 Mesa $pin, branch $branch at $revision, with Crux2's patches"
    echo "from $patches:"
    for patch in "$patches"/*.patch; do echo "  $(basename "$patch")"; done
    echo "and PSFlyCast's commits, cherry-picked:"
    for pick in "${picks[@]}"; do echo "  $(git -C "$mesa" log -1 --format='%h %s' "$pick")"; done
} >> "$native/radv-psflycast/PROVENANCE.txt"
echo "==> $native/radv-psflycast (Mesa $branch ${revision:0:11}); shell/ps5/build.sh links it"
