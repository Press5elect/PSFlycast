/*
	PSFlyCast - how fast a game really runs.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A frame counter says how many pictures a second reach the screen, which
	can be all of them while the game itself runs slow. What says how fast the
	game runs is the emulated machine's own clock against the console's: 100%
	when a second of the Dreamcast takes a second. Beside it, for each second:
	the pictures the game drew, the presents that reached the display, and
	how many of those showed a picture made by frame generation
	(ps5_framegen.cpp).

	The numbers are in the frame-rate overlay ("Show frame rate"), and in
	flycast-boot.log: a line twenty seconds into a game, one every five
	minutes, and one when the game ends, with the slowest ten seconds.
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "hw/sh4/sh4_sched.h"
#include "emulator.h"
#include "oslib/oslib.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

namespace ps5::perf
{
namespace
{
using Clock = std::chrono::steady_clock;

struct Span
{
	double seconds = 0;		// of the console's clock
	double emulated = 0;	// of the Dreamcast's
	unsigned drawn = 0, shown = 0, made = 0;

	void add(const Span& other)
	{
		seconds += other.seconds;
		emulated += other.emulated;
		drawn += other.drawn;
		shown += other.shown;
		made += other.made;
	}
	double speed() const { return seconds > 0 ? 100.0 * emulated / seconds : 0; }
};

std::mutex mutex;
bool running;				// a window is open: the last present was a moment ago
Clock::time_point windowBegan, last;
u64 cyclesBegan;
Span window;				// this second, so far
Span second;				// the last whole second: what the overlay shows
Span ten;					// ten seconds, so far
Span game;					// since the game started
double slowest = 1e9;		// the slowest ten seconds' speed
double nextLine = 20;		// seconds of play at which the log gets its next line
bool listening;

void line(const char *when)
{
	if (game.seconds < 1)
		return;
	ps5::diag::mark("speed: %s, %.0f s of play: %.1f%% of full speed, %.1f pictures a second from the game, %.1f shown, "
			"%.1f of them made in between%s", when, game.seconds, game.speed(), game.drawn / game.seconds,
			game.shown / game.seconds, game.made / game.seconds,
			slowest < 1e8 ? (", slowest ten seconds " + std::to_string((int)(slowest + 0.5)) + "%").c_str() : "");
}

void ended()
{
	std::lock_guard<std::mutex> lock(mutex);
	line("the game ended");
	running = false;
	game = Span();
	ten = Span();
	second = Span();
	slowest = 1e9;
	nextLine = 20;
}
}

void present(bool fresh, bool made)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (!listening)
	{
		listening = true;
		EventManager::listen(Event::Terminate, [](Event, void *) { ended(); });
	}
	const Clock::time_point now = Clock::now();
	// After a pause (the quick menu, a load) the count starts again.
	if (!running || std::chrono::duration<double>(now - last).count() > 0.5)
	{
		running = true;
		windowBegan = now;
		cyclesBegan = sh4_sched_now64();
		window = Span();
	}
	last = now;
	window.shown++;
	window.drawn += fresh ? 1 : 0;
	window.made += made ? 1 : 0;
	const double span = std::chrono::duration<double>(now - windowBegan).count();
	if (span < 1.0)
		return;
	const u64 cycles = sh4_sched_now64();
	window.seconds = span;
	window.emulated = (double)(cycles - cyclesBegan) / SH4_MAIN_CLOCK;
	second = window;
	// Fast forward is not how fast the game runs.
	if (!settings.input.fastForwardMode)
	{
		game.add(window);
		ten.add(window);
		if (ten.seconds >= 10)
		{
			slowest = std::min(slowest, ten.speed());
			ten = Span();
		}
		if (game.seconds >= nextLine)
		{
			line("so far");
			nextLine = nextLine < 300 ? 300 : nextLine + 300;
		}
	}
	windowBegan = now;
	cyclesBegan = cycles;
	window = Span();
}

std::string text()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (second.seconds <= 0)
		return "";
	char line[96];
	const double per = 1.0 / second.seconds;
	if (second.made > 0)
		snprintf(line, sizeof(line), "%3.0f%%  game %.0f  shown %.0f (%.0f made)", second.speed(), second.drawn * per,
				second.shown * per, second.made * per);
	else
		snprintf(line, sizeof(line), "%3.0f%%  game %.0f  shown %.0f", second.speed(), second.drawn * per, second.shown * per);
	return line;
}

}
