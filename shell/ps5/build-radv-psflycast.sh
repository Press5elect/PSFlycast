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
# The driver is PS5 Mesa (github.com/mihawk-99/PS5_Mesa) at the pin 1ba9874f478 with every
# patch of Crux2's ps5/patches/mesa-27 (github.com/Press5elect/Crux2, Crux2 by Press5elect), in
# order, applied the way Crux2's ps5/tools/build-radv-crux2.sh applies them (a git format-patch
# with `git am -3`, a plain diff with `git apply --index` and a commit). Among them:
#   0012  Mesa main (27.0-devel) merged into the PS5 RADV;
#   0015  PSFlyCast's 119.88 Hz on a VRR display, and PS5_VIDEOOUT_59HZ;
#   0016  PSFlyCast's variable refresh on request (PS5_VIDEOOUT_VRR);
#   0017  variable refresh found while the title runs: the driver calls the title's weak
#         ps5_title_videoout_vrr_unpeg (shell/ps5/ps5_videoout.cpp) instead of importing
#         sceVideoOutVrrUnpegFromFixedRate, which not every firmware exports.
# 0015 and 0016 are this repository's shell/ps5/mesa/0001 and 0002, as they are; Crux2 carries them.
# PSFlyCast's own copies are applied only when the Crux2 set does not already hold them.
# After them, four commits of PS5 Mesa's main that the Crux2 set does not have yet:
#   0003, 0004  PS5 Mesa 53cc83ff82b and 249fe49d2d8: a large buffer is backed by several
#               direct-memory ranges when no single one is free, each placed only where nothing
#               else is (0003 had one conflict with Crux2's patches, resolved by hand and said in
#               its message);
#   0005        PS5 Mesa 7bd4aac55d8: a cached graphics pipeline that lost a stage is compiled
#               again instead of created from the broken entry;
#   0006        PS5 Mesa 0f86253f674: the mesh shaders' scratch ring is 128 MiB, not 320.
#   0007        OGBox's HDR10 framebuffers on request (github.com/Press5elect/OGBox, its
#               ps5/patches/mesa-27/0019): wsi_videoout_set_hdr10 retypes the framebuffers as
#               HDR10 for shell/ps5/ps5_hdr.cpp.
# PS5 Mesa's
# f5610fe2709 (a freed buffer's memory kept until the GPU has run what was submitted before it) is
# not applied: the Crux2 set already keeps a freed buffer's memory that way (its zombie list), and
# f5610fe also covers sparse ranges and a retry after a failed allocation, which this title does not use.
#
# The Crux2 revision the release names is CRUX2_REVISION below: the patches are taken from a
# Crux2 checkout at that revision (../crux2 or CRUX2_MESA_PATCHES).
#
# Everything it applies is public, so the driver's source is the pin, Crux2 at that revision,
# and this repository at its tag. The result is branch psflycast-radv-27 of ../PS5_Mesa.
# Nothing is pushed.
#
# The build is PS5_Vulkan's tools/build-radv.sh release, at the branch's head. Its result is
# moved to PS5_Vulkan's .deps/native/radv-psflycast, which shell/ps5/build.sh and ps5-link.sh
# link when it is there. PS5_Vulkan's own release archive (.deps/native/radv-release) is put
# back as it was.
#
# With the branch already there it is built as it is; PSFLYCAST_RADV_REMAKE=1 (or no branch
# yet) makes it again from the pin.
#
# Environment: PS5_VULKAN_DIR, PS5_MESA_FORK, CRUX2_MESA_PATCHES name other places; JOBS is
# passed on to the build (when build-radv.sh reads it).
set -euo pipefail
src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
vulkan=$(cd -- "${PS5_VULKAN_DIR:-$src/../PS5_Vulkan}" && pwd)
mesa=$(cd -- "${PS5_MESA_FORK:-$src/../PS5_Mesa}" && pwd)
CRUX2_REVISION=08c3507a3ac3f3f904cb36f02cef1f36335a67bc
patches=${CRUX2_MESA_PATCHES:-$src/../crux2/ps5/patches/mesa-27}
own="$src/shell/ps5/mesa"
pin=1ba9874f478657ccf6da72ce55f0260164cc0bd0
branch=psflycast-radv-27
who=(-c user.name=PSFlyCast -c user.email=noreply@anthropic.com)

[[ -d $patches ]] && compgen -G "$patches/*.patch" > /dev/null ||
	{ echo "build-radv-psflycast: no Crux2 patches in $patches (set CRUX2_MESA_PATCHES)" >&2; exit 1; }
