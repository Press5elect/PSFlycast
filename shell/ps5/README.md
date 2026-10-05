# PSFlyCast

A Dreamcast, NAOMI, NAOMI 2 and Atomiswave emulator for a jailbroken PS5:
[Flycast](https://github.com/flyinghead/flycast) as a native app with its own
icon on the home screen, a controller-only interface in the style of Steam's
Big Picture mode, and the emulator's Vulkan renderer drawing through RADV from
[PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) /
[PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), linked into the app.

PSFlyCast is an unofficial port. It is not made or supported by Flycast's
developers: report its problems here, not to them.

**[The user manual](MANUAL.md)** says how to install it, add games and use
everything in it.

![The library](screenshots/01-library.png)

| | |
|---|---|
| ![Game details](screenshots/02-details.png) | ![The quick menu](screenshots/45-quick-menu-restart.png) |
| ![The start-up animation](screenshots/47-splash.png) | ![The Ember skin](screenshots/31-skin-ember.png) |
| ![The letters fly to the top bar as birds](screenshots/48-splash-leaving.png) | ![Settings > Online](screenshots/50-settings-online.png) |

## Status

Latest release: **v1.0.1** (build 39). I ran build 38, which has the same
code, on my PS5 before releasing it. It is built on Flycast's `dev` branch,
the one Flycast's nightly builds are made from.

Played on a PS5 over the builds that led to it: Dreamcast games from the
internal folder and from a network share, with a real BIOS; save states made
by v1.0.0; cover downloads; the quick menu and the per-game options; games on
several discs, grouped and swapped from the quick menu; the start-up
animation and the library; 119.88 Hz output on a display that takes it, and
60 frames a second on one that shows fewer than its mode says; the three
frame pacing choices; internal resolution above 6x and the texture options;
FSR 1 upscaling; per-pixel transparency with 32 to 128 layers, with its HUD
and menus at 3x and 4x; object draw distance, with which objects appeared
from further away. No frame rates or audio were measured.

In the build but not confirmed on a console: the left stick as the joystick
in arcade games (its setting is there), per-pixel transparency above 4x, USB
drives, the skins and transitions, the letter jump, the Controls page,
Restart game and the CPU clock. NAOMI and Atomiswave games have not been
tried. With per-triangle sorting a game can show surfaces flickering in and
out; per-pixel draws them right.

The source is ahead of that release (build 40, test builds only). New in it,
and none of it confirmed on a console yet: the mark made of one spiral line
and the start-up animation and sound that go with it; a second, third and
fourth player joining while the title runs; names looked up for the games'
own online modes and for network shares; Settings > Online, with Flycast's
netplay; and PSFlyCast updating itself from its releases.

> **No games, BIOS files or keys come with PSFlyCast, and none ever will.**
> Use only backups you made yourself of games you own, and BIOS files dumped
> from your own console. See `LEGAL.txt`.

If something fails, `flycast-boot.log` says where. Its first lines name the
build, as does Settings > About.

## What you need

- A jailbroken PS5 with [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)
  (or another launcher that runs titles from `/data/homebrew/`). No HEN payload
  is needed by PSFlyCast itself.
- Your own games (`.chd`, `.gdi`, `.cdi`, `.cue`; NAOMI/Atomiswave `.zip`/`.7z`)
  and, optionally, your own BIOS dumps.

No games, BIOS files or keys come with PSFlyCast.

## Install

1. Copy the `PPSA99247` folder to `/data/homebrew/PPSA99247/`. When updating,
   copy it **over** the one that is there (overwrite the files): your games,
   saves and covers are inside that folder, so do not delete it first.
2. Wait for ShadowMountPlus to report the title ready, then launch **PSFlyCast**.
3. Copy your games into `/data/homebrew/PPSA99247/games/` and press **Square**
   in the library to scan.

## The library's tabs

**L1 / R1** go through the tabs at the top, round and round:

| Tab | Shows |
|---|---|
| **Internal** | The games in `games/` in PSFlyCast's folder |
| **USB** | The games on USB drives. Only there when Settings > Library > USB drives is on |
| **Network** | The games on the SMB share named in `network.cfg` |

Each tab keeps its own place, and PSFlyCast opens on the tab you used last.
The Settings are not a tab: **OPTIONS** opens them from the library, and
closes them again.
Square scans the open tab again.

## Where your files go

Everything is in the title's own folder, `/data/homebrew/PPSA99247/`
(which the app sees as `/app0`):

| Folder | What |
|---|---|
| `games/` | Your games |
| `bios/` | BIOS files (optional) |
| `covers/` | Covers: downloaded ones, and your own |
| `network.cfg` | The network shares to read games from |
| `cheats/` | Cheat files (`.cht`); the libretro database's Dreamcast set is included |
| `patches/` | Game patches (60 FPS, widescreen): your own `patches.txt`; none is included |
| `data/` | Saves, VMUs, save states, which cheats are on, each game's shader list |
| `sounds/` | `startup.wav`, the start-up sound; a WAV file of your own there plays in its place |
| `update/` | Only while an update is on its way: the download, and the version before it until the new one has started |
| `radv-shader-cache/` | The graphics driver's compiled shaders |
| `flycast-boot.log` | Each start-up step, and a crash report if the app stops |
| `logs/flycast.log` | The emulator's own log |

- **BIOS:** Dreamcast games start without one (the emulator's built-in BIOS), but a
  real `dc_boot.bin` and `dc_flash.bin` are more compatible. NAOMI and
  Atomiswave games need their BIOS zips (`naomi.zip`, `awbios.zip`...).
- **Covers:** downloaded from the libretro thumbnails collection by the game's
  file name (Redump names such as `Game (USA).chd` match), from GitHub or,
  when that does not answer, from thumbnails.libretro.com; else read from
  the disc image. To use your own, put `<game file name>.png` (or `.jpg`) in
  `covers/`. Settings > Library turns downloading off.
- **Descriptions:** with downloads on (Settings > Library), each game's
  description and release date come from TheGamesDB, through Flycast's own
  scraper: a local disc is looked up by the ID on the disc, a game on the
  network share by its file name (the share is not read for this).
- **Game details (Triangle in the library):** the description, and four
  actions. **Play**; **Load state** starts the game from one of its saved
  states; **Options** gives the game settings of its own (resolution,
  widescreen, region, cable and others: each follows the Settings until you
  change it here); **Cheats** picks the cheat file and switches cheats on
  before the game starts. Options and cheats are kept under the ID the emulator
  reads from the game, so a network or arcade game has to be started once
  before they can be set. While a game that has options of its own runs,
  changes made in Settings are kept for that game only.
- **Cheats:** also in a game, touch pad > Cheats. The closest-named cheat file
  is loaded with every cheat off; switch on the ones you want, or pick another
  file with left/right on the first row. Your own `.cht` files go in `cheats/`.
- **Your own logo:** a picture saved as `logo.png` in PSFlyCast's folder (square,
  with a transparent background) is shown in place of PSFlyCast's turning mark.
- **USB drives:** Settings > Library > USB drives, then restart PSFlyCast. Games
  are read from a folder named `flycast` or `dreamcast` (also `dc`, `naomi`,
  `atomiswave`, `arcade`) at the top of the drive. This needs elfldr listening
  on port 9021 (the payload loader kstuff setups run): PSFlyCast sends it the
  bundled `sandbox-elevator.elf`, which lets PSFlyCast out of its sandbox.
  Cover downloads may stop working while this is on.
- **Network share (SMB):** edit `network.cfg` in PSFlyCast's folder (PSFlyCast
  writes a template the first time it runs):

  ```
  path = 192.168.1.10/Games/Dreamcast
  user = guest
  password =
  ```

  `path` is server/share/folder, with the server given by its IP address (a
  name is not looked up on the console); add a `path` line for each folder. For an
  open share leave `user = guest` and the password empty; otherwise give the
  NAS account. The share is only read.

  - **When the share is asked.** The first time you open the Network tab the
    folder is scanned, and the list of games is kept (`data/network-games.txt`).
    After that the tab shows the kept list without touching the share: a NAS
    whose disks sleep is not woken by starting PSFlyCast or by browsing. The
    share is asked again when you press Square on the tab (or use Settings >
    Library > Scan for games) and when you start one of its games or insert
    one of its discs. A scan the share did not answer to the end is not kept,
    and is made again the next time the tab is opened. Covers of network
    games are downloaded by file name; their discs are not read for a
    picture.
  - **A sleeping NAS.** A server that is on gets a minute to answer each
    request, and the screen says "Waiting for the network share (12 s)" while
    its disks spin up; Circle cancels. A server that is off, or a wrong
    address, fails after five seconds.
  - **Load network games into memory** (Settings > Library, on by default).
    On: the whole game is read into memory before it starts, with a progress
    bar, and the share is not used again while you play, so the NAS can go
    back to sleep. It takes about as long as copying the file over your
    network. Off: the game starts at once and is read
    from the share while it runs; a NAS that falls asleep during play will
    stall the game until its disks are back. A disc inserted from the quick
    menu is read the same way. Up to 3 GB is kept in memory (a Dreamcast
    disc is 1.2 GB at most); a file beyond that is streamed, and
    `flycast-boot.log` says so.
  - If nothing shows up, the tab says what the share answered, and
    `flycast-boot.log` has the lines starting `smb:`.
