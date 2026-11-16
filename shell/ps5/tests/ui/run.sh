#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-2.0-or-later
# PSFlyCast - pictures of every screen of the new interface (shell/ps5/ui), drawn on a PC.
#
#   shell/ps5/tests/ui/run.sh <work folder>      the PNGs land in <work folder>/shots
#
# Also checks that the settings table is whole (settings-table.py check). Needs FreeType (as the
# console's Dear ImGui is built with it).
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../../../.." && pwd)
work=${1:?usage: run.sh <work folder>}
mkdir -p "$work/fonts" "$work/shots"
python3 "$repo/shell/ps5/tools/settings-table.py" check
if ! pkg-config --exists freetype2; then
	echo "ui: NOT RUN (no FreeType: apt-get install libfreetype-dev)"
	exit 0
fi
for f in Roboto-Medium Roboto-Bold fa-solid-900; do
	[ -f "$work/fonts/$f.ttf" ] || python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
		"$repo/fonts/$f.ttf.zip" "$work/fonts"
done
imgui="$repo/core/deps/imgui"
"${CXX:-clang++}" -std=c++20 -O2 -Wall -Wextra -Wno-unused-parameter \
	-I "$repo/shell/ps5" -I "$repo/core" -I "$imgui" -I "$repo/core/deps/stb" \
	"$here/preview.cpp" "$repo/shell/ps5/ui/ui.cpp" "$repo/shell/ps5/ui/ui_settings.cpp" \
	"$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp" \
	"$imgui/imgui_cjk.cpp" "$imgui/misc/freetype/imgui_freetype.cpp" $(pkg-config --cflags --libs freetype2) \
	-o "$work/preview"
"$work/preview" "$work/fonts" "$work/shots"
echo "ui: $(ls "$work/shots" | wc -l) pictures in $work/shots"
