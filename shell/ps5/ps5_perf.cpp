/*
	PSFlyCast - how fast a game really runs.

	Copyright 2026 Press5elect and the PSFlyCast contributors
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
	minutes, and one when the game ends, with the slowest ten seconds, each
	followed by what a frame of the game asked of the graphics chip (render
	passes begun, pipeline barriers and image layout changes: the Vulkan
	renderer counts them, rend/vulkan/frame_counters.h). As in Crux2: a "slow:"
	line whenever the game stays under 95% of full speed for two seconds (once
	a stretch, at most 30 a game), and a "session:" line when it ends: the
	average speed, the time under 95% and the longest stretch of it.

	And what the game asks of the graphics chip (rend/pvr_facts.h): two
	"graphics:" lines at its first frame and again whenever one of those
	modes changes, so that a picture problem can be matched to them.
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "hw/sh4/sh4_sched.h"
#include "emulator.h"
#include "oslib/oslib.h"
#include "rend/vulkan/frame_counters.h"
#if __has_include("rend/pvr_facts.h")
#include "rend/pvr_facts.h"
#define PS5_PVR_FACTS 1
#endif

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

constexpr double Slow = 95.0;			// per cent of full speed: under it, a second is slow
constexpr double SlowLineAfter = 2.0;	// seconds under it in a row before a slow line
constexpr int SlowLinesAtMost = 30;		// a game

struct Span
{
	double seconds = 0;		// of the console's clock
	double emulated = 0;	// of the Dreamcast's
	unsigned drawn = 0, shown = 0, made = 0;
	u64 passes = 0, barriers = 0, layouts = 0;	// the renderer's (vkcount)

	void add(const Span& other)
	{
		seconds += other.seconds;
		emulated += other.emulated;
		drawn += other.drawn;
		shown += other.shown;
		made += other.made;
		passes += other.passes;
		barriers += other.barriers;
		layouts += other.layouts;
	}
	double speed() const { return seconds > 0 ? 100.0 * emulated / seconds : 0; }
	// What one of the game's pictures took (the presents that only show it again are not one).
	double perFrame(u64 count) const
	{
		const unsigned frames = drawn > 0 ? drawn : shown;
		return frames > 0 ? (double)count / frames : 0;
	}
};

std::mutex mutex;
bool running;				// a window is open: the last present was a moment ago
Clock::time_point windowBegan, last;
u64 cyclesBegan;
vkcount::Snapshot countsBegan;
Span window;				// this second, so far
Span second;				// the last whole second: what the overlay shows
Span ten;					// ten seconds, so far
Span game;					// since the game started
Span stretch;				// the seconds under 95% in a row, so far
double slowest = 1e9;		// the slowest ten seconds' speed
double nextLine = 20;		// seconds of play at which the log gets its next line
double slowSeconds;			// under 95% in all
double longestSlow;			// the longest stretch of it
bool stretchSaid;			// this stretch has had its slow line
int slowLines;
bool behindNow;				// the last whole second was slow
Clock::time_point gameBegan;
bool gameCounted;			// gameBegan is this game's
bool listening;

#ifdef PS5_PVR_FACTS
u32 factsSerial;			// the facts last logged; 0 for none this game

const char *packName(u32 mode)
{
	static const char *const names[] = { "0555", "565", "4444", "1555", "888", "0888", "8888", "?" };
	return names[std::min(mode, 7u)];
}

// The two lines, when the facts changed since they were last logged.
void factsLines()
{
	PvrFacts f;
	if (!pvrGameFacts(f) || f.serial == factsSerial)
		return;
	factsSerial = f.serial;
	static const char *const depths[] = { "0555 (15-bit)", "565 (16-bit)", "888 (24-bit)", "0888 (32-bit)" };
	std::string written;
	if (f.screenSeen)
		written += std::string("; the screen written as ") + packName(f.screenPackMode) + (f.screenDither ? " dithered" : "");
	if (f.rttSeen)
		written += std::string("; textures rendered as ") + packName(f.rttPackMode) + (f.rttDither ? " dithered" : "");
	ps5::diag::mark("graphics: %s: output %s, %s, %s%s%s, horizontal halving %s, vertical scale 0x%03X, flicker filter 0x%04X%s",
			f.gameId.empty() ? "the game" : f.gameId.c_str(), depths[f.fbDepth & 3], f.vgaClock ? "VGA (31 kHz)" : "TV (15 kHz)",
			f.pal ? "PAL" : f.ntsc ? "NTSC" : "neither PAL nor NTSC", f.interlace ? " interlaced" : "",
			f.lineDouble ? ", lines doubled" : "", f.hscale ? "on" : "off", (unsigned)f.vscale, (unsigned)(f.yCoeff & 0xFFFF),
			written.c_str());
	std::string sorted;
	for (u32 pass = 0; pass < f.passCount && pass < 32; pass++)
		sorted += (f.autosortMask & (1u << pass)) ? 'A' : 'P';
	ps5::diag::mark("graphics: %u render passes (%s: A sorts per pixel, P presorted%s)%s%s, culling below %.2f; "
			"%u polygons, %u textured (point %u, bilinear %u, trilinear %u, super-sampled %u, D-adjusted %u)%s%s",
			(unsigned)f.passCount, sorted.empty() ? "none" : sorted.c_str(), f.typeTwoRegions ? ", sorting set per pass" : "",
			f.cheapShadow ? (", cheap shadows at " + std::to_string(f.shadowScale) + "/256").c_str() : ", modifier volumes",
			f.fogTable ? ", fog table" : "", f.cullValue, (unsigned)f.polygons, (unsigned)f.texturedPolygons,
			(unsigned)f.filterPolygons[0], (unsigned)f.filterPolygons[1], (unsigned)(f.filterPolygons[2] + f.filterPolygons[3]),
			(unsigned)f.superSamplePolygons, (unsigned)f.dAdjustPolygons,
			f.yuvUsed ? (f.yuvForm422 ? ", YUV 422 movies" : ", YUV 420 movies") : "",
			f.yuvUsed && f.yuvMulti ? " (many textures)" : "");
}
#endif

void frameLine(const char *prefix, const Span& span)
{
	if (span.drawn == 0 && span.shown == 0)
		return;
	ps5::diag::mark("%s a frame: %.1f render passes, %.1f barriers, %.1f image layout changes", prefix,
			span.perFrame(span.passes), span.perFrame(span.barriers), span.perFrame(span.layouts));
}

void line(const char *when)
{
	if (game.seconds < 1)
		return;
	ps5::diag::mark("speed: %s, %.0f s of play: %.1f%% of full speed, %.1f pictures a second from the game, %.1f shown, "
			"%.1f of them made in between%s", when, game.seconds, game.speed(), game.drawn / game.seconds,
			game.shown / game.seconds, game.made / game.seconds,
			slowest < 1e8 ? (", slowest ten seconds " + std::to_string((int)(slowest + 0.5)) + "%").c_str() : "");
	frameLine("speed:", game);
}

void ended()
{
	std::lock_guard<std::mutex> lock(mutex);
	line("the game ended");
	if (game.seconds >= 1)
		ps5::diag::mark("session: %.1f minutes; at normal speed %.0f%% on average, under 95%% for %.0f s in all "
				"(the longest stretch %.0f s)", std::chrono::duration<double>(Clock::now() - gameBegan).count() / 60.0,
				game.speed(), slowSeconds, longestSlow);
	running = false;
#ifdef PS5_PVR_FACTS
	factsSerial = 0;
#endif
	game = Span();
	ten = Span();
	second = Span();
	stretch = Span();
	slowest = 1e9;
	nextLine = 20;
	slowSeconds = 0;
	longestSlow = 0;
	stretchSaid = false;
	slowLines = 0;
	behindNow = false;
	gameCounted = false;
}

// A whole second of play (not fast forward) was counted.
void slowOrNot(const Span& span)
{
	behindNow = span.speed() < Slow;
	if (!behindNow)
	{
		stretch = Span();
		stretchSaid = false;
		return;
	}
	stretch.add(span);
	slowSeconds += span.seconds;
	longestSlow = std::max(longestSlow, stretch.seconds);
	if (stretchSaid || stretch.seconds < SlowLineAfter || slowLines >= SlowLinesAtMost)
		return;
	stretchSaid = true;
	slowLines++;
	ps5::diag::mark("slow: %.0f%% of full speed for %.0f s, %.1f pictures a second from the game, %.1f shown%s",
			stretch.speed(), stretch.seconds, stretch.drawn / stretch.seconds, stretch.shown / stretch.seconds,
			slowLines == SlowLinesAtMost ? " (the last slow line of this game)" : "");
	frameLine("slow:", stretch);
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
	if (!gameCounted)
	{
		gameCounted = true;
		gameBegan = now;
	}
	// After a pause (the quick menu, a load) the count starts again.
	if (!running || std::chrono::duration<double>(now - last).count() > 0.5)
	{
		running = true;
		windowBegan = now;
		cyclesBegan = sh4_sched_now64();
		countsBegan = vkcount::now();
		window = Span();
		behindNow = false;
	}
	last = now;
#ifdef PS5_PVR_FACTS
	// The game's first frame, and then once a second whether the modes changed.
	if (fresh && (factsSerial == 0 || window.shown == 0))
		factsLines();
#endif
	window.shown++;
	window.drawn += fresh ? 1 : 0;
	window.made += made ? 1 : 0;
	const double span = std::chrono::duration<double>(now - windowBegan).count();
	if (span < 1.0)
		return;
	const u64 cycles = sh4_sched_now64();
	const vkcount::Snapshot counts = vkcount::now();
	window.seconds = span;
	window.emulated = (double)(cycles - cyclesBegan) / SH4_MAIN_CLOCK;
	window.passes = counts.passes - countsBegan.passes;
	window.barriers = counts.barriers - countsBegan.barriers;
	window.layouts = counts.layouts - countsBegan.layouts;
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
		slowOrNot(window);
		if (game.seconds >= nextLine)
		{
			line("so far");
			nextLine = nextLine < 300 ? 300 : nextLine + 300;
		}
	}
	else
		behindNow = false;
	windowBegan = now;
	cyclesBegan = cycles;
	countsBegan = counts;
	window = Span();
}

bool behind()
{
	std::lock_guard<std::mutex> lock(mutex);
	return running && behindNow && !settings.input.fastForwardMode;
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
