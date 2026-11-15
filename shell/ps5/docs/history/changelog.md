# Older versions

The README's notes of v1.3.4 and earlier, as they were released. The newest versions' notes stay
in the README (shell/ps5/README.md), which the title's folder carries; move a version here when it
is no longer one of the last five.

## New in v1.3.4

Added:

- **Stick directions added to a control** (quick menu > Controls): push a
  stick while setting a control and that direction is added to it, keeping
  its button, for one game. Each direction of the right stick can be any
  Dreamcast control, for a camera on buttons. A right-stick axis with a
  control on it no longer also moves the triggers, the second stick or the
  mouse.
- **Motion aiming speed and smoothing**, and **Mouse movement: Right stick,
  Motion or Both**: turning the controller moves the Dreamcast mouse.
- **Original look** (Video): the console's own texture filtering and
  mipmaps, 16-bit colour with dither and the flicker filter, in one switch.
  Off by default.
- **Sound output: Stereo or Mono** (Audio), in the console's own settings,
  which games follow.
- **Swap memory card** in the quick menu, while a game runs, as a card is
  pulled out of a controller.
- **Insert disc** lists the other discs of the game's set first.
- **Peripherals: As the game says** (Controls, off by default): the light
  gun, keyboard, microphone or vibration pack the disc says the game uses.
- **Memory cards:** copy all of a game's saves at once; a save of the same
  name asks to be overwritten.
- **Fast forward speed** (2x to 8x, or Unlimited) and **sound while fast
  forwarding**.
- **The game's graphics modes in `flycast-boot.log`.**

Changed:

- **Widescreen game patches are off by default** again, also on a console
  that already has PSFlyCast: with them a patched game's picture is
  stretched to 16:9 while Widescreen says 4:3.
- **Broadcast and Language "Follow the game"** work: a PAL disc starts in
  PAL, with its own 50/60 Hz screen, and the console's language follows the
  disc's region.
- **Render-to-texture effects** (reflections, mirrors) in the game's 16-bit
  colour with dither, the mipmap level right at a higher resolution, and
  the per-texture sharpening at angles, with Original look.
- **Sound:** the volume of a fading voice reads back as the hardware gives
  it (busy music no longer loses notes), mono ignores pan, the vibrato reset
  holds.
- **Copy-protected saves** are copied like any other.
- **Open disc lid** is offered only for a game on several discs.
- **Flycast** is its `dev` branch at 23d212f4f (27 October 2026): the flash
  and saves are written safely, the sound chip's DMA at its real rate.

Fixed:

- **The built-in BIOS:** reading a CD's subcode could stop the emulator;
  streamed reads, whole-sector reads, the old table of contents, abort and
  the reset and exit calls now work as on the console. A game going back to
  the console's menu returns to the library.
- **A disc swapped in** is read again, so a wrong disc is noticed.
- **VMU:** its buttons, setting its clock, reading its screen back.
- **The light gun** works with PAL and Japanese games.
- **The keyboard** reports more than six keys held as the real one does.
- **The YUV video mode** some games' videos use.
- **The console's region** reads right for US and European consoles with a
  language other than English.

## New in v1.3.3

Added:

- **Game options in the quick menu are the whole Settings screen, for this
  game only:** This game, Video, Audio, Controls and System, as in Settings.
  A dot marks a value the game has of its own, "As in Settings" one that
  follows the Settings. Square puts an option back to the Settings' value,
  Triangle twice all of them.
