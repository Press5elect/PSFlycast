#!/usr/bin/env bash
# PSFlyCast: turn a crash report's eboot offsets into functions.
#
#   shell/ps5/symbolize.sh [flycast-eboot.elf] < flycast-boot.log
#
# Reads the "rip: ... = eboot+0x..." and "stack (eboot offsets): ..." lines of
# flycast-boot.log and prints the function of each offset, from the link-stage
# ELF the build kept beside eboot.bin (build-ps5/flycast-eboot.elf).
set -euo pipefail
elf=${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)/build-ps5/flycast-eboot.elf}
symbolizer=$(command -v llvm-symbolizer-18 || command -v llvm-symbolizer)
offsets=()
while IFS= read -r line; do
    if [[ $line =~ eboot\+0x([0-9a-f]+) ]]; then
        offsets+=("0x${BASH_REMATCH[1]}")
    elif [[ $line == "stack (eboot offsets):"* ]]; then
        for word in ${line#stack (eboot offsets):}; do offsets+=("0x$word"); done
    fi
done
for offset in "${offsets[@]}"; do
    printf '%s  ' "$offset"
    "$symbolizer" --obj="$elf" --demangle --functions=linkage --no-inlines "$offset" | head -1
done
