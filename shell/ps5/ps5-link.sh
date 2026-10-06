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
tool="$vk/build/host/ps5-native-tool"
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
    local library=$1 source=$2 suffix=${3:-prx}
    cc -std=c11 -O2 -fPIC -c "$source" -o "$work/obj/${library}_stub.o"
    "$sdk_root/bin/prospero-lld" --shared -soname "${library}.${suffix}" \
        -o "$work/stubs/${library}.so" "$work/obj/${library}_stub.o"
}
stub libSceAgc "$vk/vendor/ps5/sdk/stubs/agc_canary_link_stub.c"
stub libSceAgcDriver "$vk/vendor/ps5/sdk/stubs/agc_driver_canary_link_stub.c"
# Two system libraries the payload SDK has no import library for: a USB mouse
# (shell/ps5/ps5_usbinput.cpp) and the common dialogs, which the keyboard on
# the screen starts first (shell/ps5/ps5_ime.cpp).
stub libSceMouse "$runtime/stubs/mouse_link_stub.c"
stub libSceCommonDialog "$runtime/stubs/common_dialog_link_stub.c"
# libSceVideoOut: the SDK's import library has no sceVideoOutVrrUnpegFromFixedRate,
# which the driver calls for variable refresh. One library can have one import
# library, so this one is made from the names of the SDK's, with that name
# added, under the SDK's own name for it (.sprx) and given before the SDK's:
# the linker reads the first of a name.
{
    printf '/* Made by ps5-link.sh from the names in the payload SDK libSceVideoOut.so. */\n'
    "$sdk_root/bin/prospero-nm" -D --defined-only "$sdk_root/target/lib/libSceVideoOut.so" |
        awk '$2 ~ /^[TW]$/ && $3 ~ /^[A-Za-z_][A-Za-z0-9_]*$/ { print "int " $3 "(void) { return -1; }" }
             $2 ~ /^[BDR]$/ && $3 ~ /^[A-Za-z_][A-Za-z0-9_]*$/ { print "int " $3 " = 0;" }' | sort -u
    printf 'int sceVideoOutVrrUnpegFromFixedRate(void) { return -1; }\n'
} > "$work/obj/videoout_link_stub.c"
grep -q '^int sceVideoOutOpen(void)' "$work/obj/videoout_link_stub.c" ||
    { echo "ps5-link: the SDK's libSceVideoOut.so gave no names" >&2; exit 2; }
if [[ $(grep -c 'sceVideoOutVrrUnpegFromFixedRate' "$work/obj/videoout_link_stub.c") != 1 ]]; then
    # The SDK has it now: its own import library is enough.
    sed -i '$d' "$work/obj/videoout_link_stub.c"
fi
stub libSceVideoOut "$work/obj/videoout_link_stub.c" sprx
title_stubs=("$work/stubs/libSceAgc.so" "$work/stubs/libSceAgcDriver.so" "$work/stubs/libSceMouse.so"
    "$work/stubs/libSceCommonDialog.so" "$work/stubs/libSceVideoOut.so")
stub_options=()
for file in "${title_stubs[@]}"; do
    stub_options+=(--stub "$file")
done

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
# bound to shell/ps5/ps5_libc.cpp's versions, kept local. What the link recipe
# already binds to the platform layer (readlink, link, symlink, mkstemp and
# localeconv, from the SDK revision this title pins) is the platform's.
# getaddrinfo and freeaddrinfo are the exception, and are the title's: they
# ask the console's own resolver, which the platform's did not reach from a
# title (shell/ps5/ps5_libc.cpp).
title_overrides=" getaddrinfo freeaddrinfo "
filtered=()
for flag in "${radv_link_flags[@]}"; do
    name=${flag#--defsym=}
    name=${name%%=*}
    if [[ $flag == --defsym=* && $title_overrides == *" $name "* ]]; then
        continue
    fi
    filtered+=("$flag")
done
radv_link_flags=("${filtered[@]}")
title_defsyms=()
{
    printf '{\n    local:\n'
    for name in fork link symlink readlink pathconf isatty getcwd realpath mkstemp \
            gai_strerror gethostbyname getnameinfo getaddrinfo freeaddrinfo in6addr_any localeconv; do
        if [[ " ${radv_link_flags[*]} " == *" --defsym=$name="* ]]; then
            continue
        fi
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
    "${title_stubs[@]}" \
    "${radv_link_inputs[@]}" \
    --as-needed "$sdk_root"/target/lib/*.so

# A title loads neither libkernel_sys's exports nor libScePosixForWebKit's: an
# import only their stubs define links, and is null at run time, so its first
# call jumps to address 0. Refused here rather than found on the console
# (PS5_VulkanTemplate's link check).
null_imports=$(comm -23 \
    <("$sdk_root/bin/llvm-nm" -D --undefined-only "$work/llvm-pie.elf" |
        awk '$1 == "U" { sub(/@.*/, "", $2); print $2 }' | sort -u) \
    <(for library in "$sdk_root"/target/lib/*.so "${title_stubs[@]}"; do
        case ${library##*/} in libkernel_sys.so | libScePosixForWebKit.so) continue ;; esac
        "$sdk_root/bin/llvm-nm" -D --defined-only "$library" 2>/dev/null | awk '{ print $NF }'
    done | sort -u))
if [[ -n $null_imports ]]; then
    echo "ps5-link: imports that no module a title loads exports (null at run time): ${null_imports//$'\n'/ }" >&2
    echo "ps5-link: bind them in shell/ps5/ps5_libc.cpp (title_defsyms above) or in the platform layer" >&2
    exit 1
fi

"$tool" link --in "$work/llvm-pie.elf" --out "$work/eboot.elf" \
    --stub-dir "$sdk_root/target/lib" "${stub_options[@]}" --module-sdk 0x02000009 \
    --companion-sdk 0x08050001 --file-name eboot.elf
"$tool" self --sign --in "$work/eboot.elf" --out "$target" --magic 0x1D3D154F
"$tool" self --inspect --file "$target" > "$work/eboot.inspect.txt"
# Kept for shell/ps5/symbolize.sh, which turns a crash report's eboot offsets
# into functions.
cp -f -- "$work/llvm-pie.elf" "$(dirname -- "$target")/flycast-eboot.elf"
printf 'ps5-link: %s (%s bytes)\n' "$target" "$(stat -c %s "$target")"
