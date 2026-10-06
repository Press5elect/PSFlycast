/*
	PSFlyCast - rewind: going back a few seconds, or minutes, in the game.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	While a game runs, a snapshot of the whole machine is taken every few
	seconds of play and kept, compressed, in memory; the in-game menu lists
	them and goes back to one. ps5_rewind.cpp says how, and on which thread
	each part runs.

	What the front end does with it:

	  - init(), once, before the first game is loaded;
	  - setEnabled() with the option, whenever it changes (it starts off);
	  - with the emulator stopped in the in-game menu: points() for the rows,
	    restore() for the one chosen, then close the menu as "Load state"
	    does. Nothing is called per frame: the snapshots are taken from
	    Flycast's own vertical blank event.

	Everything here may be called from any thread except restore(), which is
	for the thread that stops and starts the emulator (the interface's).
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ps5::rewind
{
// Listens to the emulator's events and starts the thread that compresses.
// Once, at start-up, on the main thread, before a game is loaded.
void init();

// The option. Off (as it starts), nothing is taken and what was kept is
// freed. Turned on while a game runs, the first snapshot is one interval
// later.
void setEnabled(bool on);
// Seconds of play between two snapshots (5; 1 to 60).
void setInterval(int seconds);
// How far back the oldest snapshot may be, in seconds of play (180).
void setMaxAge(int seconds);
// The memory the kept snapshots may take, pictures included (384 MiB). The
// oldest go first. Not counted: the one uncompressed state being worked on
// (as large as the machine: about 28 MiB for a Dreamcast, up to 90 MiB for a
// NAOMI 2), freed when the game ends.
void setBudget(size_t bytes);
// A small picture with each snapshot (off). It is read back from the
// graphics card on the render thread, which waits for it: the boot log has
// what the first few cost. Leave it off unless that is small on the console.
void setPictures(bool on);

struct Point
{
	uint64_t id = 0;			// the same for as long as the point exists: a key for the picture's texture
	double secondsAgo = 0;		// of play: the time spent in menus does not count
	size_t bytes = 0;			// what it takes in memory, compressed, with its picture
	size_t rawBytes = 0;		// the size of the state itself
	// RGBA, pictureWidth * pictureHeight * 4 bytes, top row first; null when
	// there is none (setPictures() is off, or it has not arrived yet).
	std::shared_ptr<const std::vector<uint8_t>> picture;
	int pictureWidth = 0;
	int pictureHeight = 0;
};
// The points one can go back to, the newest first. A snapshot taken in the
// last tenth of a second or so may still be with the compressor, and is
// listed once it is done.
std::vector<Point> points();

// Whether there is anything to offer now: the option is on, this game may be
// rewound (not netplay, not online, not a multi-board or linked arcade game,
// not hardcore achievements), and there is at least one point.
bool available();

enum class Result
{
	Ok,					// the machine is as it was then; the points after it are gone
	Unavailable,		// off, or not for this game (see available())
	EmulatorRunning,	// the emulator must be stopped first
	NoSuchPoint,		// the index is past the list, or the point has since been dropped
	NoMemory,			// nothing was changed
	Corrupt,			// the snapshot did not decompress to what it was; nothing was changed
	LoadFailed,			// Flycast refused the state part-way: treat it as a failed "Load state"
};
// Goes back to a point: index is its place in the list points() returned
// last. On the thread that stops and starts the emulator, with the emulator
// stopped (emu.running() false: the in-game menu). The state is loaded as
// Flycast's own "Load state" loads one. Afterwards the caller closes the
// menu the way "Load state" does (gui_setState(GuiState::Closed)); the
// interface then starts the emulator again.
Result restore(size_t index);

// Forgets every point. Already done here when a game starts or ends, when a
// state is loaded by other means, when the disc is changed and when the game
// goes online: the front end needs it only for a cause of its own.
void clear();
}
