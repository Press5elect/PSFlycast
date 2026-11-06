/*
	PSFlyCast - start-up record and crash report.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include <string>

namespace ps5::diag
{
// Settings > System > Logs, before open(): whether a log is written at all (off: open() makes
// no file and rotates none, lines go nowhere, and a crash leaves no report), and how often the
// file is forced onto the drive: every `syncSeconds` seconds, or 0 for only when the title
// ends, restarts or crashes. Lines always reach the file within a moment; the sync is what the
// drive is made to write, and a console that stops outright can lose what came after the last one.
void configure(bool enabled, int syncSeconds);
bool enabled();

// The next `seconds` seconds sync every 2 s whatever configure() chose: the start of PSFlyCast
// and the first moments of a game are where a console that stops leaves its reason.
void watch(int seconds);

// Opens <dir>/flycast-boot.log for this run (the two runs before it are kept
// as flycast-boot.1.log and flycast-boot.2.log) and writes the marks made
// before it was open. Returns false when the folder cannot be written, or
// when logs are off.
bool open(const std::string& dir);
const std::string& logPath();

// Everything logged so far in the file and on the disk (before the title
// closes or restarts itself).
void flush();

// One line in the boot log. It goes through memory to the log's writer thread,
// which writes it to the file at once and syncs the file as configure() says
// (every 2 s for the first 30 s of a start and of a game), so a line never
// waits for the file system; a crash report empties that memory first.
// (Before the file is open, lines are kept and written when it opens.)
void mark(const char *format, ...) __attribute__((format(printf, 1, 2)));

// A notice: a line of the boot log ("notice: ..."), and a notification in the
// console's corner when those are on (showNotifications; off until then, and
// by default: "notifications = 1" in frontend.cfg turns them on). A crash is
// reported the same way.
void notify(const char *format, ...) __attribute__((format(printf, 1, 2)));
void showNotifications(bool show);

// "memory <when>: ..." in the boot log: the largest free run of direct memory
// (where the graphics driver's allocations come from) and the flexible memory
// free, as Crux2 logs them at a game's start and after it.
void memory(const char *when);

// Fatal signals and std::terminate write a report to the boot log: the
// signal, the faulting instruction as an offset in eboot.bin, the registers,
// the return addresses found on the stack, and the last mark.
void installCrashHandler();
}