crux2=$(cd -- "$patches/../../.." && pwd)
if have=$(git -C "$crux2" rev-parse HEAD 2>/dev/null) && [[ $have != "$CRUX2_REVISION" ]]; then
	echo "build-radv-psflycast: Crux2 at $crux2 is at ${have:0:11}; this release names ${CRUX2_REVISION:0:11}" >&2
	echo "  (git -C $crux2 checkout $CRUX2_REVISION, or set CRUX2_MESA_PATCHES to a checkout there)" >&2
	exit 1
fi

# The branch is made in the fork's working tree: that must hold no change of its own.
if [[ -n $(git -C "$mesa" status --porcelain --untracked-files=no) ]]; then
	echo "build-radv-psflycast: $mesa has uncommitted changes; commit or stash them first" >&2
	exit 1
fi

# A patch's subject line, to tell whether the Crux2 set already holds one of PSFlyCast's.
subject() { sed -n 's/^Subject: \(\[PATCH[^]]*\] \)\{0,1\}//p' "$1" | head -1 | cut -c1-48; }

make_branch() {
	echo "==> PS5 Mesa ${pin:0:11} with $(ls "$patches"/*.patch | wc -l) Crux2 patches"
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
	for patch in "$own"/*.patch; do
		[[ -f $patch ]] || continue
		s=$(subject "$patch")
		if git -C "$mesa" log --format=%s "$pin..$branch" | grep -qF "$s"; then
			echo "    $(basename "$patch" .patch): in the Crux2 set already"
			continue
		fi
		git -C "$mesa" "${who[@]}" am --quiet -3 "$patch"
		echo "    $(basename "$patch" .patch)"
	done
}

if [[ ${PSFLYCAST_RADV_REMAKE:-0} == 1 ]] || ! git -C "$mesa" rev-parse --verify --quiet "refs/heads/$branch" > /dev/null; then
	make_branch
fi

# The branch must be the pin plus the patches, PSFlyCast's display changes and the
# run-time variable refresh among them.
git -C "$mesa" merge-base --is-ancestor "$pin" "$branch" ||
	{ echo "build-radv-psflycast: $branch is not built on ${pin:0:11}" >&2; exit 1; }
for wanted in "119.88 Hz on a VRR display" "leave its fixed rate" "variable refresh found while the title runs" \
	"back a large buffer with several direct-memory" "a buffer piece is mapped only where nothing else is" \
	"a cached graphics pipeline that lost a stage is a miss" "scratch ring is 128 MiB" \
	"HDR10 framebuffers on request"; do
	git -C "$mesa" log --format=%s "$pin..$branch" | grep -q "$wanted" ||
		{ echo "build-radv-psflycast: $branch lacks \"$wanted\"" >&2; exit 1; }
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
# Mesa 27 (patch 0012) replaced meson's amd-use-llvm with the amd-options array, which PS5_Vulkan's
# build-radv.sh at f410e67 still passes (meson refuses the old name). PS5_Vulkan stays as published:
# a copy of its script beside it, with the option named the new way, builds when the tree asks.
builder="$vulkan/tools/build-radv.sh"
copy=""
if git -C "$mesa" show "$branch:meson.options" 2>/dev/null | grep -q "'amd-options'" &&
	grep -q -- '-Damd-use-llvm=false' "$builder"; then
	copy="$vulkan/tools/.build-radv-psflycast.sh"
	sed 's/-Damd-use-llvm=false/-Damd-options=/' "$builder" > "$copy"
	builder="$copy"
	echo "==> PS5_Vulkan's build-radv.sh with amd-options in place of amd-use-llvm (Mesa 27)"
fi
restore_all() { [[ -n $copy ]] && rm -f -- "$copy"; restore; }
trap restore_all EXIT
(cd "$vulkan" && PS5_MESA_FORK="$mesa" PS5_MESA_REVISION="$revision" JOBS="${JOBS:-}" bash "$builder" release)
rm -rf "$native/radv-psflycast"
mv "$native/radv-release" "$native/radv-psflycast"
{
	echo
	echo "PSFlyCast: PS5 Mesa $pin, branch $branch at $revision (tree $(git -C "$mesa" rev-parse "$branch^{tree}")),"
	echo "with Crux2's patches at $CRUX2_REVISION:"
	for patch in "$patches"/*.patch; do echo "  $(basename "$patch")"; done
	echo "and PSFlyCast's own, from shell/ps5/mesa (the first two are Crux2's 0015 and 0016):"
	for patch in "$own"/*.patch; do
		[[ -f $patch ]] || continue
		echo "  $(basename "$patch")"
	done
} >> "$native/radv-psflycast/PROVENANCE.txt"
echo "==> $native/radv-psflycast (Mesa $branch ${revision:0:11}); shell/ps5/build.sh links it"
