#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# PSFlyCast - the cover matching (ps5_covers_match.cpp) on a PC, under ASan and UBSan.
#
#   shell/ps5/tests/covers/run-covers-tests.sh <work folder>
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../.." && pwd)
work=${1:?usage: run-covers-tests.sh <work folder>}
mkdir -p "$work"
flags=(-g -O1 -Wall -Wextra -Werror -std=c++20 -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$front")
"${CXX:-clang++}" "${flags[@]}" "$here/covers_test.cpp" "$front/ps5_covers_match.cpp" -o "$work/covers_test"
"$work/covers_test"
