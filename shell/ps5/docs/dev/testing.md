# Testing

Three kinds of evidence, in this order of weight: a run on the console (its boot log), a test that
runs the real code on a PC, and a build that links. Say which one a claim rests on.

## The PC tests in the repository

`shell/ps5/tests/run-pc-tests.sh [work folder]` runs them all; CI runs it with g++ and clang++
(`.github/workflows/ps5-pc-tests.yml`) on every push that touches `shell/ps5`.

| Test | What it runs | Notes |
| --- | --- | --- |
| `tests/cardsync` | the memory cards' sync engine (`ps5_card_sync.cpp`) on folders in memory, ASan and UBSan | Crux2's test plus a check of `is_card` |
| `tests/folders` | the folders engine (`ps5_folders.cpp`) on folders made in a temporary directory, ASan and UBSan (70 checks) | the browser's walk, the writable probe, `lookFor`, `folders.cfg`, which folders are in use when a drive is gone |
| `tools/interface-inventory.py check` | that no label of the interface at v1.3.9 is gone from the code | acknowledged changes are in `interface-inventory.json` |
| `tests/hdr` | HDR output's shaders and copy (`ps5_hdr.cpp`'s own text) on lavapipe, with the validation layer | OGBox's test; says NOT RUN without lavapipe |

Run them with CI's compilers before a release (`CXX=g++` and `CXX=clang++`).

## Checked on the build machine before v1.3.9

The README listed these until v1.3.8. Their harnesses were made in earlier sessions and are not in
the repository; only the two tests above are.

On the build machine, without a console:

- The title links against RADV and is converted and fake-signed by
  PS5_Vulkan's host tool, which checks every import against the console's
  modules.
- The interface is the real code, rendered on a PC with test data (the
  screenshots).
- The network code (`shell/ps5/ps5_smb.cpp` with libsmb2) against a Samba
  server on the build machine: listing, streamed reads and reads from memory
  compared byte for byte with the files, a second open sharing the image in
  memory, cancel half way, a server paused for six seconds in the middle of a
  load (the wait is shown, the load completes), a dead address (fails in five
  seconds) and a wrong share name.
- The updater's own steps (`shell/ps5/ps5_update.cpp`), with the project's
  real releases: the list GitHub gives is read and its highest version
  taken; v1.0.1's ZIP gives the SHA-256 its release names; unpacked, every
  file is the ZIP's, byte for byte; put over a copy of v1.0.0 with a user's
  files in it, the new files are in place, the old ones in `update/old/`
  and the user's untouched; with a move refused half way, at `eboot.bin` or
  earlier, the folder ends byte for byte as it was; a ZIP with settings,
  games or paths outside the folder in it has them left out; a cut-off or
  damaged ZIP is refused. What needs a console is not covered: the download
  itself, and the moves in the title's real folder.
- The text that goes to and comes from the console's keyboard
  (`shell/ps5/ps5_ime.cpp`), against a stand-in for the dialog: the
  parameter block's layout, text with accents and non-Latin characters both
  ways, a cancelled entry, a value longer than the field.
- The home-screen sound: the same file comes out of its generator every
  time, and the loop's last sample runs into its first.
- The link's import check, new in v1.1.0: it stopped a build of this
  version that called a function no module a title loads has.
- The memory card code (`shell/ps5/ps5_vmu.cpp`), on card images made for
  the purpose: listing, copying, deleting, exporting and importing saves,
  damaged and unformatted cards, a card that changed between listing and
  acting, the backup made before a card's first change.
- Rewind's store of snapshots (`shell/ps5/ps5_rewind.cpp`), under the
  compiler's thread and memory checkers: taking, dropping, listing and
  reading back snapshots from several threads, its memory limit, and when a
  snapshot may be taken, against the emulator's own scheduler code. Not
  covered: a real game going back.
- The software renderer (`core/rend/soft/`), in a harness of its own with
  made-up frames, under the same checkers. Not covered: a real game's frames,
  and its speed on the console.
- The Scanlines and CRT shader compiles with the compiler the title uses for
  its shaders, and was drawn on a PC from test pictures.
- Frame generation (`shell/ps5/ps5_framegen.cpp`), the file itself in a
  test of its own, on a software Vulkan driver with the validation layer
  on, which had nothing to say. Test pictures with the true in-between
  picture known: a pan with something crossing it and a score that stands
  still (the made picture is 32 dB from the true one, where a plain mix of
  the two frames is 18.5 dB and build 45's was 27), a turn with a zoom, a
  fast turn behind something held still in front, a ground rushing at the
  camera, a run of six frames, three cuts to another scene (all of the made
  picture is the new scene), Light and Full with two, three and four
  presents, and two picture shapes. Not covered: a real game's pictures,
  and what it costs the console.


