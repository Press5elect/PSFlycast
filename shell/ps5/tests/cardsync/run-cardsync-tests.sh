#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
# Crux2 - the memory card sync's engine (frontend/ps5_card_sync.cpp) on a PC, with the address
# and undefined-behaviour sanitizers, on two folders kept in memory.
# PSFlyCast: from Crux2 f0efa95 (ps5/tests/cardsync), for shell/ps5/ps5_card_sync.cpp.
#
#   shell/ps5/tests/cardsync/run-cardsync-tests.sh <work folder>
#
# Prints "cardsync: N checks passed"; a failure makes the exit status non-zero.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
front=$(cd "$here/../.." && pwd)
work=${1:?usage: run-cardsync-tests.sh <work folder>}
mkdir -p "$work"
flags=(-g -O1 -Wall -Wextra -Werror -std=c++20 -fno-exceptions -fsanitize=address,undefined -fno-sanitize-recover=undefined -I "$front")
"${CXX:-clang++}" "${flags[@]}" "$here/card_sync_test.cpp" "$front/ps5_card_sync.cpp" -o "$work/card_sync_test"
"$work/card_sync_test"
