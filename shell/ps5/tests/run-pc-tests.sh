#!/usr/bin/env bash
# PSFlyCast - every test of the port's own code that runs on a PC, one after another, as CI runs
# them (.github/workflows/ps5-pc-tests.yml). Each builds into <work>/<name>.
#   shell/ps5/tests/run-pc-tests.sh [work folder]       (default: build-pc-tests)
# CXX picks the compiler (CI runs it with g++ and with clang++).
#   cardsync  the memory cards' sync engine (ps5_card_sync.cpp), under ASan and UBSan
#   interface nothing of the interface is lost (shell/ps5/tools/interface-inventory.py)
#   ui        the settings table is whole, and pictures of every screen of the new interface
#   folders   the folders engine (ps5_folders.cpp), under ASan and UBSan
#   covers    the cover matching (ps5_covers_match.cpp), under ASan and UBSan
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
run interface python3 "$here/../tools/interface-inventory.py" check
run ui bash "$here/ui/run.sh" "$work/ui"
run folders bash "$here/folders/run-folders-tests.sh" "$work/folders"
run covers bash "$here/covers/run-covers-tests.sh" "$work/covers"
run hdr bash "$here/hdr/run.sh" "$work/hdr"
if ((${#failed[@]})); then
	echo "FAILED: ${failed[*]}"
	exit 1
fi
echo "every PC test passed"
