/*
	PSFlyCast - the interface's screens (ui.h). Everything is drawn on the background draw list in
	units of a 1920 x 1080 screen; S turns units into pixels.

	The look: the skin's ground, lit softly by its accent; frosted cards edged with light; the focus
	lifts what it is on, rings it and lights it with the accent; icons name the places and the
	actions. The same kit draws the screens of bigpicture.cpp that keep their own logic (kit::).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "ui.h"
#include "ui/IconsFontAwesome6.h"
#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pui
{
namespace
{
// ---------------------------------------------------------------- the kit

// The colours: the host's skin (Host::palette), set at each frame.
namespace col
{
ImU32 bg, bg2, text, sub, faint, card, cardHi, line, warm, on, off, dark, focus, onFocus;
}

ImDrawList *dl;
Host *H;
float S = 1.f, W = 1920.f, HH = 1080.f;
// The margin at the left and the right of every screen.
constexpr float M = 84;

ImVec2 V(float x, float y) { return ImVec2(x * S, y * S); }
ImU32 alpha(ImU32 c, float a)
{
	return (c & 0x00ffffffu) | ((ImU32)std::clamp(a * (float)((c >> 24) & 0xff), 0.f, 255.f) << 24);
}
ImU32 mix(ImU32 a, ImU32 b, float t)
{
	ImU32 out = 0;
	for (int k = 0; k < 4; k++)
	{
		const float ca = (float)((a >> (8 * k)) & 0xff), cb = (float)((b >> (8 * k)) & 0xff);
		out |= (ImU32)std::clamp(ca + (cb - ca) * t, 0.f, 255.f) << (8 * k);
	}
	return out;
}
// How light a colour is, 0..1.
float lightness(ImU32 c)
{
	return (0.2126f * (c & 0xff) + 0.7152f * ((c >> 8) & 0xff) + 0.0722f * ((c >> 16) & 0xff)) / 255.f;
}

void rect(float x, float y, float w, float h, ImU32 c, float r = 0, ImDrawFlags f = 0)
{
	dl->AddRectFilled(V(x, y), V(x + w, y + h), c, r * S, f);
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

// A rounded box shaded from top to bottom (the box keeps its corners, which a gradient does not).
void shaded(float x, float y, float w, float h, float r, ImU32 top, ImU32 bottom)
{
	const int from = dl->VtxBuffer.Size;
	dl->AddRectFilled(V(x, y), V(x + w, y + h), top, r * S);
	ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, from, dl->VtxBuffer.Size, V(x, y), V(x, y + h), top, bottom);
}

void disc(float cx, float cy, float r, ImU32 c)
{
	dl->AddCircleFilled(V(cx, cy), r * S, c, 96);
}

// A soft light: its colour at the centre, nothing at its edge.
void glow(float cx, float cy, float radius, ImU32 c)
{
	const int n = 72;
	const ImVec2 uv = dl->_Data->TexUvWhitePixel;
	const ImU32 edge = c & 0x00ffffffu;
	dl->PrimReserve(n * 3, n + 1);
	const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	dl->PrimWriteVtx(V(cx, cy), uv, c);
	for (int i = 0; i < n; i++)
	{
		const float a = 6.2831853f * (float)i / (float)n;
		dl->PrimWriteVtx(V(cx + std::cos(a) * radius, cy + std::sin(a) * radius), uv, edge);
	}
	for (int i = 0; i < n; i++)
	{
		dl->PrimWriteIdx(base);
		dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
		dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
	}
}

// A soft shadow (or a light) around a rounded box: rings that fade out over spread.
void shadow(float x, float y, float w, float h, float r, ImU32 c, float spread)
{
	const int steps = 7;
	for (int i = steps; i >= 1; i--)
	{
		const float g = spread * (float)i / steps;
		rect(x - g, y - g, w + 2 * g, h + 2 * g, alpha(c, 0.9f / steps * (1.f - (float)(i - 1) / steps)), r + g);
	}
}

ImFont *reg() { return H->regular; }
ImFont *bold() { return H->bold != nullptr ? H->bold : H->regular; }
ImFont *font(bool b) { return b ? bold() : reg(); }

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
	{
		cut.pop_back();
		// Not in the middle of a character.
		while (!cut.empty() && ((unsigned char)cut.back() & 0xc0) == 0x80)
			cut.pop_back();
		if (!cut.empty() && (unsigned char)cut.back() >= 0xc0)
			cut.pop_back();
	}
	return cut + "...";
}

// A section's name: small capitals, spaced.
void caption(float x, float y, const std::string& s, ImU32 c = 0)
{
	std::string up = s;
	for (char& ch : up)
		ch = (char)toupper((unsigned char)ch);
	float at = x;
	for (size_t i = 0; i < up.size();)
	{
		// One character at a time (a UTF-8 one whole), a little apart.
		size_t n = 1;
		while (i + n < up.size() && ((unsigned char)up[i + n] & 0xc0) == 0x80)
			n++;
		const std::string one = up.substr(i, n);
		text(bold(), 16, at, y, c ? c : col::faint, one);
		at += measure(bold(), 16, one).x + 1.6f;
		i += n;
	}
}

// The ground of a screen: the skin's colours, lit from the top left by its accent and from the
// bottom right by its warm colour.
void backdrop()
{
	if (H->ground)
		H->ground(dl);
	else
		gradient(0, 0, W, HH, col::bg, col::bg2);
	glow(W * 0.08f, -140, 980, alpha(col::focus, 0.20f));
	glow(W * 0.96f, HH + 160, 900, alpha(mix(col::focus, col::warm, 0.55f), 0.11f));
	glow(W * 0.62f, HH * 0.35f, 700, alpha(col::text, 0.025f));
}

// A card. The ground shows through it a little; its top edge catches the light.
void surface(float x, float y, float w, float h, float r, bool focus = false)
{
	if (focus)
	{
		shadow(x, y + 10, w, h, r, alpha(col::focus, 0.55f), 30);
		rect(x, y, w, h, mix(col::cardHi, col::focus, 0.16f), r);
		stroke(x - 6, y - 6, w + 12, h + 12, col::text, r + 6, 3.5f);
	}
	else
	{
		shadow(x, y + 8, w, h, r, IM_COL32(0, 0, 0, 70), 18);
		rect(x, y, w, h, alpha(col::card, 0.84f), r);
		stroke(x, y, w, h, alpha(col::text, 0.07f), r, 1.5f);
	}
	// The light on the top edge.
	dl->AddLine(V(x + r, y + 1), V(x + w - r, y + 1), alpha(col::text, focus ? 0.22f : 0.10f), 1.5f * S);
}

void ring(float x, float y, float w, float h, float r)
{
	shadow(x, y + 8, w, h, r, alpha(col::focus, 0.6f), 34);
	stroke(x - 7, y - 7, w + 14, h + 14, col::text, r + 7, 4);
}

// A controller button, as a small filled key; returns its width.
float keyWidth(const std::string& s)
{
	const bool shape = s == "circle" || s == "cross" || s == "triangle" || s == "square" || s == "dpad";
	return shape ? 32 : measure(bold(), 14, s).x + 22;
}

float key(float x, float cy, const char *k, ImU32 c = 0)
{
	const float h = 32;
	std::string s = k;
	if (H->circleConfirms && (s == "cross" || s == "circle"))
		s = s == "cross" ? "circle" : "cross";
	const float w = keyWidth(s);
	const ImU32 ink = c ? c : col::text;
	rect(x, cy - h / 2, w, h, alpha(col::text, 0.13f), h / 2);
	const float mx = x + w / 2;
	if (s == "circle")
		dl->AddCircle(V(mx, cy), 7.5f * S, IM_COL32(255, 112, 112, 255), 24, 2.4f * S);
	else if (s == "cross")
	{
		const ImU32 b = IM_COL32(126, 178, 255, 255);
		dl->AddLine(V(mx - 6.5f, cy - 6.5f), V(mx + 6.5f, cy + 6.5f), b, 2.4f * S);
		dl->AddLine(V(mx - 6.5f, cy + 6.5f), V(mx + 6.5f, cy - 6.5f), b, 2.4f * S);
	}
	else if (s == "triangle")
		dl->AddTriangle(V(mx, cy - 7.5f), V(mx + 8, cy + 6), V(mx - 8, cy + 6), IM_COL32(90, 222, 186, 255), 2.4f * S);
	else if (s == "square")
		dl->AddRect(V(mx - 6.5f, cy - 6.5f), V(mx + 6.5f, cy + 6.5f), IM_COL32(242, 140, 220, 255), 1.5f * S, 0, 2.4f * S);
	else if (s == "dpad")
	{
		rect(mx - 3, cy - 9, 6, 18, ink, 1.5f);
		rect(mx - 9, cy - 3, 18, 6, ink, 1.5f);
	}
	else
		text(bold(), 14, x + 11, cy - measure(bold(), 14, s).y / 2, ink, s);
	return w;
}

// The button hints, right-aligned on the bottom line.
void hints(const std::vector<std::pair<std::vector<const char *>, std::string>>& list, float right = 0, float cy = 1036)
{
	if (right <= 0)
		right = W - M;
	float total = 0;
	std::vector<float> widths;
	for (const auto& [keys, label] : list)
	{
		float w = measure(reg(), 19, label).x + 12;
		for (const char *k : keys)
			w += keyWidth(k) + 6;
		widths.push_back(w);
		total += w + 30;
	}
	float x = right - total + 30;
	for (size_t i = 0; i < list.size(); i++)
	{
		float at = x;
		for (const char *k : list[i].first)
			at += key(at, cy, k) + 6;
		text(reg(), 19, at + 6, cy - 12, col::sub, list[i].second);
		x += widths[i] + 30;
	}
}

// A switch, its right edge at x.
void switchAt(float right, float cy, bool on, bool focus = false)
{
	const float w = 64, h = 36;
	const ImU32 track = on ? (focus ? col::onFocus : col::on) : (focus ? alpha(col::onFocus, 0.35f) : alpha(col::text, 0.18f));
	rect(right - w, cy - h / 2, w, h, track, h / 2);
	const float kx = on ? right - h / 2 : right - w + h / 2;
	disc(kx, cy + 1.5f, h / 2 - 4, IM_COL32(0, 0, 0, 60));
	disc(kx, cy, h / 2 - 5, on && focus ? col::focus : IM_COL32(255, 255, 255, 255));
}

// A game's cover: its picture, or its colour with its title.
void cover(const Game& g, float x, float y, float w, float h, float r, bool focus)
{
	if (focus)
	{
		const float grow = w * 0.045f;
		x -= grow; y -= grow; w += 2 * grow; h += 2 * grow;
		shadow(x, y + 12, w, h, r, alpha(col::focus, 0.55f), 34);
	}
	else
		shadow(x, y + 8, w, h, r, IM_COL32(0, 0, 0, 90), 16);
	if (g.cover != ImTextureID())
		dl->AddImageRounded(g.cover, V(x, y), V(x + w, y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, r * S);
	else
	{
		// Its colour, lit from the top, with a ring of the disc on it.
		shaded(x, y, w, h, r, mix(g.colour, IM_COL32(255, 255, 255, 255), 0.22f), mix(g.colour, IM_COL32(0, 0, 0, 255), 0.45f));
		dl->AddCircle(V(x + w * 0.74f, y + h * 0.30f), w * 0.22f * S, alpha(IM_COL32(255, 255, 255, 255), 0.16f), 64, w * 0.035f * S);
		disc(x + w * 0.74f, y + h * 0.30f, w * 0.05f, alpha(IM_COL32(255, 255, 255, 255), 0.16f));
		const float size = std::clamp(w * 0.085f, 15.f, 34.f);
		text(bold(), size, x + size * 0.8f, y + h - size * 0.8f - measure(bold(), size, g.title, w - size * 1.6f).y,
				IM_COL32(255, 255, 255, 255), g.title, w - size * 1.6f);
	}
	if (focus)
		stroke(x - 7, y - 7, w + 14, h + 14, col::text, r + 7, 4);
	else
		stroke(x, y, w, h, alpha(IM_COL32(255, 255, 255, 255), 0.08f), r, 1.5f);
	if (g.favourite)
	{
		disc(x + w - 26, y + 26, 17, alpha(IM_COL32(0, 0, 0, 255), 0.55f));
		const std::string star = ICON_FA_STAR;
		text(reg(), 16, x + w - 26 - measure(reg(), 16, star).x / 2, y + 17, col::warm, star);
	}
}

// The clock and the controllers, at the top right.
void clockAt(float right, float y)
{
	const float cw = measure(bold(), 25, H->clock).x;
	text(bold(), 25, right - cw, y, col::text, H->clock);
	const std::string pads = std::string(ICON_FA_GAMEPAD) + "  " + std::to_string(H->pads);
	text(reg(), 22, right - cw - 30 - measure(reg(), 22, pads).x, y + 2, col::sub, pads);
}

// The top bar: PSFlyCast, the places (Home, Library, Disc) in a frosted pill, the controllers
// and the clock.
void topBar(int place, bool disc = true)
{
	// The mark: a disc of the accent with the play sign.
	glow(M + 20, 58, 46, alpha(col::focus, 0.5f));
	::pui::disc(M + 20, 58, 20, col::focus);
	text(reg(), 16, M + 15, 49, col::onFocus, ICON_FA_PLAY);
	text(bold(), 26, M + 54, 42, col::text, "PSFlyCast");
	const char *names[] = { "Home", "Library", "Disc" };
	const char *icons[] = { ICON_FA_HOUSE, ICON_FA_LAYER_GROUP, ICON_FA_COMPACT_DISC };
	const int n = disc ? 3 : 2;
	float total = 0;
	for (int i = 0; i < n; i++)
		total += measure(bold(), 21, names[i]).x + 84;
	const float l1 = keyWidth("L1"), r1 = keyWidth("R1");
	float x = (W - total) / 2;
	key(x - 16 - l1, 58, "L1");
	key(x + total + 16, 58, "R1");
	rect(x - 6, 30, total + 12, 56, alpha(col::card, 0.72f), 28);
	stroke(x - 6, 30, total + 12, 56, alpha(col::text, 0.08f), 28, 1.5f);
	(void)r1;
	for (int i = 0; i < n; i++)
	{
		const float w = measure(bold(), 21, names[i]).x + 84;
		const bool on = i == place;
		if (on)
		{
			shadow(x, 36, w, 44, 22, alpha(col::focus, 0.5f), 12);
			rect(x, 36, w, 44, col::focus, 22);
		}
		const ImU32 c = on ? col::onFocus : col::sub;
		text(reg(), 18, x + 24, 47, c, icons[i]);
		text(bold(), 21, x + 54, 45, c, names[i]);
		x += w;
	}
	clockAt(W - M, 44);
}

// A rounded button, with an icon; returns its width.
float button(float x, float y, const std::string& label, bool primary, bool focus, const char *icon = nullptr, float size = 23)
{
	const float h = 64;
	const float iw = icon ? measure(reg(), size - 2, icon).x + 14 : 0;
	const float w = measure(bold(), size, label).x + iw + 68;
	if (focus)
		shadow(x, y + 8, w, h, h / 2, alpha(col::focus, 0.6f), 24);
	const ImU32 fillC = focus ? col::focus : primary ? alpha(col::text, 0.92f) : alpha(col::card, 0.86f);
	const ImU32 ink = focus ? col::onFocus : primary ? col::dark : col::text;
	rect(x, y, w, h, fillC, h / 2);
	if (!focus && !primary)
		stroke(x, y, w, h, alpha(col::text, 0.10f), h / 2, 1.5f);
	if (focus)
		stroke(x - 6, y - 6, w + 12, h + 12, col::text, h / 2 + 6, 3);
	float tx = x + 34;
	if (icon)
	{
		text(reg(), size - 2, tx, y + (h - measure(reg(), size - 2, icon).y) / 2, ink, icon);
		tx += iw;
	}
	text(bold(), size, tx, y + (h - measure(bold(), size, label).y) / 2, ink, label);
	return w;
}

// A list's line.
void listLine(float x, float y, float w, float h, const kit::Line& l, bool focus)
{
	if (focus)
	{
		shadow(x, y + 6, w, h, 18, alpha(col::focus, 0.5f), 18);
		rect(x, y, w, h, col::focus, 18);
		// A sheen over its top half.
		rect(x, y, w, h * 0.5f, alpha(IM_COL32(255, 255, 255, 255), 0.07f), 18, ImDrawFlags_RoundCornersTop);
	}
	const ImU32 ink = focus ? col::onFocus : l.enabled ? col::text : col::faint;
	const ImU32 soft = focus ? alpha(col::onFocus, 0.82f) : col::sub;
	float lx = x + 26;
	if (l.icon)
	{
		const ImVec2 is = measure(reg(), 22, l.icon);
		text(reg(), 22, lx + (30 - is.x) / 2, y + (h - is.y) / 2, focus ? col::onFocus : l.enabled ? col::sub : col::faint, l.icon);
		lx += 48;
	}
	if (l.marked)
	{
		disc(lx + 4, y + h / 2, 5, focus ? col::onFocus : col::warm);
		lx += 20;
	}
	const float right = x + w - 26;
	float valueW = 0;
	if (l.kind == kit::Line::Toggle)
		valueW = 64;
	else if (!l.value.empty() || l.kind == kit::Line::Opens || l.kind == kit::Line::Choice)
	{
		std::string v = l.value;
		const bool arrows = l.kind == kit::Line::Choice && focus;
		const float room = w * 0.42f;
		v = fit(focus ? bold() : reg(), 21, v, room);
		float vw = measure(focus ? bold() : reg(), 21, v).x;
		float vx = right - vw;
		if (l.kind == kit::Line::Opens)
		{
			text(reg(), 16, right - 10, y + h / 2 - 10, soft, ICON_FA_CHEVRON_RIGHT);
			vx -= 30;
		}
		if (arrows)
		{
			text(reg(), 15, right - 10, y + h / 2 - 10, ink, ICON_FA_CHEVRON_RIGHT);
			vx -= 30;
			text(reg(), 15, vx - 26, y + h / 2 - 10, ink, ICON_FA_CHEVRON_LEFT);
		}
		const ImU32 vc = l.valueColour && !focus ? l.valueColour : focus ? col::onFocus : col::sub;
		text(focus ? bold() : reg(), 21, vx, y + h / 2 - 13, vc, v);
		valueW = right - vx + (arrows ? 30 : 0);
	}
	const float room = right - valueW - 24 - lx;
	if (l.note.empty())
		text(bold(), 22, lx, y + h / 2 - 14, ink, fit(bold(), 22, l.label, room));
	else
	{
		text(bold(), 22, lx, y + h / 2 - 27, ink, fit(bold(), 22, l.label, room));
		text(reg(), 17, lx, y + h / 2 + 4, soft, fit(reg(), 17, l.note, room));
	}
	if (l.kind == kit::Line::Toggle)
		switchAt(right, y + h / 2, l.on, focus);
}

// A screen's title line: where it is over it, its name, a note after it.
void header(const std::string& title, const std::string& over = "", const std::string& note = "")
{
	float y = 46;
	if (!over.empty())
	{
		caption(M, 40, over);
		y = 64;
	}
	text(bold(), 46, M, y, col::text, title);
	if (!note.empty())
		text(reg(), 21, M + measure(bold(), 46, title).x + 22, y + 20, col::sub, note);
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

bool forGame() { return !H->forGame.empty(); }

// Whether a setting is in the lists at a level: for one game, what it can have (at any level).
bool shown(const Setting& s, int level)
{
	if (forGame())
		return H->available ? H->available(s) : (s.has('g') || s.has('G'));
	return (int)s.level <= level && !s.has('G');
}

const char *levelNames[] = { "Essentials", "More", "Everything" };

// Each area's icon, in the table's order.
const char *areaIcon(int area)
{
	static const char *const icons[] = { ICON_FA_DISPLAY, ICON_FA_VOLUME_HIGH, ICON_FA_GAMEPAD, ICON_FA_FLOPPY_DISK,
			ICON_FA_MICROCHIP, ICON_FA_GLOBE, ICON_FA_PALETTE, ICON_FA_GEAR };
	return area >= 0 && area < (int)(sizeof(icons) / sizeof(icons[0])) ? icons[area] : ICON_FA_SLIDERS;
}

// The level switch at the top right of the settings.
void levelSwitch(float right, float y)
{
	if (forGame())
		return;
	float w = 0;
	for (const char *n : levelNames)
		w += measure(bold(), 19, n).x + 44;
	const float l1 = keyWidth("L1"), r1 = keyWidth("R1");
	float x = right - w - r1 - 14;
	key(x - 14 - l1, y + 22, "L1");
	key(right - r1, y + 22, "R1");
	rect(x - 5, y - 5, w + 10, 54, alpha(col::card, 0.8f), 27);
	stroke(x - 5, y - 5, w + 10, 54, alpha(col::text, 0.08f), 27, 1.5f);
	for (int i = 0; i < 3; i++)
	{
		const float bw = measure(bold(), 19, levelNames[i]).x + 44;
		if (i == st.level)
		{
			shadow(x, y, bw, 44, 22, alpha(col::focus, 0.45f), 10);
			rect(x, y, bw, 44, col::focus, 22);
		}
		text(bold(), 19, x + 22, y + 11, i == st.level ? col::onFocus : col::sub, levelNames[i]);
		x += bw;
	}
}

Kind kindOf(const Setting& s)
{
	return H->kind ? H->kind(s) : s.kind;
}

std::string valueOf(const Setting& s)
{
	if (H->value)
		return H->value(s);
	if (s.kind == Kind::Info || s.kind == Kind::Action)
		return H->text ? H->text(s) : "";
	return s.valueText(H->get ? H->get(s) : s.demo);
}

bool pinned(const Setting& s)
{
	return std::find(H->pins.begin(), H->pins.end(), s.id) != H->pins.end();
}

bool changedOf(const Setting& s)
{
	return H->changed && H->changed(s);
}

void change(const Setting& s, int dir)
{
	if (H->step)
	{
		H->step(s, dir);
		return;
	}
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

// A setting as a list's line.
kit::Line lineOf(const Setting& s)
{
	kit::Line l;
	l.label = s.label;
	l.marked = changedOf(s);
	switch (kindOf(s))
	{
	case Kind::Toggle:
		l.kind = kit::Line::Toggle;
		l.on = H->get ? H->get(s) != 0 : s.demo != 0;
		break;
	case Kind::Choice:
	case Kind::Range:
		l.kind = kit::Line::Choice;
		l.value = valueOf(s);
		break;
	case Kind::Action:
		l.kind = kit::Line::Opens;
		l.value = valueOf(s);
		break;
	default:
		l.kind = kit::Line::Info;
		l.value = valueOf(s);
		break;
	}
	if (pinned(s))
		l.note = ICON_FA_THUMBTACK "  Pinned";
	return l;
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
			if (shown(s, 2) && (st.virt == 1 ? pinned(s) : changedOf(s)))
				items.push_back({ &s, "" });
		return items;
	}
	std::string last;
	const auto extrasOf = [&](const std::string& section) {
		for (const Setting& e : H->extras)
			if (e.area == st.area && section == e.section)
				items.push_back({ &e, "" });
	};
	for (const Setting& s : settings())
	{
		if (s.area != st.area || !shown(s, st.level))
			continue;
		if (s.section != last)
		{
			if (!last.empty())
				extrasOf(last);
			items.push_back({ nullptr, s.section });
			last = s.section;
		}
		items.push_back({ &s, "" });
	}
	if (!last.empty())
		extrasOf(last);
	return items;
}

// The pane that says everything about the focused setting.
void detail(const Setting& s, float x, float y, float w)
{
	const float pad = 36;
	const float descH = measure(reg(), 21, s.desc, w - 2 * pad).y;
	const float labelH = measure(bold(), 30, s.label, w - 2 * pad).y;
	const float h = pad + 56 + labelH + 16 + descH + 34 + 4 * 46 + 40 + pad;
	surface(x, y, w, h, 28);
	glow(x + w - 60, y + 40, 260, alpha(col::focus, 0.10f));
	float cy = y + pad;
	// The area's icon, in a disc of the accent.
	disc(x + pad + 22, cy + 22, 22, alpha(col::focus, 0.22f));
	const ImVec2 is = measure(reg(), 20, areaIcon(s.area));
	text(reg(), 20, x + pad + 22 - is.x / 2, cy + 22 - is.y / 2, col::focus, areaIcon(s.area));
	const std::string where = std::string(areas()[(size_t)s.area].name) + "  \xC2\xB7  " + s.section;
	text(reg(), 18, x + pad + 58, cy + 11, col::faint, fit(reg(), 18, where, w - 2 * pad - 60));
	cy += 62;
	cy += text(bold(), 30, x + pad, cy, col::text, s.label, w - 2 * pad) + 16;
	cy += text(reg(), 21, x + pad, cy, mix(col::sub, col::text, 0.4f), s.desc, w - 2 * pad) + 30;
	const auto fact = [&](const char *icon, const char *k, const std::string& v) {
		dl->AddLine(V(x + pad, cy), V(x + w - pad, cy), alpha(col::text, 0.07f), 1.5f * S);
		text(reg(), 16, x + pad, cy + 15, col::faint, icon);
		text(reg(), 19, x + pad + 30, cy + 12, col::faint, k);
		const std::string shownV = fit(bold(), 19, v, w - 2 * pad - 180);
		text(bold(), 19, x + w - pad - measure(bold(), 19, shownV).x, cy + 12, col::text, shownV);
		cy += 46;
	};
	std::string choices;
	if (s.kind == Kind::Choice && !s.choices.empty())
	{
		for (const char *n : s.choices)
			choices += (choices.empty() ? "" : ", ") + std::string(n);
		if (measure(bold(), 19, choices).x > w - 2 * pad - 180)
			choices = std::to_string(s.choices.size()) + ", " + s.choices.front() + " to " + s.choices.back();
	}
	else if (s.kind == Kind::Range)
		choices = std::to_string(s.lo) + s.unit + " to " + std::to_string(s.hi) + s.unit;
	else if (s.kind == Kind::Toggle)
		choices = "On or Off";
	else
		choices = s.kind == Kind::Action ? "Opens a screen" : "Shown only";
	if (H->choices)
		choices = H->choices(s);
	fact(ICON_FA_LIST, "Choices", choices);
	fact(ICON_FA_LAYER_GROUP, "Applies to", s.has('G') ? "One game" : s.has('g') ? "Every game, or one game" : "Every game");
	fact(ICON_FA_CLOCK, "Takes effect", s.has('r') ? "When PSFlyCast starts again" : s.has('n') ? "At the next game" : "At once");
	fact(ICON_FA_SLIDERS, "Level", levelNames[(int)s.level]);
	if (s.source == 'F' || s.source == 'f')
		text(reg(), 16, x + pad, cy + 14, col::faint, "Description from Flycast");
}

// A list of settings with their values, grouped by section; returns the setting in focus.
const Setting *settingRows(const std::vector<Item>& items, int& focus, float x, float y, float w, float bottom, bool focused)
{
	std::vector<int> stops;
	for (int i = 0; i < (int)items.size(); i++)
		if (items[(size_t)i].s != nullptr)
			stops.push_back(i);
	if (stops.empty())
	{
		surface(x, y, w, 120, 22);
		text(reg(), 22, x + 30, y + 46, col::sub, st.virt == 1 ? "Nothing is pinned. Press Square on a setting to pin it here."
				: st.virt == 2 ? (forGame() ? "This game has none of its own: it plays with the settings for every game."
				: "Every setting is at its default.") : "Nothing here at this level: L1 / R1 shows more.", w - 60);
		return nullptr;
	}
	focus = std::clamp(focus, 0, (int)stops.size() - 1);
	const int at = stops[(size_t)focus];
	const float rowH = 74, headH = 58, groupGap = 14;
	const auto heightOf = [&](int i) { return items[(size_t)i].s ? rowH : headH + (i > 0 ? groupGap : 0); };
	float focusY = 0;
	for (int i = 0; i < at; i++)
		focusY += heightOf(i);
	const float view = bottom - y;
	const float target = std::clamp(focusY - view * 0.35f, 0.f, 1e9f);
	// The list starts on a whole row or heading, never on a cut one.
	float top = 0, snapped = 0;
	for (int i = 0; i < (int)items.size() && top <= target; i++)
	{
		snapped = top;
		top += heightOf(i);
	}
	st.scroll = snapped;
	dl->PushClipRect(V(x - 40, y - 4), V(x + w + 40, bottom), true);
	float cy = y - st.scroll;
	// The groups: the rows of a section on one card.
	for (int i = 0; i < (int)items.size();)
	{
		if (!items[(size_t)i].s)
		{
			if (i > 0)
				cy += groupGap;
			caption(x + 8, cy + 26, items[(size_t)i].heading);
			cy += headH;
			i++;
			continue;
		}
		int end = i;
		while (end < (int)items.size() && items[(size_t)end].s)
			end++;
		const float gh = (end - i) * rowH;
		if (cy < bottom && cy + gh > y - rowH)
		{
			surface(x, cy, w, gh, 22);
			for (int k = i; k < end; k++)
			{
				const float ry = cy + (k - i) * rowH;
				if (k > i && k != at && k - 1 != at)
					dl->AddLine(V(x + 26, ry), V(x + w - 26, ry), alpha(col::text, 0.07f), 1.5f * S);
			}
			for (int k = i; k < end; k++)
			{
				const float ry = cy + (k - i) * rowH;
				if (ry > bottom || ry + rowH < y - rowH)
					continue;
				const bool on = k == at && focused;
				listLine(on ? x - 6 : x, on ? ry - 2 : ry, on ? w + 12 : w, on ? rowH + 4 : rowH, lineOf(*items[(size_t)k].s), on);
			}
		}
		cy += gh;
		i = end;
	}
	dl->PopClipRect();
	return items[(size_t)at].s;
}

// ---------------------------------------------------------------- Home

const char *cardIcon(const std::string& title)
{
	std::string t;
	for (char c : title)
		t += (char)tolower((unsigned char)c);
	if (t.find("share") != std::string::npos || t.find("network") != std::string::npos) return ICON_FA_NETWORK_WIRED;
	if (t.find("vmu") != std::string::npos || t.find("card") != std::string::npos) return ICON_FA_SD_CARD;
	if (t.find("achievement") != std::string::npos) return ICON_FA_TROPHY;
	if (t.find("update") != std::string::npos) return ICON_FA_DOWNLOAD;
	if (t.find("usb") != std::string::npos || t.find("drive") != std::string::npos) return ICON_FA_FOLDER_OPEN;
	return ICON_FA_CIRCLE_INFO;
}

// A game's picture as the ground of a screen: large at the right, fading into the skin's ground.
void heroArt(const Game& g, float from = 560)
{
	if (g.cover != ImTextureID())
	{
		dl->AddImage(g.cover, V(from, -140), V(W + 40, 1220), ImVec2(0, 0.08f), ImVec2(1, 0.92f), IM_COL32(255, 255, 255, 215));
		glow(from + (W - from) * 0.5f, 380, 900, alpha(g.colour, 0.30f));
	}
	else
	{
		glow(W * 0.72f, 260, 1100, alpha(g.colour, 0.85f));
		glow(W * 0.80f, 300, 420, alpha(mix(g.colour, IM_COL32(255, 255, 255, 255), 0.35f), 0.45f));
	}
	hgradient(0, 0, from + 160, HH, col::bg, alpha(col::bg, 0.88f));
	hgradient(from + 160, 0, W - from - 160, HH, alpha(col::bg, 0.88f), alpha(col::bg, 0.0f));
	gradient(0, 520, W, 560, alpha(col::bg2, 0), col::bg2);
}

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
		else if (st.home == 2) h.game(hero, GameAct::Page);
		else h.game(h.recent[(size_t)(st.home - 2)], GameAct::Play);
	}
	if (in.triangle && st.home >= 3 && h.game)
		h.game(h.recent[(size_t)(st.home - 2)], GameAct::Page);
	if (in.r1) st.screen = Screen::Library;
	if (in.options) { st.back = Screen::Home; st.screen = Screen::SettingsHome; }

	backdrop();
	if (hero >= 0)
	{
		const Game& g = h.games[(size_t)hero];
		heroArt(g);
		caption(M, 214, "Continue playing", col::warm);
		text(bold(), 92, M - 4, 240, col::text, fit(bold(), 92, g.title, 1150));
		std::string meta = g.platform + "    " ICON_FA_CLOCK "  " + g.played + "    Played " + g.lastPlayed;
		if (!g.stateWhen.empty())
			meta += "    " ICON_FA_FLOPPY_DISK "  Saved " + g.stateWhen;
		text(reg(), 23, M, 358, col::sub, meta);
		float bx = M;
		bx += button(bx, 424, "Resume", true, st.home == 0, ICON_FA_PLAY, 24) + 20;
		bx += button(bx, 424, "Start From the Beginning", false, st.home == 1, ICON_FA_ROTATE_LEFT) + 20;
		button(bx, 424, "Game Page", false, st.home == 2, ICON_FA_CIRCLE_INFO);
	}
	else
	{
		text(bold(), 76, M, 236, col::text, h.games.empty() ? "No games yet" : "Nothing played yet");
		text(reg(), 25, M + 2, 340, col::sub, h.games.empty() ? "Add a folder of games in Settings > Games and Saves > Folders."
				: "A game you play shows up here, to continue in one press.");
	}
	// What is true now.
	float cy = 196;
	for (const Card& c : h.now)
	{
		if (cy > 520)
			break;
		const float cx = W - M - 470;
		surface(cx, cy, 470, 96, 24);
		disc(cx + 48, cy + 48, 24, alpha(col::focus, 0.2f));
		const char *icon = cardIcon(c.title);
		const ImVec2 is = measure(reg(), 20, icon);
		text(reg(), 20, cx + 48 - is.x / 2, cy + 48 - is.y / 2, col::focus, icon);
		text(bold(), 21, cx + 90, cy + 20, col::text, c.title);
		text(reg(), 18, cx + 90, cy + 52, col::sub, fit(reg(), 18, c.body, 360));
		cy += 112;
	}
	// The games played lately.
	if (lately > 0)
	{
		caption(M, 642, "Played lately");
		const float tile = (W - 2 * M - 6 * 26) / 7.f;
		for (int k = 0; k < lately; k++)
		{
			const Game& g = h.games[(size_t)h.recent[(size_t)(k + 1)]];
			const float tx = M + k * (tile + 26);
			cover(g, tx, 684, tile, tile, 22, st.home == 3 + k);
			if (st.home == 3 + k)
			{
				text(bold(), 21, tx, 684 + tile + 22, col::text, fit(bold(), 21, g.title, tile + 60));
				text(reg(), 17, tx, 684 + tile + 50, col::sub, fit(reg(), 17, g.platform + "  \xC2\xB7  " + g.lastPlayed, tile + 60));
			}
		}
	}
	topBar(0);
	hints({ { { "circle" }, "Quit" }, { { "OPTIONS" }, "Settings" }, { { "triangle" }, "Game Page" },
			{ { "cross" }, st.home == 0 ? "Resume" : st.home == 1 ? "Start" : st.home == 2 ? "Open" : "Play" } });
}

// ---------------------------------------------------------------- the Library

const char *chips[] = { "All", "Console", "USB", "Share", "Favourites", "Dreamcast", "Arcade", "Never Played" };
const char *chipIcons[] = { ICON_FA_LAYER_GROUP, ICON_FA_FOLDER, ICON_FA_FOLDER_OPEN, ICON_FA_NETWORK_WIRED, ICON_FA_STAR,
		ICON_FA_COMPACT_DISC, ICON_FA_GAMEPAD, ICON_FA_CIRCLE_PLAY };

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
	if (n > 0 && in.triangle && H->game) H->game(list[(size_t)st.lib], GameAct::Page);

	backdrop();
	if (n > 0)
	{
		// The focused game's colour lights the ground.
		const Game& g = H->games[(size_t)list[(size_t)st.lib]];
		glow(W * 0.5f, 1180, 1000, alpha(g.colour, 0.45f));
	}
	topBar(1);
	// The filters: where the games are, then what they are.
	float x = M;
	key(x, 169, "L2");
	x += keyWidth("L2") + 14;
	for (int i = 0; i < 8; i++)
	{
		if (i == 4)
		{
			rect(x + 4, 152, 2, 34, alpha(col::text, 0.12f), 1);
			x += 18;
		}
		const bool on = i == st.chip;
		const float w = measure(on ? bold() : reg(), 20, chips[i]).x + 76;
		if (on)
		{
			shadow(x, 144, w, 50, 25, alpha(col::focus, 0.45f), 12);
			rect(x, 144, w, 50, col::focus, 25);
		}
		else
		{
			rect(x, 144, w, 50, alpha(col::card, 0.8f), 25);
			stroke(x, 144, w, 50, alpha(col::text, 0.07f), 25, 1.5f);
		}
		text(reg(), 16, x + 22, 159, on ? col::onFocus : col::faint, chipIcons[i]);
		text(on ? bold() : reg(), 20, x + 50, 157, on ? col::onFocus : col::sub, chips[i]);
		x += w + 10;
	}
	key(x + 4, 169, "R2");
	const std::string count = std::to_string(n) + " games  \xC2\xB7  By name";
	text(reg(), 20, W - M - measure(reg(), 20, count).x, 157, col::sub, count);
	const float gap = 24, cw = (W - 2 * M - gap * (cols - 1)) / cols;
	const int focusRow = st.lib / cols;
	const float scroll = std::max(0, focusRow - 1) * (cw + gap);
	dl->PushClipRect(V(0, 214), V(W, 1080), true);
	for (int i = 0; i < n; i++)
	{
		const float cx = M + (i % cols) * (cw + gap), cy = 238 + (i / cols) * (cw + gap) - scroll;
		if (cy > 1080 || cy + cw < 210 || i == st.lib)
			continue;
		cover(H->games[(size_t)list[(size_t)i]], cx, cy, cw, cw, 20, false);
	}
	if (n > 0)
	{
		const int i = st.lib;
		cover(H->games[(size_t)list[(size_t)i]], M + (i % cols) * (cw + gap), 238 + (i / cols) * (cw + gap) - scroll, cw, cw, 20, true);
	}
	dl->PopClipRect();
	// The focused game, on a frosted strip at the bottom.
	gradient(0, 790, W, 110, alpha(col::bg2, 0), col::bg2);
	rect(0, 900, W, 180, col::bg2);
	if (n > 0)
	{
		const Game& g = H->games[(size_t)list[(size_t)st.lib]];
		text(bold(), 38, M, 908, col::text, fit(bold(), 38, g.title, 1000));
		text(reg(), 21, M, 962, col::sub, fit(reg(), 21, g.platform + "  \xC2\xB7  " + g.place + "  \xC2\xB7  " + g.line, 1000));
	}
	hints({ { { "circle" }, "Quit" }, { { "OPTIONS" }, "Settings" }, { { "triangle" }, "Game Page" }, { { "cross" }, "Play" } });
}

// ---------------------------------------------------------------- a game's page

void pageView(const PageView& p)
{
	const Game& g = p.game;
	const float t = p.open;
	backdrop();
	heroArt(g, 760);
	const float dy = (1 - t) * 30;
	// The cover, and what it is.
	const float cs = 300;
	cover(g, M, 120 + dy, cs, cs, 26, false);
	const float x = M + cs + 48;
	caption(x, 132 + dy, g.platform + "  \xC2\xB7  " + g.place, col::warm);
	text(bold(), 64, x - 3, 156 + dy, col::text, fit(bold(), 64, g.title, W - M - x));
	text(reg(), 22, x, 244 + dy, col::sub, fit(reg(), 22, g.line, W - M - x));
	// The discs of its set, under what it is.
	if (!p.discs.empty())
	{
		float cx = x;
		for (int i = 0; i < (int)p.discs.size(); i++)
		{
			const bool cur = i == p.disc;
			const float w = measure(bold(), 18, p.discs[(size_t)i]).x + 56;
			rect(cx, 282 + dy, w, 36, cur ? alpha(col::text, 0.92f) : alpha(col::card, 0.85f), 18);
			text(reg(), 14, cx + 16, 291 + dy, cur ? col::dark : col::faint, ICON_FA_COMPACT_DISC);
			text(bold(), 18, cx + 38, 289 + dy, cur ? col::dark : col::sub, p.discs[(size_t)i]);
			cx += w + 10;
		}
		cx += 8;
		cx += key(cx, 300 + dy, "square") + 8;
		text(reg(), 18, cx, 289 + dy, col::faint, "Next disc");
	}
	// The buttons.
	float bx = x;
	const float by = p.discs.empty() ? 318 : 342;
	for (int i = 0; i < (int)p.actions.size(); i++)
	{
		const bool on = i == p.action;
		const bool primary = i == 0;
		const bool usable = p.enabled.empty() || p.enabled[(size_t)i];
		const float bw = button(bx, by + dy, p.actions[(size_t)i].second, primary && usable, on && !p.inList, p.actions[(size_t)i].first,
				primary ? 24 : 21);
		if (!usable && !(on && !p.inList))
			rect(bx, by + dy, bw, 64, alpha(col::bg, 0.45f), 32);
		if (on && p.inList)
			rect(bx + bw / 2 - 18, by + dy + 74, 36, 5, col::focus, 2.5f);
		bx += bw + 16;
	}
	if (!p.inList && !p.note.empty())
		text(reg(), 19, x, by + dy + 82, col::faint, p.note);
	const float top = 470 + dy + (p.discs.empty() ? 0 : 24), bottom = 990;
	if (p.lines.empty() && p.listTitle.empty())
	{
		// The overview: the facts, then what the game is.
		const int n = (int)p.facts.size();
		if (n > 0)
		{
			const float fw = (W - 2 * M - (n - 1) * 20) / n;
			for (int i = 0; i < n; i++)
			{
				const float fx = M + i * (fw + 20);
				surface(fx, top, fw, 120, 22);
				caption(fx + 26, top + 26, p.facts[(size_t)i].first);
				text(bold(), 27, fx + 26, top + 58, col::text, fit(bold(), 27, p.facts[(size_t)i].second, fw - 52));
			}
		}
		float ay = top + (n > 0 ? 160 : 0);
		if (!g.about.empty())
		{
			caption(M, ay, "About");
			ay += 32;
			ay += text(reg(), 22, M, ay, mix(col::sub, col::text, 0.3f), g.about, W - 2 * M) + 20;
		}
		if (!g.file.empty())
			text(reg(), 18, M, ay + 8, col::faint, ICON_FA_FOLDER "   " + fit(reg(), 18, g.file, W - 2 * M - 40));
	}
	else
	{
		caption(M, top, p.listTitle);
		const float ly = top + 36;
		const int n = (int)p.lines.size();
		if (n == 0)
		{
			surface(M, ly, W - 2 * M, 110, 22);
			text(reg(), 22, M + 30, ly + 42, col::sub, p.empty, W - 2 * M - 60);
		}
		else
		{
			const float rowH = 72;
			const int fits = std::max(1, (int)((bottom - ly - (p.note.empty() ? 0 : 60)) / rowH));
			const int first = std::clamp(p.line - fits / 2, 0, std::max(0, n - fits));
			const int shownN = std::min(fits, n - first);
			surface(M, ly, W - 2 * M, shownN * rowH, 22);
			for (int k = 0; k < shownN; k++)
			{
				const int i = first + k;
				const float ry = ly + k * rowH;
				const bool on = p.inList && i == p.line;
				if (k > 0 && !on && !(p.inList && i - 1 == p.line))
					dl->AddLine(V(M + 26, ry), V(W - M - 26, ry), alpha(col::text, 0.07f), 1.5f * S);
				listLine(on ? M - 6 : M, on ? ry - 2 : ry, on ? W - 2 * M + 12 : W - 2 * M, on ? rowH + 4 : rowH, p.lines[(size_t)i], on);
			}
			if (!p.note.empty())
				text(reg(), 20, M + 8, ly + shownN * rowH + 22, col::faint, p.note, W - 2 * M - 16);
		}
	}
	hints(p.hints);
}

// The page of the preview's games, as the console's host would hand it over.
void gamePage(const Input& in)
{
	if (st.game < 0 || st.game >= (int)H->games.size())
	{
		st.screen = st.back;
		return;
	}
	if (in.left && st.page > 0) st.page--;
	if (in.right && st.page < 4) st.page++;
	if (in.back) st.screen = st.back;
	const Game& g = H->games[(size_t)st.game];
	PageView p;
	p.game = g;
	p.actions = { { ICON_FA_PLAY, "Play" }, { ICON_FA_CLOCK_ROTATE_LEFT, "Load State" }, { ICON_FA_SLIDERS, "Its Settings" },
			{ ICON_FA_BOLT, "Cheats" }, { ICON_FA_GEAR, "Manage" } };
	p.action = st.page;
	p.facts = { { "Played", g.played }, { "Last played", g.lastPlayed }, { "Save states", std::to_string(g.states) },
			{ "Its own settings", g.ownSettings ? std::to_string(g.ownSettings) : "None" },
			{ "Achievements", g.achievements ? std::to_string(g.achieved) + " of " + std::to_string(g.achievements) : "None" } };
	p.hints = { { { "circle" }, "Back" }, { { "dpad" }, "Choose" }, { { "cross" }, st.page == 0 ? "Play" : "Open" } };
	pageView(p);
}

// ---------------------------------------------------------------- the quick menu

ImVec4 quickView(const QuickView& q)
{
	const float t = q.open;
	const float pw = q.width;
	const float px = W - pw * (0.2f + 0.8f * t);
	// The paused game, dimmed, and the panel's shadow over it.
	rect(0, 0, W, HH, alpha(IM_COL32(0, 0, 0, 255), 0.45f * t));
	hgradient(px - 220, 0, 220, HH, IM_COL32(0, 0, 0, 0), alpha(IM_COL32(0, 0, 0, 255), 0.5f * t));
	// The panel: the skin's ground, frosted, lit by the accent at its top.
	gradient(px, 0, pw, HH, alpha(col::bg, 0.93f), alpha(col::bg2, 0.96f));
	dl->PushClipRect(V(px, 0), V(W, HH), true);
	glow(px + pw * 0.3f, -60, 620, alpha(col::focus, 0.22f));
	if (q.game.cover == ImTextureID())
		glow(px + pw * 0.9f, 40, 420, alpha(q.game.colour, 0.6f));
	dl->PopClipRect();
	dl->AddLine(V(px, 0), V(px, HH), alpha(col::text, 0.10f), 1.5f * S);
	// The game.
	const float gx = px + 48;
	cover(q.game, gx, 56, 128, 128, 18, false);
	caption(gx + 156, 68, q.game.platform);
	text(bold(), 33, gx + 154, 92, col::text, fit(bold(), 33, q.game.title, pw - 48 - 156 - 40));
	{
		const std::string s = std::string(ICON_FA_PAUSE) + "  " + q.status;
		const float sw = measure(bold(), 17, s).x + 30;
		rect(gx + 156, 142, sw, 34, alpha(col::focus, 0.2f), 17);
		text(bold(), 17, gx + 171, 149, col::focus, s);
	}
	const float top = 226;
	if (q.page)
	{
		if (!q.pageTitle.empty())
			text(bold(), 30, gx, top, col::text, q.pageTitle);
		hints(q.hints, px + pw - 48, HH - 44);
		return ImVec4(px, top + (q.pageTitle.empty() ? 0 : 60), pw, HH - 96 - top - (q.pageTitle.empty() ? 0 : 60));
	}
	const int n = (int)q.lines.size();
	const float ih = std::min(72.f, std::floor((HH - 110 - top) / std::max(1, n)));
	if (q.arranging)
		caption(gx, top - 30, "Arrange: up and down move, Square hides", col::warm);
	for (int i = 0; i < n; i++)
	{
		const float y = top + i * ih;
		const bool on = i == q.focus;
		kit::Line l = q.lines[(size_t)i];
		listLine(on ? gx - 22 : gx - 16, y, pw - 2 * 48 + (on ? 44 : 32), ih - 6, l, on);
	}
	// The state in the slot, beside the panel.
	if (q.showState)
	{
		const float cw = 440, chh = 336;
		const float cx = px - 36 - cw;
		const float rowY = top + q.focus * ih + ih / 2;
		const float cy = std::clamp(rowY - chh / 2, 40.f, HH - chh - 40);
		surface(cx, cy, cw, chh, 24);
		const float iw = cw - 36, ihh = iw * 9 / 16;
		if (q.statePicture != ImTextureID())
			dl->AddImageRounded(q.statePicture, V(cx + 18, cy + 18), V(cx + 18 + iw, cy + 18 + ihh), ImVec2(0, 0), ImVec2(1, 1),
					IM_COL32_WHITE, 16 * S);
		else
		{
			rect(cx + 18, cy + 18, iw, ihh, alpha(col::text, 0.06f), 16);
			const std::string e = ICON_FA_BOX_OPEN "   Empty slot";
			const ImVec2 es = measure(reg(), 24, e);
			text(reg(), 24, cx + 18 + (iw - es.x) / 2, cy + 18 + (ihh - es.y) / 2, col::faint, e);
		}
		text(bold(), 25, cx + 22, cy + ihh + 36, col::text, q.stateTitle);
		text(reg(), 20, cx + cw - 22 - measure(reg(), 20, q.stateWhen).x, cy + ihh + 40, col::sub, q.stateWhen);
	}
	hints(q.hints, px + pw - 48, HH - 44);
	return ImVec4(px, top, pw, HH - 96 - top);
}

// The quick menu of the preview's game, as the console's host would hand it over.
void quickMenu(const Input& in)
{
	const Game *g = H->running >= 0 && H->running < (int)H->games.size() ? &H->games[(size_t)H->running] : nullptr;
	QuickView q;
	if (g)
		q.game = *g;
	q.status = "Paused";
	const struct { const char *icon; const char *label; std::string value; kit::Line::Kind kind; } items[] = {
		{ ICON_FA_PLAY, "Resume", "", kit::Line::Plain },
		{ ICON_FA_DOWNLOAD, "Save state", "", kit::Line::Plain },
		{ ICON_FA_CLOCK_ROTATE_LEFT, "Load state", "", kit::Line::Plain },
		{ ICON_FA_LAYER_GROUP, "State slot", "Slot 1", kit::Line::Choice },
		{ ICON_FA_BACKWARD, "Rewind", "", kit::Line::Opens },
		{ ICON_FA_FORWARD, "Fast forward", "Off (2x)", kit::Line::Plain },
		{ ICON_FA_SD_CARD, "Swap memory card", "Card 2", kit::Line::Choice },
		{ ICON_FA_BOLT, "Cheats", "1 on", kit::Line::Opens },
		{ ICON_FA_STOPWATCH, "Speedrun timer", "", kit::Line::Opens },
		{ ICON_FA_MAGNIFYING_GLASS, "Find in memory", "", kit::Line::Opens },
		{ ICON_FA_TROPHY, "Achievements", "12 of 40", kit::Line::Opens },
		{ ICON_FA_SLIDERS, "Game options", "2 of its own", kit::Line::Opens },
		{ ICON_FA_GAMEPAD, "Controls", "", kit::Line::Opens },
		{ ICON_FA_POWER_OFF, "Restart game", "", kit::Line::Plain },
		{ ICON_FA_RIGHT_FROM_BRACKET, "Quit game", "", kit::Line::Plain },
	};
	for (const auto& it : items)
	{
		kit::Line l;
		l.icon = it.icon;
		l.label = it.label;
		l.value = it.value;
		l.kind = it.kind;
		q.lines.push_back(l);
	}
	const int n = (int)q.lines.size();
	if (in.up) st.quick = (st.quick + n - 1) % n;
	if (in.down) st.quick = (st.quick + 1) % n;
	if (in.back && H->close)
	{
		H->close();
		return;
	}
	q.focus = st.quick;
	q.showState = st.quick >= 1 && st.quick <= 3;
	q.stateTitle = "Slot 1";
	q.stateWhen = g ? g->stateWhen : "";
	q.hints = { { { "circle" }, "Resume" }, { { "triangle" }, "Arrange" }, { { "cross" }, "Select" } };
	quickView(q);
}

// ---------------------------------------------------------------- the settings

void settingsHeader(const std::string& place)
{
	const std::string note = forGame() ? "For " + H->forGame + " only" : H->running >= 0 ? "For every game" : "For every game";
	if (place.empty())
		header(forGame() ? "Its Settings" : "Settings", "", note);
	else
		header(place, forGame() ? "Its Settings" : "Settings", note);
	levelSwitch(W - M, 48);
}

// The search's field.
void searchField(float x, float y, float w, float h, const std::string& shownText, bool typed, bool focus)
{
	if (focus)
		shadow(x, y + 8, w, h, h / 2, alpha(col::focus, 0.5f), 22);
	rect(x, y, w, h, alpha(col::card, 0.86f), h / 2);
	stroke(x, y, w, h, focus ? col::text : alpha(col::text, 0.10f), h / 2, focus ? 3.f : 1.5f);
	text(reg(), 24, x + 34, y + h / 2 - 15, focus ? col::text : col::sub, ICON_FA_MAGNIFYING_GLASS);
	text(typed ? bold() : reg(), typed ? 27 : 24, x + 80, y + h / 2 - (typed ? 17 : 15), typed ? col::text : col::sub, shownText);
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
	if (in.square && forGame() && H->resetAll)
		H->resetAll();
	if (in.back || in.options)
	{
		// Closed: nothing more is asked of the host this frame (what it bound may be gone).
		if (H->close)
		{
			H->close();
			return;
		}
		st.screen = st.back;
	}

	backdrop();
	settingsHeader("");
	// The search.
	int all = 0;
	for (const Setting& s : settings())
		all += shown(s, 2);
	searchField(M, 136, W - 2 * M, 80, forGame() ? "Search this game's " + std::to_string(all) + " settings"
			: "Search all " + std::to_string(all) + " settings and your games", false, f < 0);
	key(W - M - 54, 176, "triangle");
	// Pinned, and changed from the default.
	int changed = 0, pinnedCount = 0;
	std::string pinnedNames, pinnedValues;
	for (const Setting& s : settings())
	{
		if (!shown(s, 2))
			continue;
		if (changedOf(s))
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
	const float tw = (W - 2 * M - 24) / 2;
	const struct { const char *icon; const char *label; std::string big, small; } tops[] = {
		{ ICON_FA_THUMBTACK, "Pinned", pinnedCount ? pinnedNames : "Nothing pinned", pinnedCount ? pinnedValues : "Press Square on a setting to pin it" },
		{ ICON_FA_ROTATE_LEFT, forGame() ? "Its own values" : "Changed from default", std::to_string(changed) + (changed == 1 ? " setting" : " settings"),
				forGame() ? "Set for this game, over those for every game" : "Review them, or reset one to its default" },
	};
	for (int i = 0; i < 2; i++)
	{
		const float x = M + i * (tw + 24), y = 252;
		const bool on = f == i;
		surface(x, y, tw, 118, 24, on);
		disc(x + 54, y + 59, 28, alpha(col::focus, on ? 0.35f : 0.18f));
		const ImVec2 is = measure(reg(), 22, tops[i].icon);
		text(reg(), 22, x + 54 - is.x / 2, y + 59 - is.y / 2, on ? col::text : col::focus, tops[i].icon);
		caption(x + 104, y + 22, tops[i].label);
		text(bold(), 24, x + 104, y + 46, col::text, fit(bold(), 24, tops[i].big, tw - 130));
		text(reg(), 18, x + 104, y + 82, col::sub, fit(reg(), 18, tops[i].small, tw - 130));
	}
	// The areas.
	const float aw = (W - 2 * M - 3 * 24) / 4, ah = 260;
	for (int a = 0; a < areaCount; a++)
	{
		const float x = M + (a % 4) * (aw + 24), y = 402 + (a / 4) * (ah + 24);
		const bool on = f == 2 + a;
		int count = 0, ch = 0;
		std::vector<const Setting *> firsts;
		for (const Setting& s : settings())
			if (s.area == a && shown(s, st.level))
			{
				count++;
				if (changedOf(s))
					ch++;
				if (s.kind != Kind::Action && s.kind != Kind::Info && firsts.size() < 2 && (forGame() || s.level == Level::Essentials))
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
		surface(x, y, aw, ah, 26, on);
		if (on)
			glow(x + 70, y + 70, 220, alpha(col::focus, 0.25f));
		// Its icon, in a disc of the accent.
		disc(x + 58, y + 58, 32, on ? col::focus : alpha(col::focus, 0.18f));
		const ImVec2 is = measure(reg(), 26, areaIcon(a));
		text(reg(), 26, x + 58 - is.x / 2, y + 58 - is.y / 2, on ? col::onFocus : col::focus, areaIcon(a));
		const std::string c = std::to_string(count);
		text(bold(), 18, x + aw - 28 - measure(bold(), 18, c).x, y + 34, count ? col::faint : alpha(col::faint, 0.5f), c);
		text(bold(), 29, x + 28, y + 108, count ? col::text : col::faint, areas()[(size_t)a].name);
		text(reg(), 17, x + 28, y + 148, col::faint, ch ? std::to_string(ch) + (forGame() ? " of its own" : " changed") : fit(reg(), 17,
				areas()[(size_t)a].blurb, aw - 56));
		if (ch)
			disc(x + 28 + measure(reg(), 17, std::to_string(ch) + (forGame() ? " of its own" : " changed")).x + 12, y + 158, 4.5f, col::warm);
		float ly = y + ah - 26 - 28 * (float)firsts.size();
		for (const Setting *s : firsts)
		{
			const std::string v = valueOf(*s);
			const std::string lv = fit(reg(), 18, s->label, aw * 0.58f);
			text(reg(), 18, x + 28, ly, col::sub, lv);
			const std::string vv = fit(bold(), 18, v, aw - 56 - measure(reg(), 18, lv).x - 16);
			text(bold(), 18, x + aw - 28 - measure(bold(), 18, vv).x, ly, col::text, vv);
			ly += 28;
		}
	}
	if (forGame())
		hints({ { { "circle" }, "Close" }, { { "square" }, "All Back to the Settings for Every Game" }, { { "triangle" }, "Search" },
				{ { "cross" }, "Open" } });
	else
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
			if (kindOf(*focused) == Kind::Action && H->run)
				H->run(*focused);
			else if (kindOf(*focused) == Kind::Toggle)
				change(*focused, 1);
		}
		if (in.square && !forGame())
		{
			auto at = std::find(H->pins.begin(), H->pins.end(), focused->id);
			if (at == H->pins.end())
				H->pins.push_back(focused->id);
			else
				H->pins.erase(at);
		}
		if (in.r3 && H->reset)
			H->reset(*focused);
	}
	if (in.triangle) { if (H->typeQuery) H->typeQuery(); st.result = 0; st.screen = Screen::Search; }
	if (in.back) st.screen = Screen::SettingsHome;
	if (in.options)
	{
		// Closed: nothing more is asked of the host this frame (what it bound may be gone).
		if (H->close)
		{
			H->close();
			return;
		}
		st.screen = st.back;
	}

	backdrop();
	settingsHeader(st.virt == 1 ? "Pinned" : st.virt == 2 ? (forGame() ? "Its Own Values" : "Changed From Default")
			: areas()[(size_t)st.area].name);
	// The sections, which follow the focus.
	const float top = 150, bottom = 996;
	std::string current;
	if (focused)
		current = focused->section;
	const float sw = 380;
	if (st.virt == 0)
	{
		std::vector<std::string> secs;
		for (const std::string& sec : sections(st.area))
		{
			bool any = false;
			for (const Item& it : items)
				any = any || (it.s && sec == it.s->section);
			if (any || !forGame())
				secs.push_back(sec);
		}
		surface(M, top, sw, 22 + 60 * (float)secs.size(), 24);
		float y = top + 11;
		for (const std::string& sec : secs)
		{
			int count = 0;
			for (const Setting& s : settings())
				if (s.area == st.area && s.section == sec && shown(s, st.level))
					count++;
			const bool on = sec == current;
			if (on)
			{
				rect(M + 10, y, sw - 20, 56, alpha(col::focus, 0.2f), 16);
				rect(M + 10, y + 14, 4, 28, col::focus, 2);
			}
			text(on ? bold() : reg(), 20, M + 32, y + 16, count ? (on ? col::text : col::sub) : col::faint, fit(reg(), 20, sec, sw - 110));
			const std::string c = std::to_string(count);
			text(bold(), 16, M + sw - 30 - measure(bold(), 16, c).x, y + 19, on ? col::focus : col::faint, c);
			y += 60;
		}
	}
	const float dw = 540;
	const float lx = st.virt == 0 ? M + sw + 30 : M;
	const float rw = W - M - dw - 30 - lx;
	settingRows(items, st.row, lx, top, rw, bottom, true);
	if (focused)
		detail(*focused, W - M - dw, top, dw);
	if (forGame())
		hints({ { { "circle" }, "Back" }, { { "R3" }, "Back to the Settings for Every Game" }, { { "triangle" }, "Search" },
				{ { "cross" }, focused && kindOf(*focused) == Kind::Action ? "Open" : "Change" } });
	else
		hints({ { { "circle" }, "Back" }, { { "L1", "R1" }, "Level" }, { { "R3" }, "Reset to Default" },
				{ { "square" }, focused && pinned(*focused) ? "Unpin" : "Pin" }, { { "triangle" }, "Search" },
				{ { "cross" }, focused && kindOf(*focused) == Kind::Action ? "Open" : "Change" } });
}

void searchScreen(const Input& in)
{
	std::vector<const Setting *> found;
	for (const Setting *s : search(H->query))
		if (shown(*s, 2))
			found.push_back(s);
	std::vector<int> games;
	if (!forGame())
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
	const int stops = (int)found.size() + (int)games.size();
	if (in.up && st.result > 0) st.result--;
	if (in.down && st.result < stops - 1) st.result++;
	st.result = std::clamp(st.result, 0, std::max(0, stops - 1));
	const bool onGame = st.result >= (int)found.size();
	const Setting *focused = !onGame && !found.empty() ? found[(size_t)st.result] : nullptr;
	if (focused && (in.left || in.right))
		change(*focused, in.right ? 1 : -1);
	if (focused && in.accept && kindOf(*focused) == Kind::Action && H->run)
		H->run(*focused);
	if (focused && in.accept && kindOf(*focused) == Kind::Toggle)
		change(*focused, 1);
	if (focused && in.square && !forGame())
	{
		auto at = std::find(H->pins.begin(), H->pins.end(), focused->id);
		if (at == H->pins.end())
			H->pins.push_back(focused->id);
		else
			H->pins.erase(at);
	}
	if (onGame && in.accept && !games.empty() && H->game)
		H->game(games[(size_t)(st.result - (int)found.size())], GameAct::Page);
	if (in.triangle && H->typeQuery) H->typeQuery();
	if (in.back) st.screen = Screen::SettingsHome;

	backdrop();
	const float dw = 540, rw = W - M - dw - 30 - M;
	searchField(M, 44, rw, 80, H->query.empty() ? "Type to search" : H->query, !H->query.empty(), false);
	const std::string n = std::to_string(found.size()) + (found.size() == 1 ? " setting" : " settings")
			+ (forGame() ? "" : "  \xC2\xB7  " + std::to_string(games.size()) + (games.size() == 1 ? " game" : " games"));
	text(reg(), 20, M + rw - 30 - measure(reg(), 20, n).x, 72, col::faint, n);
	const float top = 162;
	float y = top;
	const float rowH = 84;
	// Where the focus is, kept in view.
	const int fits = (int)((996 - top - 60) / rowH);
	const int first = std::clamp(st.result - fits / 2, 0, std::max(0, stops - fits));
	dl->PushClipRect(V(M - 30, top - 6), V(M + rw + 30, 1000), true);
	int shownCount = 0;
	if (!found.empty() && first < (int)found.size())
	{
		caption(M + 8, y, "Settings");
		y += 34;
		const int last = std::min((int)found.size(), first + fits);
		surface(M, y, rw, (last - first) * rowH, 22);
		for (int i = first; i < last; i++)
		{
			const Setting& s = *found[(size_t)i];
			const bool on = st.result == i;
			kit::Line l = lineOf(s);
			l.note = std::string(areas()[(size_t)s.area].name) + "  \xE2\x80\xBA  " + s.section
					+ (s.level == Level::Everything ? "  \xC2\xB7  Everything" : "");
			l.icon = areaIcon(s.area);
			listLine(on ? M - 6 : M, on ? y - 2 : y, on ? rw + 12 : rw, on ? rowH + 4 : rowH, l, on);
			y += rowH;
			shownCount++;
		}
	}
	if (!games.empty() && shownCount < fits)
	{
		y += 24;
		caption(M + 8, y, "Games");
		y += 34;
		const int from = std::max(0, first - (int)found.size());
		const int last = std::min((int)games.size(), from + fits - shownCount);
		surface(M, y, rw, (last - from) * rowH, 22);
		for (int k = from; k < last; k++)
		{
			const Game& g = H->games[(size_t)games[(size_t)k]];
			const bool on = st.result == (int)found.size() + k;
			kit::Line l;
			l.label = g.title;
			l.note = g.platform + "  \xC2\xB7  " + g.place;
			l.value = "Game Page";
			l.kind = kit::Line::Opens;
			l.icon = ICON_FA_COMPACT_DISC;
			listLine(on ? M - 6 : M, on ? y - 2 : y, on ? rw + 12 : rw, on ? rowH + 4 : rowH, l, on);
			y += rowH;
		}
	}
	if (stops == 0)
	{
		surface(M, top, rw, 110, 22);
		text(reg(), 22, M + 30, top + 42, col::sub, H->query.empty() ? "Press Triangle and type a word." : "Nothing has that word.");
	}
	dl->PopClipRect();
	if (focused)
		detail(*focused, W - M - dw, top, dw);
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

	backdrop();
	header("Look For My Files", "Settings  \xE2\x80\xBA  Games and Saves");
	const float sw = 380;
	surface(M, 150, sw, 300, 24);
	caption(M + 28, 176, "Looked through");
	text(reg(), 20, M + 28, 214, col::focus, ICON_FA_FOLDER_OPEN);
	text(bold(), 21, M + 62, 212, col::text, fit(bold(), 21, H->foundPlace, sw - 90));
	text(reg(), 19, M + 28, 266, col::sub, "Nothing is moved or copied. PSFlyCast reads these files, and writes what it keeps, where they are.", sw - 56);
	int using_ = 0;
	const float lx = M + sw + 30, rw = W - M - lx, rowH = 84;
	float y = 150;
	surface(lx, y, rw, n * rowH, 22);
	for (int i = 0; i < n; i++)
	{
		const FoundRow& r = H->found[(size_t)i];
		const bool on = st.found == i;
		kit::Line l;
		l.icon = r.blocked ? ICON_FA_CIRCLE_INFO : ICON_FA_FOLDER;
		l.label = r.kind + "  \xC2\xB7  " + r.what;
		l.note = r.folder + (r.note.empty() ? "" : "  \xC2\xB7  " + r.note);
		l.kind = r.blocked ? kit::Line::Info : kit::Line::Toggle;
		l.on = r.on;
		l.enabled = !r.blocked;
		if (i > 0 && !on && st.found != i - 1)
			dl->AddLine(V(lx + 26, y), V(lx + rw - 26, y), alpha(col::text, 0.07f), 1.5f * S);
		listLine(on ? lx - 6 : lx, on ? y - 2 : y, on ? rw + 12 : rw, on ? rowH + 4 : rowH, l, on);
		using_ += r.on && !r.blocked;
		y += rowH;
	}
	y += 30;
	const bool bOn = st.found == n;
	const std::string label = "Use These " + std::to_string(using_) + (using_ == 1 ? " Folder" : " Folders");
	const float bw = button(lx, y, label, true, bOn, ICON_FA_CIRCLE_CHECK, 23);
	button(lx + bw + 20, y, "Cancel", false, false);
	hints({ { { "circle" }, "Cancel" }, { { "triangle" }, "Use These Folders" }, { { "cross" }, "Switch" } });
}

void setColours(const Palette& p)
{
	col::bg = p.bg; col::bg2 = p.bg2; col::text = p.text; col::sub = p.sub; col::faint = p.faint;
	col::card = p.card; col::cardHi = p.cardHi; col::line = p.line; col::warm = p.warm; col::on = p.accent;
	col::off = alpha(p.text, 0.2f); col::dark = lightness(p.text) > 0.5f ? p.bg : IM_COL32(255, 255, 255, 255);
	col::focus = p.accent; col::onFocus = p.onAccent;
}

void setUp(Host& host, ImDrawList *target, bool scaled = false)
{
	H = &host;
	dl = target != nullptr ? target : ImGui::GetBackgroundDrawList();
	setColours(host.palette);
	const ImVec2 size = ImGui::GetIO().DisplaySize;
	HH = scaled ? 1080.f * 100.f / (float)std::clamp(host.uiScale, 70, 130) : 1080.f;
	S = size.y / HH;
	W = size.x / S;
}
}	// namespace

void show(Screen screen)
{
	st.screen = screen;
	if (screen == Screen::SettingsHome)
		st.setHome = std::max(st.setHome, -1);
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

void frame(Host& host, const Input& in, ImDrawList *target)
{
	setUp(host, target);
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

void drawPage(Host& host, const PageView& page, ImDrawList *target)
{
	setUp(host, target);
	pageView(page);
}

ImVec4 drawQuick(Host& host, const QuickView& quick, ImDrawList *target)
{
	setUp(host, target);
	return quickView(quick);
}

// ---------------------------------------------------------------- the kit, for the host's screens

namespace kit
{
void begin(Host& host, ImDrawList *target) { setUp(host, target, true); }
float width() { return W; }
const Palette& palette() { return H->palette; }
ImDrawList *list() { return dl; }
ImU32 alpha(ImU32 c, float a) { return pui::alpha(c, a); }
ImU32 mix(ImU32 a, ImU32 b, float t) { return pui::mix(a, b, t); }
void backdrop() { pui::backdrop(); }
void heroArt(const Game& g, float from) { pui::heroArt(g, from); }
void cover(const Game& g, float x, float y, float w, float h, float r, bool focus) { pui::cover(g, x, y, w, h, r, focus); }
void glow(float cx, float cy, float radius, ImU32 c) { pui::glow(cx, cy, radius, c); }
void shadow(float x, float y, float w, float h, float r, ImU32 c, float spread) { pui::shadow(x, y, w, h, r, c, spread); }
void surface(float x, float y, float w, float h, float r, bool focus) { pui::surface(x, y, w, h, r, focus); }
void ring(float x, float y, float w, float h, float r) { pui::ring(x, y, w, h, r); }
void fill(float x, float y, float w, float h, ImU32 c, float r) { pui::rect(x, y, w, h, c, r); }
float text(bool b, float size, float x, float y, ImU32 c, const std::string& s, float wrap) { return pui::text(font(b), size, x, y, c, s, wrap); }
ImVec2 measure(bool b, float size, const std::string& s, float wrap) { return pui::measure(font(b), size, s, wrap); }
std::string fit(bool b, float size, const std::string& s, float w) { return pui::fit(font(b), size, s, w); }
void caption(float x, float y, const std::string& s, ImU32 c) { pui::caption(x, y, s, c); }
float button(float x, float y, const std::string& label, bool primary, bool focus, const char *icon, float size)
{
	return pui::button(x, y, label, primary, focus, icon, size);
}
void toggle(float right, float cy, bool on, bool focus) { switchAt(right, cy, on, focus); }
void line(float x, float y, float w, float h, const Line& l, bool focus) { listLine(x, y, w, h, l, focus); }
void hints(const std::vector<std::pair<std::vector<const char *>, std::string>>& list, float right, float cy) { pui::hints(list, right, cy); }
void header(const std::string& title, const std::string& over, const std::string& note) { pui::header(title, over, note); }
void clock(float right, float y) { clockAt(right, y); }
}
}
