#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# PSFlyCast - the folders engine (ps5_folders.cpp) on a PC, with the address and
# undefined-behaviour sanitizers, on folders made in a temporary directory.
#
#   shell/ps5/tests/folders/run-folders-tests.sh <work folder>
#
# Prints "folders: N checks passed"; a failure makes the exit status non-zero.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../.." && pwd)
work=${1:?usage: run-folders-tests.sh <work folder>}
mkdir -p "$work"
flags=(-g -O1 -Wall -Wextra -Werror -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$front")
"${CXX:-clang++}" "${flags[@]}" "$here/folders_test.cpp" "$front/ps5_folders.cpp" -o "$work/folders_test"
"$work/folders_test"
