# Building PSFlyCast

The steps from an empty machine are in the README's [Building](../../README.md#building) section,
which a release's source notice points to; this page is what a developer needs beside them.

## The pins

| Part | Where | Revision (v1.3.9) | Moved by |
| --- | --- | --- | --- |
| Flycast | `upstream` = github.com/flyinghead/flycast, branch `dev` | `63e7d91` | a merge commit (`Merge Flycast dev <rev> (<date>)`) |
| PS5_Vulkan | `../PS5_Vulkan`, unmodified | `7f492b4` | README's checkout line |
| PS5_Mesa | `../PS5_Mesa`, branch `psflycast-radv-27` made by the script | pin `1ba9874f478` | `build-radv-psflycast.sh` (`pin=`) |
| Crux2's driver patches | `../crux2` (a checkout or worktree) `ps5/patches/mesa-27` | `08c3507` | `build-radv-psflycast.sh` (`CRUX2_REVISION=`) |
| PSFlyCast's driver patches | `shell/ps5/mesa/0001` to `0007` | in this repository | the script requires each by subject |
| Payload SDK fork | github.com/mihawk-99/PS5_PayloadSDK, its `platform/tools/setup-sdk.sh` | `3345cde` (SDK v0.45) | README's `sdk=` line |
| libsmb2 | `../libsmb2` | `177a9de` | README's checkout line; `shell/ps5/ps5.cmake` lists its files by name |
| FFmpeg, mbedTLS | `../deps-src/ffmpeg-n9.0.2`, `../deps-src/mbedtls-3.6.7` | n9.0.2, v3.6.7 | README's clone lines |
| libretro-database | `../libretro-database` (the cheat files) | its master | none |

Before a build: look for newer upstream Flycast `dev` and newer parts, and say what changed
(`git fetch upstream dev`, `git log a..upstream/dev`; `git ls-remote` for the others). A pin
moves in one commit with the README's Building lines and `release/components.json`.

## The driver

- `build-radv-psflycast.sh` makes branch `psflycast-radv-27` of `../PS5_Mesa` from the pin, applies
  Crux2's set, then `shell/ps5/mesa/*.patch` (skipping 0001 and 0002, which Crux2's set carries),
  and builds it with PS5_Vulkan's `tools/build-radv.sh`. `PSFLYCAST_RADV_REMAKE=1` makes the branch
  again (needed after adding a patch). About ten minutes on four cores.
- A new PS5 Mesa commit becomes the next `shell/ps5/mesa/000N` with `git format-patch`, and the
  script's list of required subjects gets its subject. A patch from PS5_Mesa's own history keeps
  its author; one written here needs Mesa's `Generated-by: LLM` trailer.
- `release/components.json` records the driver's tree (`git -C ../PS5_Mesa rev-parse
  psflycast-radv-27^{tree}`) and the patches in its `where`.
- PS5 Mesa `f5610fe2709` is deliberately not applied: Crux2's 0007 already keeps a freed buffer's
  memory until the GPU is done ([open-work.md](open-work.md#decisions-on-record)).

## The machine

Ubuntu 24.04 with the README's packages. Also needed, not in the README's list: `rsync` (the
driver build), `meson` and `mako` from pip, and the title's submodules (`git submodule update
--init --recursive`). For the PC tests: `mesa-vulkan-drivers`, `libvulkan-dev`,
`vulkan-validationlayers` and `glslang-tools` ([testing.md](testing.md)). PS5_Vulkan's `make`
stops at its own sample's link; the host tool and `libc.prx` are made before that.

## What each version was built with

v1.3.9 is made with Flycast `dev` at `63e7d91`, PS5_Vulkan `7f492b4` (its changes since
`f410e67` are its Windows-game broker and a test; nothing a title links), two more PS5
Mesa commits, `7bd4aac55d8` and `0f86253f674` (`shell/ps5/mesa/0005` and `0006`), OGBox's
HDR10 framebuffers on request (`0007`, OGBox's `ps5/patches/mesa-27/0019`), the
payload SDK fork at `3345cde` and libsmb2 `177a9de`.
v1.3.8 and v1.3.7 are made as v1.3.6 is, with two more driver commits, PS5 Mesa `53cc83ff82b`
and `249fe49d2d8` (`shell/ps5/mesa/0003` and `0004`; the first had one conflict with
Crux2's patches, resolved by hand and said in its message), and the payload SDK fork
at `e105cf6`. PS5 Mesa's `f5610fe2709` (a freed buffer's memory kept until the GPU has
run what was submitted before it) is not applied: Crux2's patches already keep a freed
buffer's memory that way, and `f5610fe2709` also covers sparse ranges and a retry after a
failed allocation, which this title does not use.
v1.3.6 and v1.3.5 are made with PS5_Vulkan `f410e67` as published, PS5_Mesa `1ba9874`
plus Crux2's driver patches at `08c3507` (Mesa 27.0-devel of 2026-10-31), the
payload SDK fork at `b5efad5` and libsmb2 `7e4ff97`; the driver is compiled
with the SDK at `b83202b`. `build-radv-psflycast.sh` runs PS5_Vulkan's
`build-radv.sh` with Mesa 27's `amd-options` in place of its older
`amd-use-llvm`. v1.3.3 and v1.3.4 had Crux2's patches at `2afe8e5`.
v1.2.0 is made with PS5_Vulkan `f410e67` (with one line changed in
`tools/build-radv.sh`: the Mesa revision), PS5_Mesa `1ba9874` plus the two
patches in `shell/ps5/mesa`, the payload SDK fork at `b5efad5` and libsmb2
`7e4ff97`; RADV's archive is compiled with the SDK at `b83202b`. v1.1.0 and its two candidates are made with PS5_Vulkan `5b5e4fc`, PS5_Mesa `7b59ef2` plus the
two patches, the payload SDK fork at `b5efad5` and libsmb2 `7e4ff97`; RADV's
archive is compiled with the SDK at `b83202b`. (PS5_Mesa's two commits after
`7b59ef2` change its OpenGL-on-Vulkan driver, which is not in the title:
RADV is as it was.) v1.0.0 to v1.0.2 were made with PS5_Vulkan
`3f3ee69`, PS5_Mesa `0b2d6d1` plus the patches (one up to v1.0.1, two from
v1.0.2), and the payload SDK fork at `cd3b239`. (Those two repositories'
histories were rewritten by their author on 2026-10-05 and every commit got
a new id: the same trees are now `504adad` and `a9ca6bd`.) `BUILD.txt` in
the title's folder names what a build was made from.
