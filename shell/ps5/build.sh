#!/usr/bin/env bash
# PSFlyCast: build the homebrew title.
#
#   shell/ps5/build.sh [OUT_DIR]
#
# Builds Flycast with shell/ps5/ps5-toolchain.cmake, links it into a signed
# eboot.bin (shell/ps5/ps5-link.sh), and stages the title folder
# OUT_DIR/PPSA99247 (default: build-ps5/dist), ready to copy to
# /data/homebrew/PPSA99247 on the console.
#
# PS5_VULKAN_DIR names a PS5_Vulkan checkout (default: ../PS5_Vulkan next to
# this repository) prepared with:
#   tools/setup-native-dependencies.sh
#   tools/build-radv.sh release       # RADV, linked into the eboot
#   tools/rebuild-libc.sh             # sce_module/libc.prx and the host tool
set -euo pipefail
src=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
vk=$(cd -- "${PS5_VULKAN_DIR:-$src/../PS5_Vulkan}" && pwd)
export PS5_VULKAN_DIR="$vk"
# The payload SDK fork at cd3b239 or later (PS5 RetroArch v0.5.6-alpha.5's):
# its heap clears what calloc promises is zero, which earlier revisions did
# not. PS5_PAYLOAD_SDK names it; PS5 RetroArch's dependency tree has it.
export PS5_PAYLOAD_SDK=${PS5_PAYLOAD_SDK:-$src/../PS5_RetroArch/.deps/native/ps5-payload-sdk}
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}
build="$src/build-ps5"
out=${1:-$build/dist}
title=PPSA99247

missing=0
for file in "$PS5_PAYLOAD_SDK/bin/prospero-clang++" \
        "$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a" \
        "$vk/runtime/libc.prx" "$vk/build/runtime-shim/ps5-native-tool"; do
    [[ -e $file ]] || { echo "missing: $file" >&2; missing=1; }
done
if (( missing )); then
    echo "Prepare $vk first: tools/setup-native-dependencies.sh," >&2
    echo "tools/build-radv.sh release and tools/rebuild-libc.sh." >&2
    exit 2
fi
(cd "$vk/runtime" && sha256sum --check --strict --quiet libc.prx.sha256)
platform_symbols=$("$PS5_PAYLOAD_SDK/bin/prospero-nm" "$PS5_PAYLOAD_SDK/target/lib/libps5platform.a" 2>/dev/null || true)
if [[ $platform_symbols != *" T ps5p_run_thread_destructors"* ]]; then
    echo "The payload SDK at $PS5_PAYLOAD_SDK is older than cd3b239; set PS5_PAYLOAD_SDK." >&2
    exit 2
fi

if [[ ! -f $build/build.ninja ]]; then
    cmake -S "$src" -B "$build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$src/shell/ps5/ps5-toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DUSE_VULKAN=ON -DUSE_OPENGL=OFF -DUSE_OPENMP=OFF \
        -DUSE_HOST_LIBZIP=OFF -DUSE_HOST_LIBCHDR=OFF -DWITH_SYSTEM_ZSTD=OFF \
        -DUSE_BREAKPAD=OFF -DUSE_LUA=OFF -DUSE_DISCORD=OFF \
        -DUSE_ALSA=OFF -DUSE_LIBAO=OFF -DUSE_PULSEAUDIO=OFF \
        -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON
fi
cmake --build "$build" --target flycast --parallel "${JOBS:-$(nproc)}"

