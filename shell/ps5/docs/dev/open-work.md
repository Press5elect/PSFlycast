# Open work

What is unfinished, untested, or decided. Update it when something is done or learned; delete
what no longer applies.

## Not run on a console

Builds 94 to 101 (v1.3.5 to v1.3.9) have not been run on a console. What each needs checking:

| Build | What to look at |
| --- | --- |
| 94, v1.3.5 | threads on cores of their own; the variable refresh lookup |
| 95, v1.3.6 | the elevation helper's counted root references (protocol 2); the boot log ring; finding servers |
| 96, v1.3.7 | 4 MiB network buffers (a share's speed); driver patches 0003 and 0004; payload SDK `e105cf6` |
| 97, v1.3.8 | the log settings; fewer writes (library file, pipeline cache, states, configs) |
| 98 to 100, v1.3.9 | Flycast `63e7d91`; SDK v0.45's binaries (untested from the fork on any console) and the `heap: trimmed` lines; driver patches 0005 to 0007; HDR (`hdr:` lines, the test screen); the card sync (`cards:` lines; a share's clock against the console's); deleting a state; the menus' vibration and Circle confirms; the quick menu's arrangement; 4.5x; the boot log kept in memory; the crash report's `thread:` line; build 100: Down-sampling (Soft, Sharp) and SMAA (`aa:` lines) |

## The new interface (build 105): the Settings are on the console, the rest is not yet

`shell/ps5/ui` draws the new screens (pictures on a PC: `tests/ui/run.sh`); the Settings for every
game are the first connected (`bigpicture_newui.inc`: each table setting bound to the row it
replaces, by its old label; rows the table does not name shown after their neighbours).
To check on the console: every area at every level, Search with the console's keyboard, pins
(`data/settings-pins.txt`), the changed dots against a new install (`settings: N defaults read`
in the boot log), R3, L2 restart, the dialogs the rows open (they draw over the new screens),
OPTIONS and Circle out. Not yet connected: Home, the Library, a game's page, the quick menu
(the build-104 screens stay for those), the game's own Settings (the old screen), Interface
size, skins and high contrast (the new screens have their own colours), Look For My Files in the
new look (the old dialog opens). `classic_ui = 1` in frontend.cfg brings the old Settings back.

## The interface revamp (build 104): nothing of it has been seen

Written without a console and without a picture of any screen ([interface-revamp.md](interface-revamp.md)).
What to look at first, in the order a person meets it:

- Start-up: Home as the first place (the game played last, "Right now" cards, played lately);
  Open on (Settings > Library) set to Library. A fresh `frontend.cfg` opens the Library on All.
- The top bar: three places and the filters under Library; L1 and R1 through Home, All, Console,
  USB (when on), Share, Disc (when on); the pill and the chips with Interface size 70 % and Text
  size at their ends; a screen with Reach::None (loading, in-game settings).
- The Library on All: games of every place once, discs sets, Square (also asks the share), the
  Share filter's scan; choosing a disc for a running game (no Home, no All surprises).
- Game Page from Home (focus has to arrive on the game: up to 8 frames are tried).
- Settings: the home, an area of one category (straight into its rows, Circle to the home), an
  area of two, Pinned (Triangle on a row; eight at most), Search (the console's keyboard), a
  setting changed from Search or Pinned (it must be the same setting), Restart now.
- Folders: Add Game Folder on a USB drive and on a share (the share's listing blocks nothing but
  shows "Looking..."); Look For My Files on a drive with a Flycast data folder; the BIOS, state,
  cheat and video folders; a covers folder (old covers stay in the old one); pulling the drive
  holding the save states out and starting a game (the question), and putting it back.
- Not checked anywhere: a share's listing time with a NAS asleep; a folder with thousands of
  entries (the browser caps a listing at 1000 names).

Found by the review and left as they are: a covers folder that is missing is not asked about
(covers are downloaded again into the default folder; nothing of the player's is hidden); a game
started from the command line (`--rom`) cannot be asked about a missing states folder (the
question is drawn by the library); whether listing a share from the folder browser's thread while
the network scanner runs is safe (each share's calls are under its own lock, not tried on a console).

Left out on purpose (see the revamp page): light skins, levels, a "Changed lately" card, the
game page and quick menu redesign, the memory cards' folder, a Flycast-words text file.

## PlayStation Move (v1.3.9, build 99)

Through Move Management (a separate payload; `ps5_pad.cpp`'s Move section, its client vendored in
`third_party/move_management`). Checked: the title builds and links (the sockets pass the import
check); Move Management's own producer-to-client test passes on the PC. Not checked: anything
with a real Move. On the console, look for the `move:` lines (the ports opened, a player's
controllers arriving and going quiet, the screen calibration read), then: the buttons and T in
Controls, the Navigation's stick, aiming in a light-gun game (direction: Motion aiming direction;
speed with and without Move Management's screen calibration), SELECT recentring, a DualSense on
the same port not aiming at the same time, and a controller turned off mid-game releasing its
buttons. Move Management's packet axes are taken as the DualSense's (yaw from the gyro's second
axis, pitch from its first); if the aim turns the wrong way, that is where to look. No rumble:
Move Management does not receive it yet (its PROTOCOL.md).

## Asked for, not done yet

- **NFS shares** (asked for v1.3.9, not started). The size of it: libnfs cross-built for the title
  (Crux2's and OGBox's pin is `7613236`, its 8.0.0 release and four fixes; OGBox's
  `ps5/tools/build-deps.sh` builds it), a backend like Crux2's and OGBox's
  `vfs/ps5_vfs_nfs.cpp` (about 1,050 lines), and `ps5_smb.cpp` made to serve both: it calls
  libsmb2 directly 71 times (listing, the streamed file, the in-memory image, `putFile`, the card
  sync's functions, finding servers), so the first step is a small file-access layer under those,
  tested against Samba as the README's verification did, before NFS goes in beside it. Then
  `nfs://` folders in `network.cfg` and in Settings > Library > Network, and libnfs's licence
  (LGPL-2.1) in `release/components.json`.
- **Debanding** (OGBox's): not asked for in the end; SSAA and SMAA are in (build after 99).
- **FSR's picture zoomed about its middle** (Press5elect, 2026-11-15, the same as GitHub issue 4's
  "stretched"): with FSR the picture shows its middle, larger (about 1.12 across and 1.5 down at
  4x on a 4K screen), at every internal resolution; CAS shows it right. Not found yet. Checked: the
  title's EASU and RCAS shaders, composed as `ps5_fsr.cpp` composes them, with
  `ps5_fsr_constants.h`'s constants, stretch a 2560x1920 line pattern to 2880x2160 and draw it
  at x 480 on a 3840x2160 screen on lavapipe with every line within a pixel of where it belongs;
  the push-constant layouts match the shaders; the setting's values match the enum; nothing else
  in the title reads the setting. So what the console gives `upscale()` and `present()` (the
  picture's size, its image, the rectangle) is the next thing to see: the boot log's
  `fsr: W x H to W x H` line and its `settings:` lines from a run with FSR.
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
