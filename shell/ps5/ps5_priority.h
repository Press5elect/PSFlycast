// Crux2 - which of the title's threads the console runs first ([ps5] ThreadPriorities).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast (from Crux2's ps5/frontend/ps5_priority.h, Crux2 by Press5elect): the roles
// and priorities are Crux2's; the threads named below are PSFlyCast's.
//
// Every thread of a title starts at the same priority (700 of 256 to 767, lower runs first), and
// the console's scheduler gives a processor to the higher priority strictly: a thread that never
// waits keeps its processor from every thread of its priority or below. (Measured by PS5_MihawkOS's
// scheduler probe, Copyright (C) 2026 Mihawk, GPL-3.0-or-later: with every processor busy, a
// periodic thread at the title's priority made no progress at all.) The emulator's threads (the
// SH4 and its dynarec, the renderer's work) can keep a processor busy, so a sound thread, the
// controllers or the interface could wait behind them, and a background job (box art, a disc
// read-ahead, a copy, a rewind snapshot, a network share) could take a processor from them.
//
// A thread starts at the priority of the thread that started it, so the emulation thread sets the
// title's own (700) again: the threads it starts are then at 700 too.
//
// So: the sound threads and the microphone run first, then the controllers, then the interface
// (PSFlyCast's main thread, which also presents the game's frames); the emulator's thread stays
// where every thread starts; background jobs run after them all. Each thread sets its own, when
// it starts. scePthreadSetprio is found while the title runs (not imported: a title that imports
// what the console does not give it does not start).
//
// As in Crux2, it is off unless [ps5] ThreadPriorities is set (emu.cfg): every thread then keeps
// the title's priority, as before.

#pragma once

#include "cfg/option.h"

namespace ps5
{
extern config::Option<bool> ThreadPriorities;
}

namespace PS5Priority
{
enum class Role
{
	Sound,      // 690: the sound output streams, the interface's sounds, the microphone
	Input,      // 692: the controllers' threads
	Interface,  // 695: the main thread (the interface, the renderer's presents)
	Emulation,  // 700: the emulation thread, and so the threads it starts
	Background, // 730: box art, disc read-ahead and copies, texture dumps, rewind, network shares
};

/// The calling thread runs at its role's priority, unless [ps5] ThreadPriorities is off.
/// Said once per role in the boot log ("priority: ...").
#ifdef __PROSPERO__
void SetCurrentThread(Role role);
#else
// Other builds of the files that call this: nothing to set there.
inline void SetCurrentThread(Role) {}
#endif
} // namespace PS5Priority
