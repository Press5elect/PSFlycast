/*
	PSFlyCast - the interface's screens (ui.h). Everything is drawn on the background draw list in
	units of a 1920 x 1080 screen; S turns units into pixels.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pui
{
namespace
{
// ---------------------------------------------------------------- the kit

namespace col
{
const ImU32 bg = IM_COL32(11, 13, 18, 255);
const ImU32 text = IM_COL32(255, 255, 255, 255);
const ImU32 sub = IM_COL32(201, 208, 219, 255);
const ImU32 faint = IM_COL32(170, 178, 192, 255);
const ImU32 card = IM_COL32(255, 255, 255, 18);
const ImU32 line = IM_COL32(255, 255, 255, 46);
const ImU32 warm = IM_COL32(255, 179, 122, 255);
const ImU32 on = IM_COL32(77, 163, 255, 255);
const ImU32 off = IM_COL32(255, 255, 255, 52);
const ImU32 dark = IM_COL32(11, 13, 18, 255);
}

ImDrawList *dl;
Host *H;
float S = 1.f, W = 1920.f, HH = 1080.f;

ImVec2 V(float x, float y) { return ImVec2(x * S, y * S); }
ImU32 alpha(ImU32 c, float a)
{
	return (c & 0x00ffffffu) | ((ImU32)std::clamp(a * (float)((c >> 24) & 0xff), 0.f, 255.f) << 24);
}

void rect(float x, float y, float w, float h, ImU32 c, float r = 0)
{
	dl->AddRectFilled(V(x, y), V(x + w, y + h), c, r * S);
}

void stroke(float x, float y, float w, float h, ImU32 c, float r, float t)
{
	dl->AddRect(V(x, y), V(x + w, y + h), c, r * S, 0, t * S);
}

void gradient(float x, float y, float w, float h, ImU32 top, ImU32 bottom)
{
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h), top, top, bottom, bottom);
}

void hgradient(float x, float y, float w, float h, ImU32 left, ImU32 right)
{
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h), left, right, right, left);
}

void disc(float cx, float cy, float r, ImU32 c)
{
	dl->AddCircleFilled(V(cx, cy), r * S, c, 96);
}

ImFont *reg() { return H->regular; }
ImFont *bold() { return H->bold != nullptr ? H->bold : H->regular; }

ImVec2 measure(ImFont *f, float size, const std::string& s, float wrap = 0)
{
	const ImVec2 v = f->CalcTextSizeA(size * S, 1e9f, wrap * S, s.c_str());
	return ImVec2(v.x / S, v.y / S);
}

// Text at (x, y); returns its height.
float text(ImFont *f, float size, float x, float y, ImU32 c, const std::string& s, float wrap = 0)
{
	dl->AddText(f, size * S, V(x, y), c, s.c_str(), nullptr, wrap * S);
	return measure(f, size, s, wrap).y;
}

std::string fit(ImFont *f, float size, const std::string& s, float w)
{
	if (measure(f, size, s).x <= w)
		return s;
	std::string cut = s;
	while (!cut.empty() && measure(f, size, cut + "...").x > w)
		cut.pop_back();
	return cut + "...";
}

// A controller button, as a small outlined key; returns its width.
float key(float x, float cy, const char *k, ImU32 c = col::sub)
{
	const float h = 30;
	const std::string s = k;
	const bool shape = s == "circle" || s == "cross" || s == "triangle" || s == "square";
	const float w = shape ? h : measure(bold(), 15, s).x + 18;
	stroke(x, cy - h / 2, w, h, c, h / 2, 2);
	const float mx = x + w / 2;
	if (s == "circle")
		dl->AddCircle(V(mx, cy), 7 * S, col::text, 24, 2 * S);
	else if (s == "cross")
	{
		dl->AddLine(V(mx - 6, cy - 6), V(mx + 6, cy + 6), col::text, 2 * S);
		dl->AddLine(V(mx - 6, cy + 6), V(mx + 6, cy - 6), col::text, 2 * S);
	}
	else if (s == "triangle")
		dl->AddTriangle(V(mx, cy - 7), V(mx + 7.5f, cy + 6), V(mx - 7.5f, cy + 6), col::text, 2 * S);
	else if (s == "square")
		dl->AddRect(V(mx - 6, cy - 6), V(mx + 6, cy + 6), col::text, 0, 0, 2 * S);
	else
		text(bold(), 15, x + 9, cy - measure(bold(), 15, s).y / 2, col::text, s);
	return w;
}

// The button hints, right-aligned on the bottom line: Circle first, Cross last.
void hints(const std::vector<std::pair<std::vector<const char *>, std::string>>& list, float right = 1836, ImU32 c = col::sub)
{
	float total = 0;
	std::vector<float> widths;
	for (const auto& [keys, label] : list)
	{
		float w = measure(reg(), 19, label).x + 10;
		for (const char *k : keys)
		{
			const std::string s = k;
			w += (s == "circle" || s == "cross" || s == "triangle" || s == "square" ? 30 : measure(bold(), 15, s).x + 18) + 6;
		}
		widths.push_back(w);
		total += w + 34;
	}
	float x = right - total + 34;
	const float cy = 1046;
	for (size_t i = 0; i < list.size(); i++)
	{
		float at = x;
		for (const char *k : list[i].first)
			at += key(at, cy, k, c) + 6;
		text(reg(), 19, at + 4, cy - 12, c, list[i].second);
		x += widths[i] + 34;
	}
}

// A switch, its right edge at x.
void switchAt(float right, float cy, bool on)
{
	const float w = 66, h = 38;
	rect(right - w, cy - h / 2, w, h, on ? col::on : col::off, h / 2);
	disc(on ? right - h / 2 : right - w + h / 2, cy, h / 2 - 5, col::text);
}

// A game's cover: its picture, or its colour with its title.
void cover(const Game& g, float x, float y, float w, float h, float r, bool focus)
{
	if (focus)
	{
		const float grow = 6;
		x -= grow; y -= grow; w += 2 * grow; h += 2 * grow;
	}
	if (g.cover != ImTextureID())
		dl->AddImageRounded(g.cover, V(x, y), V(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r * S);
	else
	{
		rect(x, y, w, h, g.colour, r);
		dl->PushClipRect(V(x, y), V(x + w, y + h), true);
		disc(x + w * 0.82f, y + h * 0.18f, w * 0.42f, alpha(IM_COL32(255, 255, 255, 255), 0.10f));
		dl->PopClipRect();
		gradient(x, y + h * 0.45f, w, h * 0.55f, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 110));
		const float size = std::clamp(w * 0.085f, 16.f, 34.f);
		text(bold(), size, x + size * 0.8f, y + h - size * 0.8f - measure(bold(), size, g.title, w - size * 1.6f).y, col::text, g.title,
				w - size * 1.6f);
	}
	if (focus)
	{
		stroke(x - 6, y - 6, w + 12, h + 12, col::text, r + 6, 5);
	}
}

// The top bar: PSFlyCast, the places (Home, Library, Disc), the controllers and the clock.
void topBar(int place, bool disc = true)
{
	text(bold(), 25, 84, 42, col::text, "PSFlyCast");
	float x = 300;
	x += key(x, 58, "L1") + 12;
	const char *names[] = { "Home", "Library", "Disc" };
	for (int i = 0; i < (disc ? 3 : 2); i++)
	{
		const float w = measure(bold(), 22, names[i]).x + 54;
		if (i == place)
			rect(x, 34, w, 48, col::text, 24);
		text(bold(), 22, x + 27, 45, i == place ? col::dark : col::sub, names[i]);
		x += w + 6;
	}
	key(x + 6, 58, "R1");
	const std::string pads = std::to_string(H->pads) + (H->pads == 1 ? " controller" : " controllers");
	const float cw = measure(bold(), 25, H->clock).x;
	text(bold(), 25, 1836 - cw, 44, col::text, H->clock);
	text(reg(), 21, 1836 - cw - 32 - measure(reg(), 21, pads).x, 48, col::sub, pads);
}

// A rounded button; returns its width.
float button(float x, float y, const std::string& label, bool primary, bool focus, float size = 25)
{
	const float h = 64, w = measure(bold(), size, label).x + 72;
	if (focus)
		rect(x - 6, y - 6, w + 12, h + 12, alpha(col::text, 0.28f), (h + 12) / 2);
	rect(x, y, w, h, primary || focus ? col::text : alpha(col::text, 0.14f), h / 2);
	text(bold(), size, x + 36, y + (h - measure(bold(), size, label).y) / 2, primary || focus ? col::dark : col::text, label);
	return w;
}

// ---------------------------------------------------------------- state

struct State
{
	Screen screen = Screen::Home;
	Screen back = Screen::Home;			// where Circle goes from a game's page or the settings
	int home = 0;						// 0 Resume, 1 Start Over, 2 Game Page, 3.. played lately
	int lib = 0, chip = 0;
	int game = 0;						// the game whose page is open
	int page = 0;						// 0 Resume, 1 Start Over, 2.. the cards
	int quick = 0;
	int setHome = 2;					// -1 search, 0 pinned, 1 changed, 2.. the areas
	int level = 0;						// 0 Essentials, 1 More, 2 Everything
	int area = 0;
	int virt = 0;						// 0 an area, 1 the pinned, 2 the changed
	int row = 0;
	float scroll = 0;
	int result = 0;
	int found = 0;
} st;

bool shown(const Setting& s, int level) { return (int)s.level <= level && !s.has('G'); }

const char *levelNames[] = { "Essentials", "More", "Everything" };

// The level switch at the top right of the settings.
void levelSwitch(float right, float y)
{
	float w = 0;
	for (const char *n : levelNames)
		w += measure(bold(), 19, n).x + 42;
	float x = right - w - 8;
	rect(x - 5, y - 5, w + 10, 54, col::card, 27);
	for (int i = 0; i < 3; i++)
	{
		const float bw = measure(bold(), 19, levelNames[i]).x + 42;
		if (i == st.level)
			rect(x, y, bw, 44, col::text, 22);
		text(bold(), 19, x + 21, y + 10, i == st.level ? col::dark : col::sub, levelNames[i]);
		x += bw;
	}
}

std::string valueOf(const Setting& s)
{
	if (s.kind == Kind::Info || s.kind == Kind::Action)
		return H->text ? H->text(s) : "";
	return s.valueText(H->get ? H->get(s) : s.demo);
}

bool pinned(const Setting& s)
{
	return std::find(H->pins.begin(), H->pins.end(), s.id) != H->pins.end();
}

void change(const Setting& s, int dir)
{
	if (!H->set || !H->get)
		return;
	const int v = H->get(s);
	if (s.kind == Kind::Toggle)
		H->set(s, v ? 0 : 1);
	else if (s.kind == Kind::Choice && !s.choices.empty())
		H->set(s, std::clamp(v + dir, 0, (int)s.choices.size() - 1));
	else if (s.kind == Kind::Range)
		H->set(s, std::clamp(v + dir * s.step, s.lo, s.hi));
}

// ---------------------------------------------------------------- the settings' list and detail

struct Item
{
	const Setting *s = nullptr;		// null: a section's heading
	std::string heading;
};

std::vector<Item> itemsFor()
{
	std::vector<Item> items;
	if (st.virt == 1 || st.virt == 2)
	{
		for (const Setting& s : settings())
			if (!s.has('G') && (st.virt == 1 ? pinned(s) : (H->changed && H->changed(s))))
				items.push_back({ &s, "" });
		return items;
	}
	std::string last;
	for (const Setting& s : settings())
	{
		if (s.area != st.area || !shown(s, st.level))
			continue;
		if (s.section != last)
		{
			items.push_back({ nullptr, s.section });
			last = s.section;
		}
		items.push_back({ &s, "" });
	}
	return items;
}

// The pane that says everything about the focused setting.
void detail(const Setting& s, float x, float y, float w)
{
	float h = 0;
	const float pad = 33;
	float cy = y + pad;
	// measured first, drawn after the panel
	const float descH = measure(reg(), 21, s.desc, w - 2 * pad).y;
	h = pad + 40 + 18 + descH + 26 + 4 * 34 + 30 + pad;
	rect(x, y, w, h, col::card, 27);
	cy += text(bold(), 30, x + pad, cy, col::text, s.label, w - 2 * pad) + 18;
	cy += text(reg(), 21, x + pad, cy, IM_COL32(227, 232, 239, 255), s.desc, w - 2 * pad) + 26;
	const auto line = [&](const char *k, const std::string& v) {
		text(reg(), 19, x + pad, cy, col::faint, k);
		text(reg(), 19, x + w - pad - measure(reg(), 19, v).x, cy, col::text, v);
		cy += 34;
	};
	std::string choices;
	if (s.kind == Kind::Choice && !s.choices.empty())
	{
		for (const char *n : s.choices)
			choices += (choices.empty() ? "" : ", ") + std::string(n);
		if (measure(reg(), 19, choices).x > w - 2 * pad - 140)
			choices = std::to_string(s.choices.size()) + ", " + s.choices.front() + " to " + s.choices.back();
	}
	else if (s.kind == Kind::Range)
		choices = std::to_string(s.lo) + s.unit + " to " + std::to_string(s.hi) + s.unit;
	else if (s.kind == Kind::Toggle)
		choices = "On or Off";
	else
		choices = s.kind == Kind::Action ? "Opens a screen" : "Shown only";
	line("Choices", choices);
	line("Applies to", s.has('G') ? "One game" : s.has('g') ? "Every game, or one game" : "Every game");
	line("Takes effect", s.has('r') ? "When PSFlyCast starts again" : s.has('n') ? "At the next game" : "At once");
	line("Level", levelNames[(int)s.level]);
	if (s.source == 'F' || s.source == 'f')
		text(reg(), 17, x + pad, cy + 4, col::faint, "Description from Flycast");
}

// A list of settings with their values; returns the setting in focus.
const Setting *settingRows(const std::vector<Item>& items, int& focus, float x, float y, float w, float bottom, bool focused)
{
	std::vector<int> stops;
	for (int i = 0; i < (int)items.size(); i++)
		if (items[(size_t)i].s != nullptr)
			stops.push_back(i);
	if (stops.empty())
	{
		text(reg(), 22, x + 24, y + 12, col::faint, st.virt == 1 ? "Nothing is pinned. Press Square on a setting to pin it here."
				: st.virt == 2 ? "Every setting is at its default." : "Nothing here at this level: L1 / R1 shows more.", w - 48);
		return nullptr;
	}
	focus = std::clamp(focus, 0, (int)stops.size() - 1);
	const int at = stops[(size_t)focus];
	const float rowH = 69, headH = 54;
	float focusY = 0;
	for (int i = 0; i < at; i++)
		focusY += items[(size_t)i].s ? rowH + 6 : headH;
	const float view = bottom - y;
	const float target = std::clamp(focusY - view * 0.35f, 0.f, 1e9f);
	// The list starts on a whole row or heading, never on a cut one.
	float top = 0, snapped = 0;
	for (int i = 0; i < (int)items.size() && top <= target; i++)
	{
		snapped = top;
		top += items[(size_t)i].s ? rowH + 6 : headH;
	}
	st.scroll = snapped;
	dl->PushClipRect(V(x - 8, y - 4), V(x + w + 8, bottom), true);
	float cy = y - st.scroll;
	for (int i = 0; i < (int)items.size(); i++)
	{
		const Item& it = items[(size_t)i];
		if (!it.s)
		{
			std::string up = it.heading;
			for (char& c : up)
				c = (char)toupper((unsigned char)c);
			text(bold(), 17, x + 24, cy + 20, col::faint, up);
			cy += headH;
			continue;
		}
		const Setting& s = *it.s;
		const bool on = i == at && focused;
		if (cy > bottom || cy + rowH < y - rowH)
		{
			cy += rowH + 6;
			continue;
		}
		if (on)
			rect(x - 4, cy - 4, w + 8, rowH + 8, col::text, 22);
		rect(x, cy, w, rowH, on ? IM_COL32(40, 44, 54, 255) : alpha(col::text, 0.05f), 18);
		float lx = x + 24;
		if (H->changed && H->changed(s))
		{
			disc(lx + 5, cy + rowH / 2, 5.5f, col::warm);
			lx += 22;
		}
		text(bold(), 22, lx, cy + rowH / 2 - 14, col::text, fit(bold(), 22, s.label, w * 0.55f));
		const float right = x + w - 24;
		const float cyv = cy + rowH / 2;
		if (pinned(s))
			text(reg(), 16, lx + measure(bold(), 22, s.label).x + 12, cy + rowH / 2 - 10, col::warm, "PINNED");
		if (s.kind == Kind::Toggle)
			switchAt(right, cyv, H->get ? H->get(s) != 0 : s.demo != 0);
		else
		{
			std::string v = valueOf(s);
			if (s.kind == Kind::Choice && on)
				v = "<  " + v + "  >";
			if (s.kind == Kind::Action)
				v = v.empty() ? ">" : v + "   >";
			const std::string shownV = fit(reg(), 21, v, w * 0.4f);
			text(on ? bold() : reg(), 21, right - measure(on ? bold() : reg(), 21, shownV).x, cyv - 13, on ? col::text : col::sub, shownV);
		}
		cy += rowH + 6;
	}
	dl->PopClipRect();
	return items[(size_t)at].s;
}

// ---------------------------------------------------------------- Home

void homeScreen(const Input& in)
{
	Host& h = *H;
	const int hero = h.recent.empty() ? -1 : h.recent[0];
	const int lately = std::min<int>(7, std::max(0, (int)h.recent.size() - 1));
	const int stops = 3 + lately;
	if (in.left && st.home > 0 && st.home != 3) st.home--;
	if (in.right && st.home < stops - 1 && st.home != 2) st.home++;
	if (in.down && st.home < 3 && lately > 0) st.home = 3;
	if (in.up && st.home >= 3) st.home = 0;
	st.home = std::clamp(st.home, 0, stops - 1);
	if (in.accept && hero >= 0 && h.game)
	{
		if (st.home == 0) h.game(hero, GameAct::Resume);
		else if (st.home == 1) h.game(hero, GameAct::StartOver);
		else if (st.home == 2) { st.game = hero; st.page = 0; st.back = Screen::Home; st.screen = Screen::GamePage; }
		else h.game(h.recent[(size_t)(st.home - 2)], GameAct::Play);
	}
	if (in.triangle && st.home >= 3)
	{
		st.game = h.recent[(size_t)(st.home - 2)];
		st.page = 0;
		st.back = Screen::Home;
		st.screen = Screen::GamePage;
	}
	if (in.r1) st.screen = Screen::Library;
	if (in.options) { st.back = Screen::Home; st.screen = Screen::SettingsHome; }

	rect(0, 0, W, HH, col::bg);
	if (hero >= 0)
	{
		const Game& g = h.games[(size_t)hero];
		// The game's picture fills the screen and fades into the ground at the left and the bottom.
		if (g.cover != ImTextureID())
			dl->AddImage(g.cover, V(560, -120), V(1920, 1240), ImVec2(0, 0.1f), ImVec2(1, 0.9f), IM_COL32(255, 255, 255, 200));
		else
		{
			rect(0, 0, W, HH, g.colour);
			disc(1700, 160, 600, alpha(IM_COL32(255, 255, 255, 255), 0.08f));
			disc(1440, 360, 225, IM_COL32(240, 138, 60, 255));
		}
		hgradient(0, 0, 720, HH, col::bg, alpha(col::bg, 0.86f));
		hgradient(720, 0, W - 720, HH, alpha(col::bg, 0.86f), alpha(col::bg, 0.0f));
		gradient(0, 580, W, 500, alpha(col::bg, 0), col::bg);
		text(bold(), 19, 84, 225, col::warm, "CONTINUE PLAYING");
		text(bold(), 96, 80, 252, col::text, fit(bold(), 96, g.title, 1050));
		text(reg(), 24, 84, 372, col::sub, g.platform + "  \xC2\xB7  Played " + g.lastPlayed + "  \xC2\xB7  " + g.played + " in all"
				+ (g.stateWhen.empty() ? "" : "  \xC2\xB7  Saved state from " + g.stateWhen));
		float bx = 84;
		bx += button(bx, 432, "Resume", true, st.home == 0, 26) + 18;
		bx += button(bx, 432, "Start From the Beginning", false, st.home == 1) + 18;
		button(bx, 432, "Game Page", false, st.home == 2);
	}
	else
	{
		text(bold(), 72, 84, 240, col::text, h.games.empty() ? "No games yet" : "Nothing played yet");
		text(reg(), 26, 86, 340, col::sub, h.games.empty() ? "Add a folder of games in Settings > Games and Saves > Folders."
				: "A game you play shows up here, to continue in one press.");
	}
	// What is true now.
	float cy = 225;
	for (const Card& c : h.now)
	{
		if (cy > 540)
			break;
		rect(1386, cy, 450, 90, alpha(col::bg, 0.78f), 24);
		text(bold(), 21, 1410, cy + 18, col::text, c.title);
		text(reg(), 19, 1410, cy + 50, col::sub, fit(reg(), 19, c.body, 400));
		cy += 105;
	}
	// The games played lately.
	if (lately > 0)
	{
		text(bold(), 22, 84, 678, col::sub, "Played Lately");
		const float tile = (1752 - 6 * 24) / 7.f;
		for (int k = 0; k < lately; k++)
			cover(h.games[(size_t)h.recent[(size_t)(k + 1)]], 84 + k * (tile + 24), 730, tile, tile, 24, st.home == 3 + k);
	}
	topBar(0);
	hints({ { { "circle" }, "Quit" }, { { "OPTIONS" }, "Settings" }, { { "triangle" }, "Game Page" },
			{ { "cross" }, st.home == 0 ? "Resume" : st.home == 1 ? "Start" : st.home == 2 ? "Open" : "Play" } });
}

// ---------------------------------------------------------------- the Library

const char *chips[] = { "All", "Console", "USB", "Share", "Favourites", "Dreamcast", "Arcade", "Never Played" };

std::vector<int> libraryGames()
{
	std::vector<int> out;
	for (int i = 0; i < (int)H->games.size(); i++)
	{
		const Game& g = H->games[(size_t)i];
		const char *c = chips[st.chip];
		const bool keep = st.chip == 0 || (st.chip <= 3 && g.place == c) || (st.chip == 4 && g.favourite)
				|| (st.chip == 5 && g.platform == "Dreamcast") || (st.chip == 6 && g.platform != "Dreamcast")
				|| (st.chip == 7 && g.neverPlayed);
		if (keep)
			out.push_back(i);
	}
	return out;
}

void libraryScreen(const Input& in)
{
	const std::vector<int> list = libraryGames();
	const int cols = 8, n = (int)list.size();
	if (in.left && st.lib % cols > 0) st.lib--;
	if (in.right && st.lib % cols < cols - 1 && st.lib < n - 1) st.lib++;
	if (in.down && st.lib + cols < n) st.lib += cols;
	if (in.up && st.lib >= cols) st.lib -= cols;
	if (in.l2 && st.chip > 0) { st.chip--; st.lib = 0; }
	if (in.r2 && st.chip < 7) { st.chip++; st.lib = 0; }
	st.lib = std::clamp(st.lib, 0, std::max(0, n - 1));
	if (in.l1) st.screen = Screen::Home;
	if (in.options) { st.back = Screen::Library; st.screen = Screen::SettingsHome; }
	if (n > 0 && in.accept && H->game) H->game(list[(size_t)st.lib], GameAct::Play);
	if (n > 0 && in.triangle) { st.game = list[(size_t)st.lib]; st.page = 0; st.back = Screen::Library; st.screen = Screen::GamePage; }

	rect(0, 0, W, HH, col::bg);
	topBar(1);
	// The filters: where the games are, then what they are.
	float x = 84;
	for (int i = 0; i < 8; i++)
	{
		if (i == 4)
		{
			rect(x + 6, 154, 1.5f, 30, col::line);
			x += 22;
		}
		const float w = measure(reg(), 21, chips[i]).x + 44;
		rect(x, 144, w, 50, i == st.chip ? col::text : alpha(col::text, 0.08f), 25);
		text(i == st.chip ? bold() : reg(), 21, x + 22, 156, i == st.chip ? col::dark : col::sub, chips[i]);
		x += w + 12;
	}
	const std::string count = std::to_string(n) + " games  \xC2\xB7  By Name  \xC2\xB7  Grid";
	text(reg(), 21, 1836 - measure(reg(), 21, count).x, 156, col::sub, count);
	const float gap = 21, cw = (W - 168 - gap * (cols - 1)) / cols;
	const int focusRow = st.lib / cols;
	const float scroll = std::max(0, focusRow - 1) * (cw + gap);
	dl->PushClipRect(V(0, 210), V(W, 1080), true);
	for (int i = 0; i < n; i++)
	{
		const float cx = 84 + (i % cols) * (cw + gap), cy = 228 + (i / cols) * (cw + gap) - scroll;
		if (cy > 1080 || cy + cw < 210)
			continue;
		cover(H->games[(size_t)list[(size_t)i]], cx, cy, cw, cw, 21, i == st.lib);
	}
	dl->PopClipRect();
	// The focused game, at the bottom.
	gradient(0, 800, W, 100, alpha(col::bg, 0), col::bg);
	rect(0, 900, W, 180, col::bg);
	if (n > 0)
	{
		const Game& g = H->games[(size_t)list[(size_t)st.lib]];
		text(bold(), 36, 84, 912, col::text, fit(bold(), 36, g.title, 900));
		text(reg(), 21, 84, 962, col::sub, fit(reg(), 21, g.platform + "  \xC2\xB7  " + g.place + "  \xC2\xB7  " + g.line, 980));
	}
	hints({ { { "circle" }, "Quit" }, { { "L2", "R2" }, "Filter" }, { { "OPTIONS" }, "Settings" }, { { "triangle" }, "Game Page" },
			{ { "cross" }, "Play" } });
}

// ---------------------------------------------------------------- a game's page

void gamePage(const Input& in)
{
	if (st.game < 0 || st.game >= (int)H->games.size())
	{
		st.screen = st.back;
		return;
	}
	const Game& g = H->games[(size_t)st.game];
	const int stops = 2 + 6;
	if (in.up && st.page >= 2 && st.page <= 4) st.page = 0;
	else if (in.up && st.page >= 5) st.page -= 3;
	if (in.down && st.page < 2) st.page = st.page == 0 ? 1 : 2;
	else if (in.down && st.page >= 2 && st.page <= 4) st.page += 3;
	if (in.left && st.page >= 2 && (st.page - 2) % 3 > 0) st.page--;
	else if (in.left && st.page >= 2) st.page = 0;
	if (in.right && st.page < 2) st.page = 2;
	else if (in.right && st.page >= 2 && (st.page - 2) % 3 < 2) st.page++;
	st.page = std::clamp(st.page, 0, stops - 1);
	if (in.accept && H->game && st.page < 2)
		H->game(st.game, st.page == 0 ? GameAct::Resume : GameAct::StartOver);
	if (in.back)
		st.screen = st.back;

	rect(0, 0, W, HH, col::bg);
	rect(0, 0, W, 450, g.colour);
	disc(1640, 60, 315, alpha(IM_COL32(255, 255, 255, 255), 0.08f));
	gradient(0, 120, W, 330, alpha(col::bg, 0), col::bg);
	// The cover and what starts it.
	cover(g, 84, 84, 390, 390, 30, false);
	button(84, 504, "Resume", true, st.page == 0, 26);
	button(84, 588, "Start From the Beginning", false, st.page == 1, 22);
	text(reg(), 19, 86, 680, col::faint, g.stateWhen.empty() ? "No state was saved when it was closed."
			: "Resume loads the state saved when the game was closed, " + g.stateWhen + ".", 390);
	// What it is.
	const float x = 534;
	text(bold(), 66, x, 78, col::text, fit(bold(), 66, g.title, 1300));
	text(reg(), 22, x + 2, 166, col::sub, g.platform + "  \xC2\xB7  " + g.place + "  \xC2\xB7  " + g.line);
	const char *tabs[] = { "Overview", "Save States", "Its Settings", "Cheats and Patches", "Achievements", "Discs" };
	float tx = x;
	tx += key(tx, 255, "L1") + 12;
	for (int i = 0; i < 6; i++)
	{
		const float w = measure(bold(), 20, tabs[i]).x + 48;
		if (i == 0)
			rect(tx, 231, w, 48, col::text, 24);
		text(bold(), 20, tx + 24, 243, i == 0 ? col::dark : col::sub, tabs[i]);
		tx += w + 6;
	}
	key(tx + 6, 255, "R1");
	// The cards: a line of each part of the game.
	struct Part { const char *label; std::string big, small; };
	const Part parts[] = {
		{ "SAVE STATES", std::to_string(g.states) + (g.states == 1 ? " state" : " states"), g.stateWhen.empty() ? "None yet" : "Newest: " + g.stateWhen },
		{ "ITS SETTINGS", g.ownSettings == 0 ? "As for every game" : std::to_string(g.ownSettings) + " of its own", "Changed for this game only" },
		{ "VMU", "Its own VMU", "Port A, slot 1" },
		{ "CHEATS AND PATCHES", g.cheats == 0 ? "None on" : std::to_string(g.cheats) + " on", "Patches and cheats for this game" },
		{ "ACHIEVEMENTS", g.achievements == 0 ? "None" : std::to_string(g.achieved) + " of " + std::to_string(g.achievements), "RetroAchievements" },
		{ "CUSTOM TEXTURES", "None found", "Looked for in the texture folders" },
	};
	const float cw = (W - 84 - x - 2 * 21) / 3;
	for (int i = 0; i < 6; i++)
	{
		const float cx = x + (i % 3) * (cw + 21), cy = 333 + (i / 3) * 171;
		const bool on = st.page == 2 + i;
		if (on)
			rect(cx - 5, cy - 5, cw + 10, 160, col::text, 29);
		rect(cx, cy, cw, 150, on ? IM_COL32(40, 44, 54, 255) : col::card, 24);
		text(bold(), 17, cx + 27, cy + 24, col::faint, parts[i].label);
		text(bold(), 27, cx + 27, cy + 54, col::text, fit(bold(), 27, parts[i].big, cw - 54));
		if (i == 4 && g.achievements > 0)
		{
			rect(cx + 27, cy + 106, cw - 54, 9, alpha(col::text, 0.16f), 4.5f);
			rect(cx + 27, cy + 106, (cw - 54) * g.achieved / g.achievements, 9, col::warm, 4.5f);
		}
		else
			text(reg(), 19, cx + 27, cy + 98, col::sub, fit(reg(), 19, parts[i].small, cw - 54));
	}
	text(bold(), 17, x, 702, col::faint, "ABOUT");
	text(reg(), 22, x, 732, col::sub, g.about, W - 84 - x);
	text(reg(), 19, x, 846, col::faint, fit(reg(), 19, g.file, W - 84 - x));
	hints({ { { "circle" }, "Back" }, { { "L2", "R2" }, "Its Pictures" }, { { "square" }, "More: Favourite, Hide, Cover" },
			{ { "cross" }, st.page == 0 ? "Resume" : st.page == 1 ? "Start" : "Open" } });
}

// ---------------------------------------------------------------- the quick menu

void quickMenu(const Input& in)
{
	struct Tile { const char *name; std::string sub; QuickAct act; };
	const Game *g = H->running >= 0 && H->running < (int)H->games.size() ? &H->games[(size_t)H->running] : nullptr;
	const Tile tiles[] = {
		{ "Resume", "Back to the game", QuickAct::Resume },
		{ "Save State", "Slot 1", QuickAct::SaveState },
		{ "Load State", g && !g->stateWhen.empty() ? "Slot 1  \xC2\xB7  " + g->stateWhen : "Slot 1", QuickAct::LoadState },
		{ "Rewind", "The last minutes", QuickAct::Rewind },
		{ "Change Disc", g && g->discs > 1 ? "Disc 2 is next" : "Another disc", QuickAct::ChangeDisc },
		{ "Quit Game", "Saves its state", QuickAct::Quit },
	};
	std::vector<const Setting *> pins;
	for (const std::string& id : H->pins)
		if (const Setting *s = find(id))
			if (pins.size() < 3)
				pins.push_back(s);
	const char *more[] = { "Swap VMU", "Cheats", "Achievements", "Fast Forward, Speedrun Timer, Memory Search, Controls, Restart Game" };
	const int nPins = (int)pins.size(), stops = 6 + nPins + 4;
	if (st.quick < 6)
	{
		if (in.left && st.quick % 3 > 0) st.quick--;
		if (in.right && st.quick % 3 < 2) st.quick++;
		if (in.up && st.quick >= 3) st.quick -= 3;
		if (in.down) st.quick = st.quick < 3 ? st.quick + 3 : 6;
	}
	else
	{
		if (in.up) st.quick = st.quick == 6 ? 3 : st.quick - 1;
		if (in.down && st.quick < stops - 1) st.quick++;
		if (st.quick >= 6 && st.quick < 6 + nPins && (in.left || in.right))
			change(*pins[(size_t)(st.quick - 6)], in.right ? 1 : -1);
	}
	st.quick = std::clamp(st.quick, 0, stops - 1);
	if (in.accept && H->quick && st.quick < 6)
		H->quick(tiles[st.quick].act);
	if (in.back && H->close)
		H->close();
	if (in.options) { st.back = Screen::Quick; st.screen = Screen::SettingsHome; }

	// The game, paused, under a veil (on the console the game's own picture is there).
	rect(0, 0, 840, HH, alpha(col::bg, 0.94f));
	gradient(840, 0, 180, HH, alpha(col::bg, 0.94f), alpha(col::bg, 0.0f));
	text(bold(), 18, 84, 66, col::faint, "PAUSED");
	text(bold(), 45, 84, 92, col::text, g ? fit(bold(), 45, g->title, 700) : "A game");
	text(reg(), 21, 84, 156, col::sub, H->runningLine);
	const float tw = (840 - 84 - 54 - 2 * 18) / 3;
	for (int i = 0; i < 6; i++)
	{
		const float x = 84 + (i % 3) * (tw + 18), y = 210 + (i / 3) * 144;
		const bool on = st.quick == i;
		if (on)
			rect(x - 6, y - 6, tw + 12, 138, alpha(col::text, 0.28f), 30);
		rect(x, y, tw, 126, on ? col::text : alpha(col::text, 0.09f), 24);
		text(bold(), 24, x + 21, y + 56, on ? col::dark : col::text, tiles[i].name);
		text(reg(), 18, x + 21, y + 90, on ? IM_COL32(59, 69, 88, 255) : col::sub, fit(reg(), 18, tiles[i].sub, tw - 42));
	}
	float y = 516;
	text(bold(), 17, 84, y, col::faint, "PINNED SETTINGS, FOR THIS GAME");
	y += 36;
	for (int i = 0; i < nPins; i++)
	{
		const bool on = st.quick == 6 + i;
		rect(84, y, 702, 56, on ? col::text : alpha(col::text, 0.07f), 18);
		text(bold(), 21, 108, y + 15, on ? col::dark : col::text, pins[(size_t)i]->label);
		std::string v = valueOf(*pins[(size_t)i]);
		if (on)
			v = "<  " + v + "  >";
		text(reg(), 21, 762 - measure(reg(), 21, v).x, y + 15, on ? col::dark : col::sub, v);
		y += 63;
	}
	if (nPins == 0)
	{
		text(reg(), 19, 84, y, col::faint, "Pin a setting with Square in Settings: it shows here, to change in a game.");
		y += 40;
	}
	y += 18;
	text(bold(), 17, 84, y, col::faint, "MORE");
	y += 32;
	for (int i = 0; i < 4; i++)
	{
		const bool on = st.quick == 6 + nPins + i;
		if (on)
			rect(84, y, 702, 50, col::text, 18);
		text(bold(), 21, 108, y + 12, on ? col::dark : col::text, fit(bold(), 21, more[i], 640));
		y += 52;
	}
	text(bold(), 21, 1836 - measure(bold(), 21, H->clock).x - 24, 66, col::text, H->clock);
	hints({ { { "circle" }, "Resume" }, { { "OPTIONS" }, "All Settings" }, { { "triangle" }, "Arrange" }, { { "cross" }, "Choose" } },
			1836, col::text);
}

// ---------------------------------------------------------------- the settings

void settingsHeader(const std::string& place)
{
	float x = 84;
	if (!place.empty())
	{
		x += text(bold(), 22, x, 64, col::faint, "Settings") * 0 + measure(bold(), 22, "Settings").x + 18;
		text(reg(), 22, x, 64, col::faint, "/");
		x += 30;
	}
	const std::string title = place.empty() ? "Settings" : place;
	text(bold(), place.empty() ? 51 : 45, x, place.empty() ? 46 : 52, col::text, title);
	x += measure(bold(), place.empty() ? 51 : 45, title).x + 24;
	text(reg(), 21, x, 66, col::sub, H->running >= 0 ? "For this game" : "For every game");
	levelSwitch(1836, 48);
}

void settingsHome(const Input& in)
{
	const int areaCount = (int)areas().size();
	const int stops = 2 + areaCount;	// pinned, changed, the areas
	int& f = st.setHome;
	if (f < 0)
	{
		if (in.down) f = 0;
	}
	else if (f < 2)
	{
		if (in.left && f == 1) f = 0;
		if (in.right && f == 0) f = 1;
		if (in.up) f = -1;
		if (in.down) f = f == 0 ? 2 : 4;
	}
	else
	{
		const int a = f - 2;
		if (in.left && a % 4 > 0) f--;
		if (in.right && a % 4 < 3 && f < stops - 1) f++;
		if (in.up) f = a >= 4 ? f - 4 : (a < 2 ? 0 : 1);
		if (in.down && a + 4 < areaCount) f += 4;
	}
	if (in.l1 && st.level > 0) st.level--;
	if (in.r1 && st.level < 2) st.level++;
	if (in.triangle || (in.accept && f < 0))
	{
		if (H->typeQuery)
			H->typeQuery();
		st.result = 0;
		st.screen = Screen::Search;
	}
	else if (in.accept)
	{
		st.virt = f == 0 ? 1 : f == 1 ? 2 : 0;
		st.area = f >= 2 ? f - 2 : 0;
		st.row = 0;
		st.screen = Screen::SettingsArea;
	}
	if (in.back || in.options)
		st.screen = st.back == Screen::Quick ? Screen::Quick : st.back;

	rect(0, 0, W, HH, col::bg);
	settingsHeader("");
	// The search.
	const bool sOn = f < 0;
	if (sOn)
		rect(80, 132, 1760, 89, col::text, 44);
	rect(84, 136, 1752, 81, IM_COL32(37, 40, 48, 255), 40);
	dl->AddCircle(V(132, 172), 10 * S, col::text, 24, 3 * S);
	dl->AddLine(V(139, 179), V(148, 188), col::text, 3 * S);
	const std::string all = "Search all " + std::to_string(settings().size()) + " settings and your games";
	text(reg(), 24, 168, 162, col::sub, all);
	key(1784, 176, "triangle");
	// Pinned, and changed from the default.
	int changed = 0, pinnedCount = 0;
	std::string pinnedNames, pinnedValues;
	for (const Setting& s : settings())
	{
		if (H->changed && H->changed(s))
			changed++;
		if (pinned(s))
		{
			pinnedCount++;
			if (pinnedCount <= 3)
			{
				pinnedNames += (pinnedNames.empty() ? "" : "  \xC2\xB7  ") + std::string(s.label);
				pinnedValues += (pinnedValues.empty() ? "" : "  \xC2\xB7  ") + valueOf(s);
			}
		}
	}
	const float tw = (1752 - 21) / 2;
	const struct { const char *label; std::string big, small; } tops[] = {
		{ "PINNED", pinnedCount ? pinnedNames : "Nothing pinned", pinnedCount ? pinnedValues : "Press Square on a setting to pin it" },
		{ "CHANGED FROM DEFAULT", std::to_string(changed) + (changed == 1 ? " setting" : " settings"), "Review them, or reset one to its default" },
	};
	for (int i = 0; i < 2; i++)
	{
		const float x = 84 + i * (tw + 21), y = 252;
		const bool on = f == i;
		if (on)
			rect(x - 5, y - 5, tw + 10, 128, col::text, 29);
		rect(x, y, tw, 118, on ? IM_COL32(40, 44, 54, 255) : col::card, 24);
		text(bold(), 17, x + 27, y + 21, col::faint, tops[i].label);
		text(bold(), 23, x + 27, y + 48, col::text, fit(bold(), 23, tops[i].big, tw - 54));
		text(reg(), 19, x + 27, y + 82, col::sub, fit(reg(), 19, tops[i].small, tw - 54));
	}
	// The areas.
	const float aw = (1752 - 3 * 21) / 4, ah = 228;
	for (int a = 0; a < areaCount; a++)
	{
		const float x = 84 + (a % 4) * (aw + 21), y = 402 + (a / 4) * (ah + 21);
		const bool on = f == 2 + a;
		int count = 0, ch = 0;
		std::vector<const Setting *> firsts;
		for (const Setting& s : settings())
			if (s.area == a && shown(s, st.level))
			{
				count++;
				if (H->changed && H->changed(s))
					ch++;
				if (s.kind != Kind::Action && s.kind != Kind::Info && firsts.size() < 2 && s.level == Level::Essentials)
					firsts.push_back(&s);
			}
		for (const Setting& s : settings())
			if (firsts.size() < 2 && s.area == a && shown(s, st.level) && s.kind != Kind::Action && s.kind != Kind::Info
					&& std::find(firsts.begin(), firsts.end(), &s) == firsts.end())
				firsts.push_back(&s);
		// An area of actions only (System): what they say, where they say something.
		for (const Setting& s : settings())
			if (firsts.size() < 2 && s.area == a && shown(s, st.level) && !valueOf(s).empty()
					&& std::find(firsts.begin(), firsts.end(), &s) == firsts.end())
				firsts.push_back(&s);
		if (on)
			rect(x - 5, y - 5, aw + 10, ah + 10, col::text, 29);
		rect(x, y, aw, ah, on ? IM_COL32(40, 44, 54, 255) : col::card, 24);
		text(bold(), 30, x + 27, y + 24, col::text, areas()[(size_t)a].name);
		text(reg(), 18, x + 27, y + 66, col::faint, std::to_string(count) + (count == 1 ? " setting" : " settings")
				+ (ch ? "  \xC2\xB7  " + std::to_string(ch) + " changed" : ""));
		float ly = y + ah - 30 - 30 * (float)firsts.size();
		for (const Setting *s : firsts)
		{
			text(reg(), 19, x + 27, ly, col::sub, fit(reg(), 19, std::string(s->label) + "  " + valueOf(*s), aw - 54));
			ly += 30;
		}
	}
	hints({ { { "circle" }, "Close" }, { { "L1", "R1" }, "Level" }, { { "triangle" }, "Search" }, { { "cross" }, "Open" } });
}

void settingsArea(const Input& in)
{
	const std::vector<Item> items = itemsFor();
	int stops = 0;
	for (const Item& it : items)
		stops += it.s != nullptr;
	if (in.up && st.row > 0) st.row--;
	if (in.down && st.row < stops - 1) st.row++;
	if (in.l1 && st.level > 0) st.level--;
	if (in.r1 && st.level < 2) st.level++;
	const Setting *focused = nullptr;
	{
		int k = 0;
		for (const Item& it : items)
			if (it.s && k++ == st.row)
				focused = it.s;
	}
	if (focused)
	{
		if (in.left || in.right)
			change(*focused, in.right ? 1 : -1);
		if (in.accept)
		{
			if (focused->kind == Kind::Action && H->run)
				H->run(*focused);
			else if (focused->kind == Kind::Toggle)
				change(*focused, 1);
		}
		if (in.square)
		{
			auto at = std::find(H->pins.begin(), H->pins.end(), focused->id);
			if (at == H->pins.end())
				H->pins.push_back(focused->id);
			else
				H->pins.erase(at);
		}
		if (in.l3 && H->reset)
			H->reset(*focused);
	}
	if (in.triangle) { if (H->typeQuery) H->typeQuery(); st.result = 0; st.screen = Screen::Search; }
	if (in.back) st.screen = Screen::SettingsHome;
	if (in.options) st.screen = st.back;

	rect(0, 0, W, HH, col::bg);
	settingsHeader(st.virt == 1 ? "Pinned" : st.virt == 2 ? "Changed From Default" : areas()[(size_t)st.area].name);
	// The sections, which follow the focus.
	const float top = 150, bottom = 1000;
	std::string current;
	for (const Item& it : items)
		if (it.s == focused && focused)
			current = focused->section;
	if (st.virt == 0)
	{
		float y = top;
		for (const std::string& sec : sections(st.area))
		{
			int count = 0;
			for (const Setting& s : settings())
				if (s.area == st.area && s.section == sec && shown(s, st.level))
					count++;
			const bool on = sec == current;
			if (on)
				rect(84, y, 375, 57, alpha(col::text, 0.12f), 18);
			text(on ? bold() : reg(), 21, 105, y + 15, count ? (on ? col::text : col::sub) : col::faint, fit(reg(), 21, sec, 290));
			const std::string c = std::to_string(count);
			text(bold(), 17, 438 - measure(bold(), 17, c).x, y + 18, col::faint, c);
			y += 60;
		}
	}
	const float lx = st.virt == 0 ? 489 : 84;
	const float rw = 1836 - 540 - 30 - lx;
	settingRows(items, st.row, lx, top, rw, bottom, true);
	if (focused)
		detail(*focused, 1836 - 540, top, 540);
	hints({ { { "circle" }, "Back" }, { { "L1", "R1" }, "Level" }, { { "L3" }, "Reset to Default" },
			{ { "square" }, focused && pinned(*focused) ? "Unpin" : "Pin" }, { { "triangle" }, "Search" },
			{ { "cross" }, focused && focused->kind == Kind::Action ? "Open" : "Change" } });
}

void searchScreen(const Input& in)
{
	const std::vector<const Setting *> found = search(H->query);
	std::vector<int> games;
	{
		std::string q;
		for (char c : H->query)
			q += (char)tolower((unsigned char)c);
		for (int i = 0; i < (int)H->games.size() && !q.empty(); i++)
		{
			std::string t;
			for (char c : H->games[(size_t)i].title)
				t += (char)tolower((unsigned char)c);
			if (t.find(q) != std::string::npos)
				games.push_back(i);
		}
	}
	std::vector<Item> items;
	if (!found.empty())
		items.push_back({ nullptr, "Settings" });
	for (const Setting *s : found)
		items.push_back({ s, "" });
	const int stops = (int)found.size() + (int)games.size();
	if (in.up && st.result > 0) st.result--;
	if (in.down && st.result < stops - 1) st.result++;
	const bool onGame = st.result >= (int)found.size();
	const Setting *focused = !onGame && !found.empty() ? found[(size_t)st.result] : nullptr;
	if (focused && (in.left || in.right))
		change(*focused, in.right ? 1 : -1);
	if (focused && in.accept && focused->kind == Kind::Action && H->run)
		H->run(*focused);
	if (onGame && in.accept && !games.empty())
	{
		st.game = games[(size_t)(st.result - (int)found.size())];
		st.page = 0;
		st.back = Screen::Library;
		st.screen = Screen::GamePage;
	}
	if (in.triangle && H->typeQuery) H->typeQuery();
	if (in.back) st.screen = Screen::SettingsHome;

	rect(0, 0, W, HH, col::bg);
	rect(84, 48, 1752, 81, IM_COL32(37, 40, 48, 255), 40);
	dl->AddCircle(V(132, 88), 10 * S, col::text, 24, 3 * S);
	dl->AddLine(V(139, 95), V(148, 104), col::text, 3 * S);
	text(bold(), 27, 168, 72, col::text, H->query.empty() ? "Type to search" : H->query);
	const std::string n = std::to_string(found.size()) + (found.size() == 1 ? " setting" : " settings") + "  \xC2\xB7  "
			+ std::to_string(games.size()) + (games.size() == 1 ? " game" : " games");
	text(reg(), 21, 1800 - measure(reg(), 21, n).x, 78, col::sub, n);
	const float top = 168, rw = 1836 - 540 - 30 - 84;
	// Settings: one line each, with where it lives.
	float y = top;
	dl->PushClipRect(V(80, top - 6), V(84 + rw + 6, 1000), true);
	text(bold(), 17, 108, y, col::faint, "SETTINGS");
	y += 36;
	for (int i = 0; i < (int)found.size() && y < 1000; i++)
	{
		const Setting& s = *found[(size_t)i];
		const bool on = st.result == i;
		if (on)
			rect(80, y - 4, rw + 8, 89, col::text, 22);
		rect(84, y, rw, 81, on ? IM_COL32(40, 44, 54, 255) : alpha(col::text, 0.05f), 18);
		text(bold(), 22, 108, y + 13, col::text, s.label);
		text(reg(), 17, 108, y + 47, col::faint, std::string(areas()[(size_t)s.area].name) + "  >  " + s.section
				+ (s.level == Level::Everything ? "  \xC2\xB7  Everything" : ""));
		if (s.kind == Kind::Toggle)
			switchAt(84 + rw - 24, y + 40, H->get ? H->get(s) != 0 : s.demo != 0);
		else
		{
			const std::string v = fit(reg(), 21, valueOf(s), rw * 0.35f);
			text(reg(), 21, 84 + rw - 24 - measure(reg(), 21, v).x, y + 27, col::sub, v);
		}
		y += 90;
	}
	if (!games.empty() && y < 960)
	{
		y += 12;
		text(bold(), 17, 108, y, col::faint, "GAMES");
		y += 36;
		for (int k = 0; k < (int)games.size() && y < 1000; k++)
		{
			const Game& g = H->games[(size_t)games[(size_t)k]];
			const bool on = st.result == (int)found.size() + k;
			if (on)
				rect(80, y - 4, rw + 8, 89, col::text, 22);
			rect(84, y, rw, 81, on ? IM_COL32(40, 44, 54, 255) : alpha(col::text, 0.05f), 18);
			rect(102, y + 13, 56, 56, g.colour, 12);
			text(bold(), 22, 180, y + 13, col::text, g.title);
			text(reg(), 17, 180, y + 47, col::faint, g.platform + "  \xC2\xB7  " + g.place);
			text(reg(), 21, 84 + rw - 24 - measure(reg(), 21, "Game Page").x, y + 27, col::sub, "Game Page");
			y += 90;
		}
	}
	dl->PopClipRect();
	if (focused)
		detail(*focused, 1836 - 540, top, 540);
	hints({ { { "circle" }, "Back" }, { { "square" }, "Pin" }, { { "triangle" }, "Type Again" },
			{ { "cross" }, onGame ? "Game Page" : "Change" } });
}

// ---------------------------------------------------------------- Look For My Files

void foundScreen(const Input& in)
{
	const int n = (int)H->found.size();
	if (in.up && st.found > 0) st.found--;
	if (in.down && st.found < n) st.found++;
	if (in.accept && st.found < n && !H->found[(size_t)st.found].blocked)
		H->found[(size_t)st.found].on = !H->found[(size_t)st.found].on;
	if ((in.triangle || (in.accept && st.found == n)) && H->useFound)
	{
		H->useFound(H->found);
		st.screen = Screen::SettingsArea;
	}
	if (in.back) st.screen = Screen::SettingsArea;

	rect(0, 0, W, HH, col::bg);
	float x = 84;
	for (const char *part : { "Settings", "/", "Games and Saves", "/" })
	{
		text(reg(), 22, x, 64, col::faint, part);
		x += measure(reg(), 22, part).x + 14;
	}
	text(bold(), 45, x, 52, col::text, "Look For My Files");
	text(bold(), 17, 105, 156, col::faint, "LOOKED THROUGH");
	rect(84, 186, 375, 57, alpha(col::text, 0.12f), 18);
	text(bold(), 21, 105, 201, col::text, fit(bold(), 21, H->foundPlace, 330));
	text(reg(), 19, 105, 268, col::faint, "Nothing is moved or copied. PSFlyCast reads these files, and writes what it keeps, where they are.", 330);
	int using_ = 0;
	float y = 150;
	const float lx = 489, rw = 1836 - lx;
	for (int i = 0; i < n; i++)
	{
		const FoundRow& r = H->found[(size_t)i];
		const bool on = st.found == i;
		if (on)
			rect(lx - 4, y - 4, rw + 8, 89, col::text, 22);
		rect(lx, y, rw, 81, on ? IM_COL32(40, 44, 54, 255) : alpha(col::text, r.blocked ? 0.0f : 0.05f), 18);
		text(bold(), 22, lx + 27, y + 27, r.blocked ? col::faint : col::text, r.kind);
		text(bold(), 22, lx + 300, y + 13, r.blocked ? col::faint : col::text, r.what);
		text(reg(), 17, lx + 300, y + 47, col::faint, fit(reg(), 17, r.folder + (r.note.empty() ? "" : "  \xC2\xB7  " + r.note), rw - 420));
		if (!r.blocked)
			switchAt(lx + rw - 24, y + 40, r.on);
		using_ += r.on && !r.blocked;
		y += 90;
	}
	y += 14;
	const bool bOn = st.found == n;
	const std::string label = "Use These " + std::to_string(using_) + (using_ == 1 ? " Folder" : " Folders");
	button(lx, y, label, true, bOn, 24);
	button(lx + measure(bold(), 24, label).x + 96, y, "Cancel", false, false, 24);
	hints({ { { "circle" }, "Cancel" }, { { "triangle" }, "Use These Folders" }, { { "cross" }, "Switch" } });
}
}	// namespace

void show(Screen screen)
{
	st.screen = screen;
}

Screen current()
{
	return st.screen;
}

void focusForPicture(int home, int library, int area, int row, int level)
{
	st.home = home;
	st.lib = library;
	st.area = area;
	st.row = row;
	st.level = level;
	st.setHome = 2 + area;
	st.virt = 0;
	st.game = 0;
	st.page = 0;
	st.quick = 0;
	st.result = 0;
	st.found = 0;
}

void frame(Host& host, const Input& in)
{
	H = &host;
	dl = ImGui::GetBackgroundDrawList();
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	S = size.y / 1080.f;
	W = size.x / S;
	HH = 1080;
	switch (st.screen)
	{
	case Screen::Home: homeScreen(in); break;
	case Screen::Library: libraryScreen(in); break;
	case Screen::GamePage: gamePage(in); break;
	case Screen::Quick: quickMenu(in); break;
	case Screen::SettingsHome: settingsHome(in); break;
	case Screen::SettingsArea: settingsArea(in); break;
	case Screen::Search: searchScreen(in); break;
	case Screen::Found: foundScreen(in); break;
	}
}
}
