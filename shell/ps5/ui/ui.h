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
	const char *was;		// the label it had in the interface before (the console binds it by that)

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

// The colours, from the host's skin. The defaults are the skin Midnight's.
struct Palette
{
	ImU32 bg = IM_COL32(14, 19, 26, 255), bg2 = IM_COL32(23, 30, 40, 255);
	ImU32 text = IM_COL32(255, 255, 255, 255), sub = IM_COL32(201, 208, 219, 255), faint = IM_COL32(150, 160, 175, 255);
	ImU32 card = IM_COL32(30, 38, 49, 255), cardHi = IM_COL32(48, 60, 76, 255);
	ImU32 line = IM_COL32(255, 255, 255, 46), warm = IM_COL32(255, 179, 122, 255);
	ImU32 accent = IM_COL32(26, 159, 255, 255), onAccent = IM_COL32(255, 255, 255, 255);
};

struct Host
{
	Palette palette;
	bool circleConfirms = false;	// Circle chooses and Cross goes back: the hints' keys swap
	// Settings > Interface > UI Scaling, in percent. The screens of ui.cpp are drawn for the whole
	// screen; the kit (kit::begin) draws in the units of the screens that call it, which scale.
	int uiScale = 100;
	// The skin's own ground (its backdrop, which may move), drawn on the list given; none: the
	// palette's colours.
	std::function<void(ImDrawList *)> ground;
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
	// The Settings of one game (its own values over those for every game): its title. Then only
	// the settings available() says it can have are shown, and changed() means "its own".
	std::string forGame;
	std::function<bool(const Setting&)> available;

	// Settings that exist only while PSFlyCast runs (a network folder, a share found): each has an
	// area and a section of the table, and is shown after that section's own rows.
	std::vector<Setting> extras;

	// The settings' values. Info and Action rows show text(). The console's host also gives
	// kind (the setting as it is bound), value (its text) and step (a change by one): then the
	// table's choices and ranges are not used.
	std::function<Kind(const Setting&)> kind;
	std::function<std::string(const Setting&)> value;
	std::function<void(const Setting&, int)> step;
	std::function<std::string(const Setting&)> choices;	// "Native (480p) to 10x (4800p)", as bound
	std::function<int(const Setting&)> get;
	std::function<void(const Setting&, int)> set;
	std::function<std::string(const Setting&)> text;
	std::function<bool(const Setting&)> changed;
	std::function<void(const Setting&)> reset;
	std::function<void()> resetAll;				// a game's Settings: every value back to every game's
	std::function<void(const Setting&)> run;
	std::function<void(int, GameAct)> game;
	std::function<void(QuickAct)> quick;
	std::function<void()> typeQuery;			// open the console's keyboard for a search
	std::function<void(const std::vector<FoundRow>&)> useFound;
	std::function<void()> close;				// the interface is left (the game resumes)
};