app="$out/$title"
rm -rf -- "$app"
mkdir -p "$app/sce_sys" "$app/sce_module"
cp -- "$build/flycast" "$app/eboot.bin"
cp -- "$src/shell/ps5/sce_sys/param.json" "$src/shell/ps5/sce_sys/icon0.png" "$app/sce_sys/"
# The picture the console shows behind the title on the home screen (pic0:
# the mark and the name) and while it loads (pic1: the first frame of the
# start-up animation, which is the backdrop alone, so the animation begins
# from what is already on the screen).
cp -- "$src/shell/ps5/sce_sys/pic0.dds" "$src/shell/ps5/sce_sys/pic1.dds" "$app/sce_sys/"
# What the console plays while the title is selected on the home screen: a
# loop made by shell/ps5/sounds/make-home-sound.py.
cp -- "$src/shell/ps5/sce_sys/snd0.at9" "$app/sce_sys/"
cp -- "$vk/runtime/libc.prx" "$app/sce_module/libc.prx"
# The folders Flycast keeps its files in (see README).
for dir in games bios covers cheats patches; do
    mkdir -p "$app/$dir"
done
# Game patches are the user's own (shell/ps5/patches/make_patches.py makes
# the list from the community's chart, which states no licence): none is in
# the repository or in a release. PATCHES_FILE stages one, for a build that
# stays on its maker's console.
if [[ -n ${PATCHES_FILE:-} ]]; then
    cp -- "$PATCHES_FILE" "$app/patches/patches.txt"
fi
# The helper elfldr runs when "USB drives" is on (shell/ps5/elevation, from
# ps5-native-app-boilerplate's sandbox-elevation example): built with the
# payload SDK as a payload, and checked to end at its section table.
make -s -C "$src/shell/ps5/elevation/payload" PS5_PAYLOAD_SDK="$PS5_PAYLOAD_SDK" \
    OUTPUT="$build/sandbox-elevator.elf"
python3 "$src/shell/ps5/elevation/validate-elevation-helper.py" "$build/sandbox-elevator.elf"
cp -- "$build/sandbox-elevator.elf" "$app/sandbox-elevator.elf"
# The Dreamcast cheat files of the libretro database (CC BY-SA 4.0), when a
# checkout is there: CHEATS_DIR, or ../libretro-database beside this repository.
cheats=${CHEATS_DIR:-$src/../libretro-database/cht/Sega - Dreamcast}
if [[ -d $cheats ]]; then
    cp -- "$cheats"/*.cht "$app/cheats/"
    echo "Staged $(find "$app/cheats" -name '*.cht' | wc -l) cheat files"
else
    echo "No cheat files staged: $cheats is not there" >&2
fi
# The start-up sound (shell/ps5/sounds: the file and the page that makes it).
mkdir -p "$app/sounds"
cp -- "$src/shell/ps5/sounds/startup.wav" "$app/sounds/startup.wav"
cp -- "$src/shell/ps5/README.md" "$src/shell/ps5/MANUAL.md" "$app/"
{
    echo "PSFlyCast${RELEASE_TAG:+ $RELEASE_TAG}, build $(sed -n 's/^#define PS5_BUILD_NUMBER //p' "$src/shell/ps5/ps5_build.h"), built $(date -u +%Y-%m-%d)"
    echo "flycast:    $(git -C "$src" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "PS5_Vulkan: $(git -C "$vk" rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "SDK:        $(cat "$PS5_PAYLOAD_SDK/.ps5-sdk-revision" 2>/dev/null || echo unknown)"
    grep -h -E '^(revision|sdk):' "$vk/.deps/native/radv-release/PROVENANCE.txt" 2>/dev/null | sed 's/^/RADV /'
    echo "eboot.bin sha256: $(sha256sum "$app/eboot.bin" | cut -d' ' -f1)"
} > "$app/BUILD.txt"
# LEGAL.txt and licenses/: every part's licence, texts, source and revision,
# and a check of what is in the folder. RELEASE_TAG makes it a release build:
# it then refuses source with uncommitted changes.
python3 "$src/shell/ps5/release/stage-notices.py" "$app" ${RELEASE_TAG:+--release "$RELEASE_TAG"}
printf 'Built %s (eboot.bin %s bytes)\n' "$app" "$(stat -c %s "$app/eboot.bin")"
