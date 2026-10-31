// PSFlyCast - where the emulator's threads run: the runtime of ps5_thread_layout.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast (from Crux2's ps5/frontend/ps5_thread_layout.cpp and ps5_profiler.cpp's
// ps5_title_thread_named, Crux2 by Press5elect).

#include "ps5_thread_layout.h"
#include "ps5_diag.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/cpuset.h>

namespace ps5
{
// On, as in Crux2, where the same calls are measured on the console. Off: every thread may run on
// any processor, as before v1.3.5.
config::Option<bool> ThreadLayout("ThreadLayout", true, "ps5");
config::Option<bool> DriverOwnCore("DriverOwnCore", true, "ps5");
}

namespace PS5ThreadLayout
{
namespace
{
std::atomic<bool> layoutOn{false};
std::atomic<int> quietFailures{0};

// The set at the size the console's kernel keeps (a set of sizeof(cpuset_t) can be refused with
// ERANGE; the first 64 bits hold every processor there is). 0, or the error.
int setAffinity(cpuwhich_t which, uint64_t mask)
{
	cpuset_t set;
	CPU_ZERO(&set);
	for (int c = 0; c < 64; c++)
		if ((mask >> c) & 1u)
			CPU_SET(c, &set);
	int error = ERANGE;
	for (const size_t size : { size_t{8}, size_t{16}, sizeof(set) })
	{
		error = cpuset_setaffinity(CPU_LEVEL_WHICH, which, -1, size, &set) == 0 ? 0 : errno;
		if (error != ERANGE)
			break;
	}
	return error;
}
} // namespace

bool pinCallingThread(uint64_t mask, const char *what, bool quiet)
{
	mask &= kTitleMask;
	if (mask == 0)
		return false;
	const int error = setAffinity(CPU_WHICH_TID, mask);
	if (error != 0)
	{
		// A thread starting says it once for all of them.
		if (!quiet || quietFailures.fetch_add(1) == 0)
			ps5::diag::mark("threads: %s could not be kept to processors 0x%llx (errno %d)", what,
					(unsigned long long)mask, error);
	}
	else if (!quiet)
		ps5::diag::mark("threads: %s on processors 0x%llx", what, (unsigned long long)mask);
	return error == 0;
}

void init()
{
	if (!ps5::ThreadLayout)
	{
		ps5::diag::mark("threads: no layout ([ps5] ThreadLayout off): every thread may run on any processor");
		return;
	}
	if (!pinCallingThread(kMainMask, "the main thread (renderer, interface)"))
		return;
	layoutOn = true;
	ps5::diag::mark("threads: layout on: emulator on processor 0, main thread on 2-3, the driver's thread on %s, the rest on 6-12",
			ps5::DriverOwnCore ? "4-5" : "the main thread's core");
}

bool active()
{
	return layoutOn;
}

void threadStarting()
{
	if (layoutOn)
		pinCallingThread(kRestMask, "a thread", true);
}

void emulationThreadStarting()
{
	if (layoutOn)
		pinCallingThread(kEmulationMask, "the emulator's thread (SH4, sound chip)");
}
} // namespace PS5ThreadLayout

// From the graphics driver's threads as they start (the driver names this function weakly, PS5_Mesa
// patch 0002), after ps5_threads.cpp put them with the other threads. The recording thread, when
// the driver runs on its own thread, gets a core of its own: it looks for work between batches (a
// spin of up to 50 us), and sharing a core would take half of it from the thread it shares with.
extern "C" void ps5_title_thread_named(const char *name)
{
	ps5::diag::mark("threads: the driver started %s", name ? name : "a thread");
	if (name && std::strcmp(name, "RADV recorder") == 0 && PS5ThreadLayout::active() && ps5::DriverOwnCore)
		PS5ThreadLayout::pinCallingThread(PS5ThreadLayout::kDriverMask, "the driver's recording thread");
}
