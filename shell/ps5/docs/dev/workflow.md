# Working on PSFlyCast

The rules, the repositories and the loop: the reference. Notes an agent keeps for itself stay on
its machine and are never committed.

## The rules

- **Never push, tag or release without Press5elect's word, each time.** He creates the tags and
  the GitHub releases himself (this environment gets HTTP 403 on them; branch pushes work). Never
  force push. Pull requests only when asked.
- **Don't break what works; work from evidence.** Find the cause before changing code; say what
  was checked and what was not. A change that shows no benefit on the console comes out again.
  Implement what is shown to help, not what might.
- **Say plainly when something has not been run on a console.**
- **Avoid writes to the console's internal NVMe** (logs, caches, settings, the library file).
- **Diagnose through the title's boot log** (`flycast-boot.log`), not klog.
- Defaults are Press5elect's v1.3.3 choices: accuracy options off, "Original look" turns them on,
  widescreen patches off.
- **Never commit or ship** games, BIOS, keys, firmware, user configs or saves. No game titles in
  docs, notes or commits. Sega SDK material: learn from it only, never copy text or code.
- Credit "Press5elect" in licence and copyright notices. Code from Crux2 or OGBox keeps its own
  licence header and attribution term (`release/components.json` lists those files).

## Commits

- As `Claude <noreply@anthropic.com>`; the trailer is exactly
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No session links, in commits or
  anywhere in the repository; no agent instruction files in the repository.
- Stage with explicit `git add` paths. One change per commit: a file with hunks for two changes is
  staged a hunk at a time (`git apply --cached` of the hunks wanted).
- A subject that says what changed for whom; then why, with the evidence, and what was not run.
- A change to how something works updates its document (MANUAL.md, these pages) in the same commit.

## The repositories

| Repository | What | Branches |
| --- | --- | --- |
| github.com/Press5elect/PSFlycast | this port | `main` and `next-1.3` kept at the same commit; work on a session branch |
| github.com/flyinghead/flycast | upstream, `dev` | remote `upstream` |
| github.com/Press5elect/Crux2 | the PS2 title: the driver patches, the thread layout, the card sync | read only from here |
| github.com/Press5elect/OGBox | the original Xbox title: the elevation helper, the boot log ring, HDR, the menu ideas | read only from here |
| github.com/mihawk-99/PS5_Vulkan, PS5_Mesa, PS5_PayloadSDK | the stack | pinned ([building.md](building.md)) |

## The loop

1. **Evidence first**: the owner's boot log (process it with scripts, don't read it whole), the
   code, the same bug in upstream's or the sibling projects' history.
2. **Change** the smallest thing the evidence supports.
3. **Check on the PC**: the title builds and links (the link's import check refuses an import no
   module exports), the PC tests pass with both compilers, new logic gets a test where it can
   run off the console.
4. **Commit locally.**
5. **A test build** for the owner: the build number bumped (`ps5_build.h`), `build.sh`,
   `release/pack-zip.py`, the ZIP's SHA-256, `release/stage-notices.py --check` on the unpacked
   folder. Say what it changes, how it was checked, what only the console can show, and exactly
   what to try and send back.
6. When the owner confirms, it stays; when it made no difference, it comes out.

## Lessons

- **A change to the interface is checked for lost labels.** `python3 shell/ps5/tools/interface-inventory.py check`
  (it runs with the PC tests) fails when a string of the interface at the last release is gone from
  the code; acknowledge a deliberate change with its reason. It catches a removed row, not an
  unreachable one: look at the screens too.
- **`using namespace ps5::folders;` is ambiguous in bigpicture.cpp.** `settings()` (the Settings
  screen), `::settings` (Flycast's global) and `Games` all exist there; qualify them
  (`ps5::folders::settings()`, `ps5::folders::Games`).
- **The covers folder is one folder.** PSFlyCast keeps notes about its covers beside them
  (`.choices`, `.yours`, `not-found-2.txt`), so a chosen folder replaces the default rather than
  adding to it (`ps5_folders.cpp`, `add()` for `Covers`).
- **Never write on a default folder in place of a chosen one that is missing.** A game would start
  with its saves looking gone. `folders::resolve` reports `askFirst`; `launch()` asks.
- **The console does not give a title every function the SDK declares.** `pthread_get_name_np`
  links against the SDK's stubs but no module exports it to a title; the link's import check
  refused it (v1.3.9's crash report keeps its own thread names instead). Look a function up by its
  NID at run time (`ps5_videoout.cpp`) when a firmware may lack it.
- **A pin move can be larger than its commit list suggests.** libsmb2 `7e4ff97..177a9de` was 202
  commits and 51 changed files; check what the build's file list (`ps5.cmake`) compiles.
- **Don't copy a feature's evidence across a different implementation.** OGBox's three cover
  downloads at once are plain HTTP on separate sockets; PSFlyCast's go through one shared HTTPS
  client, so OGBox's result says nothing about it (left out in v1.3.9).
- **Vulkan's float-to-UNORM writes may round either way**: never pack bits through a colour
  write; copy an image of the real format (HDR's 10:10:10:2, `ps5_hdr.cpp`).
- **libsmb2's rename does not replace a file**: write beside, remove, rename
  (`ps5::smb::storeFile`).
- **Every present path** has to be covered by a change to how frames reach the screen: the game's
  (`PresentFrame`) and the menus' own (`vulkan_driver.h`'s `renderDrawData`).
- **A signal handler takes no lock.** v1.3.9's SIGTERM line and its memory-kept log first went
  through mutexes the interrupted thread could hold, which would hang the title at close; they now
  write straight to the file or drop the line (`ps5_diag.cpp`). Read every path a handler reaches.
- **Don't make a UI action wait on the network.** A game's start joined the card sync's thread,
  which could be in a share request for a minute; guard only what needs guarding (the local
  card writes, `bigpicture_cards.inc`).