- **120 Hz:** on a TV that takes 4K at 120 Hz, PSFlyCast runs the output at
  119.88 Hz and shows each frame of the game for two refreshes, so a frame
  that arrives late is 8 ms late instead of 17, and the menus draw at 120.
  On any other TV the output is 59.94 Hz. Settings > About > Display says
  which one you got, and `flycast-boot.log` has the driver's own line
  (`display: the driver:`) and, a few seconds after the start, how many
  frames the menus really presented a second.
  - *VRR.* With VRR on, a display refreshes when a frame arrives, between 48
    and 120 Hz, and idles at 48 Hz before anything is shown. The driver takes
    that reading for a display in the 119.88 Hz mode, not for one that
    refused it.
  - *Settings > Video > 120 Hz output* turns it off (from the next start of
    PSFlyCast), should a display take the mode badly.
- **A game that draws wrong, or shows nothing:**
  - *Native depth interpolation* (Settings > Video, on by default). The console's graphics chip is AMD's, as in the Xbox, and without
    this option wall and floor textures warp and flicker as you move in
    some games. Flycast has it on for the Xbox for the same
    reason.
  - A real BIOS (`dc_boot.bin` and `dc_flash.bin` in `bios/`) starts games
    the built-in one does not. Settings > About > Dreamcast BIOS says which
    one is in use.
  - Every option of the Settings that can differ from game to game can be
    set for one game only: before it starts in Details > Options (Triangle
    on a game), and while it runs in the quick menu's Game options.
  - `flycast-boot.log` describes each game's first minute: the lines
    starting `game:` give what it was started with and, every five seconds,
    how many frames it drew, whether the picture was on, and where its
    program was. A game that stays black either draws black frames or waits
    for something, and those lines say which.
