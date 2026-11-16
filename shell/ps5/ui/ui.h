/*
	PSFlyCast - the interface, new (2026-11): Home, the Library, a game's page, the quick menu, the
	Settings (home, areas, search) and Look For My Files.

	Drawn with Dear ImGui's draw lists only, from what the host hands it each frame (Host): the
	games, the settings' values, what goes on. It asks the host to do things through Host's
	functions. Nothing here touches Flycast or the console, so the same code is drawn on a PC by
	shell/ps5/tests/ui (pictures of every screen) and on the console by bigpicture.cpp.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "imgui.h"

#include <functional>
#include <string>
#include <vector>

namespace pui
{
// ---- the settings (ui_settings.cpp; the table is shell/ps5/tools/settings-table.py's)

enum class Kind { Toggle, Choice, Range, Action, Info };
enum class Level { Essentials, More, Everything };

struct Area
{
	const char *name;
	const char *blurb;
};

struct Setting
{
	const char *id;
	int area;
	const char *section;
	Kind kind;
	Level level;
	const char *label;
	const char *desc;
	char source;			// F Flycast's words, f Flycast's words joined, P PSFlyCast's
	const char *flags;		// g a game can have its own, G a game's only, r at the next start, n at the next game
	std::vector<const char *> choices;
	int demo;				// a value for the pictures of shell/ps5/tests/ui
	int lo, hi, step;
	const char *unit;

	bool has(char flag) const;
	// What a value reads as: a choice's name, a number with its unit, On or Off.
	std::string valueText(int value) const;
};

const std::vector<Setting>& settings();
const std::vector<Area>& areas();
const Setting *find(const std::string& id);
// The sections of an area, in the table's order.
std::vector<std::string> sections(int area);
// Settings whose label, description, area or section have every word of query (any level).
std::vector<const Setting *> search(const std::string& query);

// ---- what the host gives

struct Game
{
	std::string title;
	std::string platform;	// Dreamcast, Naomi, Atomiswave
	std::string place;		// Console, USB, Share, Disc
	std::string line;		// what is known: "2 discs · played yesterday · 6 h 20 min"
	std::string about;		// its description
	std::string file;
	std::string lastPlayed;	// "yesterday"
	std::string played;		// "6 h 20 min"
	std::string stateWhen;	// the state it was closed with: "yesterday 22:41", or empty
	ImU32 colour = IM_COL32(40, 60, 90, 255);
	ImTextureID cover = ImTextureID();
	bool favourite = false;
	bool neverPlayed = false;
	int discs = 1;
	int states = 0;
	int achieved = 0, achievements = 0;
	int ownSettings = 0;
	int cheats = 0;
};

struct Card
{
	std::string title;
	std::string body;
};

struct FoundRow
{
	std::string kind;		// "BIOS Folders", "Game Folders" ...
	std::string what;		// "2 BIOS files"
	std::string folder;
	std::string note;		// why it is off or cannot be used
	bool on = true;
	bool blocked = false;
};

struct Input
{
	bool up = false, down = false, left = false, right = false;
	bool accept = false, back = false, triangle = false, square = false;
	bool l1 = false, r1 = false, l2 = false, r2 = false, l3 = false, r3 = false, options = false;
};

enum class Screen { Home, Library, GamePage, Quick, SettingsHome, SettingsArea, Search, Found };

// What a game can be asked to do.
enum class GameAct { Resume, StartOver, Play, Page };
// The quick menu's tiles and entries.
enum class QuickAct { Resume, SaveState, LoadState, Rewind, ChangeDisc, Quit, SwapCard, Cheats, Achievements, More };

struct Host
{
	ImFont *regular = nullptr;
	ImFont *bold = nullptr;
	std::vector<Game> games;
	std::vector<int> recent;		// games, the one played last first
	std::vector<Card> now;			// what is true now, for Home
	std::string clock;
	int pads = 1;
	int running = -1;				// the game being played, or -1
	std::string runningLine;		// "Playing for 42 min · 60 fps · 100 % speed"
	std::vector<std::string> pins;	// pinned settings, by id
	std::string query;				// the search, as typed
	std::string foundPlace;
	std::vector<FoundRow> found;
	std::string version;			// "v1.3.9 · build 105"

	// The settings' values. Info and Action rows show text().
	std::function<int(const Setting&)> get;
	std::function<void(const Setting&, int)> set;
	std::function<std::string(const Setting&)> text;
	std::function<bool(const Setting&)> changed;
	std::function<void(const Setting&)> reset;
	std::function<void(const Setting&)> run;
	std::function<void(int, GameAct)> game;
	std::function<void(QuickAct)> quick;
	std::function<void()> typeQuery;			// open the console's keyboard for a search
	std::function<void(const std::vector<FoundRow>&)> useFound;
	std::function<void()> close;				// the interface is left (the game resumes)
};

// Draws the interface over the whole of the current ImGui frame's background, and acts on input.
void frame(Host& host, const Input& in);
void show(Screen screen);
Screen current();
// For the pictures: put the focus somewhere and the level to see.
void focusForPicture(int home, int library, int area, int row, int level);
}
