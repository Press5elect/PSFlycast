/*
	PSFlyCast - where the player's files are: game folders anywhere, and the folders PSFlyCast reads
	its BIOS, save states, covers, texture packs, cheats and game videos from (ps5_folders.cpp).
	Nothing here moves or copies a file: a folder chosen is only the place to look in, and to write
	to for what PSFlyCast writes. Without a choice every kind keeps the folder it had before.

	The engine is plain C++ (POSIX calls and a function to list a share), so that PC tests run it
	(shell/ps5/tests/folders). Flycast's own settings do the work for BIOS, states, textures and
	cheats (config::BiosPath, SavestatePath, TexturePath, CheatPath): ps5_main.cpp's setFolders
	hands them the result of resolve().

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
	Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect" (the
	folder browser and the writable test follow Crux2's, Crux2 by Press5elect).
*/
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ps5::folders
{
// What a folder holds. Games are a list of folders that join the library where they are; the
// others are the folders PSFlyCast keeps a kind of file in.
enum Kind { Games, Bios, States, Covers, Textures, Cheats, Videos, KindCount };

struct KindInfo
{
	const char *key;		// in folders.cfg
	const char *label;		// Flycast's words where it has the setting
	const char *desc;
	const char *fallback;	// the default, under the root ("" for games: the games/ folder)
	bool written;			// PSFlyCast writes there: it must be writable, and never a share
};
const KindInfo& info(Kind kind);

// ---- looking into a folder

struct Entry
{
	std::string name;
	bool folder = false;
};
// How a folder is listed. The default is the file system's; the title installs one that also
// lists smb:// folders. False when the folder cannot be read.
using Lister = std::function<bool(const std::string& folder, std::vector<Entry>& out)>;
void setLister(Lister lister);
bool list(const std::string& folder, std::vector<Entry>& out);
bool isShare(const std::string& path);

// The places a folder is chosen from: each USB drive, each share, the console's storage.
struct Place
{
	std::string label;
	std::string path;
};
struct PlaceSources
{
	std::vector<std::string> usb;		// /mnt/usb0 ... that are there
	std::vector<std::string> shares;	// smb://server/share, the shares that are connected
	std::string console;				// where the console's own storage starts ("" for none)
};
std::vector<Place> places(const PlaceSources& sources);
// The folders in a folder, by name, hidden ones left out, at most 1000.
std::vector<std::string> subfolders(const std::string& folder);
// The folder above: "" when `folder` is a place itself (the list of places is above it).
std::string parentOf(const std::string& folder, const std::vector<Place>& places);
std::string join(const std::string& folder, const std::string& name);
// A folder PSFlyCast can write in: a probe file is made and taken away. Never on a share.
bool canWriteIn(const std::string& folder);

// ---- what is in a place (Look For My Files)

struct Found
{
	Kind kind;
	std::string folder;
	int count = 0;		// games, BIOS files, states, pictures, cheat files, videos, texture folders
};
struct Scan
{
	std::vector<Found> found;	// the best first within each kind; at most 5 of a kind
	bool partial = false;		// it stopped at its limits or on request: there may be more
	int visited = 0;
};
// Looks through `root` and the folders in it, `depth` levels down, at most `maxFolders` of them.
// `stop` is asked between folders. Reads only.
Scan lookFor(const std::string& root, int depth = 4, int maxFolders = 800, const std::function<bool()>& stop = {});

// ---- the choice

struct Settings
{
	std::vector<std::string> games;		// folders added to the library
	std::vector<std::string> kept[KindCount];	// per kind, the first is where a written kind is written (games unused)
};
Settings parse(const std::string& text);
std::string serialise(const Settings& settings);
// <root>/folders.cfg, read at start and written whenever it changes (one file, beside it first and
// then renamed in place).
void load(const std::string& root);
bool save(std::string& why);
Settings& settings();
std::string rootFolder();

// The folders to use for a kind: the ones chosen that are there, then the default. `missing` has
// the chosen ones that are not (a drive pulled out). `askFirst` is true when a written kind's
// first folder is not there: the game must not start on the default folder unasked, or its saves
// would look gone.
struct Resolved
{
	std::vector<std::string> use;
	std::vector<std::string> missing;
	bool askFirst = false;
};
Resolved resolve(const Settings& settings, Kind kind, const std::string& defaultFolder,
		const std::function<bool(const std::string&)>& exists);
// The same from the settings in force, the root's default and the file system.
Resolved resolveNow(Kind kind);
// The default folder of a kind under the root, without a trailing slash.
std::string defaultFolder(Kind kind);
// Where PSFlyCast writes a kind now: the first folder in use. Kept, so it is cheap to ask for
// every frame and safe to ask for from any thread; refresh() works it out again (a drive was
// plugged in or pulled out). load(), add(), remove() and reset() do it themselves.
std::string writeFolder(Kind kind);
void refresh();
// The first folder in use that holds `name` (a local folder), or "".
std::string findIn(Kind kind, const std::string& name);
// The covers folder with a slash at its end, as PSFlyCast joins file names to it.
std::string coversPrefix();
// A save state's file ("Game_2.state"): in the first folder in use that has it, else where
// states are written.
std::string stateFile(const std::string& name);

// Adds or takes off a folder of a kind (games: the game folders). False with the reason when the
// folder cannot be used (not writable for a written kind, a share for one, already there).
bool add(Kind kind, const std::string& folder, std::string& why);
void remove(Kind kind, const std::string& folder);
// Back to the default for a kind.
void reset(Kind kind);
}