// ---- the look, for every screen: the new screens draw with it, and so do the screens of
// bigpicture.cpp that keep their own logic (the quick menu's pages, the dialogs), so that every
// screen looks one. Units are those of a 1920 x 1080 screen; begin() sets the scale.
namespace kit
{
void begin(Host& host, ImDrawList *target = nullptr);
float width();								// the screen's width in units (it is 1080 high)
const Palette& palette();
ImDrawList *list();
ImU32 alpha(ImU32 c, float a);
ImU32 mix(ImU32 a, ImU32 b, float t);
// The ground: the skin's colours, lit softly by its accent.
void backdrop();
// A game's picture as the ground of a screen, large at the right, fading into the skin's ground.
void heroArt(const Game& g, float from = 560);
// A game's cover (its picture, or its colour with its title); focus lifts and rings it.
void cover(const Game& g, float x, float y, float w, float h, float r, bool focus);
// A soft round light, fading out to its edge.
void glow(float cx, float cy, float radius, ImU32 c);
void shadow(float x, float y, float w, float h, float r, ImU32 c, float spread);
// A card: frosted, edged with light; lifted and lit by the accent when it has the focus.
void surface(float x, float y, float w, float h, float r, bool focus = false);
// The focus, as a ring around a card or a cover.
void ring(float x, float y, float w, float h, float r);
void fill(float x, float y, float w, float h, ImU32 c, float r = 0);
float text(bool bold, float size, float x, float y, ImU32 c, const std::string& s, float wrap = 0);
ImVec2 measure(bool bold, float size, const std::string& s, float wrap = 0);
std::string fit(bool bold, float size, const std::string& s, float w);
// A section's name, small and in capitals.
void caption(float x, float y, const std::string& s, ImU32 c = 0);
// A rounded button with an icon; returns its width.
float button(float x, float y, const std::string& label, bool primary, bool focus, const char *icon = nullptr, float size = 23);
// A switch, its right edge at right.
void toggle(float right, float cy, bool on, bool focus = false);
// A line of a list: an icon, its name, a note under it, and its value at the right.
struct Line
{
	enum Kind { Plain, Toggle, Choice, Opens, Info };
	const char *icon = nullptr;
	std::string label, value, note;
	Kind kind = Plain;
	bool on = false;				// a switch's state
	bool enabled = true;
	bool marked = false;			// a dot before the name (changed from its default; its own value)
	ImU32 valueColour = 0;
};
void line(float x, float y, float w, float h, const Line& l, bool focus);
// The button hints on the bottom line, right-aligned at right (0: the screen's margin).
// Keys: "circle", "cross", "triangle", "square", "dpad", or a name ("L1", "OPTIONS").
void hints(const std::vector<std::pair<std::vector<const char *>, std::string>>& list, float right = 0, float cy = 1036);
// The top line of a screen: its name, where it is, the controllers and the clock.
void header(const std::string& title, const std::string& over = "", const std::string& note = "");
void clock(float right, float y);
}

// ---- screens whose logic is the host's (bigpicture.cpp): drawn from what it hands over

using HintList = std::vector<std::pair<std::vector<const char *>, std::string>>;

// A game's page: its picture, what it is, its buttons, and under them the page of the focused
// button (its save states, its own settings, its cheats, what to do with it).
struct PageView
{
	Game game;
	std::vector<std::pair<const char *, std::string>> actions;	// icon, label
	std::vector<bool> enabled;			// per action (none: all are)
	int action = 0;
	// A game on several discs: its discs, and the one the page is for (Square: the next).
	std::vector<std::string> discs;
	int disc = -1;
	std::string note;					// a line under the list, or under the buttons
	bool inList = false;				// the focus is in the list under the buttons
	std::string listTitle;
	std::vector<kit::Line> lines;
	int line = 0;
	std::string empty;					// what the list says when it has no line
	std::vector<std::pair<std::string, std::string>> facts;	// "Played", "6 h 20 min"
	HintList hints;
	float open = 1;						// 0..1, as it opens
};
void drawPage(Host& host, const PageView& page, ImDrawList *target = nullptr);

// The quick menu: a panel at the right over the paused game. Its pages (cheats, controls, ...)
// are drawn by the host in the rectangle this returns (x, y, w, h).
struct QuickView
{
	Game game;
	std::string status;					// "Paused"
	std::vector<kit::Line> lines;
	int focus = 0;
	bool arranging = false;
	bool page = false;					// a page is open: the panel is wider and has no list
	std::string pageTitle;
	float width = 680;
	// The state in the slot, beside the panel while a state line is focused.
	bool showState = false;
	ImTextureID statePicture = ImTextureID();
	std::string stateTitle, stateWhen;
	HintList hints;
	float open = 1;
};
ImVec4 drawQuick(Host& host, const QuickView& quick, ImDrawList *target = nullptr);

// Draws the interface over the whole of the current ImGui frame's background, and acts on input.
void frame(Host& host, const Input& in, ImDrawList *target = nullptr);
void show(Screen screen);
Screen current();
// For the pictures: put the focus somewhere and the level to see.
void focusForPicture(int home, int library, int area, int row, int level);
}
