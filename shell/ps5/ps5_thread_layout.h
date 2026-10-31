// PSFlyCast - where the emulator's threads run ([ps5] ThreadLayout).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast (from Crux2's ps5/frontend/ps5_thread_layout.h, Crux2 by Press5elect): the
// affinity calls and the driver thread's core are Crux2's; the threads are PSFlyCast's.
//
// A title has 13 logical processors (0 to 12; 2n and 2n+1 are one Zen 2 core's two threads), and a
// core running two threads splits its queues between them. The layout gives the threads that
// limit a game a core each:
//  - the emulator's thread (the SH4 and the sound chip): processor 0, with 1, its core's second
//    thread, left empty;
//  - the main thread (the renderer and the interface): core 1, processors 2 and 3;
//  - the graphics driver's recording thread, when the driver runs on its own thread: core 2,
//    processors 4 and 5 ([ps5] DriverOwnCore), where it would otherwise share the core of the
//    thread that started it;
//  - every other thread (sound output, disc reads, covers, network): processors 6 to 12.
// Each thread is put in its place as it starts (ps5_threads.cpp's start of every thread,
// core/emulator.cpp for the emulator's), read once when the title starts.
#pragma once

#include <cstdint>

#include "cfg/option.h"

namespace ps5
{
extern config::Option<bool> ThreadLayout;
extern config::Option<bool> DriverOwnCore;
}

namespace PS5ThreadLayout
{
/// A title's logical processors.
constexpr uint64_t kTitleMask = 0x1FFFu;
constexpr uint64_t kEmulationMask = uint64_t{1} << 0;
constexpr uint64_t kMainMask = (uint64_t{1} << 2) | (uint64_t{1} << 3);
constexpr uint64_t kDriverMask = (uint64_t{1} << 4) | (uint64_t{1} << 5);
constexpr uint64_t kRestMask = kTitleMask & ~uint64_t{0x3F};

/// Reads the setting and puts the calling thread (the main thread) on its core. Once, at start-up.
void init();

/// Whether the layout is in effect (decided by init).
bool active();

/// The calling thread to these processors (logged as `what`, unless quiet), whatever the process's set.
bool pinCallingThread(uint64_t mask, const char *what, bool quiet = false);

/// A thread starting (ps5_threads.cpp): to the processors left for other threads.
void threadStarting();

/// The emulator's thread starting (core/emulator.cpp): to its own processor.
void emulationThreadStarting();
} // namespace PS5ThreadLayout
