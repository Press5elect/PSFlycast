# PSFlyCast documentation

`shell/ps5/README.md` (the player's guide, carried in the title's folder) and `MANUAL.md` (every
screen and setting) are for players; `PRIVACY.md` says what leaves the console. These pages are
for whoever works on the port.

| Page | Read it when |
| --- | --- |
| [dev/workflow.md](dev/workflow.md) | Before any change: the rules, commits, the repositories, the loop, lessons |
| [dev/building.md](dev/building.md) | Building, moving a pin, adding a driver patch; what each version was built with |
| [dev/testing.md](dev/testing.md) | The PC tests and CI; what was checked off the console |
| [dev/releasing.md](dev/releasing.md) | Making a test build or a release |
| [dev/open-work.md](dev/open-work.md) | What is untested, unfinished or decided |
| [history/console-testing.md](history/console-testing.md) | What ran on a console, version by version |
| [history/changelog.md](history/changelog.md) | The notes of v1.3.4 and earlier |

Where the code is: `shell/ps5/` (the port: `bigpicture*.cpp` the interface, `ps5_main.cpp` the
start and the options, `ps5_diag.cpp` the boot log and crash report, `ps5_smb.cpp` the shares,
`ps5_hdr.cpp`, `ps5_card_sync.cpp`, `ps5_fsr.cpp` the picture passes), `shell/ps5/mesa/` the driver
patches, `shell/ps5/release/` the notices and packing, `shell/ps5/tests/` the PC tests; Flycast's
own code under `core/`, changed behind `USE_PS5` or `__PROSPERO__`.
