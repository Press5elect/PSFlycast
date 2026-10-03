# PSFlyCast user manual

How to install PSFlyCast, add your games and use everything in it. For how it
is built and what it is made of, see [README.md](README.md).

> **No games, BIOS files or keys come with PSFlyCast, and none ever will.**
> Use only backups you made yourself of games you own, and BIOS files dumped
> from your own console.

## Contents

1. [What you need](#1-what-you-need)
2. [Install and update](#2-install-and-update)
3. [Add your games](#3-add-your-games)
4. [The library](#4-the-library)
5. [Playing a game](#5-playing-a-game)
6. [The quick menu](#6-the-quick-menu)
7. [Saving](#7-saving)
8. [Games on several discs](#8-games-on-several-discs)
9. [Options for one game](#9-options-for-one-game)
10. [Cheats and patches](#10-cheats-and-patches)
11. [Changing the controls](#11-changing-the-controls)
12. [Settings](#12-settings)
13. [Games on a network share](#13-games-on-a-network-share)
14. [Games on a USB drive](#14-games-on-a-usb-drive)
15. [The look](#15-the-look)
16. [Where your files are](#16-where-your-files-are)
17. [When something goes wrong](#17-when-something-goes-wrong)

## 1. What you need

- A jailbroken PS5 with
  [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), or another
  launcher that starts titles from `/data/homebrew/`.
- A way to copy files to the console, such as its FTP server.
- Your own games. A DualSense controller; up to four work, one for each
  logged-in user.

## 2. Install and update

1. Unpack the release ZIP. It holds one folder, `PPSA99247`.
2. Copy that folder to `/data/homebrew/` on the console, so that it is
   `/data/homebrew/PPSA99247/`.
3. Wait for the launcher to report the title ready, then start **PSFlyCast**
   from the home screen.

**To update**, copy the new `PPSA99247` folder **over** the old one and let
it replace the files. Do not delete the old folder first: your games, saves,
settings and covers are inside it.

## 3. Add your games

Copy your games into `/data/homebrew/PPSA99247/games/`, then press **Square**
in the library to scan. Folders inside `games/` are scanned too.

| System | File types |
|---|---|
| Dreamcast | `.chd`, `.gdi` (with its track files), `.cdi`, `.cue` (with its `.bin` files) |
| NAOMI, NAOMI 2, Atomiswave | `.zip` or `.7z` arcade sets |

`.chd` is the best choice for Dreamcast games: one file, and the smallest.

**BIOS files** go in `bios/`:

- Dreamcast games start without one, using the built-in BIOS. A real
  `dc_boot.bin` and `dc_flash.bin` start more games correctly. Settings >
  About > Dreamcast BIOS says which one is in use.
- Arcade games need their BIOS sets (`naomi.zip`, `awbios.zip` and so on).

Games can also be played from a [network share](#13-games-on-a-network-share)
or a [USB drive](#14-games-on-a-usb-drive).

## 4. The library

The library is what you see when PSFlyCast starts.

| Button | Does |
|---|---|
| D-pad or left stick | Move between games |
| Cross | Play the game |
| Triangle | Game details |
| Square | Scan the open tab for games again |
| L1 / R1 | Previous / next tab: Internal, USB, Network |
| L2 / R2 | Jump by letter: R2 to the first game of the next letter, L2 back |
| OPTIONS | Open or close the Settings |

- **Tabs.** Internal is the `games/` folder. The USB tab is there only when
  USB drives are turned on. Each tab remembers where you were, and PSFlyCast
  opens on the tab you used last.
- **Views.** Shelves (with a Recently played row), a compact grid, or a list.
  Change it in Settings > Library > Library view.
- **Covers and descriptions** are downloaded by the game's file name. Names
  in the usual dump style, such as `Game (USA).chd`, are found most reliably.
  To use a cover of your own, save it as `<game file name>.png` (or `.jpg`) in
  `covers/`. Settings > Library > Download covers and descriptions turns
  downloading off.
- **Game details** (Triangle) shows the description and four actions: **Play**,
  **Load state** (start from a saved state), **Options** (settings for this
  game only) and **Cheats**. Circle goes back.

While a game loads, Circle cancels.

## 5. Playing a game

| DualSense | Dreamcast | Arcade |
|---|---|---|
| Cross / Circle / Square / Triangle | A / B / X / Y | Buttons 1 / 3 / 2 / 4 |
| L2 / R2 | Left / right trigger | Left / right trigger |
| L1 / R1 | Z / C | Buttons 6 / 5 |
| OPTIONS | Start | Start |
| Left stick | Analog stick | Stick, or joystick (see below) |
| D-pad | D-pad | Joystick |
| L3 / R3 | | Insert coin / Service |
| **Touch pad click** | **Quick menu** | **Quick menu** |

Most arcade games have a digital joystick, which is the D-pad. In those the
**left stick moves the joystick too**; in a game with a wheel, a flight stick
or a light gun it stays the analog stick. Settings > Controls >
**Left stick as D-pad** turns this on for every game or off, and a game can
have its own choice (Details > Options, or Game options in the quick menu):
on suits a Dreamcast game that only reads the D-pad.

Any of these can be changed: see [Changing the controls](#11-changing-the-controls).

## 6. The quick menu

Click the **touch pad** during a game. The game pauses and the menu opens;
Circle or the touch pad again resumes.

| Item | Does |
|---|---|
| Resume | Back to the game |
| Save state | Saves the game exactly as it is now, in the chosen slot |
| Load state | Goes back to the state in the chosen slot |
| State slot | Chooses one of 10 slots; each shows a picture of what it holds |
| Open disc lid / Insert disc | Changes the disc (see [Games on several discs](#8-games-on-several-discs)) |
| Cheats | Switches cheats on and off |
| Game options | Settings for this game only |
| Controls | Changes which button does what, for this game |
| Restart game | Starts the game again from its beginning |
| Quit game | Back to the library |

**Restart game** asks twice: press Cross again within four seconds. Anything
not saved is lost.

## 7. Saving

There are two kinds of save, and they are separate.

- **The memory card.** Games save to it as they do on a Dreamcast. It is
  kept in `data/` and is shared by all the discs of a game.
- **Save states.** A snapshot of the whole console, made from the quick menu.
  There are 10 slots for each game. Load one from the quick menu, or start
  the game straight from one with Triangle > Load state in the library.

Settings > System has **Auto save state** (save when you quit a game) and
**Auto load state** (resume from that state when the game starts).

A save state made by one version of PSFlyCast may not load in a later one;
memory card saves do. Keep a memory card save of anything you care about.

## 8. Games on several discs

Name the discs as dumps usually are: `Game (USA) (Disc 1).chd`,
`Game (USA) (Disc 2).chd`. `Disk B`, `CD2` and `Disc 2 of 4` are understood
too. The library then shows them as one entry, with the number of discs on
its cover.

- **Play** starts the disc you played last, or disc 1 the first time.
- In **Game details**, Square goes to the next disc.
- **When the game asks for the next disc:** touch pad > **Open disc lid**,
  then touch pad > **Insert disc**. The game's own discs are listed first,
  with the next one already chosen.

The memory card is shared by all the discs, so your save carries over. Save
states belong to one disc.

Settings > Library > Group the discs of a game turns the grouping off.

## 9. Options for one game

Every setting can be different for one game. What you change this way is
kept for that game and leaves the Settings as they are.

- **Before the game starts:** Triangle on the game, then **Options**.
- **While it runs:** touch pad, then **Game options**.

A dot marks an option the game has a value of its own for. **Square** puts it
back to the Settings' value. Most changes take effect when the game resumes;
the few that wait for the next start say so.

A game on the Network tab, or an arcade game, has to be started once before
it can have options of its own.

**CPU clock** runs the emulated processor slower or faster than the
Dreamcast's 200 MHz, from 100 to 400 MHz. Raising it smooths a game that
slows down or whose frame rate is uneven. It does not turn a 30 fps game into
a 60 fps one, and some games break or run too fast.

## 10. Cheats and patches

**Cheats.** PSFlyCast comes with a set of cheat files in `cheats/`. For a
game, open Triangle > **Cheats** (or touch pad > **Cheats** while playing).
The file with the closest name is loaded with every cheat off. Switch on the
ones you want; left and right on the first row pick another file. Your own
`.cht` files go in `cheats/`.

**Patches (60 FPS, widescreen).** These are code changes for one version of a
game, kept apart from cheats. None come with PSFlyCast. If you put your own
list in `patches/patches.txt`, a game it covers gets a **60 FPS patch** or
**Widescreen patch** switch at the top of its options. The README says how to
make the list. A patch can do nothing or break a game: turn it off again if
so.

Settings > Video also has **Widescreen** and **Widescreen game patches**,
which are the emulator's own and need no file.

**Object draw distance (experimental).** In two Dreamcast games (the
option's own line names them), rings, enemies and item boxes appear only a
short way in front of you. That is how the games were made, and a real Dreamcast does the same.
This option, at the top of a game's options, makes them appear from 1.5, 2, 3
or 5 times as far. It takes effect the next time the game starts, and a stage
has to be loaded after that for it to show.

- It is made for those two games and does nothing in others.
- More objects are alive at once, so the game has more to do: if it slows
  down, raise the **CPU clock** for that game, or choose a smaller distance.
- The games have limits on how many objects can exist. A large distance can
  go past them, and the game may then glitch or stop. Start with 2x.
- It changes the objects only, not how far the level itself is drawn.
- A save state made with it on keeps the longer distances.

## 11. Changing the controls

While a game runs: touch pad, then **Controls**.

1. Move to the control you want to change and press **Cross**.
2. Press the DualSense button it should be on.

A button that did something else stops doing it and shows as **Not set**
until you give it a control. L2 and R2 stay analog when a trigger is put on
them. The sticks and the touch pad are not changed.

The first change gives the game a layout of its own, loaded whenever that
game starts. The **Layout** row at the top puts the game back on the layout
every game has. Settings > Controls > **Restore the default layout** resets
that shared one.

**Rumble** needs the rumble pack, which is in the controller's second slot by
default. Settings > Controls > Controller slot 2 swaps it for a second memory
card. Vibration strength and the stick's dead zone are there too.

## 12. Settings

Press **OPTIONS** in the library. D-pad moves, left and right change a value,
Circle goes back.

| Category | What is in it |
|---|---|
| **Video** | Internal resolution (up to 10x; 3x by default), transparency sorting, widescreen, texture filtering and upscaling, custom textures, mipmaps, native depth interpolation, frame skipping, VSync, the FPS counter, 120 Hz output |
| **Audio** | Volume, the sound chip's effects, the memory card's beeps |
| **Controls** | Vibration, stick dead zone, left stick as D-pad, the controller's second slot, restore the default layout |
| **System** | Region, language, TV standard, video cable, built-in BIOS, fast disc loading, the CPU recompiler, CPU clock, auto save and load state |
| **Interface** | Skin, accent colour, background, motion, the start-up animation |
| **Library** | Scan for games, library view, cover downloads, USB drives, disc grouping, network game loading |
| **About** | The build number, the display mode in use, which BIOS is in use, where the files are, credits, Quit PSFlyCast |

Each setting has a line under it saying what it does.

**A sharper picture.** A Dreamcast game is drawn at 640 x 480; these raise
what you see, most useful first:

- **Internal resolution.** 5x and up is more than a 4K screen shows, and the
  extra is used to smooth edges. 9x (4320 lines) is exactly twice 4K, which
  smooths best; 10x (4800 lines) draws more still, but a 4K screen shows no
  more of it than of 9x. If a game slows down or stays black, go lower, and use
  per-strip Transparency sorting at the highest settings.
- **Anisotropic filtering** at 16x keeps floors and walls sharp into the
  distance.
- **Texture upscaling** redraws the game's small textures 2 to 6 times
  larger. It suits flat, drawn art more than photographs. **Upscale textures
  up to** chooses how large a texture may be and still be upscaled; larger
  sizes use much more memory and can make a game hitch when it loads them.
- **Custom textures.** If you have a texture pack for a game, put it in
  `data/textures/<the game's ID>/` and turn this on. Game details shows the
  ID. No packs come with PSFlyCast.

**120 Hz.** On a TV that takes 4K at 120 Hz, PSFlyCast uses that mode, which
makes the menus smoother and a late frame less visible. Settings > About >
Display shows the mode in use. Settings > Video > 120 Hz output turns it off,
from the next start.

## 13. Games on a network share

PSFlyCast can read games from a shared folder on a PC or a NAS (SMB, the
Windows kind of sharing).

1. Start PSFlyCast once. It writes a template, `network.cfg`, in its folder.
2. Edit that file on the console:

   ```
   path = 192.168.1.10/Games/Dreamcast
   user = guest
   password =
   ```

   `path` is server/share/folder. Give the server by its IP address; a name
   is not looked up. Add a `path` line for each folder. For an open share
   leave `user = guest` and the password empty; otherwise give the account.
3. Start PSFlyCast again and open the **Network** tab.

The folder is scanned the first time the tab is opened, and the list is kept.
After that the share is only contacted when you press **Square** on the tab
or start one of its games, so a NAS whose disks sleep is left asleep.

With Settings > Library > **Load network games into memory** on (the
default), the whole game is read before it starts, with a progress bar, and
the share is not needed while you play. With it off the game starts at once
and is read as it runs.

A NAS that is waking up gets a minute to answer; the screen shows how long it
has waited, and Circle cancels.

## 14. Games on a USB drive

1. Put your games in a folder named `flycast` or `dreamcast` at the top of
   the drive. `dc`, `naomi`, `atomiswave` and `arcade` are read too.
2. Turn on Settings > Library > **USB drives**.
3. Restart PSFlyCast. A **USB** tab appears.

This needs the payload loader **elfldr** running on the console (port 9021).
If the tab says it has no access, start elfldr and then PSFlyCast again.
Cover downloads may stop working while USB drives are on.

## 15. The look

Settings > Interface. A change shows at once.

- **Skin:** Midnight (the default), Ember, Pocket LCD, Arcade, Carbon.
- **Accent colour:** the skin's own, or one of seven.
- **Background:** the skin's own, or Still, Aurora, Waves, Sparks, Horizon,
  Dot matrix, Cover colours.
- **Motion:** All, Less (short fades, still background) or None.
- **Start-up animation:** on or off. Any button skips it.

**Your own logo.** A square picture with a transparent background, saved as
`logo.png` in PSFlyCast's folder, replaces the turning disc.

## 16. Where your files are

Everything is in `/data/homebrew/PPSA99247/`:

| Folder or file | Holds |
|---|---|
| `games/` | Your games |
| `bios/` | BIOS files |
| `covers/` | Downloaded covers, and your own |
| `cheats/` | Cheat files |
| `patches/` | Your own `patches.txt`, if you made one |
| `data/` | Memory cards, save states, each game's own options |
| `network.cfg` | The network shares |
| `flycast-boot.log` | What happened at start-up, and a crash report if there was one |

**To back up your saves, copy `data/`.**

## 17. When something goes wrong

**A game shows a black screen, or draws wrong.**

- Put a real BIOS in `bios/` if you are on the built-in one.
- In the game's options, try Transparency sorting on per-strip, and a lower
  Internal resolution.
- Try Native depth interpolation the other way round, and Mipmaps off.
- Try Video cable on TV, or another Region.

**A game slows down or stutters.** The first time a game shows a new effect
there is a short hitch; it is prepared in advance from the second start on.
If the game itself slows down, lower the Internal resolution, or raise the
CPU clock for that game.

**The BIOS asks for the date and time every time.** Set it once and let the
BIOS save it; it is kept with the BIOS's settings in `data/`.

**No rumble.** Check Settings > Controls: Controller slot 2 must be the
rumble pack, and Vibration above zero.

**No covers.** Check that downloading is on and that the game's file is named
in the usual dump style. Turning USB drives off can bring downloads back.

**The Network tab is empty.** The tab says what the share answered. Check
the IP address, the share and folder names, and the account in `network.cfg`.

**PSFlyCast closes or does not start.** Copy `flycast-boot.log` and
`flycast-boot.1.log` from its folder **before starting it again**, and report
the problem with both files and the build number (Settings > About). Never
attach games, BIOS files or keys.

To get a notification on the console when PSFlyCast crashes, add
`notifications = 1` to `frontend.cfg` in its folder.

---

PSFlyCast is an unofficial port of Flycast. It is not affiliated with or
endorsed by Sony Interactive Entertainment, Sega or the Flycast project.
"PlayStation" and "PS5" are trademarks of Sony Interactive Entertainment;
"Dreamcast" is a trademark of Sega.
