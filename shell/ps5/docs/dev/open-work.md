# Open work

What is unfinished, untested, or decided. Update it when something is done or learned; delete
what no longer applies.

## Not run on a console

Builds 94 to 98 (v1.3.5 to v1.3.9) have not been run on a console. What each needs checking:

| Build | What to look at |
| --- | --- |
| 94, v1.3.5 | threads on cores of their own; the variable refresh lookup |
| 95, v1.3.6 | the elevation helper's counted root references (protocol 2); the boot log ring; finding servers |
| 96, v1.3.7 | 4 MiB network buffers (a share's speed); driver patches 0003 and 0004; payload SDK `e105cf6` |
| 97, v1.3.8 | the log settings; fewer writes (library file, pipeline cache, states, configs) |
| 98, v1.3.9 | Flycast `63e7d91`; SDK v0.45's binaries (untested from the fork on any console) and the `heap: trimmed` lines; driver patches 0005 to 0007; HDR (`hdr:` lines, the test screen); the card sync (`cards:` lines; a share's clock against the console's); deleting a state; the menus' vibration and Circle confirms; the quick menu's arrangement; 4.5x; the boot log kept in memory; the crash report's `thread:` line |

## Asked for, not done yet

- **SMAA, debanding, Lanczos down-sampling** (OGBox's; asked for v1.3.9, not started). They go in
  `ps5_fsr.cpp`'s picture chain, off by default; SMAA needs its area and search tables (MIT).
  Crux2's SMAA needed two black-screen fixes (Crux2 `eca7923`, `d01f2cd`): read them first.
- **NFS shares** (asked for v1.3.9, not started): libnfs cross-built for the title, a backend
  beside libsmb2 in `ps5_smb.cpp` (Crux2's and OGBox's `vfs/ps5_vfs_nfs.cpp` are the model), its
  licence in `release/components.json`.
- **Covers three at a time** (OGBox's, asked for v1.3.9, left out): PSFlyCast's one HTTPS client
  (`ps5_covers.cpp`, `httpMutex`) shares its error code, its one-time setup and the switch to the
  title's own TLS across requests. Concurrent requests need those made per request first, and a
  console run to show they help.
- **An updater test**: `ps5_update.cpp` has a `PS5_UPDATE_TEST` hook and no harness.

## Known problems and ideas

- **Archive unpacking** (`ps5_archive.cpp`) keeps one unpacked game and wipes the cache for the
  next: alternating zip or 7z games rewrites a whole disc image each time. Offered: a size-capped
  cache of several games, or unpacking into memory. Not asked for yet.
- **Free up space** does not clean logs kept on a USB drive.
- **The homebrew catalog entry** for PSFlyCast is stale (v1.3.2, licence listed as GPL-2.0-only).
- **GitHub issue 4** ("A 4:3 aspect ratio at 4K is bad", performance at 4K): what the reporter
  sees is not known; v1.3.9 adds 4.5x (2160p). A reply asking for the version, the settings and
  the boot log is drafted, not posted.
- **HDR's layers between frames**: they are kept from being redrawn while the frame before still
  reads them by an execution barrier (fragment shader to colour output) before their render pass
  (`ps5_hdr.cpp` beginPicture). The swapchain's render pass, which they use, has no external
  dependency, so by the letter of Vulkan its layout transition from undefined does not chain onto
  that barrier; on RADV a barrier waits for the earlier fragment work, so it holds in practice. A
  render pass of their own with an external dependency would be exact; the pipelines are made
  with the swapchain's, so that is more than a one-line change.
- **HDR's overlays**: Flycast's quad overlays (the VMU screen, crosshairs) blend their alpha
  squared into the menus' layer, so their translucent edges are a little light in HDR.
- **Data on a USB drive** (OGBox's measured slowdown of a title's writes to internal storage after
  a few GB): only logs can go there yet.

## Decisions on record

- PS5 Mesa `f5610fe2709` is not applied: Crux2's patch 0007 already keeps a freed buffer's memory
  until the GPU is done; f5610fe also covers sparse ranges and a retry this title does not use.
- The one-way memory card backup became the two-way sync (v1.3.9), in the same share folder and
  behind the same setting (`MemoryCardBackup`).
- HDR is declared in `param.json` (attribute `0x20000000`): VideoOut refuses HDR10 buffers
  otherwise, and an HDR TV is then in HDR mode while PSFlyCast runs, setting on or off.
- The boot log's default stays every 15 s; Kept in memory is a choice.
- SIGTERM is the console ending the title (closed from the PS menu too): one line, not a crash.
