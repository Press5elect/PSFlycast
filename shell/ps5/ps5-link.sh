#!/usr/bin/env bash
# PSFlyCast: link the executable CMake built into a signed eboot.
#
#   ps5-link.sh TARGET OBJECT... -- LIBRARY...
#
# CMake calls this as the executable's link rule (shell/ps5/ps5.cmake). The
# recipe is PS5_Vulkan's tools/build-radv-title.sh, the title that runs RADV on
# the console: the repository's CRT and C++ runtime, the AGC link stubs, RADV
# and the payload SDK's libc++ through tools/radv-link.sh, then the host tool's
# link (imports and SCE dynamic tables) and fake-signed SELF. TARGET receives
# the signed eboot.
#
# PS5_VULKAN_DIR names the PS5_Vulkan checkout, with RADV built
# (tools/build-radv.sh release) and libc.prx and the host tool rebuilt
# (tools/rebuild-libc.sh). RADV_ARCHIVE may name another RADV archive.
set -euo pipefail
target=$1
shift
objects=()
while (( $# )) && [[ $1 != -- ]]; do
    objects+=("$1")
    shift
done
[[ ${1:-} == -- ]] && shift
archives=()
for item in "$@"; do
    case $item in
        # RADV's archive carries zlib 1.3.1 (Mesa's subproject), linked whole:
        # Flycast's bundled copy of the same release would duplicate it.
        */libz.a) ;;
        *.a | *.o) archives+=("$item") ;;
        *) ;;  # -l and -pthread flags: the console's libraries come from the SDK below
    esac
done

: "${PS5_VULKAN_DIR:?set PS5_VULKAN_DIR to the PS5_Vulkan checkout}"
vk=$(cd -- "$PS5_VULKAN_DIR" && pwd)
# The payload SDK fork: PS5_PAYLOAD_SDK (shell/ps5/build.sh), else PS5_Vulkan's.
sdk_root=${PS5_PAYLOAD_SDK:-$vk/.deps/native/ps5-payload-sdk}
native="$vk/tooling/native"
tool="$vk/build/runtime-shim/ps5-native-tool"
archive=${RADV_ARCHIVE:-$vk/.deps/native/radv-release/lib/libvulkan_radeon.ps5.a}
for file in "$archive" "$tool" "$sdk_root/bin/prospero-lld"; do
    [[ -e $file ]] || { echo "ps5-link: missing $file" >&2; exit 2; }
done
export PS5_PAYLOAD_SDK="$sdk_root"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang)}

work="$(dirname -- "$target")/ps5-link"
mkdir -p "$work/obj" "$work/stubs"
cc() { sh "$vk/tooling/prospero-clang18" "$@"; }
# The title's start (shell/ps5/runtime): ps5_crt.cpp clears the BSS, which
# the console's loader leaves holding old memory, before anything runs.
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
runtime="$here/runtime"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$runtime/ps5_crt.cpp" -o "$work/obj/ps5_crt.o"
cc -std=c++20 -O2 -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
    -c "$runtime/app_cpp_runtime.cpp" -o "$work/obj/app_cpp_runtime.o"
# AGC comes from system modules; these host-link stubs only name the imports.
stub() {
    local library=$1 source=$2
    cc -std=c11 -O2 -fPIC -c "$vk/$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.prx" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc vendor/ps5/sdk/stubs/agc_canary_link_stub.c
stub libSceAgcDriver vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c

# shellcheck source=/dev/null
source "$vk/tools/radv-link.sh"
radv_link_recipe "$vk" "$sdk_root" "$archive" || exit 2
# Threads: shell/ps5/ps5_threads.cpp's wrap (2 MiB stacks from libkernel, as
# PS5 RetroArch's threads have) replaces the SDK's direct-memory stacks.
filtered=()
for flag in "${radv_link_flags[@]}"; do
    case $flag in
        --wrap=pthread_create | --wrap=pthread_join | --wrap=pthread_detach) ;;
        *) filtered+=("$flag") ;;
    esac
done
radv_link_flags=("${filtered[@]}" --wrap=pthread_create)
# fgetpos and fsetpos: the console's fpos_t is larger than the SDK's
# (shell/ps5/ps5_stdio.cpp).
radv_link_flags+=(--wrap=fgetpos --wrap=fsetpos)

# Imports a title cannot use as they are (only libkernel_sys exports them, the
# SDK lists them but the console lacks them, or a title is refused them):
# bound to shell/ps5/ps5_libc.cpp's versions, kept local.
title_defsyms=()
{
    printf '{\n    local:\n'
    for name in fork link symlink readlink pathconf isatty getcwd realpath mkstemp \
            gai_strerror gethostbyname getnameinfo getaddrinfo freeaddrinfo in6addr_any localeconv; do
        title_defsyms+=("--defsym=$name=ps5_flycast_$name")
        printf '        %s;\n' "$name"
    done
    printf '};\n'
} > "$work/title-local.map"

# Mesa leaves the entry points RADV does not implement as weak references that
# resolve to null; they must not become imports (PS5 RetroArch links the same way).
# The title's own layout (shell/ps5/runtime/ps5-title.ld: PS5 RetroArch's, with
# the BSS and code bounds) instead of the recipe's.
"$sdk_root/bin/prospero-lld" -T "$runtime/ps5-title.ld" --eh-frame-hdr --error-limit=0 \
    --no-dynamic-linker -z nodynamic-undefined-weak \
    "${radv_link_flags[@]}" "${title_defsyms[@]}" --version-script "$work/title-local.map" \
    --version-script "$runtime/app-symbols.map" --exclude-libs=ALL \
    --Map="$work/flycast.map" \
    -e _start -o "$work/llvm-pie.elf" \
    "$work/obj/ps5_crt.o" "$work/obj/app_cpp_runtime.o" "${objects[@]}" \
    --start-group "${archives[@]}" --end-group \
    "$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" --stub "$work/stubs/libSceAgc.so" \
    --stub "$work/stubs/libSceAgcDriver.so" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$target" --magic 0x1D3D154F
"$tool" self --inspect --file "$target" > "$work/eboot.inspect.txt"
# Kept for shell/ps5/symbolize.sh, which turns a crash report's eboot offsets
# into functions.
cp -f -- "$work/llvm-pie.elf" "$(dirname -- "$target")/flycast-eboot.elf"
printf 'ps5-link: %s (%s bytes)\n' "$target" "$(stat -c %s "$target")"
