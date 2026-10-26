#pragma once
#include "maple_devs.h"
#include <memory>

extern std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

void maple_Init();
void maple_Reset(bool Manual);
void maple_Term();
void maple_ReconnectDevices();
void maple_ReconnectDevice(int bus, int port);

void maple_vblank();

// How many Maple frames each bus has been sent since the emulator started (it
// wraps). Used to tell that a game has seen a change on a bus, such as a memory
// card pulled out (maple_unplug_expansion).
extern u32 maple_busFrames[MAPLE_PORTS];
