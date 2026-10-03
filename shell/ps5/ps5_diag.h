/*
	PSFlyCast - start-up record and crash report.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include <string>

namespace ps5::diag
{
// Opens <dir>/flycast-boot.log for this run (the two runs before it are kept
// as flycast-boot.1.log and flycast-boot.2.log) and writes the marks made
// before it was open. Returns false when the folder cannot be written.
bool open(const std::string& dir);
const std::string& logPath();

// One line in the boot log, written straight to the file (no stdio buffer),
// so it is there whatever happens next.
void mark(const char *format, ...) __attribute__((format(printf, 1, 2)));

// A notice: a line of the boot log ("notice: ..."), and a notification in the
// console's corner when those are on (showNotifications; off until then, and
// by default: "notifications = 1" in frontend.cfg turns them on). A crash is
// reported the same way.
void notify(const char *format, ...) __attribute__((format(printf, 1, 2)));
void showNotifications(bool show);

// Fatal signals and std::terminate write a report to the boot log: the
// signal, the faulting instruction as an offset in eboot.bin, the registers,
// the return addresses found on the stack, and the last mark.
void installCrashHandler();
}