- **Picture options from the Dreamcast's hardware** (Video): Accurate
  mipmaps, Original 16-bit colour (the console's dither) and Flicker filter
  (its vertical softening in TV modes).
- **Driver and thread options** (Settings > System > PlayStation 5): the
  graphics driver on its own thread, how the driver waits for the graphics
  chip, a short display queue, and sound and controls first. All
  experimental and off.
- **Speed lines in `flycast-boot.log`:** render passes and barriers a frame,
  a `slow:` line when a game stays under full speed, and a `session:`
  summary when it ends.

Changed:

- **New defaults:** per-pixel transparency with 96 layers at 4x (1920p), FSR
  1 (sharp), 16x anisotropic and linear texture filtering, widescreen game
  patches, translucent depth mask, the picture's light beside the picture,
  the memory card's screen at 200%, 120 Hz and variable refresh, the sound
  DSP, a 64 ms audio buffer, VMU beeps in the controller, the RGB cable and
  a save state when a game stops. A console that already has PSFlyCast only
  moves the settings still at their old default.
- **Descriptions** of the settings are Flycast's own where it has them.
- **Interface size** goes down to 70%.
- **The graphics driver** is Mesa 27.0-devel's RADV with Crux2's driver
  patches: workarounds for the graphics chip's own hardware bugs, cheaper
  draws, fewer cache flushes.
- **Sound** from 44.1 to 48 kHz through a better filter; the VMU's beep is
  made with the game's tone and duty, at 48 kHz in the controller's speaker.
- **The microphone** follows the game's gain, its 8-bit mode and its
  replies, as the Dreamcast's microphone does.
- **Rumble** fades, pulses and stops as the vibration pack does; the
  controller's large motor only, unless a game asks for a fast buzz.
- **A faster clock, shorter waits** between the emulator and the renderer,
  and textures freed when a game closes.
- **Flycast** is its `dev` branch at e2f148457 (24 October 2026): achievement
  pop-ups can be hidden, and fixes to the broadband adapter and the
  scheduler.

Fixed:

- **The Dreamcast Twin Stick's lever buttons:** the triggers are L2 and R2,
  the turbo buttons L1 and R1, D is Triangle (in v1.3.2 they did nothing).
- **A stick pushed all the way right or down** reaches the end of its range;
  a resting trigger reads 0.
- **PAL games on a 60 or 120 Hz display** are no longer pushed 0.5% fast.
- **Game time** in the tools ran at half speed at 120 Hz.
- **The driver's shader cache** could switch itself off.
- **Anisotropic filtering** changed in a game now applies without a restart.

## New in v1.3.2

Added:

- **Twin-stick games:** a game played with two levers starts with a layout of
  its own made for them: the left stick and the D-pad the left lever, the
  right stick the right lever, L2 and L1 the left lever's trigger and turbo,
  R2 and R1 the right lever's. For the arcade games whose controls name a
  right lever, and for a Dreamcast game with Port A set to the new **Twin
  Stick**.
- **A stick's direction can be put on a control** in the quick menu's
  Controls: Cross, then a push of a stick all the way.

Changed:

- **An arcade game's controls are listed by the names the game gives them**
  ("R UP", "SHOT1") in the quick menu's Controls, with every button it has
  (its seventh to ninth buttons were missing).
- **Flycast** is its `dev` branch at fa9d39a35: a savestate made with the
  broadband adapter is loaded on a console that has none.

## New in v1.3.1

Added:

- **Right stick as the triggers** (quick menu > Controls, for one game if
  wanted): the right stick, left and right, presses the Dreamcast's L and R
  triggers, as far as it is pushed, for a camera that a game turns with the
  triggers. L2 and R2 still work; not while the right stick is the mouse.

Changed:

- **The defaults are the original's** on a new install: the Dreamcast's own
  640x480, transparency sorted per pixel, anisotropic filtering off.
  Everything that improves on the original is the player's choice. A console
  that already has PSFlyCast keeps its settings.
- **Flycast** is its `dev` branch as of 21 October 2026 (a9bd1c74f): among
  its changes, flat-shaded triangles drawn in the right colours after
  sorting, two fixes to the emulated memory map, and the broadband adapter
  polled only while it is used.

Fixed:

- **Covers for games whose files are named in another way** than the cover
  collection ("[GDI] Game (US)[10S 51035]"): the name is tidied (the
  bracketed tags taken out, (US), (EU) and (JP) spelled out) and the game's
  title is tried as well. Games that found no cover before are asked for
  again.
- **The title's folder on extended storage or a USB drive** is found with
  USB drives on (it was looked for in /data/homebrew only).

## New in v1.3.0

docs/history/console-testing.md says which of it has been run on a console.

Added:

- **Controller profiles:** what the controller is in the Dreamcast's ports
  beside its controller. Four built in: Controller; Controller and mouse (the
  right stick aims, the controller keeps every button); Mouse and keyboard
  with WASD or with the arrow keys (the keys an FPS on a PC expects). And four
  of your own, Custom 1 to 4, where every button, trigger, D-pad direction and
  left-stick direction can stay the controller's, do nothing, be a mouse
  button, the mouse wheel, or any of about 80 keys. An input a profile gives
  away is not the Dreamcast controller's as well, so nothing is pressed twice.
  The profile in use is a setting, and a game can have its own.
- **The right stick as the Dreamcast's mouse**, with its own settings: Mouse
  speed, Right stick dead zone, Right stick response (Linear, Precise,
  Aggressive, S-curve), Right stick smoothing, and left and right, up and down
  each Normal, Inverted or Off. They shape the second stick and the turning as
  well.
- **Right stick turns like the left stick:** the right stick, left and right,
  adds to the left stick's, for games that read no second stick.
- **Controller type:** the standard Dreamcast controller, or one that reports
  the right stick as a second stick, for software that reads one.
- **A picture of the Dreamcast controller** in the quick menu's Controls: each
  control labelled with the DualSense input that drives it, what is in the
  two slots, what the profile gives to the mouse or the keyboard instead
  (struck through), what the DualSense's extra buttons do, and the control
  the focused row is about lit.
- **Microphone:** the Dreamcast's microphone in a controller slot, hearing the
  DualSense's microphone, for the games that listen, with a Microphone volume
  (50% by default; loud sound rounded off rather than cut flat).
- **Texture dump** (Settings > Video, for one game if wanted): saves the
  textures a game loads as PNG pictures, sorted into 2D (drawn flat on the
  screen) and 3D, to the console's storage, a USB drive or the network share,
  for making a texture pack. Skip changing textures; Textures saved.

Changed:

- **Everything about the controller for a game is in the quick menu's
  Controls:** the profile, the right stick's settings, turbo, the slots, the
  microphone's volume, the light gun and the button layout. These options
  left Game options and a game's Details > Options.
- **The USB disc drive** asks for a Dreamcast disc ("Insert a Dreamcast
  disc").
- **Flycast** is its `dev` branch as of 17 October 2026 (6bb0fc3e1): among
  its changes, a savestate with a bad region or cable no longer stops the
  emulator, an unknown Maple device is ignored rather than a crash, and the
  game is stopped while a savestate is made in memory.

Fixed:

- **The update check, cover downloads and RetroAchievements with USB drives
  on.** Outside its sandbox the console's HTTPS client refuses every
  certificate; PSFlyCast now uses a TLS of its own there (mbedTLS 3.6.7, as
  PSSwanStation does), checked against the console's certificate list and
  Mozilla's.

## New in v1.2.0

docs/history/console-testing.md says which of it has been run on a console.

Added:

- **Game videos in the library:** the focused game's video plays where its
  cover is, on the Shelves, List and Grid views. Videos are found the way
  ES-DE keeps them (an `.mp4` named like the game or its file, in a `videos`
  folder beside the games or above them, on the console, a USB drive or a
  network share). Settings > Interface > Game videos: Off, Silent or a
  volume. H.264 video with AAC or MP3 sound.
- **Beside the picture** (Settings > Video, for one game if wanted): what
  fills the screen beside a 4:3 picture: black, the picture's own light, a
  gradient in the interface's colour, a picture of your own
  (`borders/<game ID>.png` or `default.png`), or the picture carried on past
  its edges.
- **OLED care** (Settings > Interface): the picture and the interface move a
  few pixels in a slow loop; with dim, the library darkens after two minutes
  without a button.
- **Picture:** Sharp bilinear, NIS and CAS upscaling; Sharpening; Film
  grain; Brightness, Contrast, Colour and Gamma, each for one game if wanted;
  Super widescreen; Horizontal stretching; Rotate screen 90 degrees; Fog,
  Shadows, Translucent depth mask and Fix edge bleeding as options; Frame
  skipping; Memory card screen size (50% to 300%).
- **Frame generation:** two more ways of making the pictures beside
  PSFlyCast's own: AMD FSR3's optical flow and MihawkGen
  (Settings > Video > Generation method).
- **A USB CD/DVD drive:** insert a Dreamcast disc and play it from the Disc
  Drive tab, ported from PSSwanStation.
- **Disc images from zip, 7z and rar archives**, and copying a disc to CDI
  or CHD.
- **Memory cards:** backed up to the network share when a game ends and on
  demand; Format or Delete a card from the memory card manager (Triangle);
  Controller slot 1 and Player 2, 3 and 4 memory card; Own memory card for
  each game; the VMU's beeps through the controller's speaker or the TV
  (Settings > Audio > VMU sound plays in).
- **Network folders added on the console** (Settings > Library > Network):
  Add a network folder, Remove, Network user, Network password, Scan now.
  They are kept in `network.cfg`.
- **Settings:** Interface size, high contrast and colour-blind safe colours;
  settings in sections; a bar that says what needs a restart; Free up space
  (unpacked archives, shader cache, logs); Arcade free play; 32 MB of memory;
  Audio buffer; CPU clock steps of 220 and 240 MHz, the Dreamcast hardware
  mods' +10% and +20%.
- **Turbo** for the face and shoulder buttons, per game.
- **Mods:** text files that change a game a feature at a time.
- **Find in memory,** and a speedrun timer with splits.
- **Forwarder arguments** (`--rom`, `--exit-after-game`).
- **Achievements** unlocked while the site cannot be reached are kept and
  sent later; `PRIVACY.md` says what leaves the console.
- **A "What's new" notice** with the release notes after PSFlyCast has
  updated itself.
- The version on the console's home screen follows the build
  (`01.002.064` for this one).

Changed:

- The emulator is Flycast's `dev` branch at `91c3084d5` (v1.1.0 had
  `4dca1988d`): Capcom's online games, Bleemcast sector conversion, a
  renamed button in one arcade game.
- PSFlyCast is built on the newest RADV stack, and its clock is the
  processor's counter.
- A setting's description is shown whole, wrapped, beside its name.
- A network game stays in memory across Restart game.
- Flycast's compiled-pipeline file (`data/vulkan_pipeline.cache`) is saved
  when a game ends, and counts as the shader cache in Settings.
- The CPU clock goes up to 400 MHz.
- `flycast-boot.log` records the shader cache's size and the pipeline counts.
- NIS and CAS are from NVIDIA and AMD (both MIT); their licences are in
  `licenses/`. FFmpeg (LGPL) plays the videos. PSFlyCast's own files carry
  the additional term in `licenses/psflycast/ADDITIONAL-TERMS.txt`: keep the
  credit to Press5elect.

Fixed:

- **Windows CE games ran slowly.** Flycast's latest `dev` removed the extra
  cycles a memory access costs with the MMU on; PSFlyCast keeps them.
- **Restarting a game read it into memory again** from the network share.
- **Descriptions in Settings were cut to one line** and overlapped the
  settings' values.
- Picture options that were missing from Settings.

## New in v1.1.0

docs/history/console-testing.md says which of it has been run on a console.

Added:

- **RetroAchievements** (Settings > Achievements): the achievements of
  retroachievements.org, earned as you play, through Flycast's own support
  for the site. Softcore only: hardcore mode is not offered. See
  [Achievements](#achievements).
- **Rewind** (Settings > System, off by default, experimental): a game's
  last three minutes are kept in memory, and the quick menu goes back into
  them. See [Controls](#controls).
- **A memory card manager** (Settings > System > Memory cards): the saves
  on each card, copied to another card, deleted, and taken to and from
  files. See [Memory cards](#memory-cards).
- **Favourites, hidden games, time played and last played**, under
  **Manage** in a game's details, with a Favourites shelf in the library
  and Settings > Library > Show hidden games.
- **Change cover**, under Manage too: the collection's box art, title
  screen or picture of a moment of play, the cover PSFlyCast finds by
  itself, or your own picture.
- **Fast forward** in the quick menu, and as a control that can be put on a
  button of the controller.
- **Scanlines** and **CRT**, two picture filters in Settings > Video >
  Upscaling.
- **Software renderer** (Settings > Video, or for one game; experimental):
  the picture computed on the CPU by a software model of the Dreamcast's
  graphics chip.
- **Memory card screen** and **Stretch to fill** in Settings > Video.
- **Frame generation** (Settings > Video, or for one game; experimental, off
  by default): pictures made in between the game's own, for the presents
  that would show a picture again - a 30 fps game on any TV, a 60 fps game
  at 120 Hz. Light makes one between two of the game's, Full as many as the
  TV shows. PSFlyCast's own: it finds how each part of the picture moved,
  from the last frame to this one and back, and where it cannot it shows
  the game's picture as it is. See the manual for what it costs.
- **Four start-up animations**, one each start, made to a new start-up
  sound: see [Skins, backgrounds and motion](#skins-backgrounds-and-motion).
- **Show frame rate** also says how fast the game really runs, as a
  percentage of full speed, and how many of the pictures shown were made by
  frame generation; the same goes to `flycast-boot.log`, with the slowest
  ten seconds.
- **Per-pixel layers** (Settings > Video, 8 to 128) is an option of its own,
  and Transparency sorting has its three choices in one row: per-triangle,
  per-strip, per-pixel.
- A folder, `vmu/`, for memory card saves as files.

Changed:

- The emulator is Flycast's `dev` branch at
  `4dca1988dab56f603c2309e8882e264252fa7c8f`; v1.0.2 had
  `f83fda7069ee7a32e206cb5092c38fd0216d80af`. That is 16 commits of
  Flycast's, among them a fix to the synchronisation between render passes
  and frames in the Vulkan per-pixel transparency renderer, checks of the
  buffer lengths in a save state and of the packets netplay and DCNet
  receive, and the Stretch to fill option.
- The build uses the stack of
  [PS5_VulkanTemplate](https://github.com/mihawk-99/PS5_VulkanTemplate):
  newer revisions of PS5_Vulkan, PS5_Mesa and the payload SDK fork, which
  [Building](#building) names.
- The link step refuses a build that imports a function no module a title
  loads exports. Such an import links, and is null at run time: its first
  call would stop the title.
- With USB drives on, PSFlyCast runs outside its sandbox, where there is no
  `/app0` for the driver to read the title's `param.json` from. The title
  now names the file to the driver, so that 120 Hz output can be taken
  there too.
- Every request PSFlyCast makes over the web says what it comes from:
  `PSFlyCast/1.1.0 (PlayStation 5) Flycast/<Flycast's version>`.
- From Flycast's open pull requests, each named with its number there:
  two players' netplay of a Dreamcast game or of a game from a CDI, GDI or
  CUE image no longer ends in "Peer verification failed" (the checksum both
  sides compare was taken of a memory address, not of the file: #2513; it
  works between two copies that have the fix); a status bit of the
  Dreamcast's processor that a game had set is cleared as on the console,
  which stopped a game from going back to the BIOS (#2531); a CHD image
  with over-long text in its track list can no longer write past the
  memory set aside for it (#2384, made where this build reads CHDs); the
  pointer no longer jumps when a game changes between the two ways a mouse
  reports (#2528); a trigger that reports as a button is recognised when a
  control is given a button (#2213); and what is read from a disc's
  directory, a compressed save state, a name server's answer, an arcade
  game's file list and the other player's packets is checked before it is
  used (#2414, #2415, #2416, #2519, #2520, #2521).
- The updater knows release candidates: `v1.1.0-rc1` is older than
  `v1.1.0-rc2`, and both are older than `v1.1.0`. A release's build is
  offered releases only; a candidate's build is offered the next candidate
  and the release.
- In the candidate (build 44) and not in build 42: the fixes above, the
  updater's candidates, a newer payload SDK and libsmb2
  ([Building](#building)), and the quick menu's Screenshot row and control
  taken out again: the console's Create button takes screenshots.
- In the second candidate (build 46) and not in the first: frame
  generation, the four start-up animations and their sound, the speed in
  Show frame rate, and Per-pixel layers. The release (build 47) is the
  second candidate with its name and number changed.
- `flycast-boot.log` has a line starting `storage:` for each of the folders
  a title may be given (`/app0`, `/download0`, `/data`, `/data/homebrew`,
  `/user/data`): whether it is there, and whether a file can be made in it.
  They are for a later version, which may
  keep what is yours outside the title's folder. Nothing is moved or kept
  anywhere else yet.

