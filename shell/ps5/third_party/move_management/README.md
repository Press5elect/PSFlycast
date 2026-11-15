# Move Management's input client

`move_management_client.hpp` is Move Management for PS5's reference UDP client
(github.com/REXCRAFT88/Move-Management-PS5 at 4d8851b6809756906a9c029ba11baaecda7ceb49,
`integrations/move_management_client.hpp`), unmodified. Its project credits Project Porpoise
(github.com/elripalda/Porpoise-Dolphin-Emulator-for-PS5, copyright 2026 Ruben) as the source of the
adapted reference client, and AnyPad PS5 for the producer's foundations. GPL-3.0-or-later; the
licence is `LICENSE`.

Move Management is a separate payload: it owns Bluetooth, the controllers' registration and their
calibration, and sends each player's Move and Navigation controller to the console's loopback
(`docs/PROTOCOL.md` there). PSFlyCast only receives (`shell/ps5/ps5_pad.cpp`, the Move section).