- **Shaders:** the first time a game shows a new effect there is a short
  hitch while it is compiled. PSFlyCast remembers what each game used and
  prepares all of it when the game next starts.

## If it does not start

Send `flycast-boot.log` from `/data/homebrew/PPSA99247/` (and
`flycast-boot.1.log`, the run before). It is written line by line as the app
starts, so it is there even when the app is closed with an error, and it ends
with a crash report saying where the app stopped.

PSFlyCast shows no notifications of its own on the console. To have one when
it crashes ("PSFlyCast stopped...") or when USB drives cannot be read, set
`notifications = 1` in `frontend.cfg` in its folder; the log has the same
lines (`notice: ...`) either way.

## Controls

**Library**

| Button | Does |
|---|---|
| L1 / R1 | Previous / next tab: Internal, USB, Network |
| D-pad / left stick | Browse |
| L2 / R2 | Jump by letter: R2 to the first game of the next letter, L2 to the first of this one, then of the one before. The letter shows for a moment |
| Cross | Play |
| Triangle | Game details: description, load state, the game's own options, cheats |
| Square | Scan the open tab again |
| OPTIONS | Settings. (The view - shelves, grid or list - is chosen there: Library > Library view) |

While a game loads, Circle cancels.

**In a game**

| DualSense | Dreamcast | Arcade |
|---|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y | Buttons 1 / 3 / 2 / 4 |
| L2 / R2 | Analog triggers | Analog triggers |
| L1 / R1 | Z / C | Buttons 6 / 5 |
| OPTIONS | Start | Start |
| Left / right stick | Analog stick / second stick | Left: analog stick, or the joystick |
| D-pad | D-pad | Joystick |
| L3 / R3 | | Insert coin / Service |
| **Touch pad click** | **Quick menu** | **Quick menu** |

