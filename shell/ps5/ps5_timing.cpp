// Crux2 - how late the console's sleeps wake, and the frame limiter's wait: the runtime of
// ps5_timing.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0+
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast (from Crux2's ps5/common/PS5Misc.cpp, Crux2 by Press5elect): the probe, the
// two sleeps and the limiter's wait as Crux2 has them, on PSFlyCast's clock (ps5_clock.c) and
// boot log (ps5::diag::mark) in place of PCSX2's GetCPUTicks and Console.

#include "ps5_timing.h"
#include "ps5_clock.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "types.h"
#include "cfg/option.h"
#include "hw/pvr/spg.h"

#include <atomic>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <sched.h>
#include <thread>

extern "C" int sceKernelUsleep(unsigned microseconds);

namespace
{
// What the start-up probe found (ps5_timing.h): the margin the frame limiter waits out awake,
// and which call sleeps.
std::atomic<uint64_t> s_spin_margin_ns{PS5Timing::MARGIN_DEFAULT_NS};
std::atomic<bool> s_sleep_with_usleep{false};

// One sleep with nanosleep. A sleep that a signal cut short goes on for what is left of it.
bool NanosleepFor(uint64_t ns)
{
	const uint64_t until = PS5Timing::NowNs() + ns;
	for (;;)
	{
		struct timespec ts;
		ts.tv_sec = static_cast<time_t>(ns / 1000000000ULL);
		ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
		if (nanosleep(&ts, nullptr) == 0)
			return true;
		if (errno != EINTR)
			return false;
		const uint64_t now = PS5Timing::NowNs();
		if (now >= until)
			return true;
		ns = until - now;
	}
}

// The same with sceKernelUsleep, which takes microseconds and answers with an error code.
bool UsleepFor(uint64_t ns)
{
	const uint64_t until = PS5Timing::NowNs() + ns;
	for (;;)
	{
		const uint64_t us = (ns + 999) / 1000;
		const int r = sceKernelUsleep(static_cast<unsigned>(std::min<uint64_t>(us, 0xffffffffULL)));
		if (r == 0)
			return true;
		const uint64_t now = PS5Timing::NowNs();
		if (now >= until)
			return true;
		// An interrupted sleep goes on; any other failure falls back to nanosleep for the rest.
		if (r != EINTR && r != static_cast<int>(0x80020004))
			return NanosleepFor(until - now);
		ns = until - now;
	}
}
} // namespace

uint64_t PS5Timing::NowNs()
{
	return ps5px_monotonic_ns();
}

uint64_t PS5Timing::SpinMarginNs()
{
	return s_spin_margin_ns.load(std::memory_order_relaxed);
}

bool PS5Timing::SleepsWithUsleep()
{
	return s_sleep_with_usleep.load(std::memory_order_relaxed);
}

void PS5Timing::SleepFor(uint64_t ns)
{
	if (s_sleep_with_usleep.load(std::memory_order_relaxed))
		UsleepFor(ns);
	else
		NanosleepFor(ns);
}

void PS5Timing::StartProbe()
{
	static std::atomic<bool> started{false};
	if (started.exchange(true))
		return;
	std::thread([] {
		const auto now = [] { return NowNs(); };
		const Measured nano = Measure([](uint64_t ns) { return NanosleepFor(ns); }, now);
		const Measured usl = Measure([](uint64_t ns) { return UsleepFor(ns); }, now);
		const bool second = PreferSecond(nano, usl);
		const Measured& taken = second ? usl : nano;
		const uint64_t margin = taken.ok ? MarginFor(taken.lateness) : MARGIN_DEFAULT_NS;
		s_sleep_with_usleep.store(second, std::memory_order_relaxed);
		s_spin_margin_ns.store(margin, std::memory_order_relaxed);
		ps5::diag::mark("timing: sleeps wake late by %.2f ms typically, %.2f ms at worst (nanosleep %.2f/%.2f, "
				"sceKernelUsleep %.2f/%.2f ms at 90%%/worst); %s sleeps, the frame limiter waits the last %.2f ms awake",
				taken.p50 / 1e6, taken.worst / 1e6, nano.p90 / 1e6, nano.worst / 1e6, usl.p90 / 1e6, usl.worst / 1e6,
				second ? "sceKernelUsleep" : "nanosleep", margin / 1e6);
	}).detach();
}

void PS5Timing::SleepUntil(uint64_t end)
{
	// No clock_nanosleep() with an absolute time on the console. Sleep for what is left less the
	// margin the start-up probe measured for the kernel's wake-up, then wait out the margin awake,
	// giving the core to the other threads while more than SPIN_LAST_NS is left and spinning only
	// the end.
	LimiterWaits waits;
	WaitUntil(
		end, SpinMarginNs(), [] { return NowNs(); }, [](uint64_t ns) { SleepFor(ns); },
		[] { sched_yield(); }, [] { __builtin_ia32_pause(); }, waits);
}

namespace
{
// Whether the screen paces the game now: presents wait for the display (VSync, no fast
// forward), each refresh is not a frame of its own (no variable refresh), and the display's
// pace (its refresh over the presents a frame, vulkan_context.cpp: refresh / 60, rounded) is the
// game's field rate within 0.3 % (as ps5_audio.cpp's displayFollowsGame). The field rate, 0 when
// the screen does not pace.
float ScreenPacedRate()
{
	if (!config::VSync || settings.input.fastForwardMode || ps5::variableRefresh)
		return 0.f;
	const float refresh = settings.display.refreshRate;
	const float field = spg_GetFieldRate();
	if (refresh <= 1.f || field <= 1.f)
		return 0.f;
	const float pace = refresh / (float)std::max(1L, std::lround(refresh / 60.f));
	return std::fabs(field / pace - 1.f) <= 0.003f ? field : 0.f;
}

uint64_t s_last_frame;	// when the last game frame was let through (main thread)
unsigned s_held;		// frames held back by the floor
} // namespace

void PS5Timing::GameFramePresented()
{
	const float rate = ScreenPacedRate();
	if (rate <= 0.f)
	{
		s_last_frame = 0;
		return;
	}
	// A hundredth short of a frame, so that a present that waited for its refresh never meets it.
	const uint64_t floor = (uint64_t)(1e9 / rate * 0.99);
	const uint64_t now = NowNs();
	if (s_last_frame != 0 && now > s_last_frame && now - s_last_frame < floor)
	{
		if (s_held++ == 0)
			ps5::diag::mark("pacing: a frame came %.1f ms after the one before while the screen paced (%.1f ms is the "
					"game's): the clock holds it back (said once)", (now - s_last_frame) / 1e6, 1e3 / rate);
		SleepUntil(s_last_frame + floor);
	}
	s_last_frame = NowNs();
}

void PS5Timing::MenuFrameWait()
{
	// Flycast's 16 ms, from now, as before; only the wake-up is on time (a 10 ms tick late would
	// make the next present miss its refresh: 40 frames a second in the menus, not 60).
	SleepUntil(NowNs() + 16000000);
}
