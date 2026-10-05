/*
	PSFlyCast - the front end's own options and state.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "cfg/option.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ps5
{
extern std::string rootDir;		// with a trailing '/'
extern bool elevated;			// the sandbox was left (USB drives, /data)
// Frame pacing, "Sync to display": the display's refresh paces the game, and
// the sound follows it by resampling a little faster or slower (ps5_audio.cpp).
// Off, the sound keeps its exact rate, and now and then a frame is shown twice
// or the game waits for the sound. A setting, and a game can have its own.
extern config::Option<bool> SyncToDisplay;

// "Upscaling": how the game's picture is stretched to the screen when it is
// rendered smaller: the emulator's own filter, or FSR 1 at one of three
// sharpnesses (ps5_fsr.cpp). A setting, and a game can have its own.
enum { UpscalingOff, UpscalingFsr, UpscalingFsrSharp, UpscalingFsrSoft };
extern config::Option<int> Upscaling;

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
	bool splashSound = true;	// and its sound
	bool updateCheck = true;	// a release's build asks GitHub for a newer one as it starts
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
	// rumble pack in the controller's second slot (build 19), 3 UPnP off
	// (build 40).
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

namespace net
{
// The console's HTTP client (ps5_covers.cpp), one request at a time. A GET
// into memory (up to 8 MB): the HTTP status, or -1 when the request could
// not be made (lastError() then has the system's code, 0 for "not connected").
int get(const std::string& url, std::vector<uint8_t>& out, unsigned seconds = 20);
// A GET into a file, for something large. progress hears how much has
// arrived and stops the download by returning false. The HTTP status, -1 as
// above, or -2 when progress stopped it; the file is there only on success.
int download(const std::string& url, const std::string& file, const std::function<bool(uint64_t)>& progress);
int lastError();
// This console's address on its network ("192.168.1.23"), or empty.
std::string localAddress();
}

namespace update
{
// Updating the title from the project's releases on GitHub (ps5_update.cpp).
enum class Phase
{
	Idle,			// nothing asked yet
	Checking,		// asking GitHub
	UpToDate,		// the newest release is not newer than this build
	Available,		// it is
	Downloading, Verifying, Unpacking, Swapping,	// install(), in this order
	Installed,		// in place: it starts the next time the title is opened
	Failed,			// error says why
};
struct Status
{
	Phase phase = Phase::Idle;
	std::string latest;					// the newest release's tag, "v1.0.2"
	std::vector<std::string> notes;		// what it changed, from its notes: a few short lines
	uint64_t size = 0, done = 0;		// its ZIP: the size, and how much has arrived
	std::string error;
	bool restored = true;				// Failed: the title's files are as they were
};
Status status();
// Reads which build this is, deletes what the last update left once the new
// version has started, and, in a release's build with the option on, asks
// GitHub in the background. Called once, at the start.
void init();
// "v1.0.1, build 39", or "Test build 40, after v1.0.1"; whether it is a
// release's build; and the release it is compared with, "v1.0.1".
std::string thisBuild();
bool isRelease();
std::string thisVersion();
// Each returns at once and works in the background; status() follows it.
void check();
void install();		// the newest release, whether or not it is newer
void cancel();		// a download or an unpacking; not the last step
// The newest release is not offered at start-up again.
void skip();
// True once, when the check at start-up found a newer release that was not skipped.
bool offerAtStart();
}

namespace sound
{
// The start-up sound, <root>/sounds/startup.wav (16-bit PCM), through a port
// of its own that is closed when the sound ends: from a moment of it, at a
// loudness (1 as recorded). Returns at once; nothing happens without the file.
void playStartup(float fromSeconds, float gain);
// Ends it within a few hundredths of a second. With wait, returns once its
// port is closed (the emulator's own sound opens one next).
void stop(bool wait = false);
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

namespace drawdist
{
// "Object draw distance" (ps5_drawdist.cpp): the Sonic Adventure games' objects
// from further away, for the games that have ps5.drawdist over 100 in their
// section of emu.cfg.
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