The **quick menu** pauses the game: resume, save and load states (10 slots,
with a picture of each), change discs, cheats, game options, controls,
restart the game, and quit it. Circle or the touch pad resumes. Up to four DualSense controllers
work, one per logged-in user.

In an arcade game with a digital joystick (most of them) the **left stick
moves the joystick** as the D-pad does; a game with a wheel, a flight stick or
a light gun keeps it as the analog stick. Settings > Controls > "Left stick as
D-pad" is Automatic (that), On (every game, Dreamcast ones too) or Off, and a
game can have its own choice.

**Restart game** loads the game again from its beginning, as the console's
power button would: asked twice (press Cross again within four seconds), as
what was not saved is lost. The options that wait for a start (region,
cable, BIOS, patches, the controller's second slot) take effect with it,
and "Auto load state" is passed over for that one start.

**CPU clock** (Game options, and Settings > System for every game) runs the
Dreamcast's processor slower or faster than its 200 MHz, from 100 to
400 MHz. Overclocking smooths a game that slows down or whose frame rate
is uneven; it does not make a 30 fps game run at 60, and some games break
or run too fast with it. It takes effect when the game resumes.

**Controls**, in the quick menu, changes which DualSense button is which
Dreamcast (or arcade) one, for the running game: Cross on a control, then
the button for it. A button that did something else stops doing it, and
shows as "Not set" until it is given one. L2 and R2 stay analog when a
trigger is put on them; the sticks and the touch pad (the quick menu) are
not changed. The first change gives the game a layout of its own, kept
with the emulator's controller mappings and loaded whenever the game starts;
the Layout row at the top puts the game back on the layout every game has
(Settings > Controls restores that one to its default). The layout is the
DualSense's: every pad plays with it.

**Game options** are the running game's own, and hold every setting the
Settings have for a game: what you change there is in effect when the game
resumes (a few options, marked, when it is next started), is kept for that
game only and leaves the Settings as they are. (Vibration and the stick's
dead zone belong to the controller and are the same for every game.) A
dot marks an option the game has of its own; Square puts one back to the
Settings' value. The Settings for every game are in the library.

**Games on several discs.** Each disc is a file of its own, named as dumps
are: `Game (USA) (Disc 1).chd`, `Game (USA) (Disc 2).chd` (also `Disk B`,
`CD2`, `Disc 2 of 4`, with or without brackets). The files of one game, on
one source, are shown as one entry with the number of discs on its cover.

- Play starts the disc you last played, or disc 1 the first time. Details
  (Triangle) shows the discs; Square goes to the next one, and Play, Load
  state, Options and Cheats are then that disc's.
- When the game asks for the next disc: touch pad, **Open disc lid**, touch
  pad again, **Insert disc**. The game's own discs are listed first, with
  the one after the disc in the drive already chosen; every other disc
  image is below.
- The memory card is the same for all discs, so the save carries over. Save
  states are kept per disc. Options and cheats are kept under the ID on the
  disc, which the discs of most games share (Details shows it).
- Settings > Library > Group the discs of a game turns the grouping off;
  each disc is then an entry, as before.

**Game patches: 60 FPS and widescreen.** PSFlyCast can apply patches of the
kind the community's
[Flycast widescreen and 60 FPS chart](https://github.com/nexus382/Flycast-Widescreen-Compatability-And-Cheat-Chart)
collects: codes that make a game draw a 16:9 picture itself, or run at 60
frames a second. They are apart from cheats: a game that has one gets a
switch for it, **60 FPS patch** or **Widescreen patch**, at the top of its
options (Details > Options before it starts, the quick menu's Game options
while it runs), off until you turn it on, and working with whatever cheat
file the game has. A change takes effect the next time the game starts.

**No patches come with a release.** The chart states no licence, so its codes
are not mine to pass on. To use them, make the list yourself from the chart
and put it in `patches/` in PSFlyCast's folder:

```sh
python3 shell/ps5/patches/make_patches.py "<chart>/Dreamcast Widescreen.md" \
    "<libretro-database>/metadat/redump/Sega - Dreamcast.dat" > patches.txt
```

Without a `patches/patches.txt` the switches are not shown.

- A patch belongs to one version of a game, and the game file is recognised
  by its name: the Redump name (`Game (USA).chd`), or just the title
  with `(USA)`, `(Europe)` or `(Japan)`. A demo, a beta, a limited edition
  or a release in one language is another build and gets a patch only where
  the chart has one for it.
- The chart is a work in progress: a patch may do nothing or break a game.
  Turn it off again if so. A 60 FPS patch can make a game run too fast.
- Flycast has widescreen codes of its own for many games (Widescreen game
  patches, in Video); the Widescreen patch is for a game that option does
  nothing for.
- The list is one patch a line, and can be added to by hand.
- `make_patches.py` does not read the chart's arcade lists (NAOMI,
  Atomiswave).

**Object draw distance (experimental).** Two Dreamcast games (the option's
own line in the app names them) make a
stage's objects exist only near the player, by a distance each kind of object
has in a table, which is why they appear a short way ahead on a Dreamcast as
here. A game's options have **Object draw distance** (off, 1.5x to 5x): while
the game runs, PSFlyCast looks through its memory for those tables and
multiplies their distances, and gives the objects that are on the game's own
distance (taken to be 400 units) that distance multiplied (`shell/ps5/ps5_drawdist_scan.h` says how they are
recognised; the idea is the PC version's "Higher Draw Distance" mod's, by Kell
and SonicFreak94). The game's limits on live objects are not raised, and the
level's own draw distance is not changed. `flycast-boot.log` says what was
found (`drawdist: ...`). On a console (build 33) objects appeared from
further away with it on.

**Rumble:** the controller holds a memory card and a rumble pack (Flycast's
own default is two memory cards, with which nothing rumbles).
Settings > Controls > Controller slot 2 puts a second memory card back; the
saves on it are kept in `data/` either way.

**More players:** each DualSense is a signed-in user's. A second, third or
fourth player presses the PS button on their controller and signs in, before
PSFlyCast starts or while it runs, and gets the next controller port: B, C,
D. (A user who is signed in with the controller off takes no port.) In a Dreamcast game that port then holds a controller with a memory card
and a rumble pack of its own, for as long as the game is loaded (Flycast's
settings leave ports B to D empty, and are not changed); a pad that joins in
the middle of a game is plugged in there and then, with a pause of a moment.
Arcade games read every port as it is. The top bar counts the pads, and
Settings > Controls > Controllers names their ports. The menus follow the
first pad that is connected.

## Skins, backgrounds and motion

Settings > Interface changes how PSFlyCast looks and moves; a change shows at
once.

- **Skin:** Midnight (the look PSFlyCast has had from the start, and the
  default), Ember (warm charcoal and orange), Pocket LCD (the olive glass
  of a memory card's screen), Arcade (violet and magenta) and Carbon (black,
  for an OLED television).
- **Accent colour:** the skin's own, or one of seven.
- **Background:** the skin's own, or Still, Aurora (lights that wander),
  Waves, Sparks, Horizon (a floor of lines running away), Dot matrix and
  Cover colours (the focused game's cover, enlarged until only its colours
  are left).
- **Motion:** All, Less or None. With All the background lives, a tab slides
  in from the side it was reached from, covers rise into place one after
  another, the focus travels from one cover or row to the next, the hero
  art breathes and a band of light crosses the focused cover now and then.
  Less keeps short fades and a still background. None shows everything at
  once.
- **Start-up animation:** when PSFlyCast starts, one line winds in to the
  middle of the screen and out again and is a disc, the app's mark, with
  its name under it. The letters lift off as birds, one after another, and
  fly to the top left; the line lets its turns out until it is straight,
  leaves the screen, coils up again in the top bar and starts turning; the
  birds land beside it and are the name, and the library comes in under
  them. A little over seven seconds; any button ends it; the games are
  looked for meanwhile. With Motion on Less it is the mark and the name,
  still, for a second; with None, or with this option off, the library is
  there at once. While the title loads the console shows the animation's
  first frame (`sce_sys/pic1.dds`), so it starts from that picture.
- **Start-up sound:** the sound made for the animation, which follows it:
  a tone down and up with the line, a chord for the disc, a chirp and
  wings for each bird, a note for each landing. It is `sounds/startup.wav`
  in the title's folder; a 16-bit WAV file of your own with that name plays
  in its place. With Motion on Less only its last chord plays, quieter.

The mark and the sound are PSFlyCast's own. The disc is one line that starts
at its left edge, winds in to the centre and out again to the right edge;
`sounds/make-startup-sound.html` in the source makes the sound from
oscillators and noise, with nothing sampled or recorded.

## Settings

Interface (above), Video (per-triangle, per-strip or per-pixel transparency with 32 to 128 layers, internal resolution up to 10x,
widescreen and widescreen patches, filtering, texture upscaling and custom textures, scaling,
FSR 1 upscaling, frame skipping, mipmaps, native depth interpolation, framebuffer emulation,
frame pacing, FPS counter), Audio, Controls (vibration, dead zone, left stick as D-pad, the controller's
second slot, default layout, which ports have a pad), System (region, language, TV standard, cable, built-in BIOS,
auto save/load states, fast loading, CPU recompiler), Online (below), Library and About (the
build, checking for updates, credits). The
defaults: Vulkan, 3x resolution (1440p), 4x anisotropic filtering.

## Online

Not confirmed on a console yet.

**A game's own online mode.** Flycast emulates the Dreamcast's modem and its
broadband adapter, and by default connects them through DCNet, its service
for the games whose servers were brought back. Up to build 39 no name could
be looked up from the title ("A non-recoverable error occurred during
database lookup"), so none of it connected. Names are now looked up by the
console's own resolver. Settings > Online > **Dreamcast online** chooses the
modem or the broadband adapter, through DCNet or directly.

**Netplay** is Flycast's own (GGPO): two players in one game, each on a
console or a PC running Flycast, over the home network or the internet.

1. Both: Settings > Online > **Netplay**. One is **Host: player 1**, the
   other **Join: player 2**.
2. Both: **Other player**, the other's address. Settings > Online > This
   console shows yours. Over the internet it is the router's public address,
   and UDP port 19713 has to reach the console (forward it in the router, or
   try **Open the router's port (UPnP)**).
3. Both start the same game. Each waits on a screen of its own until the
   other is there. **Input delay** hides a slow connection.

Both need the same game file and the same BIOS, and the game must start from
the same state on both. An arcade game does when both have never changed its
settings. A Dreamcast game needs one save state, copied to both with `.net`
added to its name (`data/savestates/<game>.state.net`): Flycast compares everything
else the game would start from (the console's settings and clock, the memory
cards), and those differ between two consoles. In netplay port B is the
other player's, with a controller in it.

While Netplay is not Off, **every** game waits for another player when it
starts. Turn it off to play alone.

## Updates

Not confirmed on a console yet; it is made so that a failure leaves the title
as it was.

Settings > About > **Check for updates** asks the project's GitHub page for
its releases. When one is newer than what is running, PSFlyCast can install
it: it downloads the release's ZIP, checks it against the SHA-256 the release
names, unpacks it, and moves each file of the title that the ZIP has to
`update/old/` and the new one into its place. If the console refuses any of
those moves, every file already moved is moved back. The new version starts
the next time PSFlyCast is opened, and that start deletes `update/`. What is
yours is never touched: games, BIOS files, covers, `data/`, the settings.

A release's build also asks once as it starts and offers a newer release
(**Update now**, **Later**, **Skip this version**); About > **Look for
updates at start-up** turns that off. A test build asks only when told to,
and can install the newest release in its own place.

The step nobody has seen work on a console is replacing `eboot.bin` while it
runs. If it is refused, the dialog says so and names the reason,
`flycast-boot.log` has every step ("update: ..."), and updating by copying
the ZIP works as before.

## What has been verified

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

On a console: see Status, at the top.

## How it works

- `shell/ps5/runtime/` - the process start (clears the BSS) and the link layout.
- `shell/ps5/ps5_main.cpp` - start-up, folders, log, first-run settings.
- `shell/ps5/ps5_diag.cpp` - `flycast-boot.log` and the crash report
  (`shell/ps5/symbolize.sh` turns its offsets into functions).
- `shell/ps5/ps5_pad.cpp` - DualSense through libScePad, rumble, up to four pads.
- `shell/ps5/ps5_audio.cpp` - libSceAudioOut at 48 kHz, resampled from the
  emulator's 44.1 kHz.
- `core/rend/vulkan/vulkan_context.cpp` - RADV's ICD entry point and a
  `VK_KHR_display` surface on VideoOut, as PS5_Vulkan's RADV test title drives
  it; the 4K mode with the highest refresh rate the driver offers (119.88 Hz
  where `param.json` asks for it and the display follows, else 59.94 Hz), and
  two presents a frame at 119.88 Hz, or one where the presents are counted
  and the display shows fewer than its mode says.
- `shell/ps5/ps5_fsr.cpp`, `ps5_fsr_constants.h`, `shell/ps5/fsr/` - FSR 1
  upscaling of the picture: AMD's two headers, compiled as GLSL, in two draws
  between the game's picture and the swapchain.
- `core/linux/posix_vmem.cpp` - guest memory and JIT code in direct memory
  through the PS5 payload SDK fork's platform layer (`ps5platform/shm.h`,
  `ps5platform/exec.h`), with fastmem.
- `shell/ps5/bigpicture.cpp` - the interface.
- `shell/ps5/sce_sys/` - the title's name, its icon and `pic0.dds`, the
  picture the console shows behind the title and while it loads (4K, BC7):
  the start-up animation's first frame, made with PS5_Vulkan's
  `tools/prepare-assets.sh --background`.
- `shell/ps5/ps5_smb.cpp` - SMB shares as a Flycast storage (libsmb2), games
  read into memory or streamed.
- `shell/ps5/ps5_drawdist.cpp`, `ps5_drawdist_scan.h` - the object draw
  distance option.
- `shell/ps5/ps5_covers.cpp`, `ps5_cheats.cpp`, `ps5_pipelines.cpp` - cover
  downloads, `.cht` cheat files, the per-game shader warm-up.
- `shell/ps5/elevation/` - the USB helper.
- `shell/ps5/ps5-link.sh` - the link: PS5_Vulkan's RADV title recipe.
- `shell/ps5/ps5-toolchain.cmake` - the cross-compile: for the console's
  processor (`-march=znver2`), with floating point left as on every other
  x86-64 build of Flycast (`-ffp-contract=off`).

## Building

On Linux with clang 18, lld 18, CMake, Ninja and Python 3. The repositories
sit side by side in one folder:

```sh
git clone --recursive <this repository's URL> PSFlyCast
git clone https://github.com/mihawk-99/PS5_Vulkan
git clone https://github.com/mihawk-99/PS5_Mesa
git clone https://github.com/mihawk-99/PS5_PayloadSDK
git clone https://github.com/mihawk-99/PS5_RetroArch         # for its SDK install script only
git clone https://github.com/sahlberg/libsmb2
git clone https://github.com/libretro/libretro-database     # optional: the cheat files

# RADV, with the display-mode patch of this repository (119.88 Hz on a VRR
# display; PS5_VIDEOOUT_59HZ). The patch is made on PS5_Mesa 0b2d6d1.
git -C PS5_Mesa checkout -b psflycast 0b2d6d1a61d9bbf89cf8beb88a696144f67c61f8
git -C PS5_Mesa am ../PSFlyCast/shell/ps5/mesa/*.patch
sed -i "s/^mesa_revision=.*/mesa_revision=$(git -C PS5_Mesa rev-parse HEAD)/" PS5_Vulkan/tools/build-radv.sh

cd PS5_Vulkan
tools/setup-native-dependencies.sh
tools/build-radv.sh release      # needs LLVM/Clang 18, libclc, llvm-spirv, meson, mako
tools/rebuild-libc.sh
cd ..

# The payload SDK the title is compiled with: PS5_PayloadSDK at cd3b239 or
# later, installed in PS5_RetroArch/.deps/native/ps5-payload-sdk, where
# build.sh looks for it (PS5_PAYLOAD_SDK names another place).
(cd PS5_RetroArch && tools/setup-native-dependencies.sh)

cd PSFlyCast
shell/ps5/build.sh               # -> build-ps5/dist/PPSA99247
```

The builds published so far were made with PS5_Vulkan `3f3ee69`, PS5_Mesa
`0b2d6d1` plus the patch, and the payload SDK fork at `cd3b239`; `BUILD.txt`
in the title's folder names what a build was made from.

## Credits and licences

What is in this build, and whose it is:

| Part | From | Licence |
|---|---|---|
| The emulator | [Flycast](https://github.com/flyinghead/flycast), by flyinghead and contributors | GPL-2.0-or-later |
| The Vulkan driver, and the tool that links and signs the title | Mesa's RADV as ported in [PS5_Vulkan](https://github.com/mihawk-99/PS5_Vulkan) and [PS5_Mesa](https://github.com/mihawk-99/PS5_Mesa), by Mihawk-99 | GPL-3.0-or-later; Mesa under its own licences |
| The title's start-up, link layout and `libc.prx`; the platform layer (heap, guest memory, the libc functions a title lacks) | Mihawk-99's fork of the payload SDK; [ps5-payload-sdk](https://github.com/ps5-payload-dev/sdk) by John Törnblom and contributors | GPL-3.0-or-later |
| USB drive access (`sandbox-elevator.elf` and its client) | The sandbox-elevation example of ps5-native-app-boilerplate, by BlackBearReloaded | GPL-3.0 |
| Network shares | [libsmb2](https://github.com/sahlberg/libsmb2), by Ronnie Sahlberg | LGPL-2.1 |
| Upscaling (`shell/ps5/fsr`) | [FidelityFX Super Resolution 1.0](https://github.com/GPUOpen-Effects/FidelityFX-FSR), by AMD | MIT |
| The cheat files | [libretro-database](https://github.com/libretro/libretro-database), `cht/Sega - Dreamcast` | CC BY-SA 4.0 |
| The mark, the icon, the start-up animation and its sound | PSFlyCast's own: `shell/ps5/bigpicture.cpp`, `shell/ps5/sce_sys/make-icon.py`, `shell/ps5/sounds/make-startup-sound.html` | GPL-3.0-or-later |
| Covers, downloaded when PSFlyCast runs | [libretro-thumbnails](https://github.com/libretro-thumbnails/Sega_-_Dreamcast) | |
| Descriptions and release dates, downloaded when PSFlyCast runs | [TheGamesDB](https://thegamesdb.net), with Flycast's own scraper and its key | |

ShadowMountPlus (drakmor, after VoidWhisper's ShadowMount) is what mounts and
starts the title on the console; none of it is in the build either.

Flycast compiles a number of libraries in with it (Dear ImGui, glslang,
libchdr, FreeType, xBRZ, picoTCP and others), and the title links the LLVM
runtime and zlib. `licenses/README.txt` in the title's folder lists every
part, its licence, its source and the revision it was built from, and
`licenses/` holds the licence texts.

The combined app is distributed under the GPL, version 3, with no warranty.
Each release has the complete source of everything in it attached, beside
the ZIP.

This is an independent project, not affiliated with or endorsed by Sony
Interactive Entertainment, Sega or the Flycast project. "PlayStation" and
"PS5" are trademarks of Sony Interactive Entertainment; "Dreamcast" is a
trademark of Sega. Vulkan is a registered trademark of the Khronos Group, and
the RADV port in this title is not a conformant product. Use your own games
and BIOS.
