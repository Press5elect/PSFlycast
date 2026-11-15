#!/usr/bin/env bash
# PSFlyCast - every test of the port's own code that runs on a PC, one after another, as CI runs
# them (.github/workflows/ps5-pc-tests.yml). Each builds into <work>/<name>.
#   shell/ps5/tests/run-pc-tests.sh [work folder]       (default: build-pc-tests)
# CXX picks the compiler (CI runs it with g++ and with clang++).
#   cardsync  the memory cards' sync engine (ps5_card_sync.cpp), under ASan and UBSan
#   hdr       HDR output's shaders and copy on lavapipe (ps5_hdr.cpp); says NOT RUN without it
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=${1:-$here/../../../build-pc-tests}
mkdir -p "$work"
failed=()
run() {
	local name=$1
	shift
	echo "== $name"
	"$@" || failed+=("$name")
}
run cardsync bash "$here/cardsync/run-cardsync-tests.sh" "$work/cardsync"
run hdr bash "$here/hdr/run.sh" "$work/hdr"
if ((${#failed[@]})); then
	echo "FAILED: ${failed[*]}"
	exit 1
fi
echo "every PC test passed"
