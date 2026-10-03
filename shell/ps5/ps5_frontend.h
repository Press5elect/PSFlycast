/*
	PSFlyCast - the front end's own options and state.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include <string>
#include <vector>

namespace ps5
{
extern std::string rootDir;		// with a trailing '/'
extern bool elevated;			// the sandbox was left (USB drives, /data)
extern std::vector<std::string> usbDirs;	// game folders found on USB drives

// <root>/frontend.cfg: what the library looks like and what it may do.
struct Options
{
	int view = 0;			// 0 shelves, 1 grid, 2 list
	bool covers = true;		// download missing covers
	bool usb = false;		// leave the sandbox at start to read USB drives
	bool ramCache = true;	// read a network game whole into memory before it starts
	int source = 0;			// the library tab last open: 0 internal, 1 USB, 2 network
	bool hz120 = true;		// take the display's 119.88 Hz mode where it has one
	bool groupDiscs = true;	// the discs of one game are one entry in the library
	bool splash = true;		// the start-up animation, before the library
	// The console's pop-up notices (USB drives without elfldr, a crash). Off:
	// they are lines of flycast-boot.log only. No setting; frontend.cfg has it.
	bool notifications = false;
	// The interface's look (bigpicture.cpp): which skin, its accent colour
	// (0: the skin's own), what moves behind the screens (0: the skin's own)
	// and how much the interface itself moves (0 all of it, 1 less, 2 none).
	int skin = 0;
	int accent = 0;
	int backdrop = 0;
	int motion = 0;
	// Which of the port's own defaults were already written over the settings
	// an earlier build saved: 1 native depth interpolation on (build 18), 2 a
	// rumble pack in the controller's second slot (build 19).
	int defaults = 0;
};
Options& options();
void loadOptions(const std::string& dir);
void saveOptions();

// The root as the user reaches it over FTP.
std::string shownRoot();

// Looks for game folders on the USB drives again and sets the folders the
// library scans (a drive plugged in after the start). The scanner must not be
// running.
void rescanUsb();

namespace smb
{
// Reads <root>/network.cfg (and writes a template when there is none).
void loadConfig();
// The folders it names, as smb://server/share/folder.
const std::vector<std::string>& gameFolders();
bool isNetworkPath(const std::string& path);

// A network game is about to be loaded. Until endLoad(), and with the
// "load into memory" option on, each of its files is read whole into memory
// when it is opened and the share is not needed after that; with the option
// off the files are read from the share as the game asks for them. Also
// forgets that a share did not answer a moment ago: the user asked again.
void beginLoad();
void endLoad();
// The user gave up: the read in progress stops at its next piece.
void cancelLoad();
// Forgets that a share did not answer a moment ago (before a scan the user
// asked for).
void retryNow();

// What the share is doing, for the screen: "Waiting for the network share
// (12 s)" while a request has had no answer for a while (a NAS waking its
// disks), "Loading into memory ..." with the part done. Empty text when idle.
struct Status
{
	std::string text;
	float progress = -1.f;		// 0..1, or -1 when there is none to show
};
Status status();
// How many times a share could not be reached or stopped answering, since the
// start: a scan during which it grew did not see everything.
unsigned failures();
// What went wrong last (also a folder that could not be listed on a share
// that answers), since clearError().
std::string lastError();
void clearError();
// Whether a TCP port at an IPv4 address takes a connection within waitMs: 0
// yes, -1 no answer in that time, else the errno of the attempt.
int tcpAnswers(const std::string& ip, int port, int waitMs);
}

namespace covers
{
// Asks for <root>/covers/<base>.png to be downloaded if it is missing.
void request(const std::string& base);
// Bumped whenever a cover arrives, so cached look-ups are redone.
unsigned generation();
// "Downloading covers (3 left)" or empty.
std::string status();
}

namespace games
{
// The ID Flycast gave a game when it last ran (a disc's product number, an
// arcade board's game name): its own options and its cheats are kept under
// it. Empty for a game that has not been started yet (data/game-ids.txt).
std::string knownId(const std::string& path);
// The game about to start loads this state slot as it starts (0-based); -1
// for none. One start only.
void loadStateAtStart(int slot);
// The game about to start starts from its beginning: the state "Auto load
// state" would resume from is left alone. One start only.
void startFresh();
// Whether a Dreamcast BIOS file is in <root>/bios (else the built-in one runs).
bool biosFound();
void init();
}

namespace patches
{
// Game patches, apart from cheats (ps5_patches.cpp): a 16:9 picture
// ("widescreen") and 60 frames a second ("60fps"), for the games
// <root>/patches/patches.txt has them for.
void init();
// Whether there is a patch of this kind for a game file, and the entry it is
// ("Grandia II [NA]").
bool has(const std::string& fileName, const char *kind);
std::string label(const std::string& fileName, const char *kind);
// Where its switch is kept: this entry of the game's section of emu.cfg.
std::string optionKey(const char *kind);
}

namespace cheats
{
void init();
// For the library, where no game is running. The cheats a file has, by their
// descriptions, in the file's order.
std::vector<std::string> descriptions(const std::string& file);
// The file chosen for a game: a name, "-" for none by choice, or empty when
// nothing was chosen (the closest name is then used when the game starts).
std::string chosen(const std::string& gameId);
// The closest-named file for a game's file name, or empty.
std::string closest(const std::string& gameFileName);
// Chooses a file for a game (empty: none); which cheats are on starts again.
void choose(const std::string& gameId, const std::string& name);
// Which cheats are on, by description.
std::vector<std::string> enabled(const std::string& gameId);
void setEnabled(const std::string& gameId, const std::vector<std::string>& on);
// The .cht files in <root>/cheats, best match for the running game first.
std::vector<std::string> candidates(const std::string& gameFileName);
// Loads the running game's cheats if none are loaded: its own saved copy,
// else the best match. Called when a game starts.
void autoLoad();
// Loads the named file from <root>/cheats (empty: none) for the running game.
void select(const std::string& name);
// The file the running game's cheats came from.
std::string current();
// Saves which cheats are on, in <root>/data/cheats/<game id>.txt.
void saveStates();
}
}
