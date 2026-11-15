# Releasing

Press5elect asks for a release; he creates the tag and the GitHub release. Everything else is
prepared here.

1. **The source.** Upstream and the parts checked for newer revisions, and what changed said
   ([building.md](building.md)). Every commit of the release on the branch, the tree clean.
2. **The version.** `ps5_build.h`: `PS5_VERSION`, one more `PS5_BUILD_NUMBER` than any build handed
   out. The README's Status and its `## New in vX.Y.Z` (Added, Changed, Fixed only; what was not
   run on a console said in Status); a version older than the last five moves to
   [../history/changelog.md](../history/changelog.md). `release/components.json` for every pin
   and the driver's tree.
3. **The build**, from the committed tree:
   ```bash
   RELEASE_TAG=vX.Y.Z PS5_PAYLOAD_SDK=<sdk> PS5_VULKAN_DIR=../PS5_Vulkan bash shell/ps5/build.sh
   python3 shell/ps5/release/pack-zip.py build-ps5/dist/PPSA99247 PSFlyCast-vX.Y.Z.zip --date YYYY-MM-DD
   sha256sum PSFlyCast-vX.Y.Z.zip > PSFlyCast-vX.Y.Z.zip.sha256
   ```
4. **The checks**: the ZIP unpacked into an empty folder and
   `python3 shell/ps5/release/stage-notices.py <unpacked>/PPSA99247 --check --release vX.Y.Z`;
   `BUILD.txt` names the commit, the SDK and the driver; no game, BIOS, key, firmware, save or user
   config in it; the PC tests pass ([testing.md](testing.md)).
5. **The push**, only when Press5elect says so, to `main` and `next-1.3` (the same commit).
6. **The release page** (his): short notes laid out as v1.3.5's (New in, Install or update,
   Reporting problems, Source, Build table); assets only the ZIP and its `.sha256`.
7. **Read it back**: download the published ZIP and compare its SHA-256 with the one made here.
