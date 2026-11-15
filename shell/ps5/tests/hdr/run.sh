#!/usr/bin/env bash
# PSFlyCast - HDR output's last steps (ps5_hdr.cpp's shaders, the 10-bit image copied into the
# framebuffer's format) on the PC, on lavapipe (Mesa's software Vulkan driver): OGBox's test
# (OGBox by Press5elect), on the shaders as ps5_hdr.cpp has them.
#   shell/ps5/tests/hdr/run.sh <work folder>
# Needs a C++ compiler, glslangValidator, libvulkan-dev and mesa-vulkan-drivers; without lavapipe
# it says so and checks nothing. The validation layer is used when installed.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ps5=$(cd "$here/../.." && pwd)
work=${1:?usage: run.sh <work folder>}
mkdir -p "$work"
icd=/usr/share/vulkan/icd.d/lvp_icd.json
[[ -f $icd ]] || { echo "hdr on the PC: NOT RUN (needs Mesa's lavapipe, $icd)"; exit 0; }
# The shaders as the title compiles them: ps5_hdr.cpp's Vertex and Fragment, after "#version 430".
python3 - "$ps5/ps5_hdr.cpp" "$work" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
for name, stage in (("Vertex", "vert"), ("Fragment", "frag")):
    m = re.search(r'const char ' + name + r'\[\] = R"\((.*?)\)";', text, re.S)
    open(f"{sys.argv[2]}/hdr.{stage}", "w").write("#version 430\n" + m.group(1))
PY
glslangValidator -V "$work/hdr.vert" -o "$work/hdr.vert.spv" > /dev/null
glslangValidator -V "$work/hdr.frag" -o "$work/hdr.frag.spv" > /dev/null
"${CXX:-g++}" -std=gnu++20 -O1 -g -Wall "$here/hdr_host_test.cpp" -o "$work/hdr_host_test" -lvulkan
VK_ICD_FILENAMES=$icd VK_DRIVER_FILES=$icd "$work/hdr_host_test" "$work/"
