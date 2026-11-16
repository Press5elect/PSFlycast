# The interface revamp (2026-11)

How the interface was changed from the tabbed library to Home, places and filters, a Settings home
and folders you choose, and what is still to do. It follows Crux2's revamp playbook
(`docs/dev/interface-revamp.md` in the Crux2 repository) where PSFlyCast's single file allowed.
Read [workflow.md](workflow.md) first.

## What the standing brief meant here

- **Nothing lost.** No setting, action or screen was removed. [`tools/interface-inventory.py`](../../tools/interface-inventory.py)
  keeps the labels of the interface at v1.3.9 (`interface-inventory.json`, about 1000 strings: every
  setting, heading, button, quick menu entry) and fails when one is gone from the code. A label
  that was changed on purpose goes in `acknowledged` with the reason. It runs with the PC tests.
  It is a coarse net (a string is there or not); it does not see a row that is still there but
  cannot be reached.
- **Whole categories only.** The Settings areas group the existing categories; no row moved
  between them. Pins and search use copies of rows (`Row::from` names the category), so a change
  made there is the same change.
- **Draw with the `col::` tokens.** The new screens use the skin's colours and `motion()`.

## What was done

| Piece | Where | Notes |
| --- | --- | --- |
| Folders engine | `ps5_folders.{h,cpp}`, `tests/folders` (70 checks, ASan and UBSan) | places, browse, writable probe, `lookFor`, `folders.cfg`, which folders are in use |
| Folder screens | `bigpicture_folders.inc` | list picker with async loaders, browser, Look For My Files, the Folders category |
| Wiring | `ps5_main.cpp` `setFolders`, `ps5_covers.cpp` `coversDir`, `ps5_video.cpp`, `bigpicture.cpp` | without a choice every folder is the one it was |
| Top bar and filters | `bigpicture.cpp` `topBar`, `placeOrder`, `stepPlace`, `inSource` | Source `All` is new (4); the filters are drawn inside the 96-unit bar, so no screen moved |
| Home | `bigpicture.cpp` `homeScreen` | hero, "Right now", played lately; only real data |
| Settings home | `bigpicture.cpp` `settingsHome`, `refilter`, `areaDefs` | seven areas, search, eight pins (`Options::pins`, a hash of category and label) |

## Decisions of the plan, as they were settled

1. **Bring Crux2's interface home, or restyle in place:** in place. Crux2's screens sit on a
   Backend interface over PCSX2 (about 80 virtual functions); PSFlyCast's are one file tied to
   Flycast. The changes were made to the existing screens, with Crux2's ideas.
2. **Flycast's words for its settings:** not done. The Settings keep their wording.
3. **Top bar of places, storage as filters:** done.
4. **Library views:** the three stay.
5. **Skins:** not done. Crux2's 36 are mostly light; PSFlyCast draws white text in every skin
   (`col::text` is a constant), so a light skin needs the text colour made variable across the
   whole interface. Of the 30 kit themes only four are dark with white text and a readable
   accent (Tiles, Pill, Admin, Code); they were left out until the licence notice is made.
6. **Look For My Files:** done.
7. **Folders early as v1.3.10:** no; it came with the rest.
8. **Fallback to the old interface:** not kept. The old screens are the same code, changed, so a
   fallback would mean two copies of 11,000 lines.

## Not done, on purpose

- **No PC picture of the real screens.** Crux2 has a software-rendered preview of its interface;
  PSFlyCast's file needs the Flycast core and about 300 `ps5::` functions to run. Nothing in
  this revamp has been seen by anyone: not on a PC, not on a console. The first look is the
  console's.
- **Levels** (Essentials, More, Everything): PSFlyCast has about 90 settings of its own; the
  three levels were for Crux2's 731.
- **"Changed lately"** card, the game page and quick menu redesign, the performance overlay sizes,
  a settings text file with Flycast's words: not started.
- **The memory cards' folder** (Flycast's `VMUPath`): the card manager, the card swap and the
  sync each assume `data/`. Changing one without the others could make a card look empty.
- **Polish** (the playbook's phase 5): text cut off at Interface size 70 %, hints and toasts over
  content, motion, empty states, per-frame cost, every screen in every skin. Left for review.

## Continuing

Run `python3 shell/ps5/tools/interface-inventory.py check` after any change to a screen. To start a
new baseline at a release: `interface-inventory.py make --from <tag>` (keeps `acknowledged`).
The next step is the polish checklist in Crux2's playbook, with the console's pictures as the
evidence.
