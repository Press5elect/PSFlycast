/*
	PSFlyCast - a Big Picture style, controller-only interface.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Layout, in units of 1/1080 of the screen height:
	  top bar    0..96     the app, the tabs (L1 / R1), the clock
	  content    96..1008  the library (hero + shelves) or the settings
	  hint bar   1008..1080  what each DualSense button does here
	The tabs are where the games come from - Internal (the title's own games
	folder), USB (when USB drives are on) and Network (the SMB folders of
	network.cfg) - and Settings. Internal and USB games are scanned at the
	start; network games come from the list the last scan saved
	(data/network-games.txt), so that looking at the library does not wake a
	NAS: the share is asked when the Network tab is opened with no list yet,
	with Square on that tab, and when one of its games is started.
	The library has three views (OPTIONS switches): shelves under a hero, a
	compact grid, and a list with the focused game's details beside it.
	The loading screen shows the game and, for a network game, what the share
	is doing (ps5_smb.cpp): waiting for an answer, or reading into memory.
	Triangle opens a game's details: its description (Flycast's scraper, from
	TheGamesDB), and what can be done before it starts - play, start from a
	saved state, options of its own (Flycast's per-game settings), cheats - and
	Manage: whether it is a favourite or hidden, its cover, and how long it was
	played (ps5_library.cpp keeps these, in data/library.txt).
	The mark in the top bar is a disc made of one spiral line, turning
	clockwise; a picture of the user's own, <root>/logo.png, is shown in its
	place. The start-up animations (drawSplash; four, one each start) end with
	the mark and the name where the top bar has them.
	The quick menu slides in from the right over the paused game; its Cheats
	page lists the game's cheats (shell/ps5/ps5_cheats.cpp). Fast forward is
	Flycast's own, from the quick menu or a button of the pad.
*/
#include "bigpicture.h"
#include "ps5_pad.h"
#include "ps5_build.h"
#include "ps5_rewind.h"
#include "ps5_vmu.h"
#include "rend/soft/soft_renderer.h"
#include "achievements/achievements.h"
#include "ps5_frontend.h"
#include "cheats.h"

#include "types.h"
#include "emulator.h"
#include "hw/sh4/sh4_if.h"
#include "stdclass.h"
#include "version.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "input/gamepad_device.h"
#include "imgread/common.h"
#include "oslib/oslib.h"
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "ui/settings.h"
#include "ui/imgui_driver.h"
#include "ui/game_scanner.h"
#include "ui/boxart/boxart.h"
#include "ui/IconsFontAwesome6.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <ctime>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <future>
#include <mutex>
#include <thread>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <type_traits>
#include <string>
#include <vector>

extern ImFont *boldFont;
void SaveSettings();

using ps5::shownRoot;

bool gui_error_shown();		// core/ui/gui.cpp: Flycast's message box is up

namespace bigpicture
{
namespace
{

// ---------------------------------------------------------------- the theme

// The colours every screen draws with. They are the skin's (applySkin, below):
// Midnight's until a skin is applied. Text is white in every skin, so what is
// drawn on the accent reads in all of them.
namespace col
{
constexpr ImU32 rgba(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
ImU32 bgTop = rgba(14, 19, 26);
ImU32 bgBottom = rgba(23, 30, 40);
ImU32 bar = rgba(10, 13, 18, 235);
ImU32 panel = rgba(30, 38, 49);
ImU32 panelHi = rgba(41, 52, 66);
ImU32 card = rgba(36, 45, 58);
ImU32 sheet = rgba(18, 23, 31);		// the quick menu's panel
ImU32 sheetHi = rgba(24, 30, 40);
const ImU32 line = rgba(255, 255, 255, 22);
const ImU32 text = rgba(255, 255, 255);
ImU32 dim = rgba(160, 172, 186);
ImU32 faint = rgba(105, 117, 131);
ImU32 accent = rgba(26, 159, 255);
ImU32 accentDark = rgba(18, 104, 176);
ImU32 accentGlow = rgba(26, 159, 255, 70);
ImU32 warm = rgba(255, 128, 48);	// the warm accent (arcade, the mark)
ImU32 good = rgba(92, 200, 120);
}

float S = 1.f;	// pixels per unit
float W = 1920.f, H = 1080.f;	// the screen, in units
double timeNow;

ImVec2 V(float x, float y) { return ImVec2(x * S, y * S); }
ImU32 alpha(ImU32 c, float a) {
	const int a0 = (c >> IM_COL32_A_SHIFT) & 0xff;
	return (c & ~IM_COL32_A_MASK) | ((ImU32)std::clamp(a0 * a, 0.f, 255.f) << IM_COL32_A_SHIFT);
}
ImU32 mix(ImU32 a, ImU32 b, float t)
{
	ImVec4 ca = ImGui::ColorConvertU32ToFloat4(a), cb = ImGui::ColorConvertU32ToFloat4(b);
	return ImGui::ColorConvertFloat4ToU32(ImVec4(ca.x + (cb.x - ca.x) * t, ca.y + (cb.y - ca.y) * t,
			ca.z + (cb.z - ca.z) * t, ca.w + (cb.w - ca.w) * t));
}

ImFont *regular() { return ImGui::GetIO().Fonts->Fonts[0]; }
ImFont *bold() { return boldFont != nullptr ? boldFont : regular(); }

// Eases v toward target; speed is roughly "per second".
float approach(float v, float target, float speed = 14.f)
{
	const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
	return v + (target - v) * (1.f - std::exp(-speed * dt));
}

ImVec2 textSize(ImFont *font, float size, const char *s, float wrap = 0.f) {
	return font->CalcTextSizeA(size * S, FLT_MAX, wrap * S, s) / S;
}

// Text at (x, y) units; returns its size in units.
ImVec2 text(ImDrawList *dl, ImFont *font, float size, float x, float y, ImU32 c, const char *s, float wrap = 0.f)
{
	dl->AddText(font, size * S, V(x, y), c, s, nullptr, wrap * S);
	return textSize(font, size, s, wrap);
}

// Text cut to a width with an ellipsis.
std::string fit(ImFont *font, float size, const std::string& s, float width)
{
	if (textSize(font, size, s.c_str()).x <= width)
		return s;
	std::string out = s;
	while (!out.empty() && textSize(font, size, (out + "...").c_str()).x > width)
		out.pop_back();
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	return out + "...";
}

void rect(ImDrawList *dl, float x, float y, float w, float h, ImU32 c, float r = 0.f, ImDrawFlags f = 0) {
	dl->AddRectFilled(V(x, y), V(x + w, y + h), c, r * S, f);
}
void outline(ImDrawList *dl, float x, float y, float w, float h, ImU32 c, float r, float t) {
	dl->AddRect(V(x, y), V(x + w, y + h), c, r * S, 0, t * S);
}
void gradientV(ImDrawList *dl, float x, float y, float w, float h, ImU32 top, ImU32 bottom) {
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h), top, top, bottom, bottom);
}
void gradientH(ImDrawList *dl, float x, float y, float w, float h, ImU32 left, ImU32 right) {
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h), left, right, right, left);
}

// A soft glow: concentric rounded rectangles, fading out.
void glow(ImDrawList *dl, float x, float y, float w, float h, float r, ImU32 c, float spread = 18.f)
{
	for (int i = 6; i >= 1; i--)
	{
		const float g = spread * i / 6.f;
		dl->AddRectFilled(V(x - g, y - g), V(x + w + g, y + h + g), alpha(c, 0.22f / i), (r + g) * S);
	}
}

// ------------------------------------------------------- skins and motion
//
// A skin is a palette and what moves behind the screens. Midnight is the look
// the interface has had from the start, and the default. Settings > Interface
// chooses the skin, an accent colour of one's own, the backdrop and how much
// the interface moves.

enum Backdrop { BackStill, BackAurora, BackWaves, BackSparks, BackHorizon, BackMatrix, BackCover, BackdropCount };
const char *const backdropNames[] = { "Still", "Aurora", "Waves", "Sparks", "Horizon", "Dot matrix", "Cover colours" };

struct Skin
{
	const char *name;
	ImU32 bgTop, bgBottom, bar, panel, panelHi, card, sheet, sheetHi, dim, faint, accent, warm;
	Backdrop backdrop;
};

const Skin skins[] = {
	// The first look: slate and blue.
	{ "Midnight", col::rgba(14, 19, 26), col::rgba(23, 30, 40), col::rgba(10, 13, 18, 235), col::rgba(30, 38, 49),
			col::rgba(41, 52, 66), col::rgba(36, 45, 58), col::rgba(18, 23, 31), col::rgba(24, 30, 40),
			col::rgba(160, 172, 186), col::rgba(105, 117, 131), col::rgba(26, 159, 255), col::rgba(255, 128, 48), BackStill },
	// Warm charcoal and the orange of the console's power light.
	{ "Ember", col::rgba(22, 15, 12), col::rgba(40, 25, 18), col::rgba(16, 11, 9, 235), col::rgba(49, 35, 28),
			col::rgba(68, 48, 38), col::rgba(58, 41, 33), col::rgba(28, 19, 16), col::rgba(38, 27, 22),
			col::rgba(200, 180, 168), col::rgba(140, 120, 108), col::rgba(236, 100, 20), col::rgba(255, 196, 84), BackAurora },
	// The memory card's screen: olive glass, and its dots behind.
	{ "Pocket LCD", col::rgba(13, 18, 13), col::rgba(24, 33, 23), col::rgba(9, 13, 9, 235), col::rgba(32, 43, 31),
			col::rgba(46, 60, 44), col::rgba(39, 51, 37), col::rgba(18, 25, 18), col::rgba(26, 35, 25),
			col::rgba(172, 188, 162), col::rgba(112, 129, 104), col::rgba(98, 148, 54), col::rgba(222, 204, 96), BackMatrix },
	// An arcade at night: violet, magenta and a floor that runs to the horizon.
	{ "Arcade", col::rgba(15, 9, 27), col::rgba(35, 16, 50), col::rgba(11, 7, 21, 235), col::rgba(45, 26, 63),
			col::rgba(62, 38, 86), col::rgba(53, 32, 74), col::rgba(23, 13, 35), col::rgba(33, 20, 48),
			col::rgba(194, 174, 214), col::rgba(130, 110, 152), col::rgba(224, 46, 138), col::rgba(255, 180, 40), BackHorizon },
	// For an OLED television: black, and little else lit.
	{ "Carbon", col::rgba(0, 0, 0), col::rgba(7, 7, 8), col::rgba(0, 0, 0, 235), col::rgba(22, 22, 24),
			col::rgba(38, 38, 42), col::rgba(28, 28, 31), col::rgba(11, 11, 12), col::rgba(19, 19, 21),
			col::rgba(172, 172, 178), col::rgba(110, 110, 116), col::rgba(212, 42, 58), col::rgba(255, 172, 64), BackStill },
};
constexpr int SkinCount = (int)(sizeof(skins) / sizeof(skins[0]));

// The accent colours one can choose in place of a skin's own.
struct Accent { const char *name; ImU32 colour; };
const Accent accents[] = {
	{ "The skin's own", 0 },
	{ "Blue", col::rgba(26, 159, 255) }, { "Orange", col::rgba(236, 100, 20) }, { "Green", col::rgba(52, 168, 96) },
	{ "Teal", col::rgba(0, 158, 164) }, { "Violet", col::rgba(126, 92, 240) }, { "Magenta", col::rgba(224, 46, 138) },
	{ "Red", col::rgba(212, 42, 58) },
};
constexpr int AccentCount = (int)(sizeof(accents) / sizeof(accents[0]));

const Skin& skin()
{
	return skins[std::clamp(ps5::options().skin, 0, SkinCount - 1)];
}

// The palette the screens draw with, from the options; cheap enough for every frame.
void applySkin()
{
	const Skin& s = skin();
	col::bgTop = s.bgTop; col::bgBottom = s.bgBottom; col::bar = s.bar;
	col::panel = s.panel; col::panelHi = s.panelHi; col::card = s.card;
	col::sheet = s.sheet; col::sheetHi = s.sheetHi; col::dim = s.dim; col::faint = s.faint;
	col::warm = s.warm;
	const int chosen = std::clamp(ps5::options().accent, 0, AccentCount - 1);
	col::accent = chosen == 0 ? s.accent : accents[chosen].colour;
	col::accentDark = mix(col::accent, col::rgba(0, 0, 0), 0.35f);
	col::accentGlow = alpha(col::accent, 70 / 255.f);
}

Backdrop backdropChoice()
{
	const int chosen = ps5::options().backdrop;
	return chosen <= 0 || chosen > BackdropCount ? skin().backdrop : (Backdrop)(chosen - 1);
}

// How much moves: everything, only what answers the controller (no drifting
// backdrop, no entrances one by one, shorter fades), or nothing.
enum Motion { MotionFull, MotionReduced, MotionOff };
Motion motion()
{
	return (Motion)std::clamp(ps5::options().motion, 0, 2);
}

// The backdrop's clock: it stands still unless everything moves.
double driftTime()
{
	return motion() == MotionFull ? timeNow : 40.0;
}

float easeOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return 1.f - (1.f - t) * (1.f - t) * (1.f - t);
}

float easeInOut(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return t * t * (3.f - 2.f * t);
}

// The start-up animation (drawSplash, shown by the library the first time it
// is on): not begun, showing, leaving, over. There are four, and each start
// shows one of them: the one chosen in the settings, or one by chance. Each
// ends with its mark and name where the top bar has them, and the top bar
// leaves its own out until then: flight is under 1 while the animation still
// draws them.
enum SplashKind { SplashBirds, SplashDock, SplashComet, SplashShutter, SplashKinds };

struct SplashState
{
	enum { NotBegun, Showing, Leaving, Over } state = NotBegun;
	int kind = SplashBirds;
	double began = 0, leaving = 0;
	int frames = 0;
	bool sounded = false;	// its sound was started, with its clock
	bool skip = false;		// a button was pressed this frame: it ends early
	bool cut = false;		// it ended early (or there is less motion): it fades where it is
	float cutAt = 0;		// the moment of the animation it was cut at
	float flight = 1.f;
} splash;

// A disc of light, bright at its centre and gone at its edge.
void softDisc(ImDrawList *dl, float cx, float cy, float radius, ImU32 colour, int segments = 36)
{
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	const ImU32 edge = colour & ~IM_COL32_A_MASK;
	dl->PrimReserve(segments * 3, segments + 1);
	const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	dl->PrimWriteVtx(V(cx, cy), uv, colour);
	for (int i = 0; i < segments; i++)
	{
		const float angle = 2 * IM_PI * i / segments;
		dl->PrimWriteVtx(V(cx + std::cos(angle) * radius, cy + std::sin(angle) * radius), uv, edge);
	}
	for (int i = 0; i < segments; i++)
	{
		dl->PrimWriteIdx(base);
		dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
		dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % segments));
	}
}

// A quad with a colour at each corner (top left, top right, bottom right, bottom left).
void quad(ImDrawList *dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd)
{
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	dl->PrimReserve(6, 4);
	const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	dl->PrimWriteVtx(a, uv, ca); dl->PrimWriteVtx(b, uv, cb); dl->PrimWriteVtx(c, uv, cc); dl->PrimWriteVtx(d, uv, cd);
	dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 1)); dl->PrimWriteIdx((ImDrawIdx)(base + 2));
	dl->PrimWriteIdx(base); dl->PrimWriteIdx((ImDrawIdx)(base + 2)); dl->PrimWriteIdx((ImDrawIdx)(base + 3));
}

// A number between 0 and 1 from a whole number, the same each time.
float chance(unsigned n)
{
	n = (n ^ 61u) ^ (n >> 16); n *= 9u; n ^= n >> 4; n *= 0x27d4eb2du; n ^= n >> 15;
	return (n & 0xffffff) / 16777216.f;
}

// The art behind the screens with "Cover colours": the focused game's cover,
// so enlarged that only its colours are left, the last one fading under it.
struct Ambient
{
	ImTextureID now = ImTextureID(), before = ImTextureID();
	float fade = 1;
} ambient;

void ambientArt(ImTextureID id)
{
	if (id == ambient.now)
		return;
	ambient.before = ambient.now;
	ambient.now = id;
	ambient.fade = 0;
}

void imageFill(ImDrawList *dl, ImTextureID id, float x, float y, float w, float h, float r, ImU32 tint);

// What is behind every opaque screen: the skin's gradient, then its backdrop.
void drawBackdrop(ImDrawList *dl)
{
	gradientV(dl, 0, 0, W, H, col::bgTop, col::bgBottom);
	const float t = (float)driftTime();
	const ImU32 second = mix(col::accent, col::warm, 0.5f);
	switch (backdropChoice())
	{
	case BackStill:
	default:
		// A wash of light from the top left, as Big Picture's backdrop has.
		dl->AddCircleFilled(V(W * 0.12f, -260), 900 * S, alpha(col::accent, 10 / 255.f), 96);
		dl->AddCircleFilled(V(W * 0.12f, -260), 600 * S, alpha(col::accent, 10 / 255.f), 96);
		break;

	case BackAurora:
	{
		// Four lights that wander slowly and never line up twice.
		const ImU32 lights[4] = { col::accent, col::warm, second, col::accent };
		for (int i = 0; i < 4; i++)
		{
			const float px = W * (0.5f + 0.42f * std::sin(t * (0.050f + 0.013f * i) + i * 1.9f));
			const float py = H * (0.42f + 0.36f * std::sin(t * (0.037f + 0.011f * i) + i * 2.7f + 1.f));
			softDisc(dl, px, py, 760 + 140 * std::sin(t * 0.07f + i), alpha(lights[i], i == 1 ? 0.13f : 0.17f), 48);
		}
		break;
	}

	case BackWaves:
	{
		// Ribbons of light across the lower half, each on its own swell.
		const int steps = 48;
		for (int band = 0; band < 4; band++)
		{
			const ImU32 colour = band % 2 == 0 ? col::accent : second;
			const float baseY = H * (0.50f + 0.11f * band);
			auto height = [&](float x) {
				return baseY + 46 * std::sin(x * 0.0031f + t * (0.22f + 0.05f * band) + band * 1.7f)
						+ 24 * std::sin(x * 0.0072f - t * (0.15f + 0.03f * band) + band);
			};
			for (int i = 0; i < steps; i++)
			{
				const float x0 = W * i / steps, x1 = W * (i + 1) / steps;
				const float y0 = height(x0), y1 = height(x1);
				const ImU32 top = alpha(colour, 0.13f - 0.02f * band), none = colour & ~IM_COL32_A_MASK;
				quad(dl, V(x0, y0), V(x1, y1), V(x1, y1 + 300), V(x0, y0 + 300), top, top, none, none);
				dl->AddLine(V(x0, y0), V(x1, y1), alpha(colour, 0.30f - 0.05f * band), 2 * S);
			}
		}
		break;
	}

	case BackSparks:
	{
		// Motes rising at their own pace, the large ones slower and fainter.
		for (unsigned i = 0; i < 70; i++)
		{
			const float size = 6 + 46 * chance(i * 7 + 1) * chance(i * 7 + 2);
			const float speed = 26 - size * 0.4f;
			float y = std::fmod((H + 120) * chance(i * 7 + 3) - t * speed, H + 120);
			if (y < 0)
				y += H + 120;
			y -= 60;
			const float x = W * chance(i * 7 + 4) + 30 * std::sin(t * 0.3f + i);
			const ImU32 colour = i % 5 == 0 ? col::warm : i % 3 == 0 ? col::text : col::accent;
			softDisc(dl, x, y, size, alpha(colour, 0.10f + 0.16f * chance(i * 7 + 5)), 16);
		}
		break;
	}

	case BackHorizon:
	{
		// A floor of lines running to a horizon, and the glow where it ends.
		const float horizon = H * 0.56f, vx = W * 0.5f;
		softDisc(dl, vx, horizon, 620, alpha(col::accent, 0.20f), 48);
		softDisc(dl, vx, horizon, 260, alpha(col::warm, 0.16f), 32);
		gradientV(dl, 0, horizon, W, H - horizon, alpha(col::bgTop, 0.85f), alpha(col::bgBottom, 0.55f));
		for (int i = -14; i <= 14; i++)
		{
			const float bottom = vx + i * 260.f;
			dl->AddLine(V(vx + i * 6.f, horizon), V(bottom, H), alpha(col::accent, 0.26f), 2 * S);
		}
		const float phase = std::fmod(t * 0.35f, 1.f);
		for (int i = 0; i < 14; i++)
		{
			const float depth = (i + phase) / 14.f;			// 0 at the horizon
			const float y = horizon + (H - horizon) * depth * depth;
			dl->AddLine(V(0, y), V(W, y), alpha(col::accent, 0.06f + 0.26f * depth), 2 * S);
		}
		break;
	}

	case BackMatrix:
	{
		// The dots of a small screen, with a swell of light passing through them.
		const float pitch = 40;
		for (int row = 0; row * pitch < H; row++)
			for (int column = 0; column * pitch < W; column++)
			{
				const float wave = std::sin(column * 0.21f + row * 0.13f - t * 0.6f) * std::sin(row * 0.17f + t * 0.23f);
				const float lit = 0.028f + 0.075f * std::max(0.f, wave);
				const float x = column * pitch + 8, y = row * pitch + 8;
				dl->AddRectFilled(V(x, y), V(x + pitch - 14, y + pitch - 14), alpha(col::accent, lit));
			}
		break;
	}

	case BackCover:
	{
		ambient.fade = approach(ambient.fade, 1, 3);
		if (ambient.before != ImTextureID() && ambient.fade < 0.99f)
			imageFill(dl, ambient.before, -W * 0.25f, -H * 0.25f, W * 1.5f, H * 1.5f, 0, alpha(col::text, 0.30f * (1 - ambient.fade)));
		if (ambient.now != ImTextureID())
			imageFill(dl, ambient.now, -W * 0.25f, -H * 0.25f, W * 1.5f, H * 1.5f, 0, alpha(col::text, 0.30f * ambient.fade));
		else
			dl->AddCircleFilled(V(W * 0.12f, -260), 900 * S, alpha(col::accent, 14 / 255.f), 96);
		// Darker toward the bottom, where the shelves and the lists are read.
		gradientV(dl, 0, 0, W, H, alpha(col::bgTop, 0.25f), alpha(col::bgBottom, 0.80f));
		break;
	}
	}
}

// ---- transitions
//
// What is drawn between beginLayer and endLayer can fade, slide and grow as
// one piece: its vertices are moved and their alpha scaled once it is drawn.

struct Layer
{
	ImDrawList *dl;
	int from;
};

Layer beginLayer(ImDrawList *dl)
{
	return { dl, dl->VtxBuffer.Size };
}

void endLayer(const Layer& layer, float opacity, float dx = 0, float dy = 0, float scale = 1, float pivotX = 0, float pivotY = 0)
{
	if (opacity >= 0.999f && dx == 0 && dy == 0 && scale == 1)
		return;
	opacity = std::clamp(opacity, 0.f, 1.f);
	ImDrawList *dl = layer.dl;
	for (int i = layer.from; i < dl->VtxBuffer.Size; i++)
	{
		ImDrawVert& v = dl->VtxBuffer[i];
		v.pos.x = (pivotX + dx) * S + (v.pos.x - pivotX * S) * scale;
		v.pos.y = (pivotY + dy) * S + (v.pos.y - pivotY * S) * scale;
		const ImU32 a = (ImU32)(((v.col >> IM_COL32_A_SHIFT) & 0xff) * opacity);
		v.col = (v.col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
	}
}

// A screen coming in. screen names it: a new name, or frames without it,
// start its entrance again. The return is how far in it is, 0 to 1; side is
// the way it comes from (-1 the left, 1 the right, 0 neither).
struct Entrance
{
	int screen = -1;
	int frame = -10;
	double at = 0;
	float side = 0, next = 0;
} entrance;

float entered(int screen)
{
	const int frame = ImGui::GetFrameCount();
	if (screen != entrance.screen || frame > entrance.frame + 2)
	{
		entrance.screen = screen;
		entrance.at = timeNow;
		entrance.side = entrance.next;
		entrance.next = 0;
	}
	entrance.frame = frame;
	const float seconds = motion() == MotionFull ? 0.32f : motion() == MotionReduced ? 0.16f : 0.f;
	return seconds <= 0 ? 1.f : easeOut((float)(timeNow - entrance.at) / seconds);
}

// Ends a screen's layer at its entrance: faded, and slid from its side.
void endEntrance(const Layer& layer, float e)
{
	endLayer(layer, e, (1 - e) * 56 * entrance.side, entrance.side == 0 ? (1 - e) * 18 : 0);
}

// ------------------------------------------------------------- the mark
//
// The app's mark is a disc made of one line: it starts at the disc's left
// edge, winds in to the centre in two and a half turns, and winds out again
// to the right edge, so that its loops are the disc's grooves. The start-up
// animation draws that line, lets its turns out until it is straight, and
// coils it up again in the top bar (drawSplash). The design is PSFlyCast's
// own.

struct SpiralLine
{
	enum { Half = 220, Count = 2 * Half + 1 };
	ImVec2 at[Count];			// its points, on a disc of radius 1
	float piece[Count - 1];		// the length of each piece between two points,
	float heading[Count - 1];	// its direction (never jumping by a whole turn),
	float along[Count];			// and the length of the line up to each point
	float length;
	float straight;				// the direction the line has at the centre
};

const SpiralLine& spiralLine()
{
	static SpiralLine line;
	static bool made;
	if (made)
		return line;
	made = true;
	const float turns = 2.5f * 2 * IM_PI;
	for (int k = 0; k < SpiralLine::Count; k++)
	{
		const float u = (float)k / SpiralLine::Half - 1;
		const float angle = turns * std::fabs(u), r = std::fabs(u), side = u < 0 ? 1.f : -1.f;
		line.at[k] = ImVec2(side * r * std::cos(angle), side * r * std::sin(angle));
	}
	line.along[0] = 0;
	float before = 0;
	for (int k = 0; k < SpiralLine::Count - 1; k++)
	{
		const float dx = line.at[k + 1].x - line.at[k].x, dy = line.at[k + 1].y - line.at[k].y;
		float heading = std::atan2(dy, dx);
		if (k > 0)
		{
			while (heading - before > IM_PI)
				heading -= 2 * IM_PI;
			while (heading - before < -IM_PI)
				heading += 2 * IM_PI;
		}
		before = heading;
		line.heading[k] = heading;
		line.piece[k] = std::sqrt(dx * dx + dy * dy);
		line.along[k + 1] = line.along[k] + line.piece[k];
	}
	line.length = line.along[SpiralLine::Count - 1];
	// It crosses the centre almost level: let out, it is level.
	line.straight = std::round((line.heading[SpiralLine::Half - 1] + line.heading[SpiralLine::Half]) / 2 / IM_PI) * IM_PI;
	return line;
}

// The same line with its turns let out by e (0 the spiral, 1 a straight
// line), its middle staying where it is and every piece keeping its length.
void uncoil(float e, ImVec2 *out)
{
	const SpiralLine& line = spiralLine();
	const int mid = SpiralLine::Half;
	const float keep = 1 - std::clamp(e, 0.f, 1.f);
	out[mid] = ImVec2(0, 0);
	for (int k = mid; k < SpiralLine::Count - 1; k++)
	{
		const float a = line.straight + (line.heading[k] - line.straight) * keep;
		out[k + 1] = ImVec2(out[k].x + line.piece[k] * std::cos(a), out[k].y + line.piece[k] * std::sin(a));
	}
	for (int k = mid - 1; k >= 0; k--)
	{
		const float a = line.straight + (line.heading[k] - line.straight) * keep;
		out[k] = ImVec2(out[k + 1].x - line.piece[k] * std::cos(a), out[k + 1].y - line.piece[k] * std::sin(a));
	}
}

// Draws the line's points (the spiral's own, or uncoiled ones): on a disc
// `radius` large at (cx, cy), turned by `angle`, from the line's start up to
// `share` of its length, `width` wide with round ends.
void strokeSpiral(ImDrawList *dl, const ImVec2 *points, float cx, float cy, float radius, float angle, float share,
		ImU32 colour, float width, bool roundEnds = true)
{
	if (share <= 0 || (colour & IM_COL32_A_MASK) == 0)
		return;
	const SpiralLine& line = spiralLine();
	// A small one needs fewer of its points.
	const float pixels = radius * S;
	const int stride = pixels > 160 ? 1 : pixels > 60 ? 2 : 4;
	const float c = std::cos(angle), s = std::sin(angle);
	auto place = [&](const ImVec2& p) {
		return V(cx + (p.x * c - p.y * s) * radius, cy + (p.x * s + p.y * c) * radius);
	};
	const float until = line.length * std::min(share, 1.f);
	// The points on the screen. One that lies on the straight way between
	// its neighbours is left out, so a line let out straight is one piece.
	static ImVec2 path[SpiralLine::Count + 1];
	int count = 0;
	auto add = [&](const ImVec2& p) {
		if (count >= 2)
		{
			const ImVec2& a = path[count - 2], & b = path[count - 1];
			const float dx = p.x - a.x, dy = p.y - a.y, length = std::sqrt(dx * dx + dy * dy);
			if (length > 0 && std::fabs((b.x - a.x) * dy - (b.y - a.y) * dx) / length < 0.06f)
				count--;
		}
		path[count++] = p;
	};
	add(place(points[0]));
	int k = stride;
	for (; k < SpiralLine::Count && line.along[k] <= until; k += stride)
		add(place(points[k]));
	if (k < SpiralLine::Count)
	{
		// The line ends within this piece.
		const float from = line.along[k - stride];
		const float over = (until - from) / (line.along[k] - from);
		if (over > 0.01f)
		{
			const ImVec2& a = points[k - stride], & b = points[k];
			add(place(ImVec2(a.x + (b.x - a.x) * over, a.y + (b.y - a.y) * over)));
		}
	}
	const ImVec2 first = path[0], last = path[count - 1];
	dl->PathClear();
	for (int i = 0; i < count; i++)
		dl->PathLineTo(path[i]);
	dl->PathStroke(colour, 0, width * S);
	if (roundEnds)
	{
		dl->AddCircleFilled(first, width * S / 2, colour, 12);
		dl->AddCircleFilled(last, width * S / 2, colour, 12);
	}
}

// How wide the line is on a mark of a size: a small mark's is bolder for its
// size, so that it still reads in the top bar.
float markWidth(float radius)
{
	return radius * 0.0225f + 1.9f;
}

// The disc under the line: its body, lighter toward the hub, its rim and
// the hub.
void discBody(ImDrawList *dl, float cx, float cy, float radius, ImU32 line, ImU32 fill, float a)
{
	if (a <= 0)
		return;
	const float outer = radius * 1.14f;
	const int segments = radius * S > 120 ? 96 : 48;
	const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
	const ImU32 centre = alpha(fill, 0.30f * a), edge = alpha(fill, 0.10f * a);
	dl->PrimReserve(segments * 3, segments + 1);
	const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	dl->PrimWriteVtx(V(cx, cy), uv, centre);
	for (int i = 0; i < segments; i++)
	{
		const float angle = 2 * IM_PI * i / segments;
		dl->PrimWriteVtx(V(cx + std::cos(angle) * outer, cy + std::sin(angle) * outer), uv, edge);
	}
	for (int i = 0; i < segments; i++)
	{
		dl->PrimWriteIdx(base);
		dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
		dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % segments));
	}
	const float rim = std::max(1.2f, radius * 0.013f) * S;
	dl->AddCircle(V(cx, cy), outer * S, alpha(line, 0.55f * a), segments, rim);
	dl->AddCircleFilled(V(cx, cy), radius * 0.085f * S, alpha(col::bgTop, a), 24);
	dl->AddCircle(V(cx, cy), radius * 0.085f * S, alpha(line, 0.55f * a), 24, rim);
}

// The mark, still and in one colour: on a game's card that has no art.
void discMark(ImDrawList *dl, float cx, float cy, float radius, ImU32 colour)
{
	discBody(dl, cx, cy, radius, colour, colour, 1.f);
	strokeSpiral(dl, spiralLine().at, cx, cy, radius, 0, 1, colour, markWidth(radius), false);
}

// The mark turns clockwise, one turn every five seconds. markEpoch is the
// moment its angle was 0. The start-up animation, which leaves the mark in
// the top bar already turning, sets both: the mark starts from standing
// still at markSpunAt, turns faster for a moment, and settles into its turn.
constexpr float MarkRate = 2 * IM_PI / 5;
constexpr float SpinRise = 0.25f, SpinFall = 0.9f, SpinExtra = 3.4f;
constexpr float SpinBoth = SpinRise * SpinFall / (SpinRise + SpinFall);
double markEpoch = 0, markSpunAt = -1e9;

// From a moment t on, the mark spins up: a turn that is 0 at t.
void spinMarkUpAt(double t)
{
	const float settled = -MarkRate * SpinRise + SpinExtra * SpinFall - SpinExtra * SpinBoth;
	markSpunAt = t;
	markEpoch = t - settled / MarkRate;
}

float markAngle()
{
	double angle = (timeNow - markEpoch) * MarkRate;
	const float x = (float)(timeNow - markSpunAt);
	if (x < 0)
		return 0;
	if (x < 30)
		angle += MarkRate * SpinRise * std::exp(-x / SpinRise) - SpinExtra * SpinFall * std::exp(-x / SpinFall)
				+ SpinExtra * SpinBoth * std::exp(-x / SpinBoth);
	return (float)std::fmod(angle, 2.0 * IM_PI);
}

// A picture of the user's own, <root>/logo.png, takes the mark's place when
// there is one, turning the same way.
ImTextureID customLogo()
{
	static double checkedAt = -100;
	static bool present;
	static std::string path;
	if (timeNow > checkedAt + 5)
	{
		checkedAt = timeNow;
		path = ps5::rootDir + "logo.png";
		struct stat st;
		present = stat(path.c_str(), &st) == 0 && st.st_size > 0;
	}
	if (!present)
		return ImTextureID();
	ImguiFileTexture picture(path);
	return picture.getId();
}

// The mark, alive: the disc and its line, turning.
void spinMark(ImDrawList *dl, float cx, float cy, float radius, float a = 1.f)
{
	if (a <= 0)
		return;
	const float turn = markAngle();
	const ImTextureID logo = customLogo();
	if (logo != ImTextureID())
	{
		// The picture, turned, over its shadow.
		const float r = radius * 1.25f;
		dl->AddCircleFilled(V(cx + r * 0.06f, cy + r * 0.12f), r * 0.92f * S, col::rgba(0, 0, 0, (int)(70 * a)), 48);
		ImVec2 corner[4];
		for (int i = 0; i < 4; i++)
		{
			const float angle = turn + IM_PI * (0.25f + 0.5f * i) + IM_PI;	// top left first
			corner[i] = V(cx + std::cos(angle) * r * 1.414f, cy + std::sin(angle) * r * 1.414f);
		}
		dl->AddImageQuad(logo, corner[0], corner[1], corner[2], corner[3], ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
				ImVec2(0, 1), alpha(col::text, a));
		return;
	}
	discBody(dl, cx, cy, radius, col::text, col::accent, a);
	strokeSpiral(dl, spiralLine().at, cx, cy, radius, turn, 1, alpha(col::text, a), markWidth(radius));
}

// ------------------------------------------------------------ button glyphs

enum class Glyph { Cross, Circle, Square, Triangle, L1, R1, L1R1, L2, R2, L2R2, R3, Options, TouchPad, DPadLR, DPadUD };

// A DualSense button, as the face buttons are printed: white symbol on a dark disc.
float glyph(ImDrawList *dl, Glyph g, float x, float cy, float size = 34.f)
{
	const float r = size / 2;
	const ImU32 bg = col::rgba(255, 255, 255, 34), fg = col::text;
	const float t = size * 0.075f;
	auto label = [&](const char *s, float w) {
		rect(dl, x, cy - r, w, size, bg, r * 0.55f);
		const ImVec2 ts = textSize(bold(), size * 0.5f, s);
		text(dl, bold(), size * 0.5f, x + (w - ts.x) / 2, cy - ts.y / 2, fg, s);
		return w;
	};
	switch (g)
	{
	case Glyph::L1: return label("L1", size * 1.35f);
	case Glyph::R1: return label("R1", size * 1.35f);
	case Glyph::L1R1:
	{
		// Both shoulder buttons, side by side.
		const float w = size * 1.35f, gap = size * 0.2f, x0 = x;
		label("L1", w);
		x += w + gap;
		label("R1", w);
		x = x0;
		return w * 2 + gap;
	}
	case Glyph::L2: return label("L2", size * 1.35f);
	case Glyph::R2: return label("R2", size * 1.35f);
	case Glyph::R3: return label("R3", size * 1.35f);
	case Glyph::L2R2:
	{
		const float w = size * 1.35f, gap = size * 0.2f, x0 = x;
		label("L2", w);
		x += w + gap;
		label("R2", w);
		x = x0;
		return w * 2 + gap;
	}
	case Glyph::Options:
	{
		rect(dl, x, cy - r * 0.8f, size * 1.1f, size * 0.8f, bg, r * 0.5f);
		for (int i = -1; i <= 1; i++)
			rect(dl, x + size * 0.3f, cy + i * size * 0.18f - t / 2, size * 0.5f, t, fg, t / 2);
		return size * 1.1f;
	}
	case Glyph::TouchPad:
	{
		rect(dl, x, cy - r * 0.75f, size * 1.6f, size * 1.5f, bg, r * 0.4f);
		outline(dl, x + size * 0.22f, cy - r * 0.45f, size * 1.16f, size * 0.9f, fg, r * 0.2f, t);
		return size * 1.6f;
	}
	case Glyph::DPadLR:
	case Glyph::DPadUD:
	{
		const float cx = x + r;
		dl->AddCircleFilled(V(cx, cy), r * S, bg);
		const float a = size * 0.16f;
		const bool lr = g == Glyph::DPadLR;
		for (int s : { -1, 1 })
		{
			const float ox = lr ? s * size * 0.22f : 0, oy = lr ? 0 : s * size * 0.22f;
			const ImVec2 tip = V(cx + ox + (lr ? s * a : 0), cy + oy + (lr ? 0 : s * a));
			const ImVec2 b1 = V(cx + ox + (lr ? -s * a * 0.4f : -a), cy + oy + (lr ? -a : -s * a * 0.4f));
			const ImVec2 b2 = V(cx + ox + (lr ? -s * a * 0.4f : a), cy + oy + (lr ? a : -s * a * 0.4f));
			dl->AddTriangleFilled(tip, b1, b2, fg);
		}
		return size;
	}
	default:
		break;
	}
	const float cx = x + r;
	dl->AddCircleFilled(V(cx, cy), r * S, bg);
	const float k = size * 0.22f;
	switch (g)
	{
	case Glyph::Cross:
		dl->AddLine(V(cx - k, cy - k), V(cx + k, cy + k), fg, t * S);
		dl->AddLine(V(cx - k, cy + k), V(cx + k, cy - k), fg, t * S);
		break;
	case Glyph::Circle:
		dl->AddCircle(V(cx, cy), k * 1.15f * S, fg, 0, t * S);
		break;
	case Glyph::Square:
		outline(dl, cx - k, cy - k, k * 2, k * 2, fg, 0, t);
		break;
	case Glyph::Triangle:
		dl->AddTriangle(V(cx, cy - k * 1.15f), V(cx - k * 1.15f, cy + k * 0.85f), V(cx + k * 1.15f, cy + k * 0.85f), fg, t * S);
		break;
	default:
		break;
	}
	return size;
}

struct Hint
{
	Glyph glyph;
	const char *label;
};

// The bottom bar: hints right-aligned, as on Steam's.
void hintBar(ImDrawList *dl, const std::vector<Hint>& hints)
{
	rect(dl, 0, H - 72, W, 72, col::bar);
	rect(dl, 0, H - 72, W, 1, col::line);
	float x = W - 48;
	for (auto it = hints.rbegin(); it != hints.rend(); ++it)
	{
		const ImVec2 ts = textSize(regular(), 24, it->label);
		x -= ts.x;
		text(dl, regular(), 24, x, H - 36 - ts.y / 2, col::text, it->label);
		x -= 12;
		const float gw = glyph(dl, it->glyph, 0, -1000);	// measure
		x -= gw;
		glyph(dl, it->glyph, x, H - 36);
		x -= 44;
	}
}

// ------------------------------------------------------------------ input

// The console's own keyboard (ps5_ime.cpp) is up, and what its text is for.
// The pad is the keyboard's meanwhile.
enum KeyboardFor { KeyboardNone, KeyboardSearch, KeyboardAddress, KeyboardName, KeyboardRaUser, KeyboardRaPassword };
int keyboardFor = KeyboardNone;

struct Input
{
	bool up, down, left, right;
	bool accept, back, triangle, square, l1, r1, l2, r2, r3, options;
};
Input in;

// Edge-triggered buttons, and directions that repeat while held (D-pad or
// left stick), as a TV interface's lists do.
void readInput()
{
	const auto& pad = ps5::pad::player1();
	in = Input{};
	in.accept = pad.pressed & ps5::pad::Cross;
	in.back = pad.pressed & ps5::pad::Circle;
	in.triangle = pad.pressed & ps5::pad::Triangle;
	in.square = pad.pressed & ps5::pad::Square;
	in.l1 = pad.pressed & ps5::pad::L1;
	in.r1 = pad.pressed & ps5::pad::R1;
	in.l2 = pad.pressed & ps5::pad::L2;
	in.r2 = pad.pressed & ps5::pad::R2;
	in.r3 = pad.pressed & ps5::pad::R3;
	in.options = pad.pressed & ps5::pad::Options;

	struct Repeat { double next = 0; bool held = false; };
	static Repeat rep[4];
	const bool held[4] = {
		(pad.buttons & ps5::pad::Up) || pad.ly < -0.55f,
		(pad.buttons & ps5::pad::Down) || pad.ly > 0.55f,
		(pad.buttons & ps5::pad::Left) || pad.lx < -0.55f,
		(pad.buttons & ps5::pad::Right) || pad.lx > 0.55f,
	};
	bool *out[4] = { &in.up, &in.down, &in.left, &in.right };
	for (int i = 0; i < 4; i++)
	{
		if (!held[i])
			rep[i].held = false;
		else if (!rep[i].held)
		{
			rep[i].held = true;
			rep[i].next = timeNow + 0.38;
			*out[i] = true;
		}
		else if (timeNow >= rep[i].next)
		{
			rep[i].next = timeNow + 0.075;
			*out[i] = true;
		}
	}
	// The start-up animation takes the pad: any button ends it early, and
	// none reaches the library under it.
	if (splash.state == SplashState::Showing || splash.state == SplashState::Leaving)
	{
		splash.skip = pad.pressed != 0;
		in = Input{};
	}
	// So do the console's keyboard and Flycast's own message box (a game
	// that did not start), which is closed with Cross.
	if (keyboardFor != KeyboardNone || gui_error_shown())
		in = Input{};
	// The menu's sounds, one a frame: what the press is for, most telling first.
	using ps5::sound::Cue;
	if (in.accept)
		ps5::sound::cue(Cue::Select);
	else if (in.back)
		ps5::sound::cue(Cue::Back);
	else if (in.l1 || in.r1)
		ps5::sound::cue(Cue::Tab);
	else if (in.triangle || in.square || in.options || in.r3)
		ps5::sound::cue(Cue::Open);
	else if (in.up || in.down || in.left || in.right || in.l2 || in.r2)
		ps5::sound::cue(Cue::Move);
}

// ----------------------------------------------------------- frame set-up

ImDrawList *beginScreen(const char *id, bool opaque)
{
	const ImGuiIO& io = ImGui::GetIO();
	H = 1080.f;
	S = io.DisplaySize.y / H;
	W = io.DisplaySize.x / S;
	timeNow = ImGui::GetTime();
	readInput();

	ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
	ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav
			| ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollWithMouse);
	ImGui::PopStyleVar(2);
	ImDrawList *dl = ImGui::GetWindowDrawList();
	applySkin();
	if (opaque)
		drawBackdrop(dl);
	return dl;
}

void endScreen() {
	ImGui::End();
}

// --------------------------------------------------------------- top bar

// Where a game comes from: each is a tab of the library. Settings is the tab
// after them.
enum Source { Internal, Usb, Network };
constexpr int SourceCount = 3;
constexpr int SettingsTab = SourceCount;

struct TopTab
{
	int id;
	const char *icon;
	const char *name;
};

// The tabs, left to right; USB only while USB drives are on in the settings.
std::vector<TopTab> topTabs()
{
	std::vector<TopTab> tabs{ { Internal, ICON_FA_HARD_DRIVE, "Internal" } };
	if (ps5::options().usb)
		tabs.push_back({ Usb, ICON_FA_PLUG, "USB" });
	tabs.push_back({ Network, ICON_FA_NETWORK_WIRED, "Network" });
	// Settings is not a tab: OPTIONS opens it from the library.
	return tabs;
}

// Which other tabs L1 / R1 go to from a screen; the rest are fainter. Not
// Settings while a disc is chosen; none from the in-game settings and from the
// loading screen.
enum class Reach { All, Sources, None };

void topBar(ImDrawList *dl, int active, bool inGame, Reach reach = Reach::All)
{
	rect(dl, 0, 0, W, 96, col::bar);
	rect(dl, 0, 95, W, 1, col::line);

	// The app: its mark and its name.
	if (splash.flight >= 1.f)
	{
		spinMark(dl, 76, 48, 22);
		text(dl, bold(), 34, 112, 28, col::text, "PSFlyCast");
	}

	// Tabs, switched with L1 / R1.
	const std::vector<TopTab> tabs = topTabs();
	float total = 0;
	std::vector<float> widths;
	for (const TopTab& tab : tabs)
	{
		const std::string label = std::string(tab.icon) + "   " + tab.name;
		widths.push_back(textSize(bold(), 26, label.c_str()).x + 56);
		total += widths.back() + 12;
	}
	float x = (W - total) / 2 + 6;
	glyph(dl, Glyph::L1, x - 64, 48, 30);
	// The pill under the open tab slides to the next one.
	static float pillX = -1, pillW = 0;
	static int pillFrame = -10;
	{
		float at = x;
		for (size_t i = 0; i < tabs.size(); i++)
		{
			if (active == tabs[i].id)
			{
				const bool fresh = pillX < 0 || ImGui::GetFrameCount() > pillFrame + 2 || motion() == MotionOff;
				pillX = fresh ? at : approach(pillX, at, 20);
				pillW = fresh ? widths[i] : approach(pillW, widths[i], 20);
				pillFrame = ImGui::GetFrameCount();
				glow(dl, pillX, 24, pillW, 48, 24, alpha(col::accent, 0.5f), 10);
				rect(dl, pillX, 24, pillW, 48, col::accent, 24);
			}
			at += widths[i] + 12;
		}
	}
	for (size_t i = 0; i < tabs.size(); i++)
	{
		const bool on = active == tabs[i].id;
		const bool reachable = on || reach == Reach::All || (reach == Reach::Sources && tabs[i].id != SettingsTab);
		const std::string label = std::string(tabs[i].icon) + "   " + tabs[i].name;
		text(dl, bold(), 26, x + 28, 33, on ? col::text : reachable ? col::dim : col::faint, label.c_str());
		x += widths[i] + 12;
	}
	glyph(dl, Glyph::R1, x + 10, 48, 30);

	// The clock, and the pads.
	char clock[16] = "";
	const time_t t = time(nullptr);
	struct tm local;
	if (localtime_r(&t, &local) != nullptr)
		strftime(clock, sizeof(clock), "%H:%M", &local);
	const ImVec2 cs = textSize(bold(), 30, clock);
	text(dl, bold(), 30, W - 48 - cs.x, 48 - cs.y / 2, col::text, clock);
	const int pads = ps5::pad::connectedCount();
	std::string padText = std::string(ICON_FA_GAMEPAD) + (pads > 1 ? "  " + std::to_string(pads) : "");
	const ImVec2 ps = textSize(regular(), 28, padText.c_str());
	text(dl, regular(), 28, W - 48 - cs.x - 36 - ps.x, 48 - ps.y / 2, pads > 0 ? col::text : col::faint, padText.c_str());
	if (inGame)
	{
		const char *badge = ICON_FA_CIRCLE_PLAY "  Game running";
		const ImVec2 bs = textSize(regular(), 22, badge);
		const float bx = W - 48 - cs.x - 36 - ps.x - 36 - bs.x - 28;
		rect(dl, bx, 30, bs.x + 28, 36, alpha(col::good, 0.18f), 18);
		text(dl, regular(), 22, bx + 14, 48 - bs.y / 2, col::good, badge);
	}
}

// ---------------------------------------------------------------- library

struct Game
{
	GameMedia media;
	GameBoxart art;
	std::string title;
	Source source = Internal;
	bool disc = false;
	double artAskedAt = -10;
	// The user's or a downloaded cover, looked up once per covers generation.
	std::string cover;
	unsigned coverGeneration = 0;
	// A disc of a game with several (see the disc sets, below).
	std::string set;			// the set's key; empty for a game on its own
	int discOrder = 0;			// 1 for "Disc 1"
	int setSize = 0;			// the discs the library has of the set
	bool front = true;			// the disc that stands for the set when sets are grouped
	std::string fullTitle;		// with the disc: "Shenmue  ·  Disc 2"
	std::string setTitle;		// without: "Shenmue"
	// What the library keeps about it (updateMarks, below).
	bool favourite = false;
	bool hidden = false;
};

std::vector<Game> games;
size_t scannedCount = (size_t)-1;
std::vector<std::string> recentPaths;		// the games last played, the latest first

// ---- disc sets
//
// The discs of one game, by their file names: "Shenmue (USA) (Disc 1).chd" and
// "Shenmue (USA) (Disc 2).chd" are a set, as are "Game - CD1" and "Game - CD2".
// With "Group the discs of a game" on, the library shows a set as one entry,
// by the disc last played (the first disc until one was); Details chooses
// another disc. While a disc is being chosen for the running game, every disc
// is listed, the running game's own first.
std::map<std::string, std::vector<size_t>> discSets;	// key -> indexes into games, in disc order
std::map<std::string, std::string> frontChoice;			// key -> the disc chosen in Details, this run
bool setsDirty = true;
bool marksDirty = true;			// the games' favourite and hidden marks are to be read again
// Counts what the library's lists are made from changing: the games, the
// disc that stands for each set and the titles shown, the games last played,
// the favourites and the hidden games.
// The lists (buildShelves, buildFlat) are made again when it has moved, and
// not before: sorting a few thousand titles every frame cost several times
// what drawing the screen does.
unsigned libraryGeneration = 1;
bool setsGrouped = false;		// what the titles and fronts were last made for
// The disc in the drive of the running game.
std::string insertedPath;

bool grouping(bool selectDisk)
{
	return ps5::options().groupDiscs && !selectDisk;
}

// The disc number in a dump's name, and the name up to it. Taken: "Disc",
// "Disk", "CD" or "GD-ROM" at the start of a word, then a number or a letter
// A to D, alone ("Disc 2", "CD2", "Disk B") - and with its brackets and "of 4"
// when it has them.
bool splitDisc(const std::string& base, std::string& rest, int& order)
{
	auto alnum = [&](size_t i) { return i < base.size() && isalnum((unsigned char)base[i]); };
	for (size_t i = 0; i < base.size(); i++)
	{
		if (i > 0 && alnum(i - 1))
			continue;
		size_t at = std::string::npos;
		for (const char *word : { "disc", "disk", "cd", "gd-rom" })
			if (strncasecmp(base.c_str() + i, word, strlen(word)) == 0)
			{
				at = i + strlen(word);
				break;
			}
		if (at == std::string::npos)
			continue;
		while (at < base.size() && (base[at] == ' ' || base[at] == '_'))
			at++;
		size_t end = at;
		int number = 0;
		while (end < base.size() && isdigit((unsigned char)base[end]) && end - at < 2)
			number = number * 10 + (base[end++] - '0');
		if (end == at && at < base.size() && toupper((unsigned char)base[at]) >= 'A' && toupper((unsigned char)base[at]) <= 'D')
		{
			number = toupper((unsigned char)base[at]) - 'A' + 1;
			end = at + 1;
		}
		if (end == at || number == 0)
			continue;
		// " of 4", "of4"
		for (const char *of : { " of ", "of" })
			if (strncasecmp(base.c_str() + end, of, strlen(of)) == 0)
			{
				size_t count = end + strlen(of);
				const size_t digits = count;
				while (count < base.size() && isdigit((unsigned char)base[count]))
					count++;
				if (count > digits && !alnum(count))
				{
					end = count;
					break;
				}
			}
		if (alnum(end))
			continue;
		size_t from = i;
		if (i > 0 && (base[i - 1] == '(' || base[i - 1] == '[') && end < base.size()
				&& base[end] == (base[i - 1] == '(' ? ')' : ']'))
		{
			from = i - 1;
			end++;
		}
		// What comes before the tag is the game (and its region); what comes
		// after is the disc's own ("(Disc 4) (Passport)").
		rest = base.substr(0, from);
		while (!rest.empty() && (rest.back() == ' ' || rest.back() == '-' || rest.back() == '_'))
			rest.pop_back();
		order = number;
		return !rest.empty();
	}
	return false;
}

void findDiscSets()
{
	discSets.clear();
	for (size_t i = 0; i < games.size(); i++)
		if (!games[i].set.empty())
			discSets[games[i].set].push_back(i);
	for (auto it = discSets.begin(); it != discSets.end(); )
	{
		std::vector<size_t>& discs = it->second;
		std::sort(discs.begin(), discs.end(), [](size_t a, size_t b) { return games[a].discOrder < games[b].discOrder; });
		// A set is two discs or more, each number once: else its files are games of their own.
		bool distinct = discs.size() >= 2;
		for (size_t i = 1; i < discs.size(); i++)
			distinct = distinct && games[discs[i]].discOrder != games[discs[i - 1]].discOrder;
		if (!distinct)
		{
			for (size_t i : discs)
				games[i].set.clear();
			it = discSets.erase(it);
			continue;
		}
		for (size_t i : discs)
			games[i].setSize = (int)discs.size();
		++it;
	}
	setsDirty = true;
	marksDirty = true;
}

// Which disc stands for each set, and the titles the lists show: made again
// when the games, the last game played or the choice in Details changed, and
// when the library goes from its own lists to choosing a disc and back.
void updateDiscSets(bool selectDisk)
{
	const bool grouped = grouping(selectDisk);
	if (!setsDirty && grouped == setsGrouped)
		return;
	setsDirty = false;
	setsGrouped = grouped;
	libraryGeneration++;
	for (auto& [key, discs] : discSets)
	{
		size_t front = discs.front();
		const auto chosen = frontChoice.find(key);
		bool found = false;
		if (chosen != frontChoice.end())
			for (size_t i : discs)
				if (games[i].media.path == chosen->second)
				{
					front = i;
					found = true;
				}
		for (auto path = recentPaths.begin(); !found && path != recentPaths.end(); ++path)
			for (size_t i : discs)
				if (games[i].media.path == *path)
				{
					front = i;
					found = true;
					break;
				}
		for (size_t i : discs)
		{
			games[i].front = i == front;
			games[i].title = grouped ? games[i].setTitle : games[i].fullTitle;
		}
	}
}

// Left out of the library's own lists: a set's other discs.
bool hiddenDisc(const Game& g, bool selectDisk)
{
	return grouping(selectDisk) && !g.set.empty() && !g.front;
}

// ---- favourites and hidden games
//
// ps5_library.cpp keeps them, by the game's path. A game on several discs is
// one game: what is asked for one disc is asked for them all, and what one of
// them is, the set is, whether or not its discs are grouped. A hidden game is
// in none of the library's lists until "Show hidden games" is on in the
// Settings: it is then listed dimmed, so that its details can show it again.
unsigned marksGeneration;
int hiddenGames;		// how many there are, a set counted once

// The files a game is: its own, or every disc of its set.
std::vector<std::string> pathsOf(const Game& g)
{
	std::vector<std::string> paths;
	const auto set = g.set.empty() ? discSets.end() : discSets.find(g.set);
	if (set == discSets.end())
		paths.push_back(g.media.path);
	else
		for (size_t i : set->second)
			paths.push_back(games[i].media.path);
	return paths;
}

// The marks, read again when the games or what is kept about them changed.
void updateMarks()
{
	const unsigned generation = ps5::library::generation();
	if (!marksDirty && generation == marksGeneration)
		return;
	marksDirty = false;
	marksGeneration = generation;
	libraryGeneration++;
	hiddenGames = 0;
	for (Game& g : games)
	{
		const ps5::library::Entry kept = ps5::library::entry(g.media.path);
		g.favourite = kept.favourite;
		g.hidden = kept.hidden;
		hiddenGames += g.hidden && g.set.empty() ? 1 : 0;
	}
	for (const auto& [key, discs] : discSets)
	{
		bool favourite = false, hidden = false;
		for (size_t i : discs)
		{
			favourite = favourite || games[i].favourite;
			hidden = hidden || games[i].hidden;
		}
		for (size_t i : discs)
		{
			games[i].favourite = favourite;
			games[i].hidden = hidden;
		}
		hiddenGames += hidden ? 1 : 0;
	}
}

// Left out of a list: a set's other discs, and a hidden game. The running
// game's own discs are not, while one of them is chosen for it.
bool leftOut(const Game& g, bool selectDisk)
{
	if (hiddenDisc(g, selectDisk))
		return true;
	if (!g.hidden || ps5::options().showHidden)
		return false;
	if (selectDisk)
		for (const Game& running : games)
			if (running.media.path == insertedPath)
				return &running != &g && (g.set.empty() || g.set != running.set);
	return true;
}

Source sourceOf(const std::string& path)
{
	if (ps5::smb::isNetworkPath(path))
		return Network;
	if (path.rfind("/mnt/usb", 0) == 0)
		return Usb;
	return Internal;
}

// The network games: a scanner of their own over the folders of network.cfg,
// and the list its last scan found, kept in <root>/data/network-games.txt.
// The list is shown without asking the share; a scan is run when the Network
// tab is opened and there is no list for these folders, and when the user asks
// (Square on the tab, "Scan for games").
struct NetworkLibrary
{
	GameScanner scanner;
	std::vector<GameMedia> games;
	bool loaded = false;		// the saved list was read
	bool known = false;			// there is a list (it may be empty) for these folders
	bool asked = false;			// the scan for a missing list was started this run
	bool wanted = false;		// a scan is to start
	bool scanning = false;
	unsigned failuresBefore = 0;
	bool unreachable = false;	// the last scan: the share did not answer, or not to the end
	std::string error;			// and what it said
	unsigned generation = 1;	// bumped when the list changes
} net;
unsigned shownNetGeneration;

std::string networkListFile() {
	return ps5::rootDir + "data/network-games.txt";
}

std::vector<std::string> splitTabs(const std::string& line)
{
	std::vector<std::string> fields;
	size_t start = 0;
	for (;;)
	{
		const size_t tab = line.find('\t', start);
		fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
		if (tab == std::string::npos)
			break;
		start = tab + 1;
	}
	return fields;
}

void loadNetworkList()
{
	FILE *f = fopen(networkListFile().c_str(), "r");
	if (f == nullptr)
		return;
	std::vector<GameMedia> list;
	bool sameFolders = false;
	char line[2048];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		const std::vector<std::string> fields = splitTabs(s);
		if (fields[0] == "folders")
			// A list made for other folders is no list.
			sameFolders = std::vector<std::string>(fields.begin() + 1, fields.end()) == ps5::smb::gameFolders();
		else if (fields[0] == "game" && fields.size() >= 6 && ps5::smb::isNetworkPath(fields[1]))
			list.push_back(GameMedia{ fields[3], fields[1], fields[2], fields[4], fields[5] == "1" });
	}
	fclose(f);
	if (!sameFolders)
		return;
	net.games = std::move(list);
	net.known = true;
	net.generation++;
}

void saveNetworkList()
{
	FILE *f = fopen(networkListFile().c_str(), "w");
	if (f == nullptr)
		return;
	fputs("folders", f);
	for (const std::string& folder : ps5::smb::gameFolders())
		fprintf(f, "\t%s", folder.c_str());
	fputc('\n', f);
	for (const GameMedia& m : net.games)
		fprintf(f, "game\t%s\t%s\t%s\t%s\t%d\n", m.path.c_str(), m.fileName.c_str(), m.name.c_str(),
				m.gameName.c_str(), m.arcade ? 1 : 0);
	fclose(f);
}

// Once a frame: starts the scan that was asked for, takes its result.
void networkTick(bool tabOpen)
{
	if (!net.loaded)
	{
		net.loaded = true;
		loadNetworkList();
	}
	const std::vector<std::string>& folders = ps5::smb::gameFolders();
	if (folders.empty())
		return;
	if (tabOpen && !net.known && !net.asked)
	{
		net.asked = true;
		net.wanted = true;
	}
	// A scan that was abandoned (a game was started) may still be waiting for
	// the share's answer: the next one starts when its thread has ended.
	if (net.wanted && !net.scanning && !net.scanner.alive())
	{
		net.wanted = false;
		net.scanning = true;
		net.unreachable = false;
		net.error.clear();
		ps5::smb::retryNow();
		ps5::smb::clearError();
		net.failuresBefore = ps5::smb::failures();
		net.scanner.folders = folders;
		net.scanner.refresh();
		net.scanner.fetch_game_list();
	}
	if (net.scanning && !net.scanner.alive())
	{
		net.scanning = false;
		if (!net.scanner.done())
			return;		// abandoned
		std::vector<GameMedia> found;
		{
			std::lock_guard<std::mutex> lock(net.scanner.get_mutex());
			found = net.scanner.get_game_list();
		}
		net.unreachable = ps5::smb::failures() != net.failuresBefore;
		net.error = ps5::smb::lastError();
		if (!net.unreachable)
		{
			// The whole of the folders was seen: this is the list, and it is kept.
			net.games = std::move(found);
			net.known = true;
			saveNetworkList();
			net.generation++;
		}
		else if (net.games.empty() && !found.empty())
		{
			// The share stopped answering half way and there was no list: what
			// was found is shown, and not kept as if it were everything.
			net.games = std::move(found);
			net.generation++;
		}
		// Otherwise the list of the last whole scan stays.
	}
}

// A scan of the network folders, when the user asks for one.
void scanNetwork()
{
	if (!ps5::smb::gameFolders().empty() && !net.scanning)
		net.wanted = true;
}
bool recentLoaded;

std::string recentFile() {
	return ps5::rootDir + "data/recent.txt";
}

// A path saved under another run's root is read under this run's
// (ps5::underRoot, which the library's own file shares).
using ps5::underRoot;

void loadRecents()
{
	if (recentLoaded)
		return;
	recentLoaded = true;
	libraryGeneration++;
	FILE *f = fopen(recentFile().c_str(), "r");
	if (f == nullptr)
		return;
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		if (!s.empty())
			recentPaths.push_back(underRoot(s));
	}
	fclose(f);
}

void saveRecents()
{
	FILE *f = fopen(recentFile().c_str(), "w");
	if (f == nullptr)
		return;
	for (const auto& p : recentPaths)
		fprintf(f, "%s\n", p.c_str());
	fclose(f);
}

std::string lowerExt(const std::string& path)
{
	std::string e = get_file_extension(path);
	for (char& c : e)
		c = (char)tolower((unsigned char)c);
	return e;
}

// A game's name as shown: its file name without the extension and without the
// dump's tags, "Re-Volt (USA) (En,Fr,De).chd" -> "Re-Volt"; a disc number stays.
std::string cleanTitle(const std::string& name)
{
	std::string base = name;
	const std::string e = lowerExt(name);
	for (const char *known : { "chd", "gdi", "cdi", "cue", "zip", "7z", "bin", "lst", "dat", "elf", "iso", "m3u" })
		if (e == known)
			base = get_file_basename(name);
	std::string out, disc;
	for (size_t i = 0; i < base.size(); i++)
	{
		const char c = base[i];
		if (c == '(' || c == '[')
		{
			const size_t end = base.find(c == '(' ? ')' : ']', i);
			if (end == std::string::npos)
				break;
			const std::string tag = base.substr(i + 1, end - i - 1);
			if (tag.rfind("Disc ", 0) == 0 || tag.rfind("Disk ", 0) == 0 || tag.rfind("GD-ROM ", 0) == 0)
				disc = tag;
			i = end;
			continue;
		}
		out += c == '_' ? ' ' : c;
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '-'))
		out.pop_back();
	while (!out.empty() && out.front() == ' ')
		out.erase(out.begin());
	size_t pos;
	while ((pos = out.find("  ")) != std::string::npos)
		out.erase(pos, 1);
	if (out.empty())
		return base;
	if (!disc.empty())
		out += "  ·  " + disc;
	return out;
}

// The games of every source: the scanner's (the games folder, the USB
// folders) and the network list. True when the list was made again.
bool refreshGames(bool networkOpen)
{
	networkTick(networkOpen);
	GameScanner& sc = scanner();
	sc.fetch_game_list();
	std::lock_guard<std::mutex> lock(sc.get_mutex());
	const auto& list = sc.get_game_list();
	if (list.size() == scannedCount && net.generation == shownNetGeneration)
		return false;
	scannedCount = list.size();
	shownNetGeneration = net.generation;
	games.clear();
	auto add = [](const GameMedia& m) {
		Game g;
		g.media = m;
		g.source = sourceOf(m.path);
		// An arcade set is listed as "file.zip (Its Full Name)": the name is the title.
		g.title = m.arcade && !m.gameName.empty() && m.gameName != get_file_basename(m.fileName)
				? m.gameName : cleanTitle(m.fileName.empty() ? m.name : m.fileName);
		const std::string e = lowerExt(m.path);
		g.disc = !m.path.empty() && (e == "gdi" || e == "chd" || e == "cdi" || e == "cue");
		g.fullTitle = g.setTitle = g.title;
		std::string rest;
		if (g.disc && !m.arcade && splitDisc(get_file_basename(m.fileName.empty() ? m.name : m.fileName), rest, g.discOrder))
		{
			// A set is of one source and one dump: the name's other tags stay in the key.
			g.set = std::to_string((int)g.source) + "\n" + rest;
			for (char& c : g.set)
				c = (char)tolower((unsigned char)c);
			g.setTitle = cleanTitle(rest);
		}
		games.push_back(g);
	};
	for (const auto& m : list)
		if (!m.device && !ps5::smb::isNetworkPath(m.path))
			add(m);
	for (const auto& m : net.games)
		add(m);
	findDiscSets();
	return true;
}

// Box art and the game's details are looked up lazily, for the games on
// screen, and again until they have arrived. A local game's disc is read for
// its ID and picture; a network game's is not (core/ui/boxart/boxart.cpp: the
// share is left alone), and it is looked up online by name.
void ensureArt(Game& g)
{
	const bool complete = g.art.scraped || (!config::FetchBoxart && g.art.parsed);
	if (g.media.path.empty() || (complete && !g.art.busy))
		return;
	// Not every frame: the look-up takes a lock.
	if (timeNow < g.artAskedAt + 0.5)
		return;
	g.artAskedAt = timeNow;
	g.art = boxart().getBoxartAndLoad(g.media);
}

// A cover in <root>/covers/<file name without extension>.png or .jpg - the
// user's own, or a downloaded one - wins over the picture read from the disc.
// Looked up once, and again when a download arrives; a disc game with none is
// asked for (shell/ps5/ps5_covers.cpp).
const std::string& userCover(Game& g)
{
	const unsigned generation = ps5::covers::generation();
	if (g.coverGeneration == generation)
		return g.cover;
	g.coverGeneration = generation;
	g.cover.clear();
	const std::string base = get_file_basename(g.media.fileName);
	for (const char *ext : { ".png", ".jpg", ".jpeg" })
	{
		const std::string p = ps5::rootDir + "covers/" + base + ext;
		if (file_exists(p))
		{
			g.cover = p;
			return g.cover;
		}
	}
	if (g.disc)
		ps5::covers::request(base);
	return g.cover;
}

ImTextureID coverTexture(Game& g)
{
	ensureArt(g);
	std::string path = userCover(g);
	if (path.empty())
		path = g.art.boxartPath;
	if (path.empty())
		return ImTextureID();
	ImguiFileTexture tex(path);
	return tex.getId();
}

// The image cropped to fill the rectangle, its centre kept.
void imageFill(ImDrawList *dl, ImTextureID id, float x, float y, float w, float h, float r, ImU32 tint = col::text)
{
	float ar = imguiDriver != nullptr ? imguiDriver->getAspectRatio(id) : 1.f;
	if (ar <= 0.f)
		ar = 1.f;
	const float boxAr = w / h;
	ImVec2 uv0(0, 0), uv1(1, 1);
	if (ar > boxAr)
	{
		const float k = boxAr / ar;
		uv0.x = (1 - k) / 2;
		uv1.x = 1 - uv0.x;
	}
	else
	{
		const float k = ar / boxAr;
		uv0.y = (1 - k) / 2;
		uv1.y = 1 - uv0.y;
	}
	dl->AddImageRounded(id, V(x, y), V(x + w, y + h), uv0, uv1, tint, r * S);
}

// The image cropped to fill the rectangle, drawn as a grid whose vertex
// alphas are ax[i] * ay[j] * k: art that fades into the backdrop with no seam.
void imageFade(ImDrawList *dl, ImTextureID id, float x, float y, float w, float h, float k,
		const float (&xs)[3], const float (&ax)[3], const float (&ys)[3], const float (&ay)[3])
{
	float ar = imguiDriver != nullptr ? imguiDriver->getAspectRatio(id) : 1.f;
	if (ar <= 0.f)
		ar = 1.f;
	const float boxAr = w / h;
	ImVec2 uv0(0, 0), uv1(1, 1);
	if (ar > boxAr) { const float c = boxAr / ar; uv0.x = (1 - c) / 2; uv1.x = 1 - uv0.x; }
	else { const float c = ar / boxAr; uv0.y = (1 - c) / 2; uv1.y = 1 - uv0.y; }
	dl->PushTexture(id);
	dl->PrimReserve(4 * 6, 9);
	const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
	for (int j = 0; j < 3; j++)
		for (int i = 0; i < 3; i++)
			dl->PrimWriteVtx(V(x + w * xs[i], y + h * ys[j]),
					ImVec2(uv0.x + (uv1.x - uv0.x) * xs[i], uv0.y + (uv1.y - uv0.y) * ys[j]),
					alpha(col::text, k * ax[i] * ay[j]));
	for (int j = 0; j < 2; j++)
		for (int i = 0; i < 2; i++)
		{
			const ImDrawIdx a = base + j * 3 + i;
			dl->PrimWriteIdx(a); dl->PrimWriteIdx(a + 1); dl->PrimWriteIdx(a + 4);
			dl->PrimWriteIdx(a); dl->PrimWriteIdx(a + 4); dl->PrimWriteIdx(a + 3);
		}
	dl->PopTexture();
}

// A card for a game with no art: its title on a tinted gradient, with the mark.
void placeholder(ImDrawList *dl, const Game& g, float x, float y, float w, float h, float r)
{
	unsigned hash = 2166136261u;
	for (char c : g.title)
		hash = (hash ^ (unsigned char)c) * 16777619u;
	const float hue = (hash % 360) / 360.f;
	float cr, cg, cb;
	ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.42f, cr, cg, cb);
	const ImU32 top = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 1.f));
	rect(dl, x, y, w, h, col::card, r);
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h * 0.75f), alpha(top, 0.9f), alpha(top, 0.6f), alpha(top, 0.f), alpha(top, 0.f));
	discMark(dl, x + w * 0.5f, y + h * 0.36f, w * 0.17f, col::rgba(255, 255, 255, 84));
	const float size = std::max(18.f, w * 0.085f);
	dl->PushClipRect(V(x + 12, y), V(x + w - 12, y + h), true);
	const ImVec2 ts = textSize(bold(), size, g.title.c_str(), w - 32);
	text(dl, bold(), size, x + 16, y + h - 16 - std::min(ts.y, h * 0.45f), col::text, g.title.c_str(), w - 32);
	dl->PopClipRect();
}

void drawCover(ImDrawList *dl, Game& g, float x, float y, float w, float h, float r, float lift)
{
	// Drop shadow.
	rect(dl, x + 6, y + 12, w, h, col::rgba(0, 0, 0, (int)(70 + 60 * lift)), r);
	const ImTextureID id = coverTexture(g);
	if (id != ImTextureID())
	{
		rect(dl, x, y, w, h, col::card, r);
		imageFill(dl, id, x, y, w, h, r);
	}
	else
		placeholder(dl, g, x, y, w, h, r);
	// A sheen on the top edge.
	dl->AddRectFilledMultiColor(V(x, y), V(x + w, y + h * 0.18f), col::rgba(255, 255, 255, (int)(18 + 20 * lift)),
			col::rgba(255, 255, 255, (int)(18 + 20 * lift)), 0, 0);
	// A set shown as one entry: how many discs it has.
	if (setsGrouped && g.setSize > 1 && w >= 140)
	{
		const float size = std::clamp(w * 0.075f, 16.f, 22.f);
		const std::string label = std::string(ICON_FA_COMPACT_DISC) + "  " + std::to_string(g.setSize);
		const ImVec2 ls = textSize(bold(), size, label.c_str());
		const float bw = ls.x + size * 1.2f, bh = size * 1.8f;
		rect(dl, x + w - bw - 10, y + 10, bw, bh, alpha(col::bar, 0.92f), bh / 2);
		text(dl, bold(), size, x + w - bw - 10 + (bw - ls.x) / 2, y + 10 + (bh - ls.y) / 2, col::text, label.c_str());
	}
}

// What the library marks on a cover in its lists: a star on a favourite (in
// the grid; the shelves have a shelf of them), and a hidden game that is
// listed after all is dimmed, under an eye struck through.
void coverMarks(ImDrawList *dl, const Game& g, float x, float y, float w, float h, float r, bool star)
{
	if (g.hidden)
		rect(dl, x, y, w, h, alpha(col::bgTop, 0.64f), r);
	const float size = std::clamp(w * 0.075f, 16.f, 22.f), bh = size * 1.8f;
	float bx = x + 10;
	auto badge = [&](const char *icon, ImU32 colour) {
		const ImVec2 is = textSize(regular(), size, icon);
		const float bw = std::max(bh, is.x + size);
		rect(dl, bx, y + 10, bw, bh, alpha(col::bar, 0.92f), bh / 2);
		text(dl, regular(), size, bx + (bw - is.x) / 2, y + 10 + (bh - is.y) / 2, colour, icon);
		bx += bw + 6;
	};
	if (star && g.favourite)
		badge(ICON_FA_STAR, col::warm);
	if (g.hidden)
		badge(ICON_FA_EYE_SLASH, col::text);
}

struct Shelf
{
	std::string name;
	std::vector<size_t> items;	// indexes into games
	int focus = 0;
	float scroll = 0;	// in cards, animated
	float focusAnim = -1;	// where the focus ring is, on its way to focus
};

// The library as one tab shows it. Each tab keeps its own (the shelf, the
// focus, the scroll), put aside while another is open.
struct LibraryState
{
	int source = Internal;
	std::vector<Shelf> shelves;
	int shelf = 0;
	float shelfAnim = 0;
	bool details = false;
	float detailsAnim = 0;
	float heroFade = 1;
	std::string heroPath;
	// The grid and list views: every game of the tab, by title.
	std::vector<size_t> flat;
	int flatFocus = 0;
	std::string flatPath;	// the focused game, which survives a new scan
	float flatScroll = 0;	// grid: rows; list: rows
	// Where the focus is in the grid (a column and a row) and in the list (a
	// row), on its way to the focused game.
	float ringColumn = -1, ringRow = 0, listRow = -1;
	// What the shelves and the flat list were last made for (listsKey).
	size_t shelvesKey = 0, flatKey = 0;
	// The letter L2 / R2 last jumped to, shown for a moment.
	char letter = 0;
	double letterAt = -10;
} lib;
LibraryState otherTabs[SourceCount];
bool sourceChosen;

// What the library is searched for (R3 and the console's keyboard): the games
// whose title has every word of it, in any tab. Empty: every game.
std::string librarySearch;
std::vector<std::string> searchWords;		// its words, in small letters

std::string trimmed(const std::string& given)
{
	const size_t from = given.find_first_not_of(" \t\r\n");
	if (from == std::string::npos)
		return "";
	return given.substr(from, given.find_last_not_of(" \t\r\n") - from + 1);
}

std::string lowered(std::string words)
{
	for (char& c : words)
		c = (char)tolower((unsigned char)c);
	return words;
}

void setSearch(const std::string& given)
{
	librarySearch = trimmed(given);
	searchWords.clear();
	std::string word;
	for (const char c : lowered(librarySearch) + " ")
		if (c == ' ')
		{
			if (!word.empty())
				searchWords.push_back(word);
			word.clear();
		}
		else
			word += c;
	// From the first of what is found, in this tab and the others.
	lib.shelf = 0;
	lib.flatFocus = 0;
	lib.flatPath.clear();
	lib.flatScroll = 0;
	for (LibraryState& other : otherTabs)
	{
		other.shelf = 0;
		other.flatFocus = 0;
		other.flatPath.clear();
		other.flatScroll = 0;
	}
}

std::string searchShelfName()
{
	return "Search: " + librarySearch;
}
// The game on the loading screen.
std::string loadingPath, loadingTitle;

void switchSource(int source)
{
	if (source == lib.source)
		return;
	entrance.next = source > lib.source ? 1.f : -1.f;
	otherTabs[lib.source] = lib;
	lib = otherTabs[source];
	lib.source = source;
	lib.details = false;
	lib.detailsAnim = 0;
	lib.heroPath.clear();
	ps5::options().source = source;
	ps5::saveOptions();
}

// The tab the library opens on: the one last open, if it is still there.
void chooseSource()
{
	if (!sourceChosen)
	{
		sourceChosen = true;
		lib.source = std::clamp(ps5::options().source, 0, SourceCount - 1);
	}
	if (lib.source == Usb && !ps5::options().usb)
		switchSource(Internal);
}

const char *regionText(u32 r);

enum View { Shelves, Grid, List };

View view()
{
	return (View)std::clamp(ps5::options().view, 0, 2);
}

// What the open tab's lists depend on, as one number.
size_t listsKey(bool selectDisk)
{
	size_t key = libraryGeneration;
	key = key * 31 + (size_t)lib.source;
	key = key * 31 + (selectDisk ? 1 : 0);
	key = key * 31 + (grouping(selectDisk) ? 1 : 0);
	key = key * 31 + (ps5::options().showHidden ? 1 : 0);
	if (selectDisk)
		key = key * 31 + std::hash<std::string>()(insertedPath);
	key = key * 31 + std::hash<std::string>()(librarySearch);
	return key;
}

bool matchesSearch(const Game& g)
{
	if (searchWords.empty())
		return true;
	const std::string title = lowered(g.title + " " + g.setTitle);
	for (const std::string& word : searchWords)
		if (title.find(word) == std::string::npos)
			return false;
	return true;
}

void buildFlat(bool selectDisk)
{
	const size_t key = listsKey(selectDisk);
	if (key == lib.flatKey)
		return;
	lib.flatKey = key;
	lib.flat.clear();
	for (size_t i = 0; i < games.size(); i++)
		if (games[i].source == lib.source && (!selectDisk || games[i].disc) && !leftOut(games[i], selectDisk)
				&& matchesSearch(games[i]))
			lib.flat.push_back(i);
	std::sort(lib.flat.begin(), lib.flat.end(), [](size_t a, size_t b) {
		return strcasecmp(games[a].title.c_str(), games[b].title.c_str()) < 0;
	});
	// The focus stays on its game when the list changes around it.
	if (!lib.flatPath.empty())
		for (size_t i = 0; i < lib.flat.size(); i++)
			if (games[lib.flat[i]].media.path == lib.flatPath)
				lib.flatFocus = (int)i;
	lib.flatFocus = std::clamp(lib.flatFocus, 0, std::max(0, (int)lib.flat.size() - 1));
}

// The letter a game is filed under: its title's first letter, '#' for a digit.
char letterOf(const Game& g)
{
	for (char c : g.title)
		if (isalnum((unsigned char)c))
			return isdigit((unsigned char)c) ? '#' : (char)toupper((unsigned char)c);
	return '#';
}

// L2 and R2 in a list sorted by title: R2 goes to the first game of the next
// letter, L2 to the first of this letter and, from there, of the one before.
int letterJump(const std::vector<size_t>& items, int focus, int direction)
{
	const int n = (int)items.size();
	if (n == 0)
		return 0;
	focus = std::clamp(focus, 0, n - 1);
	auto letter = [&](int i) { return letterOf(games[items[i]]); };
	int to = focus;
	if (direction > 0)
	{
		while (to < n - 1 && letter(to) == letter(focus))
			to++;
		if (letter(to) == letter(focus))
			to = n - 1;		// the last letter: its last game
	}
	else
	{
		while (to > 0 && letter(to - 1) == letter(focus))
			to--;
		if (to == focus && to > 0)
		{
			to--;
			while (to > 0 && letter(to - 1) == letter(to))
				to--;
		}
	}
	lib.letter = letter(to);
	lib.letterAt = timeNow;
	return to;
}

// The focus follows the game from one view to the next.
void focusPath(const std::string& path)
{
	lib.flatPath = path;
	for (size_t i = 0; i < lib.flat.size(); i++)
		if (games[lib.flat[i]].media.path == path)
			lib.flatFocus = (int)i;
	for (size_t si = 0; si < lib.shelves.size(); si++)
		for (size_t i = 0; i < lib.shelves[si].items.size(); i++)
			if (games[lib.shelves[si].items[i]].media.path == path
					&& (lib.shelves[si].name != "Recently played" || lib.shelves.size() == 1))
			{
				lib.shelf = (int)si;
				lib.shelves[si].focus = (int)i;
			}
}

// The focus, put back on a game after the lists changed around it: on the
// shelf it was on if the game is still there (the other shelves keep their
// own focus), else where focusPath has it. False when no list has the game
// any more.
bool refocus(const std::string& path, const std::string& shelf)
{
	bool listed = false;
	for (size_t i : lib.flat)
		listed = listed || games[i].media.path == path;
	for (const Shelf& sh : lib.shelves)
		for (size_t i : sh.items)
			listed = listed || games[i].media.path == path;
	if (!listed)
		return false;
	for (size_t si = 0; si < lib.shelves.size(); si++)
		if (lib.shelves[si].name == shelf)
			for (size_t i = 0; i < lib.shelves[si].items.size(); i++)
				if (games[lib.shelves[si].items[i]].media.path == path)
				{
					lib.shelf = (int)si;
					lib.shelves[si].focus = (int)i;
					lib.flatPath = path;
					for (size_t at = 0; at < lib.flat.size(); at++)
						if (games[lib.flat[at]].media.path == path)
							lib.flatFocus = (int)at;
					return true;
				}
	focusPath(path);
	return true;
}

std::string platformText(const Game& g)
{
	return g.media.arcade ? "ARCADE" : "DREAMCAST";
}

std::string metaText(Game& g)
{
	std::string meta;
	if (*regionText(g.art.region))
		meta = regionText(g.art.region);
	if (!g.art.releaseDate.empty())
		meta += (meta.empty() ? "" : "   ·   ") + g.art.releaseDate;
	// A set shown as one entry: which of its discs this is.
	if (setsGrouped && g.setSize > 1)
		meta += (meta.empty() ? "" : "   ·   ") + ("Disc " + std::to_string(g.discOrder) + " of " + std::to_string(g.setSize));
	return meta;
}

void buildShelves(bool selectDisk)
{
	loadRecents();
	updateDiscSets(selectDisk);
	updateMarks();
	const size_t key = listsKey(selectDisk);
	if (key == lib.shelvesKey)
		return;
	lib.shelvesKey = key;
	std::vector<Shelf> shelves;
	if (!librarySearch.empty())
	{
		// One shelf: what was found, by title.
		Shelf found{ searchShelfName() };
		for (size_t i = 0; i < games.size(); i++)
			if (games[i].source == lib.source && (!selectDisk || games[i].disc) && !leftOut(games[i], selectDisk)
					&& matchesSearch(games[i]))
				found.items.push_back(i);
		std::sort(found.items.begin(), found.items.end(), [](size_t a, size_t b) {
			return strcasecmp(games[a].title.c_str(), games[b].title.c_str()) < 0;
		});
		if (!found.items.empty())
			shelves.push_back(found);
		for (const auto& old : lib.shelves)
			if (!shelves.empty() && old.name == shelves[0].name)
			{
				shelves[0].focus = std::clamp(old.focus, 0, (int)shelves[0].items.size() - 1);
				shelves[0].scroll = old.scroll;
				shelves[0].focusAnim = old.focusAnim;
			}
		lib.shelves = std::move(shelves);
		lib.shelf = 0;
		return;
	}
	if (!selectDisk)
	{
		Shelf recent{ "Recently played" };
		for (const auto& path : recentPaths)
			for (size_t i = 0; i < games.size(); i++)
				if (games[i].media.path == path && games[i].source == lib.source)
				{
					// A set is there once, by the disc that stands for it.
					size_t shown = i;
					if (hiddenDisc(games[i], selectDisk))
						for (size_t disc : discSets[games[i].set])
							if (games[disc].front)
								shown = disc;
					if (!leftOut(games[shown], selectDisk)
							&& std::find(recent.items.begin(), recent.items.end(), shown) == recent.items.end())
						recent.items.push_back(shown);
				}
		if (!recent.items.empty())
			shelves.push_back(recent);
		// The favourites, by title: the tab's Dreamcast and arcade games alike.
		Shelf favourites{ "Favourites" };
		for (size_t i = 0; i < games.size(); i++)
			if (games[i].favourite && games[i].source == lib.source && !leftOut(games[i], selectDisk))
				favourites.items.push_back(i);
		std::sort(favourites.items.begin(), favourites.items.end(), [](size_t a, size_t b) {
			return strcasecmp(games[a].title.c_str(), games[b].title.c_str()) < 0;
		});
		if (!favourites.items.empty())
			shelves.push_back(favourites);
	}
	else
	{
		// The running game's own discs first, in their order.
		Shelf own{ "This game's discs" };
		for (const Game& g : games)
			if (g.media.path == insertedPath && !g.set.empty() && g.source == lib.source)
				own.items = discSets[g.set];
		if (!own.items.empty())
			shelves.push_back(own);
	}
	Shelf all{ selectDisk ? "Choose a disc" : "All games" };
	Shelf arcade{ "Arcade" };
	for (size_t i = 0; i < games.size(); i++)
	{
		if (games[i].source != lib.source || (selectDisk && !games[i].disc) || leftOut(games[i], selectDisk))
			continue;
		if (!selectDisk && games[i].media.arcade)
			arcade.items.push_back(i);
		else
			all.items.push_back(i);
	}
	for (Shelf *shelf : { &all, &arcade })
		std::sort(shelf->items.begin(), shelf->items.end(), [](size_t a, size_t b) {
			return strcasecmp(games[a].title.c_str(), games[b].title.c_str()) < 0;
		});
	if (!all.items.empty())
		shelves.push_back(all);
	if (!arcade.items.empty())
		shelves.push_back(arcade);
	// Keep the focus where it was.
	for (auto& s : shelves)
		for (const auto& old : lib.shelves)
			if (old.name == s.name)
			{
				s.focus = std::clamp(old.focus, 0, std::max(0, (int)s.items.size() - 1));
				s.scroll = old.scroll;
				s.focusAnim = old.focusAnim;
			}
	lib.shelves = std::move(shelves);
	lib.shelf = std::clamp(lib.shelf, 0, std::max(0, (int)lib.shelves.size() - 1));
}

// A network disc being inserted: off this thread, which the share must not
// hold up (its NAS may be waking), behind the loading screen.
struct DiscSwap
{
	std::future<void> task;
	bool active = false;
	bool cancelling = false;
} discSwap;

// stateSlot: the state the game starts from (0-based), or -1 for a fresh start.
void launch(Game& g, bool selectDisk, int stateSlot = -1)
{
	ensureArt(g);
	loadingPath = g.media.path;
	loadingTitle = g.title.empty() ? get_file_basename(g.media.fileName) : g.title;
	// The share is the game's from here on: a scan in progress gives way.
	if (g.source == Network)
		net.scanner.abandon();
	insertedPath = g.media.path;
	if (selectDisk && g.source == Network)
	{
		ps5::smb::beginLoad();
		discSwap.active = true;
		discSwap.cancelling = false;
		const std::string path = g.media.path;
		discSwap.task = std::async(std::launch::async, [path] { emu.insertGdrom(path); });
		return;
	}
	if (selectDisk)
	{
		try {
			emu.insertGdrom(g.media.path);
			gui_setState(GuiState::Closed);
		} catch (const FlycastException& e) {
			gui_error(e.what());
		}
		return;
	}
	::settings.content.title = loadingTitle;
	ps5::games::loadStateAtStart(stateSlot);
	if (g.source == Network)
		ps5::smb::beginLoad();
	gameStarted(g.media.path);
	gui_start_game(g.media.path);
}

const char *regionText(u32 r)
{
	static std::string s;
	s.clear();
	if (r & GameBoxart::USA) s += "USA ";
	if (r & GameBoxart::EUROPE) s += "Europe ";
	if (r & GameBoxart::JAPAN) s += "Japan ";
	if (!s.empty())
		s.pop_back();
	return s.c_str();
}

// A tab with no games: what it shows, and what to do about it.
void emptyLibrary(ImDrawList *dl, bool selectDisk)
{
	const std::string root = shownRoot();
	std::string title, body, foot = "Press Square to scan again.";
	bool busy = false;
	if (!librarySearch.empty())
	{
		title = "No game here matches";
		body = "\"" + librarySearch + "\"\n\nThe titles of this tab's games were searched for every word of it.\n"
				"L1 and R1 look in the other tabs.";
		foot = "Press R3 to search for something else, or Circle to show every game.";
	}
	else if (lib.source == Network)
	{
		const std::vector<std::string>& folders = ps5::smb::gameFolders();
		std::string named;
		for (const std::string& folder : folders)
			named += (named.empty() ? "" : "\n") + folder.substr(6);
		if (folders.empty())
		{
			title = "No network share yet";
			body = "PSFlyCast reads games from an SMB share (Windows sharing, a NAS).\n"
					"Name the folder in\n" + root + "network.cfg\n\n"
					"path = 192.168.1.10/Share/Folder\n"
					"user = guest\n"
					"password =";
			foot = "Then start PSFlyCast again.";
		}
		else if (net.scanning || net.wanted)
		{
			busy = true;
			title = "Looking for games on the share";
			const std::string doing = ps5::smb::status().text;
			body = named + "\n\n" + (doing.empty() ? "Reading the folders..." : doing + "\nA NAS that let its disks sleep takes a while to answer.");
			foot = "";
		}
		else if (!net.error.empty())
		{
			title = net.unreachable ? "The share did not answer" : "The folder could not be read";
			body = named + "\n\n" + net.error;
			foot = "Press Square to try again.";
		}
		else
		{
			title = selectDisk ? "No disc images on the share" : "No games on the share";
			body = named + "\n\nDisc images: .chd  .gdi  .cdi  .cue     Arcade: .zip  .7z";
		}
	}
	else if (lib.source == Usb)
	{
		if (!ps5::elevated)
		{
			title = "USB drives are not available";
			body = "PSFlyCast leaves its sandbox to read USB drives, and that takes elfldr "
					"running on the console (port 9021) when PSFlyCast starts.";
			foot = "Start elfldr, then start PSFlyCast again.";
		}
		else if (ps5::usbDirs.empty())
		{
			title = "No game folder on a USB drive";
			body = "Put your games in a folder named\nflycast, dreamcast, dc, naomi, atomiswave or arcade\n"
					"at the top of the drive.\n\nDisc images: .chd  .gdi  .cdi  .cue     Arcade: .zip  .7z";
			foot = "Press Square to look again.";
		}
		else
		{
			title = selectDisk ? "No disc images on the USB drive" : "No games on the USB drive";
			for (const std::string& dir : ps5::usbDirs)
				body += (body.empty() ? "" : "\n") + dir;
			body += "\n\nDisc images: .chd  .gdi  .cdi  .cue     Arcade: .zip  .7z";
		}
	}
	else
	{
		title = selectDisk ? "No disc images found" : "Your library is empty";
		body = "Copy your games to\n" + root + "games/\n\n"
				"Disc images: .chd  .gdi  .cdi  .cue     Arcade: .zip  .7z\n"
				"BIOS files (optional, recommended): " + root + "bios/\n"
				"Covers (optional): " + root + "covers/<game file name>.png";
	}
	const float w = 1100, h = 460;
	const float x = (W - w) / 2, y = 250;
	rect(dl, x, y, w, h, col::panel, 24);
	spinMark(dl, x + 120, y + 130, 52);
	if (busy)
	{
		// An arc going round the mark while the share is asked.
		const float turn = (float)timeNow * 3.2f;
		dl->PathClear();
		dl->PathArcTo(V(x + 120, y + 130), 74 * S, turn, turn + IM_PI * 0.7f, 24);
		dl->PathStroke(col::accent, 0, 6 * S);
	}
	text(dl, bold(), 46, x + 220, y + 80, col::text, title.c_str());
	text(dl, regular(), 28, x + 220, y + 150, col::dim, body.c_str(), w - 280);
	text(dl, regular(), 24, x + 220, y + h - 64, col::faint, foot.c_str());
}

// ---- the details panel: the game, its description, and what can be done
// with it before it starts - play, start from a saved state, options of its
// own, cheats - and Manage: a favourite or not, hidden or not, its cover, and
// how long it was played.

// A game's ID, under which Flycast keeps its own options and its cheats are
// kept: the one learned when it last ran, else a local disc's product number
// as the scraper read it. Empty for a network or arcade game not started yet.
std::string gameIdOf(const Game& g)
{
	const std::string known = ps5::games::knownId(g.media.path);
	if (!known.empty())
		return known;
	if (g.disc && !g.media.arcade && g.source != Network)
		return g.art.uniqueId;
	return "";
}

// The options a game can have of its own: Flycast's per-game settings, a
// section of emu.cfg named after the game's ID with one entry for each option
// that differs from the Settings.
//
// One table for three screens, so that none of them lacks what another has:
// the Settings (the value for every game), Details > Options in the library
// (a game's own values, written to its section) and the quick menu's "Game
// options" (the running game's own values: in effect at once, and written to
// its section when the page is left).
struct GameOption
{
	// Own: a Flycast option that only a game has here (the Settings do not
	// show it): what is right for one game and wrong for the rest.
	enum Category { Video, Audio, System, Controls, Patch, Own };
	const char *label;
	const char *desc;
	// In a game's section of emu.cfg: the option's section, a dot, its name.
	// Null for what is not a Flycast option and is the same for every game
	// (the controller's vibration and dead zone).
	const char *key;
	bool isBool;
	std::vector<const char *> names;
	std::vector<int> values;
	Category category;
	bool nextStart;					// a running game takes it the next time it starts
	std::function<int()> get;		// the value in effect
	std::function<void(int)> set;
	int step = 0;					// a range (a slider in the Settings): its step
	const char *unit = "";
	// A game patch (ps5_patches.cpp): its kind. Offered for the games that
	// have one, and never in the Settings: it is a game's or nobody's.
	const char *patch = nullptr;
	// A choice made of several of the emulator's options (transparency sorting
	// is the renderer, how it sorts and its layers): those options, and for
	// each choice the value of each, or Any where the choice leaves it alone.
	// The values of such an option are the choices' indices.
	struct Part
	{
		const char *key;
		bool isBool;
		std::function<int()> get;
		std::function<void(int)> set;
	};
	static constexpr int Any = INT_MIN;
	std::vector<Part> parts;
	std::vector<std::vector<int>> combos;
};

std::shared_ptr<GamepadDevice> pad1();

// A range of numbers, each a choice.
GameOption range(const char *label, const char *desc, const char *key, int low, int high, int step, const char *unit,
		GameOption::Category category, std::function<int()> get, std::function<void(int)> set)
{
	static std::deque<std::string> names;		// kept: the options point at them
	GameOption o{ label, desc, key, false, {}, {}, category, false, std::move(get), std::move(set), step, unit };
	for (int v = low; v <= high; v += step)
	{
		names.push_back(std::to_string(v) + unit);
		o.names.push_back(names.back().c_str());
		o.values.push_back(v);
	}
	return o;
}

template<typename T>
GameOption onOff(const char *label, const char *desc, const char *key, T& opt, GameOption::Category category,
		bool nextStart = false)
{
	GameOption o{ label, desc, key, true, { "Off", "On" }, { 0, 1 }, category, nextStart };
	o.get = [&opt] { return opt.get() ? 1 : 0; };
	o.set = [&opt](int v) { opt.set(v != 0); };
	return o;
}

template<typename T>
GameOption pick(const char *label, const char *desc, const char *key, std::vector<const char *> names,
		std::vector<int> values, T& opt, GameOption::Category category, bool nextStart = false)
{
	GameOption o{ label, desc, key, false, std::move(names), std::move(values), category, nextStart };
	o.get = [&opt] { return (int)opt.get(); };
	o.set = [&opt](int v) { opt.set((std::decay_t<decltype(opt.get())>)v); };
	return o;
}

// A game patch's switch: kept in the running game's section of emu.cfg (where
// Details > Options keeps it for a game that is not running).
GameOption patchSwitch(const char *label, const char *desc, const char *key, const char *kind)
{
	GameOption o{ label, desc, key, true, { "Off", "On" }, { 0, 1 }, GameOption::Patch, true };
	o.patch = kind;
	o.get = [key] { return config::loadBool(::settings.content.gameId, key, false) ? 1 : 0; };
	o.set = [key](int v) {
		const std::string& id = ::settings.content.gameId;
		if (id.empty())
			return;
		if (v != 0)
			config::saveBool(id, key, true);
		else
			config::deleteEntry(id, key);
	};
	return o;
}

// What only a game can have, and is no patch from the list: kept like a
// patch's switch, in the game's section of emu.cfg. The first choice is off.
GameOption ownChoice(const char *label, const char *desc, const char *key, std::vector<const char *> names,
		std::vector<int> values)
{
	GameOption o{ label, desc, key, false, std::move(names), std::move(values), GameOption::Patch, true };
	const int off = o.values[0];
	o.get = [key, off] { return config::loadInt(::settings.content.gameId, key, off); };
	o.set = [key, off](int v) {
		const std::string& id = ::settings.content.gameId;
		if (id.empty())
			return;
		if (v != off)
			config::saveInt(id, key, v);
		else
			config::deleteEntry(id, key);
	};
	return o;
}

template<typename T>
GameOption::Part part(const char *key, T& opt)
{
	using V = std::decay_t<decltype(opt.get())>;
	return { key, std::is_same_v<V, bool>, [&opt] { return (int)opt.get(); }, [&opt](int v) { opt.set((V)v); } };
}

// Which choice the parts' values make: the first whose values they all have.
int comboOf(const GameOption& option, const std::vector<int>& values)
{
	for (size_t c = 0; c < option.combos.size(); c++)
	{
		bool same = true;
		for (size_t i = 0; i < values.size() && same; i++)
			same = option.combos[c][i] == GameOption::Any || option.combos[c][i] == values[i];
		if (same)
			return (int)c;
	}
	return -1;
}

// One choice over several options (GameOption::parts).
GameOption combined(const char *label, const char *desc, std::vector<const char *> names,
		std::vector<GameOption::Part> parts, std::vector<std::vector<int>> combos, GameOption::Category category)
{
	GameOption o{ label, desc, parts[0].key, false, std::move(names), {}, category, false };
	for (size_t c = 0; c < combos.size(); c++)
		o.values.push_back((int)c);
	o.parts = std::move(parts);
	o.combos = std::move(combos);
	const GameOption copy = o;
	o.get = [copy] {
		std::vector<int> values;
		for (const GameOption::Part& p : copy.parts)
			values.push_back(p.get());
		return comboOf(copy, values);
	};
	o.set = [copy](int choice) {
		if (choice < 0 || choice >= (int)copy.combos.size())
			return;
		for (size_t i = 0; i < copy.parts.size(); i++)
			if (copy.combos[choice][i] != GameOption::Any)
				copy.parts[i].set(copy.combos[choice][i]);
	};
	return o;
}

const std::vector<GameOption>& gameOptions()
{
	using G = GameOption;
	static const std::vector<GameOption> rows = {
		patchSwitch("60 FPS patch", "Makes this game run at 60 frames a second. Turn it off if the game runs too fast or breaks",
				"ps5.patch.60fps", "60fps"),
		patchSwitch("Widescreen patch",
				"The game draws a 16:9 picture itself. For a game the Widescreen game patches option does nothing for",
				"ps5.patch.widescreen", "widescreen"),
		ownChoice("Object draw distance",
				"For Sonic Adventure and Sonic Adventure 2: rings, enemies and boxes appear from further away. Experimental: too far can slow the game or break it",
				"ps5.drawdist", { "Off", "1.5x", "2x", "3x", "5x" }, { 100, 150, 200, 300, 500 }),

		combined("Transparency sorting",
				"Per-triangle suits most games. Per-pixel is the most accurate, and the heaviest",
				{ "Per-triangle", "Per-strip", "Per-pixel" },
				{ part("config.pvr.rend", config::RendererType), part("config.rend.PerStripSorting", config::PerStripSorting) },
				{ { (int)RenderType::Vulkan, 0 }, { (int)RenderType::Vulkan, 1 }, { (int)RenderType::Vulkan_OIT, G::Any } },
				G::Video),
		// Flycast's own "Maximum Layers", 8 to 128. For some games Flycast
		// itself sets a number that is none of these (core/emulator.cpp): it
		// is shown as that number (oddValue).
		pick("Per-pixel layers",
				"For per-pixel sorting: the see-through surfaces it orders at a pixel. More is slower; 32 suits most",
				"config.rend.PerPixelLayers", { "8", "16", "32", "64", "96", "128" }, { 8, 16, 32, 64, 96, 128 },
				config::PerPixelLayers, G::Video),
		pick("Internal resolution", "The Dreamcast renders 640 x 480; higher looks sharper on a 4K TV",
				"config.rend.Resolution",
				{ "Native (480p)", "2x (960p)", "3x (1440p)", "4x (1920p)", "5x (2400p)", "6x (2880p)", "7x (3360p)",
						"8x (3840p)", "9x (4320p)", "10x (4800p)" },
				{ 480, 960, 1440, 1920, 2400, 2880, 3360, 3840, 4320, 4800 }, config::RenderResolution, G::Video),
		onOff("Widescreen", "Draws outside the 4:3 frame where the game allows it", "config.rend.WideScreen",
				config::Widescreen, G::Video),
		[] {
			// Flycast's own settings grey this out with "Super widescreen" and
			// grey out "Horizontal stretching" with this. Neither is among
			// these options (a television is never wider than 16:9), so the
			// first rule is kept here, for an emu.cfg that has super widescreen
			// on: it is then not turned on.
			GameOption o = onOff("Stretch to fill",
					"Stretches the 4:3 picture over the whole screen: no black bars, and everything a third wider",
					"config.rend.StretchToFill", config::StretchToFill, GameOption::Video);
			o.set = [](int v) { config::StretchToFill.set(v != 0 && !config::SuperWidescreen); };
			return o;
		}(),
		onOff("Widescreen game patches", "Built-in 16:9 patches for the games that have one",
				"config.rend.WidescreenGameHacks", config::WidescreenGameHacks, G::Video, true),
		pick("Anisotropic filtering", "Sharper textures at grazing angles", "config.rend.AnisotropicFiltering",
				{ "Off", "2x", "4x", "8x", "16x" }, { 1, 2, 4, 8, 16 }, config::AnisotropicFiltering, G::Video),
		pick("Texture filtering", "How textures are sampled", "config.rend.TextureFiltering",
				{ "Game default", "Nearest (pixelated)", "Linear (smooth)" }, { 0, 1, 2 }, config::TextureFiltering,
				G::Video),
		pick("Texture upscaling", "xBRZ upscaling of the game's textures (uses CPU)", "config.rend.TextureUpscale2",
				{ "Off", "2x", "3x", "4x", "5x", "6x" }, { 1, 2, 3, 4, 5, 6 }, config::TextureUpscale, G::Video),
		pick("Upscale textures up to", "The largest texture that is upscaled. Larger sizes take more memory and can make a game hitch",
				"config.rend.MaxFilteredTextureSize", { "256 x 256", "512 x 512", "1024 x 1024" }, { 256, 512, 1024 },
				config::MaxFilteredTextureSize, G::Video),
		onOff("Custom textures", "Replacement textures from data/textures/<the game's ID>, for a texture pack you have",
				"config.rend.CustomTextures", config::CustomTextures, G::Video),
		onOff("Mipmaps", "Smaller copies of a texture for far surfaces; off for a game whose textures look wrong",
				"config.rend.UseMipmaps", config::UseMipmaps, G::Video),
		pick("Automatic frame skipping", "Skips rendering frames when the game runs slow", "config.pvr.AutoSkipFrame",
				{ "Off", "Normal", "Maximum" }, { 0, 1, 2 }, config::AutoSkipFrame, G::Video),
		onOff("Native depth interpolation",
				"Stops textures warping on this console's graphics chip; a few games draw better with it off",
				"config.rend.NativeDepthInterpolation", config::NativeDepthInterpolation, G::Video),
		onOff("Full framebuffer emulation",
				"For the few games that draw straight to the screen; slower, and the picture stays at 480p",
				"config.rend.EmulateFramebuffer", config::EmulateFramebuffer, G::Video),
		onOff("Copy rendered textures to VRAM", "For games that read back what they drew; slower",
				"config.rend.RenderToTextureBuffer", config::RenderToTextureBuffer, G::Video),
		onOff("Delay frame swapping", "Avoids a flashing screen and glitchy videos in some games",
				"config.rend.DelayFrameSwapping", config::DelayFrameSwapping, G::Video),
		onOff("Show frame rate", "The FPS counter in the corner", "config.rend.ShowFPS", config::ShowFPS, G::Video),
		// Flycast's overlay (core/rend/vulkan/overlay.cpp): each memory card's
		// 48 x 32 dots at 288 x 192 pixels of a 4K screen, three quarters opaque,
		// 24 pixels from its corner. The first controller's card has the top
		// left corner, the second controller's the top right.
		onOff("Memory card screen", "Shows the memory card's little screen in a corner of the picture while you play",
				"config.rend.FloatVMUs", config::FloatVMUs, G::Video),
		onOff("Integer scaling", "Whole-number scaling of the picture, no blur", "config.rend.IntegerScale",
				config::IntegerScale, G::Video),
		onOff("Smooth scaling", "Bilinear filtering of the final picture", "config.rend.LinearInterpolation",
				config::LinearInterpolation, G::Video),
		pick("Upscaling", "FSR 1 sharpens a picture smaller than the screen. Scanlines and CRT show it as an old TV would",
				"ps5.Upscaling", { "Off", "FSR 1 (soft)", "FSR 1", "FSR 1 (sharp)", "Scanlines", "CRT" },
				{ ps5::UpscalingOff, ps5::UpscalingFsrSoft, ps5::UpscalingFsr, ps5::UpscalingFsrSharp, ps5::UpscalingScanlines,
						ps5::UpscalingCrt },
				ps5::Upscaling, G::Video),
		// Pictures made in between the game's own (ps5_framegen.cpp).
		onOff("Frame generation",
				"Experimental: pictures made in between the game's own. Smoother, a little later, and it can smear",
				"ps5.FrameGeneration", ps5::FrameGeneration, G::Video),
		// The software model of the Dreamcast's graphics chip (core/rend/soft):
		// it takes the place of the graphics processor from the next frame.
		onOff("Software renderer",
				"Experimental: the processor draws each pixel the way the Dreamcast's chip did. Slower, and at 480p",
				"ps5.SoftwareRenderer", ps5::SoftwareRenderer, G::Video),
		combined("Frame pacing",
				"Sync to display is the smoothest: the sound follows the TV. VSync: the sound keeps its exact rate",
				{ "Sync to display", "VSync", "Off" },
				{ part("config.rend.vsync", config::VSync), part("ps5.SyncToDisplay", ps5::SyncToDisplay) },
				{ { 1, 1 }, { 1, 0 }, { 0, G::Any } }, G::Video),

		range("Volume", "Master volume", "config.aica.Volume", 0, 100, 5, "%", G::Audio,
				[] { return (int)config::AudioVolume.get(); },
				[](int v) { config::AudioVolume.set(v); config::AudioVolume.calcDbPower(); }),
		onOff("DSP emulation", "The sound chip's effects (reverb, echo); needed by some games",
				"config.aica.DSPEnabled", config::DSPEnabled, G::Audio),
		onOff("VMU sounds", "The beeps of the VMU", "audio.VmuSound", config::VmuSound, G::Audio),

		range("Vibration", "How strongly the DualSense rumbles", nullptr, 0, 100, 10, "%", G::Controls,
				[] { auto p = pad1(); return p ? p->get_rumble_power() : 100; },
				[](int v) { if (auto p = pad1()) p->set_rumble_power(v); }),
		range("Stick dead zone", "Stick movement ignored around the centre", nullptr, 0, 40, 2, "%", G::Controls,
				[] { auto p = pad1(); return p ? (int)std::lround(p->get_dead_zone() * 100) : 10; },
				[](int v) { if (auto p = pad1()) p->set_dead_zone(v / 100.f); }),
		pick("Left stick as D-pad",
				"The left stick also moves a digital joystick. Automatic: in arcade games that have no analog stick",
				"ps5.StickAsDpad", { "Automatic", "On", "Off" },
				{ ps5::pad::StickDpadAuto, ps5::pad::StickDpadOn, ps5::pad::StickDpadOff }, ps5::pad::StickAsDpad,
				G::Controls),
		pick("Controller slot 2", "What is in the controller beside the memory card; rumble needs the rumble pack",
				"input.device1.2", { "Rumble pack", "Memory card" }, { (int)MDT_PurupuruPack, (int)MDT_SegaVMU },
				config::MapleExpansionDevices[0][1], G::Controls, true),
		pick("Port A", "A light gun in place of the controller, for the Dreamcast games that are played with one",
				"input.device1", { "Controller", "Light gun" }, { (int)MDT_SegaController, (int)MDT_LightGun },
				config::MapleMainDevices[0], G::Own, true),
		pick("Light gun aiming",
				"The left stick, a finger on the touch pad, or turning the controller (a touch on the pad centres it)",
				"ps5.LightGunAim", { "Left stick", "Touch pad", "Motion" },
				{ ps5::pad::AimStick, ps5::pad::AimTouch, ps5::pad::AimMotion }, ps5::pad::LightGunAim, G::Controls),
		pick("Motion aiming direction", "For a gun that goes the other way when the controller turns",
				"ps5.MotionAimDirection", { "As it is", "Left and right swapped", "Up and down swapped", "Both swapped" },
				{ 0, 1, 2, 3 }, ps5::pad::MotionAimDirection, G::Controls),
		combined("Light gun crosshair", "Shows where each gun points: white for player 1, then red, green and pink",
				{ "Off", "On" },
				{ part("config.rend.CrossHairColor1", config::CrosshairColor[0]),
						part("config.rend.CrossHairColor2", config::CrosshairColor[1]),
						part("config.rend.CrossHairColor3", config::CrosshairColor[2]),
						part("config.rend.CrossHairColor4", config::CrosshairColor[3]) },
				{ { 0, 0, 0, 0 }, { (int)0xC0FFFFFF, (int)0xC00000FF, (int)0xC000FF00, (int)0xC0FF00FF } }, G::Controls),

		pick("Region", "The console's region", "config.Dreamcast.Region",
				{ "Japan", "USA", "Europe", "Follow the game" }, { 0, 1, 2, 3 }, config::Region, G::System, true),
		pick("Language", "The Dreamcast's system language", "config.Dreamcast.Language",
				{ "Japanese", "English", "German", "French", "Spanish", "Italian", "Follow the game" },
				{ 0, 1, 2, 3, 4, 5, 6 }, config::Language, G::System, true),
		pick("Broadcast", "The TV standard the console reports", "config.Dreamcast.Broadcast",
				{ "NTSC", "PAL", "PAL-M", "PAL-N", "Follow the game" }, { 0, 1, 2, 3, 4 }, config::Broadcast, G::System,
				true),
		pick("Video cable", "VGA gives the sharpest picture; some games need TV", "config.Dreamcast.Cable",
				{ "VGA", "RGB", "TV (composite)" }, { 0, 2, 3 }, config::Cable, G::System, true),
		onOff("Built-in BIOS", "Boot without dc_boot.bin (a real BIOS is more compatible)", "config.UseReios",
				config::UseReios, G::System, true),
		onOff("Fast disc loading", "Shorter load times (a few games need it off)", "config.FastGDRomLoad",
				config::FastGDRomLoad, G::System),
		onOff("CPU recompiler", "The dynarec; turning it off is much slower", "config.Dynarec.Enabled",
				config::DynarecEnabled, G::System),
		[] {
			// The Dreamcast's processor, slower or faster than the real one's 200 MHz.
			GameOption o = pick("CPU clock",
					"Overclocking smooths a game that slows down or stutters; some games break or run too fast",
					"config.Sh4Clock",
					{ "100 MHz (half)", "150 MHz", "200 MHz (standard)", "250 MHz", "300 MHz", "350 MHz", "400 MHz (double)" },
					{ 100, 150, 200, 250, 300, 350, 400 }, config::Sh4Clock, GameOption::System);
			o.set = [](int v) {
				config::Sh4Clock.set(v);
				// The code already translated was timed for the old clock.
				if (game_started && emu.getSh4Executor() != nullptr)
					emu.getSh4Executor()->ResetCache();
			};
			return o;
		}(),
		onOff("Auto save state", "Saves the game's state when you quit it", "config.Dreamcast.AutoSaveState",
				config::AutoSaveState, G::System),
		onOff("Auto load state", "Resumes from that state when the game starts", "config.Dreamcast.AutoLoadState",
				config::AutoLoadState, G::System),
	};
	return rows;
}

// The options offered for a game file: without the patches it has none of,
// and for Details > Options (saved) without what is not kept per game.
std::vector<GameOption> optionsFor(const std::string& fileName, bool saved)
{
	std::vector<GameOption> rows;
	for (const GameOption& option : gameOptions())
	{
		if (saved && option.key == nullptr)
			continue;
		if (option.patch != nullptr && !ps5::patches::has(fileName, option.patch))
			continue;
		rows.push_back(option);
	}
	return rows;
}

// An option's section and name in emu.cfg, from its key.
std::pair<std::string, std::string> optionEntry(const std::string& key)
{
	const size_t dot = key.find('.');
	return { key.substr(0, dot), key.substr(dot + 1) };
}

// The value the Settings have for one of the options of a combined choice.
int partSettingsValue(const GameOption::Part& part)
{
	const int now = part.get();
	const auto [section, name] = optionEntry(part.key);
	return part.isBool ? (config::loadBool(section, name, now != 0) ? 1 : 0) : config::loadInt(section, name, now);
}

// The value the Settings have for an option (the one in effect may be the
// running game's own).
int settingsValue(const GameOption& option)
{
	if (option.category == GameOption::Patch)
		return option.values[0];	// the Settings have none of these: one that is on is the game's own
	if (!option.parts.empty())
	{
		std::vector<int> values;
		for (const GameOption::Part& part : option.parts)
			values.push_back(partSettingsValue(part));
		return comboOf(option, values);
	}
	const int now = option.get();
	if (option.key == nullptr)
		return now;
	const auto [section, name] = optionEntry(option.key);
	return option.isBool ? (config::loadBool(section, name, now != 0) ? 1 : 0) : config::loadInt(section, name, now);
}

int choiceOf(const GameOption& option, int value)
{
	for (size_t i = 0; i < option.values.size(); i++)
		if (option.values[i] == value)
			return (int)i;
	if (option.step > 0)
	{
		// A value between two steps of a range: the nearer one.
		int nearest = 0;
		for (size_t i = 1; i < option.values.size(); i++)
			if (std::abs(option.values[i] - value) < std::abs(option.values[nearest] - value))
				nearest = (int)i;
		return nearest;
	}
	return -1;
}

// What is shown for a value that is none of an option's choices: the number,
// and for the per-pixel layers whose number it is.
std::string oddValue(const GameOption& option, int value)
{
	if (option.key != nullptr && !strcmp(option.key, "config.rend.PerPixelLayers"))
		return std::to_string(value) + " (Flycast's, for this game)";
	return std::to_string(value);
}

// Which choice a game has for an option; -1 when it follows the Settings.
int gameOptionChoice(const std::string& id, const GameOption& option)
{
	const std::vector<std::string> entries = config::getEntries(id);
	if (!option.parts.empty())
	{
		// Its own where the game has any of the options; the Settings' for the rest.
		bool own = false;
		std::vector<int> values;
		for (const GameOption::Part& part : option.parts)
		{
			if (std::find(entries.begin(), entries.end(), part.key) == entries.end())
				values.push_back(partSettingsValue(part));
			else
			{
				own = true;
				values.push_back(part.isBool ? (config::loadBool(id, part.key, false) ? 1 : 0) : config::loadInt(id, part.key, 0));
			}
		}
		return own ? comboOf(option, values) : -1;
	}
	if (std::find(entries.begin(), entries.end(), option.key) == entries.end())
		return -1;
	return choiceOf(option, option.isBool ? (config::loadBool(id, option.key, false) ? 1 : 0) : config::loadInt(id, option.key, 0));
}

// A game's own value for an option, gone: it follows the Settings again.
void clearGameOption(const std::string& id, const GameOption& option)
{
	for (const GameOption::Part& part : option.parts)
		config::deleteEntry(id, part.key);
	if (option.parts.empty())
		config::deleteEntry(id, option.key);
}

void setGameOptionChoice(const std::string& id, const GameOption& option, int choice)
{
	if (!option.parts.empty())
	{
		clearGameOption(id, option);
		if (choice < 0 || choice >= (int)option.combos.size())
			return;
		for (size_t i = 0; i < option.parts.size(); i++)
		{
			const int value = option.combos[choice][i];
			if (value == GameOption::Any)
				continue;
			if (option.parts[i].isBool)
				config::saveBool(id, option.parts[i].key, value != 0);
			else
				config::saveInt(id, option.parts[i].key, value);
		}
		return;
	}
	if (choice < 0)
		config::deleteEntry(id, option.key);
	else if (option.isBool)
		config::saveBool(id, option.key, option.values[choice] != 0);
	else
		config::saveInt(id, option.key, option.values[choice]);
}

enum DetailsPage { PageActions, PageStates, PageOptions, PageCheats, PageManage };
// The rows of Manage.
enum { ManageFavourite, ManageHide, ManageCover, ManageTime, ManageLast, ManageRows };

struct DetailsState
{
	int page = PageActions;
	int action = 0;				// Play, Load state, Options, Cheats, Manage
	int row = 0;				// in a page's list
	float scroll = 0;
	// The game the panel is for and the shelf it was opened on: a favourite
	// made or a game hidden changes the lists under it.
	std::string path;
	std::string shelf;
	std::string gameId;
	// How long the game was played and when last (Unix time, 0 for never), a
	// set's discs together; whether it was started by a build that kept neither.
	uint64_t seconds = 0;
	int64_t lastPlayed = 0;
	bool startedBefore = false;
	// What its cover is (ps5::covers::current), asked again when a cover changed.
	int cover = ps5::covers::Automatic;
	bool coverSure = true;
	unsigned coverGeneration = 0;
	std::vector<std::pair<int, time_t>> states;		// slot, when saved
	// The cheats page.
	std::vector<std::string> cheatFiles;	// closest names first
	int cheatFile = -1;						// index in cheatFiles; -1 for none
	bool cheatFileKept = false;				// the choice is saved (not just the closest name)
	std::vector<std::string> cheatNames;
	std::vector<bool> cheatOn;
} det;

void loadCheatList()
{
	det.cheatNames.clear();
	det.cheatOn.clear();
	if (det.cheatFile < 0)
		return;
	det.cheatNames = ps5::cheats::descriptions(det.cheatFiles[det.cheatFile]);
	const std::vector<std::string> on = ps5::cheats::enabled(det.gameId);
	for (const std::string& name : det.cheatNames)
		det.cheatOn.push_back(std::find(on.begin(), on.end(), name) != on.end());
}

// "3 h 12 min", "12 min"; "less than a minute" for less.
std::string playTimeText(uint64_t seconds)
{
	const uint64_t minutes = seconds / 60;
	if (minutes == 0)
		return "less than a minute";
	if (minutes < 60)
		return std::to_string(minutes) + " min";
	return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

// "4 Oct 2026"
std::string dayText(int64_t when)
{
	const time_t t = (time_t)when;
	struct tm local;
	if (localtime_r(&t, &local) == nullptr)
		return "";
	static const char *const months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	return std::to_string(local.tm_mday) + " " + months[local.tm_mon] + " " + std::to_string(local.tm_year + 1900);
}

// The quiet line under the cover: "Played 3 h 12 min  ·  last on 4 Oct 2026".
std::string playedLine()
{
	if (det.lastPlayed == 0 && det.seconds == 0)
		return det.startedBefore ? "Played before play time was kept" : "Not played yet";
	std::string line = "Played " + playTimeText(det.seconds);
	if (det.lastPlayed != 0)
		line += "  \xc2\xb7  last on " + dayText(det.lastPlayed);
	return line;
}

const char *coverChoiceName(int choice)
{
	switch (choice)
	{
	case ps5::covers::Boxart: return "Box art";
	case ps5::covers::Title: return "Title screen";
	case ps5::covers::Snap: return "In game";
	case ps5::covers::Own: return "Yours";
	default: return "Automatic";
	}
}

void openDetails(Game& g)
{
	ensureArt(g);
	lib.details = true;
	det = DetailsState();
	det.path = g.media.path;
	if (view() == Shelves && !lib.shelves.empty())
		det.shelf = lib.shelves[std::clamp(lib.shelf, 0, (int)lib.shelves.size() - 1)].name;
	for (const std::string& path : pathsOf(g))
	{
		const ps5::library::Entry kept = ps5::library::entry(path);
		det.seconds += kept.seconds;
		det.lastPlayed = std::max(det.lastPlayed, kept.lastPlayed);
		det.startedBefore = det.startedBefore || std::find(recentPaths.begin(), recentPaths.end(), path) != recentPaths.end();
	}
	det.gameId = gameIdOf(g);
	// The states saved for this game, by Flycast's own file names.
	const std::string base = ps5::rootDir + "data/savestates/" + get_file_basename(g.media.fileName);
	for (int slot = 0; slot < 10; slot++)
	{
		struct stat st;
		if (stat((base + (slot > 0 ? "_" + std::to_string(slot) : "") + ".state").c_str(), &st) == 0)
			det.states.emplace_back(slot, st.st_mtime);
	}
	// Its cheat file: the one chosen, else the closest name, as the game's start would pick.
	det.cheatFiles = ps5::cheats::candidates(g.media.fileName);
	std::string file = ps5::cheats::chosen(det.gameId);
	det.cheatFileKept = !file.empty();
	if (file.empty())
		file = ps5::cheats::closest(g.media.fileName);
	for (size_t i = 0; i < det.cheatFiles.size(); i++)
		if (det.cheatFiles[i] == file)
			det.cheatFile = (int)i;
	loadCheatList();
}

void saveCheatChoice()
{
	// The file first (it forgets which cheats were on), then those.
	if (!det.cheatFileKept)
	{
		ps5::cheats::choose(det.gameId, det.cheatFile < 0 ? "" : det.cheatFiles[det.cheatFile]);
		det.cheatFileKept = true;
	}
	std::vector<std::string> on;
	for (size_t i = 0; i < det.cheatNames.size(); i++)
		if (det.cheatOn[i])
			on.push_back(det.cheatNames[i]);
	ps5::cheats::setEnabled(det.gameId, on);
}

void openCover(Game& g);

void detailsInput(Game& g, bool selectDisk)
{
	if (selectDisk)
	{
		if (in.back || in.triangle)
			lib.details = false;
		else if (in.accept)
		{
			lib.details = false;
			launch(g, true);
		}
		return;
	}
	const bool hasId = !det.gameId.empty();
	switch (det.page)
	{
	case PageActions:
		if (in.left && det.action > 0) det.action--;
		if (in.right && det.action < 4) det.action++;
		if (in.square && setsGrouped && g.setSize > 1)
		{
			// The set's next disc: it stands for the set from here, and the
			// panel is its own (its states; its options and cheats, where the
			// discs do not share an ID).
			const std::vector<size_t>& discs = discSets[g.set];
			size_t at = 0;
			for (size_t i = 0; i < discs.size(); i++)
				if (&games[discs[i]] == &g)
					at = i;
			Game& next = games[discs[(at + 1) % discs.size()]];
			frontChoice[next.set] = next.media.path;
			setsDirty = true;
			lib.flatPath = next.media.path;
			const int action = det.action;
			openDetails(next);
			det.action = action;
			return;
		}
		if (in.back || in.triangle)
			lib.details = false;
		else if (in.accept)
		{
			det.row = 0;
			det.scroll = 0;
			if (det.action == 0)
			{
				lib.details = false;
				launch(g, false);
			}
			else if (det.action == 1)
			{
				if (!det.states.empty())
					det.page = PageStates;
			}
			else
				det.page = det.action == 2 ? PageOptions : det.action == 3 ? PageCheats : PageManage;
		}
		break;
	case PageStates:
	{
		const int n = (int)det.states.size();
		if (in.up && det.row > 0) det.row--;
		if (in.down && det.row < n - 1) det.row++;
		if (in.back)
			det.page = PageActions;
		else if (in.accept && n > 0)
		{
			lib.details = false;
			launch(g, false, det.states[std::clamp(det.row, 0, n - 1)].first);
		}
		break;
	}
	case PageOptions:
	{
		const std::vector<GameOption> options = optionsFor(g.media.fileName, true);
		const int n = hasId ? (int)options.size() + 1 : 0;	// and "Same as the Settings for all"
		if (in.up && det.row > 0) det.row--;
		if (in.down && det.row < n - 1) det.row++;
		if (in.back)
			det.page = PageActions;
		else if (hasId && det.row < (int)options.size())
		{
			const GameOption& option = options[det.row];
			int choice = gameOptionChoice(det.gameId, option);
			const int last = (int)option.values.size() - 1;
			if (in.left && choice > -1)
				setGameOptionChoice(det.gameId, option, choice - 1);
			else if (in.right && choice < last)
				setGameOptionChoice(det.gameId, option, choice + 1);
			else if (in.accept)
				setGameOptionChoice(det.gameId, option, choice >= last ? -1 : choice + 1);
		}
		else if (hasId && in.accept)
		{
			for (const GameOption& option : options)
				clearGameOption(det.gameId, option);
		}
		break;
	}
	case PageCheats:
	{
		const int n = hasId ? 1 + (int)det.cheatNames.size() : 0;
		if (in.up && det.row > 0) det.row--;
		if (in.down && det.row < n - 1) det.row++;
		if (in.l2) det.row = std::max(0, det.row - 8);
		if (in.r2) det.row = std::min(std::max(0, n - 1), det.row + 8);
		if (in.back)
			det.page = PageActions;
		else if (hasId && det.row == 0)
		{
			int next = det.cheatFile;
			if (in.left) next--;
			if (in.right || in.accept) next++;
			next = std::clamp(next, -1, (int)det.cheatFiles.size() - 1);
			if (next != det.cheatFile)
			{
				det.cheatFile = next;
				ps5::cheats::choose(det.gameId, next < 0 ? "" : det.cheatFiles[next]);
				det.cheatFileKept = true;
				loadCheatList();
			}
		}
		else if (hasId && (in.accept || in.left || in.right) && det.row - 1 < (int)det.cheatNames.size())
		{
			det.cheatOn[det.row - 1] = !det.cheatOn[det.row - 1];
			saveCheatChoice();
		}
		break;
	}
	case PageManage:
		if (in.up && det.row > 0) det.row--;
		if (in.down && det.row < ManageRows - 1) det.row++;
		if (in.back)
			det.page = PageActions;
		else if (in.accept && det.row == ManageFavourite)
			ps5::library::setFavourite(pathsOf(g), !g.favourite);
		else if (in.accept && det.row == ManageHide)
			// The lists are made again after this: the library closes the
			// panel of a game that has left them.
			ps5::library::setHidden(pathsOf(g), !g.hidden);
		else if (in.accept && det.row == ManageCover)
			openCover(g);
		break;
	}
}

// A switch, as the settings draw it.
void drawSwitch(ImDrawList *dl, float right, float cy, bool on, bool focused)
{
	const float sw = 68, sh = 36;
	rect(dl, right - sw, cy - sh / 2, sw, sh, on ? (focused ? col::text : col::accent) : col::rgba(255, 255, 255, 40), sh / 2);
	dl->AddCircleFilled(V(right - sw + (on ? sw - sh / 2 : sh / 2), cy), (sh / 2 - 5) * S, on && focused ? col::accent : col::text);
}

void detailsPanel(ImDrawList *dl, Game& g, float t, bool selectDisk)
{
	dl->AddRectFilled(V(0, 96), V(W, H - 72), col::rgba(0, 0, 0, (int)(150 * t)));
	const float w = 1440, h = 740;
	const float x = (W - w) / 2, y = 96 + (H - 72 - 96 - h) / 2 + (1 - t) * 40;
	glow(dl, x, y, w, h, 24, col::rgba(0, 0, 0, (int)(255 * t)), 30);
	rect(dl, x, y, w, h, alpha(col::panel, t), 24);
	drawCover(dl, g, x + 48, y + 48, 420, 420, 16, 1);
	text(dl, regular(), 20, x + 48, y + 492, alpha(col::faint, t), fit(regular(), 20, g.media.fileName, 420).c_str());
	const std::string id = !det.gameId.empty() ? det.gameId : g.art.uniqueId;
	if (!id.empty())
		text(dl, regular(), 20, x + 48, y + 522, alpha(col::faint, t), ("ID  " + id).c_str());
	if (!selectDisk)
		text(dl, regular(), 20, x + 48, y + (id.empty() ? 522 : 552), alpha(col::faint, t),
				fit(regular(), 20, playedLine(), 420).c_str());

	const float tx = x + 520, tw = w - 520 - 56;
	text(dl, bold(), 48, tx, y + 48, alpha(col::text, t), fit(bold(), 48, g.title, tw).c_str());
	std::string meta = g.media.arcade ? "ARCADE" : "DREAMCAST";
	if (*regionText(g.art.region))
		meta += std::string("   ·   ") + regionText(g.art.region);
	if (!g.art.releaseDate.empty())
		meta += "   ·   " + g.art.releaseDate;
	if (setsGrouped && g.setSize > 1)
		meta += "   ·   Disc " + std::to_string(g.discOrder) + " of " + std::to_string(g.setSize);
	text(dl, bold(), 24, tx, y + 118, alpha(col::accent, t), meta.c_str());

	const float top = y + 170, bottom = y + h - 48;
	if (selectDisk || det.page == PageActions)
	{
		std::string overview = !g.art.overview.empty() ? g.art.overview
				: g.art.busy ? "Looking for a description..."
				: !config::FetchBoxart ? "No description: downloads are off in Settings > Library."
				: "No description available.";
		if (overview.size() > 1100)
			overview = overview.substr(0, 1100) + "...";
		dl->PushClipRect(V(tx, top), V(tx + tw, bottom - 110), true);
		text(dl, regular(), 26, tx, top + 2, alpha(col::dim, t), overview.c_str(), tw);
		dl->PopClipRect();
		// The actions, side by side.
		struct Action { const char *label; bool enabled; };
		std::vector<Action> actions;
		if (selectDisk)
			actions = { { ICON_FA_COMPACT_DISC "   Insert disc", true } };
		else
			actions = { { ICON_FA_PLAY "   Play", true },
					{ ICON_FA_CLOCK_ROTATE_LEFT "   Load state", !det.states.empty() },
					{ ICON_FA_SLIDERS "   Options", true }, { ICON_FA_BOLT "   Cheats", true },
					{ ICON_FA_GEAR "   Manage", true } };
		// Five of them share the row: each is a little narrower than one alone.
		const float inside = actions.size() > 1 ? 22.f : 32.f, between = actions.size() > 1 ? 14.f : 20.f;
		float bx = tx;
		const float by = bottom - 68;
		if (!selectDisk && setsGrouped && g.setSize > 1)
		{
			// The set's discs: the one this panel is for, and Square for the next.
			float px = tx;
			const float py = by - 64;
			for (size_t disc : discSets[g.set])
			{
				const bool current = &games[disc] == &g;
				const std::string label = "Disc " + std::to_string(games[disc].discOrder);
				const ImVec2 ls = textSize(bold(), 20, label.c_str());
				const float pw = ls.x + 36;
				rect(dl, px, py, pw, 40, alpha(current ? col::text : col::panelHi, t), 20);
				text(dl, bold(), 20, px + 18, py + 20 - ls.y / 2, alpha(current ? col::panel : col::dim, t), label.c_str());
				px += pw + 10;
			}
			px += 14;
			px += glyph(dl, Glyph::Square, px, py + 20, 28) + 10;
			text(dl, regular(), 20, px, py + 8, alpha(col::faint, t), "Next disc");
		}
		for (int i = 0; i < (int)actions.size(); i++)
		{
			const bool on = selectDisk || i == det.action;
			const ImVec2 ls = textSize(bold(), 26, actions[i].label);
			const float bw = ls.x + 2 * inside;
			if (on)
				glow(dl, bx, by, bw, 64, 32, alpha(col::accent, 0.7f * t), 12);
			rect(dl, bx, by, bw, 64, alpha(on ? col::accent : col::panelHi, t), 32);
			text(dl, bold(), 26, bx + inside, by + 32 - ls.y / 2,
					alpha(!actions[i].enabled ? col::faint : on ? col::text : col::dim, t), actions[i].label);
			bx += bw + between;
		}
		if (!selectDisk && det.action == 1 && det.states.empty() && !(setsGrouped && g.setSize > 1))
			text(dl, regular(), 20, tx, by - 34, alpha(col::faint, t),
					"No saved state for this game yet: save one from the quick menu while playing.");
		return;
	}

	// A page: its name, then its rows.
	const char *names[] = { "", ICON_FA_CLOCK_ROTATE_LEFT "   Start from a saved state", ICON_FA_SLIDERS "   Options for this game",
			ICON_FA_BOLT "   Cheats", ICON_FA_GEAR "   Manage" };
	text(dl, bold(), 30, tx, top, alpha(col::text, t), names[det.page]);
	const float listTop = top + 58, rowH = 58;
	const float viewH = bottom - listTop;
	if ((det.page == PageOptions || det.page == PageCheats) && det.gameId.empty())
	{
		text(dl, regular(), 26, tx, listTop + 8, alpha(col::dim, t),
				"Start this game once first.\n\nPSFlyCast keeps a game's own options and cheats under the ID it reads from the "
				"game when it runs, and this one has not run yet.", tw);
		return;
	}
	const std::vector<GameOption> options = optionsFor(g.media.fileName, true);
	const int rows = det.page == PageStates ? (int)det.states.size() : det.page == PageManage ? (int)ManageRows
			: det.page == PageOptions ? (int)options.size() + 1 : 1 + (int)det.cheatNames.size();
	if (det.page == PageManage && det.coverGeneration != ps5::covers::generation())
	{
		// Asked of the files once, and again when a cover changed: not every frame.
		det.coverGeneration = ps5::covers::generation();
		const std::string base = get_file_basename(g.media.fileName);
		det.cover = ps5::covers::current(base);
		// A .png in the covers folder that nothing is noted about is the
		// user's, or one an earlier build downloaded (which kept no notes):
		// the row does not say whose. "Change cover" finds out.
		det.coverSure = det.cover != ps5::covers::Own || ps5::covers::ownFile(base) != ps5::rootDir + "covers/" + base + ".png";
	}
	det.row = std::clamp(det.row, 0, std::max(0, rows - 1));
	const float visible = viewH / rowH;
	const float target = std::clamp((float)det.row - (visible - 1) / 2, 0.f, std::max(0.f, (float)rows - visible));
	det.scroll = approach(det.scroll, target, 18);
	dl->PushClipRect(V(tx - 16, listTop), V(tx + tw + 16, bottom), true);
	for (int i = 0; i < rows; i++)
	{
		const float ry = listTop + (i - det.scroll) * rowH;
		if (ry > bottom || ry + rowH < listTop - rowH)
			continue;
		const bool on = i == det.row;
		if (on)
			rect(dl, tx - 12, ry + 2, tw + 24, rowH - 6, col::accent, 12);
		const float right = tx + tw - 8, cy = ry + rowH / 2 - 1;
		const ImU32 ink = on ? col::text : col::dim;
		auto value = [&](const std::string& v, bool arrows) {
			const std::string shown = fit(regular(), 24, v, tw * 0.5f);
			const ImVec2 vs = textSize(regular(), 24, shown.c_str());
			text(dl, regular(), 24, right - 22 - vs.x, cy - vs.y / 2, on ? col::text : col::faint, shown.c_str());
			if (on && arrows)
			{
				dl->AddTriangleFilled(V(right - 42 - vs.x, cy), V(right - 30 - vs.x, cy - 10), V(right - 30 - vs.x, cy + 10), col::text);
				dl->AddTriangleFilled(V(right, cy), V(right - 12, cy - 10), V(right - 12, cy + 10), col::text);
			}
		};
		if (det.page == PageStates)
		{
			const auto& [slot, when] = det.states[i];
			text(dl, bold(), 26, tx + 8, ry + 14, ink, ("Slot " + std::to_string(slot + 1)).c_str());
			value(timeToShortDateTimeString(when), false);
		}
		else if (det.page == PageOptions)
		{
			if (i < (int)options.size())
			{
				const int choice = gameOptionChoice(det.gameId, options[i]);
				text(dl, bold(), 26, tx + 8, ry + 14, choice >= 0 || on ? col::text : col::dim, options[i].label);
				value(choice < 0 ? "As in Settings" : options[i].names[choice], true);
			}
			else
				text(dl, bold(), 26, tx + 8, ry + 14, ink, ICON_FA_ROTATE_LEFT "   Back to the Settings for all of these");
		}
		else if (det.page == PageManage)
		{
			// A row that opens something: what it is now, before an arrow.
			auto opens = [&](const std::string& now) {
				const char *arrow = ICON_FA_CHEVRON_RIGHT;
				const ImVec2 as = textSize(regular(), 24, arrow);
				text(dl, regular(), 24, right - as.x, cy - as.y / 2, on ? col::text : col::faint, arrow);
				const ImVec2 ns = textSize(regular(), 24, now.c_str());
				text(dl, regular(), 24, right - as.x - 22 - ns.x, cy - ns.y / 2, on ? col::text : col::faint, now.c_str());
			};
			// A fact: nothing to press.
			auto fact = [&](const std::string& what) {
				const ImVec2 ws = textSize(regular(), 24, what.c_str());
				text(dl, regular(), 24, right - ws.x, cy - ws.y / 2, on ? col::text : col::faint, what.c_str());
			};
			// Its icon in a column of its own, so that the words line up.
			auto label = [&](const char *icon, const char *words) {
				const ImVec2 is = textSize(regular(), 26, icon);
				text(dl, regular(), 26, tx + 26 - is.x / 2, ry + 14, ink, icon);
				text(dl, bold(), 26, tx + 60, ry + 14, ink, words);
			};
			std::string time = det.seconds == 0 && det.lastPlayed == 0 ? "None yet" : playTimeText(det.seconds);
			time[0] = (char)toupper((unsigned char)time[0]);
			switch (i)
			{
			case ManageFavourite:
				label(ICON_FA_STAR, "Favourite");
				drawSwitch(dl, right, cy, g.favourite, on);
				break;
			case ManageHide:
				label(g.hidden ? ICON_FA_EYE : ICON_FA_EYE_SLASH, g.hidden ? "Show in the library again" : "Hide from the library");
				opens("");
				break;
			case ManageCover:
				label(ICON_FA_IMAGE, "Change cover");
				opens(det.coverSure ? coverChoiceName(det.cover) : "");
				break;
			case ManageTime:
				label(ICON_FA_CLOCK, "Time played");
				fact(time);
				break;
			default:
				label(ICON_FA_CLOCK_ROTATE_LEFT, "Last played");
				fact(det.lastPlayed != 0 ? dayText(det.lastPlayed) : det.startedBefore ? "Before play time was kept" : "Never");
				break;
			}
		}
		else if (i == 0)
		{
			text(dl, bold(), 26, tx + 8, ry + 14, ink, "Cheat file");
			const std::string file = det.cheatFile < 0 ? "None" : det.cheatFiles[det.cheatFile].substr(0, det.cheatFiles[det.cheatFile].size() - 4);
			value(file, true);
		}
		else
		{
			const bool enabled = det.cheatOn[i - 1];
			text(dl, regular(), 24, tx + 8, ry + 15, on || enabled ? col::text : col::dim,
					fit(regular(), 24, det.cheatNames[i - 1], tw - 130).c_str());
			drawSwitch(dl, right, cy, enabled, on);
		}
	}
	dl->PopClipRect();
	if (det.page == PageCheats && det.cheatNames.empty())
		text(dl, regular(), 22, tx + 8, listTop + rowH + 20, alpha(col::faint, t),
				det.cheatFiles.empty() ? "No cheat files in the cheats folder."
				: "No file chosen. Left and right go through the files: the closest names come first.", tw - 16);
	if (det.page == PageManage)
	{
		// What the focused row is about.
		const bool set = setsGrouped && g.setSize > 1;
		const char *note = "";
		switch (det.row)
		{
		case ManageFavourite:
			note = "A favourite is on the Favourites shelf, and has a star in the grid and in the list.";
			break;
		case ManageHide:
			note = g.hidden ? "Puts the game back in the library's lists, as it was."
					: "The game leaves every list. Settings > Library > Show hidden games lists it again, dimmed, "
						"and this row then brings it back.";
			break;
		case ManageCover:
			note = set ? "Another picture from the covers' collection, or the one PSFlyCast finds by itself. "
						"Every disc of the game takes it."
					: "Another picture from the covers' collection, or the one PSFlyCast finds by itself.";
			break;
		case ManageTime:
			note = set ? "The time the game itself ran, all its discs together: not the time in the quick menu or the Settings."
					: "The time the game itself ran: not the time in the quick menu or the Settings.";
			break;
		default:
			break;
		}
		text(dl, regular(), 22, tx + 8, listTop + ManageRows * rowH + 22, alpha(col::faint, t), note, tw - 16);
	}
}

} // namespace

void gameStarted(const std::string& path)
{
	// Its time played counts from when it runs.
	ps5::library::playing(path);
	loadRecents();
	recentPaths.erase(std::remove(recentPaths.begin(), recentPaths.end(), path), recentPaths.end());
	recentPaths.insert(recentPaths.begin(), path);
	if (recentPaths.size() > 15)
		recentPaths.resize(15);
	saveRecents();
	// The disc started stands for its set from now on.
	setsDirty = true;
	frontChoice.clear();
	lib.shelf = 0;
	if (!lib.shelves.empty())
		lib.shelves[0].focus = 0;
}

namespace
{

// ---- the shelves view: the focused game's art behind its title, shelves below

void drawShelves(ImDrawList *dl, Game *focused, bool selectDisk)
{
	const float heroTop = 96, heroH = 470;
	if (focused != nullptr)
	{
		if (focused->media.path != lib.heroPath)
		{
			lib.heroPath = focused->media.path;
			lib.heroFade = 0;
		}
		lib.heroFade = approach(lib.heroFade, 1, 8);
		const ImTextureID id = coverTexture(*focused);
		if (id != ImTextureID())
		{
			// The art fills the right of the hero, fading into the backdrop
			// from its left edge and toward the shelves.
			// It breathes: a slow push in and out, when everything moves.
			const float push = motion() == MotionFull ? 1.035f + 0.03f * std::sin((float)timeNow * 0.21f) : 1.f;
			const float aw = W * 0.66f * push, ah = (heroH + 150) * push;
			imageFade(dl, id, W - W * 0.66f - (aw - W * 0.66f) * 0.5f, heroTop - (ah - heroH - 150) * 0.3f, aw, ah, 0.6f * lib.heroFade,
					{ 0.f, 0.5f, 1.f }, { 0.f, 1.f, 1.f }, { 0.f, 0.55f, 1.f }, { 1.f, 0.8f, 0.f });
		}
		const float tx = 96;
		const float a = lib.heroFade;
		const std::string badge = platformText(*focused);
		const ImVec2 bs = textSize(bold(), 20, badge.c_str());
		rect(dl, tx, heroTop + 96, bs.x + 28, 36, alpha(focused->media.arcade ? col::warm : col::accent, 0.25f * a), 8);
		text(dl, bold(), 20, tx + 14, heroTop + 114 - bs.y / 2, alpha(focused->media.arcade ? col::warm : col::accent, a), badge.c_str());
		text(dl, bold(), 72, tx, heroTop + 150 + (1 - a) * 16, alpha(col::text, a), fit(bold(), 72, focused->title, W * 0.55f).c_str());
		const std::string meta = metaText(*focused);
		text(dl, regular(), 26, tx, heroTop + 248, alpha(col::dim, a), meta.c_str());

		// The Play button.
		const char *play = selectDisk ? ICON_FA_COMPACT_DISC "   Insert disc" : ICON_FA_PLAY "   Play";
		const ImVec2 ps = textSize(bold(), 30, play);
		const float bw = ps.x + 96, by = heroTop + 304;
		const float pulse = 0.5f + 0.5f * std::sin((float)timeNow * 2.4f);
		glow(dl, tx, by, bw, 68, 34, alpha(col::accent, 0.6f + 0.4f * pulse), 16);
		rect(dl, tx, by, bw, 68, col::accent, 34);
		glyph(dl, Glyph::Cross, tx + 18, by + 34, 34);
		text(dl, bold(), 30, tx + 66, by + 34 - ps.y / 2, col::text, play);
	}

	const float cardW = 228, cardH = 228, gap = 28;
	const float shelfH = cardH + 92;
	const float shelvesTop = heroTop + heroH + 10;
	lib.shelfAnim = approach(lib.shelfAnim, (float)lib.shelf, 12);
	dl->PushClipRect(V(0, shelvesTop - 30), V(W, H - 72), true);
	for (int si = 0; si < (int)lib.shelves.size(); si++)
	{
		Shelf& sh = lib.shelves[si];
		const float y = shelvesTop + (si - lib.shelfAnim) * shelfH;
		if (y > H || y + shelfH < shelvesTop - shelfH)
			continue;
		const bool active = si == lib.shelf;
		const std::string header = sh.name + "   " + std::to_string(sh.items.size());
		text(dl, bold(), 28, 96, y, active ? col::text : col::faint, header.c_str());

		// Keep the focused card near the left third, as Steam does.
		const int visible = std::max(1, (int)((W - 96) / (cardW + gap)));
		float target = std::clamp((float)sh.focus - 1.f, 0.f, std::max(0.f, (float)sh.items.size() - visible + 0.6f));
		sh.scroll = approach(sh.scroll, target, 12);
		// The focus travels from card to card: each card is lifted by how
		// near the focus is to it.
		sh.focusAnim = sh.focusAnim < 0 || motion() == MotionOff ? (float)sh.focus : approach(sh.focusAnim, (float)sh.focus, 18);
		const float sinceEntrance = (float)(timeNow - entrance.at);
		for (int i = 0; i < (int)sh.items.size(); i++)
		{
			const float x = 96 + (i - sh.scroll) * (cardW + gap);
			if (x > W || x + cardW < -cardW)
				continue;
			Game& g = games[sh.items[i]];
			const float near = active && !lib.details ? std::clamp(1.f - std::fabs((float)i - sh.focusAnim), 0.f, 1.f) : 0.f;
			const bool on = active && i == sh.focus && !lib.details;
			const float grow = 18.f * near;
			const float cx = x - grow / 2, cy = y + 48 - grow / 2;
			// The cards of a screen that has just come in rise one after another.
			const bool rising = motion() == MotionFull && sinceEntrance < 1.2f;
			const Layer card = beginLayer(dl);
			if (near > 0.02f)
			{
				const float breath = motion() == MotionFull ? 0.78f + 0.22f * std::sin((float)timeNow * 2.4f) : 1.f;
				glow(dl, cx, cy, cardW + grow, cardH + grow, 14, alpha(col::accent, near * breath), 22);
			}
			drawCover(dl, g, cx, cy, cardW + grow, cardH + grow, 12, near);
			coverMarks(dl, g, cx, cy, cardW + grow, cardH + grow, 12, false);
			if (on && motion() == MotionFull)
			{
				// A band of light crosses the focused cover now and then.
				const float sweep = std::fmod((float)timeNow, 5.f) / 0.9f;
				if (sweep < 1.f)
				{
					const float w = cardW + grow, bx = cx - w * 0.6f + sweep * w * 1.8f;
					dl->PushClipRect(V(cx + 4, cy + 4), V(cx + w - 4, cy + cardH + grow - 4), true);
					const ImU32 lit = col::rgba(255, 255, 255, 34), none = col::rgba(255, 255, 255, 0);
					quad(dl, V(bx, cy), V(bx + w * 0.22f, cy), V(bx + w * 0.22f - 70, cy + cardH + grow), V(bx - 70, cy + cardH + grow),
							none, lit, lit, none);
					quad(dl, V(bx + w * 0.22f, cy), V(bx + w * 0.44f, cy), V(bx + w * 0.44f - 70, cy + cardH + grow),
							V(bx + w * 0.22f - 70, cy + cardH + grow), lit, none, none, lit);
					dl->PopClipRect();
				}
			}
			if (near > 0.02f)
				outline(dl, cx - 4, cy - 4, cardW + grow + 8, cardH + grow + 8, alpha(col::text, near), 15, 4);
			else if (!active)
				rect(dl, cx, cy, cardW, cardH, col::rgba(0, 0, 0, 90), 12);
			if (rising)
			{
				const float e = easeOut((sinceEntrance - 0.05f * std::min(i - (int)sh.scroll, 8) - 0.08f * si) / 0.4f);
				endLayer(card, e, 0, (1 - e) * 34);
			}
		}
	}
	dl->PopClipRect();
}

// ---- the grid view: every game's cover, eight to a row

constexpr int GridColumns = 8;

void drawGrid(ImDrawList *dl, Game *focused)
{
	// The focused game, named above the grid.
	const float top = 96;
	if (focused != nullptr)
	{
		text(dl, bold(), 44, 96, top + 34, col::text, fit(bold(), 44, focused->title, W - 96 - 520).c_str());
		const std::string badge = platformText(*focused);
		std::string meta = metaText(*focused);
		const ImU32 tint = focused->media.arcade ? col::warm : col::accent;
		const ImVec2 bs = textSize(bold(), 20, badge.c_str());
		rect(dl, 96, top + 96, bs.x + 24, 32, alpha(tint, 0.25f), 8);
		text(dl, bold(), 20, 108, top + 112 - bs.y / 2, tint, badge.c_str());
		text(dl, regular(), 24, 96 + bs.x + 44, top + 98, col::dim, meta.c_str());
	}
	const std::string count = std::to_string(lib.flat.size()) + (lib.flat.size() == 1 ? " game" : " games");
	const ImVec2 cs = textSize(regular(), 24, count.c_str());
	text(dl, regular(), 24, W - 96 - cs.x, top + 98, col::faint, count.c_str());

	const float gap = 24;
	const float cardW = (W - 192 - gap * (GridColumns - 1)) / GridColumns;
	const float cardH = cardW;
	const float gridTop = top + 160;
	const float viewH = H - 72 - gridTop;
	const int rows = ((int)lib.flat.size() + GridColumns - 1) / GridColumns;
	const int focusRow = lib.flatFocus / GridColumns;
	const float rowH = cardH + gap;
	const float visibleRows = viewH / rowH;
	const float target = std::clamp((float)focusRow - (visibleRows - 1) / 2,
			0.f, std::max(0.f, (float)rows - visibleRows + 0.25f));
	lib.flatScroll = approach(lib.flatScroll, target, 14);
	{
		const float column = (float)(lib.flatFocus % GridColumns), row = (float)(lib.flatFocus / GridColumns);
		const bool jump = lib.ringColumn < 0 || motion() == MotionOff;
		lib.ringColumn = jump ? column : approach(lib.ringColumn, column, 20);
		lib.ringRow = jump ? row : approach(lib.ringRow, row, 20);
	}
	dl->PushClipRect(V(0, gridTop - 20), V(W, H - 72), true);
	for (int i = 0; i < (int)lib.flat.size(); i++)
	{
		const int row = i / GridColumns, column = i % GridColumns;
		const float y = gridTop + (row - lib.flatScroll) * rowH;
		if (y > H || y + cardH < gridTop - cardH)
			continue;
		const float x = 96 + column * (cardW + gap);
		Game& g = games[lib.flat[i]];
		// Lifted by how near the travelling focus is.
		const float near = lib.details ? 0.f : std::clamp(1.f - std::fabs(column - lib.ringColumn), 0.f, 1.f)
				* std::clamp(1.f - std::fabs(row - lib.ringRow), 0.f, 1.f);
		const float grow = 14.f * near;
		if (near > 0.02f)
			glow(dl, x - grow / 2, y - grow / 2, cardW + grow, cardH + grow, 12, alpha(col::accent, near), 20);
		drawCover(dl, g, x - grow / 2, y - grow / 2, cardW + grow, cardH + grow, 10, near);
		coverMarks(dl, g, x - grow / 2, y - grow / 2, cardW + grow, cardH + grow, 10, true);
		if (near > 0.02f)
			outline(dl, x - grow / 2 - 4, y - grow / 2 - 4, cardW + grow + 8, cardH + grow + 8, alpha(col::text, near), 13, 4);
	}
	dl->PopClipRect();
}

// ---- the list view: titles on the left, the focused game on the right

void drawList(ImDrawList *dl, Game *focused, bool selectDisk)
{
	const float top = 96 + 40;
	const float listX = 96, listW = W * 0.56f - 96;
	const float rowH = 76;
	const float viewH = H - 72 - top - 24;
	const float visibleRows = viewH / rowH;
	const float target = std::clamp((float)lib.flatFocus - (visibleRows - 1) / 2,
			0.f, std::max(0.f, (float)lib.flat.size() - visibleRows));
	lib.flatScroll = approach(lib.flatScroll, target, 16);
	rect(dl, listX - 16, top - 12, listW + 32, viewH + 24, col::panel, 18);
	dl->PushClipRect(V(listX - 16, top), V(listX + listW + 16, top + viewH), true);
	// The highlight slides from row to row.
	lib.listRow = lib.listRow < 0 || motion() == MotionOff ? (float)lib.flatFocus : approach(lib.listRow, (float)lib.flatFocus, 22);
	if (!lib.details)
		rect(dl, listX, top + (lib.listRow - lib.flatScroll) * rowH + 2, listW, rowH - 4, col::accent, 12);
	for (int i = 0; i < (int)lib.flat.size(); i++)
	{
		const float y = top + (i - lib.flatScroll) * rowH;
		if (y > H || y + rowH < top - rowH)
			continue;
		Game& g = games[lib.flat[i]];
		const bool on = i == lib.flatFocus && !lib.details;
		const ImTextureID id = coverTexture(g);
		if (id != ImTextureID())
			imageFill(dl, id, listX + 12, y + 8, rowH - 16, rowH - 16, 8);
		else
			rect(dl, listX + 12, y + 8, rowH - 16, rowH - 16, col::card, 8);
		if (g.hidden)
			rect(dl, listX + 12, y + 8, rowH - 16, rowH - 16, alpha(on ? col::accent : col::panel, 0.64f), 8);
		const std::string badge = platformText(g);
		const ImVec2 bs = textSize(regular(), 20, badge.c_str());
		// A favourite's star and a hidden game's struck-through eye, before the badge.
		float titleEnd = listX + listW - 48 - bs.x;
		auto mark = [&](const char *icon, ImU32 colour) {
			const ImVec2 is = textSize(regular(), 20, icon);
			text(dl, regular(), 20, titleEnd - is.x, y + 28, on ? col::text : colour, icon);
			titleEnd -= is.x + 18;
		};
		if (g.hidden)
			mark(ICON_FA_EYE_SLASH, col::faint);
		if (g.favourite)
			mark(ICON_FA_STAR, col::warm);
		text(dl, bold(), 28, listX + rowH + 12, y + 22, on ? col::text : g.hidden ? col::faint : col::dim,
				fit(bold(), 28, g.title, titleEnd - (listX + rowH + 12)).c_str());
		text(dl, regular(), 20, listX + listW - 24 - bs.x, y + 28,
				on ? col::text : (g.media.arcade ? alpha(col::warm, 0.8f) : col::faint), badge.c_str());
	}
	dl->PopClipRect();

	if (focused == nullptr)
		return;
	const float px = listX + listW + 64, pw = W - px - 96;
	const float cover = std::min(pw, 440.f);
	drawCover(dl, *focused, px, top, cover, cover, 16, 1);
	coverMarks(dl, *focused, px, top, cover, cover, 16, true);
	float y = top + cover + 36;
	y += text(dl, bold(), 40, px, y, col::text, focused->title.c_str(), pw).y + 14;
	const ImU32 tint = focused->media.arcade ? col::warm : col::accent;
	std::string meta = platformText(*focused);
	const std::string more = metaText(*focused);
	if (!more.empty())
		meta += "   ·   " + more;
	y += text(dl, bold(), 22, px, y, tint, meta.c_str(), pw).y + 18;
	if (!focused->art.overview.empty())
	{
		std::string overview = focused->art.overview;
		if (overview.size() > 420)
			overview = overview.substr(0, 420) + "...";
		dl->PushClipRect(V(px, y), V(px + pw, H - 72 - 150), true);
		text(dl, regular(), 22, px, y, col::dim, overview.c_str(), pw);
		dl->PopClipRect();
	}
	// The Play button.
	const char *play = selectDisk ? ICON_FA_COMPACT_DISC "   Insert disc" : ICON_FA_PLAY "   Play";
	const ImVec2 ps = textSize(bold(), 28, play);
	const float bw = ps.x + 90, by = H - 72 - 110;
	rect(dl, px, by, bw, 62, col::accent, 31);
	glyph(dl, Glyph::Cross, px + 16, by + 31, 32);
	text(dl, bold(), 28, px + 62, by + 31 - ps.y / 2, col::text, play);
	text(dl, regular(), 20, px + bw + 24, by + 20, col::faint, fit(regular(), 20, focused->media.fileName, pw - bw - 24).c_str());
}

} // namespace

bool loading(const char *label, float progress, bool cancelling);

// The screen while a network disc is inserted; false when there is none.
bool discSwapScreen()
{
	if (!discSwap.active)
		return false;
	if (discSwap.task.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		discSwap.active = false;
		const bool cancelled = discSwap.cancelling;
		const std::string share = ps5::smb::lastError();
		ps5::smb::endLoad();
		try {
			discSwap.task.get();
			if (!cancelled)
			{
				gui_setState(GuiState::Closed);
				return true;	// nothing more to draw: the game goes on
			}
		} catch (const std::exception& e) {
			if (!cancelled)
				gui_error(std::string(e.what()) + (share.empty() ? "" : "\n\nThe network share: " + share));
		}
		return false;
	}
	if (loading("Inserting the disc...", 0.f, discSwap.cancelling))
	{
		discSwap.cancelling = true;
		ps5::smb::cancelLoad();
	}
	return true;
}

// ---------------------------------------------------------------- dialogs
//
// A panel over a screen, which is dimmed and hears nothing meanwhile: the
// screen hands the dialog the pad's input and keeps none of it. They are
// drawn on the foreground list, over everything the screen draws.

// The panel; returns its top left.
ImVec2 dialogPanel(ImDrawList *dl, float w, float h)
{
	rect(dl, 0, 0, W, H, col::rgba(4, 6, 10, 176));
	const float x = (W - w) / 2, y = (H - h) / 2 - 16;
	glow(dl, x, y, w, h, 28, col::rgba(0, 0, 0, 150), 36);
	rect(dl, x, y, w, h, col::panel, 28);
	outline(dl, x, y, w, h, col::line, 28, 1.5f);
	return ImVec2(x, y);
}

// A dialog's button: the pad's button and what it does. Returns its width.
float dialogButton(ImDrawList *dl, float x, float y, Glyph g, const char *label, bool primary)
{
	const float h = 60;
	const float gw = glyph(dl, g, 0, -1000);	// measure
	const float w = 20 + gw + 14 + textSize(bold(), 24, label).x + 30;
	if (primary)
		glow(dl, x, y, w, h, h / 2, alpha(col::accent, 0.5f), 12);
	rect(dl, x, y, w, h, primary ? col::accent : col::panelHi, h / 2);
	glyph(dl, g, x + 20, y + h / 2);
	const ImVec2 ls = textSize(bold(), 24, label);
	text(dl, bold(), 24, x + 20 + gw + 14, y + h / 2 - ls.y / 2, col::text, label);
	return w;
}

// ---- the console's own keyboard (ps5_ime.cpp)

bool openKeyboard(int purpose, const char *title, const char *placeholder, const std::string& value, size_t maxLength,
		ps5::ime::Kind kind = ps5::ime::Kind::Text)
{
	if (!ps5::ime::open(title, placeholder, value, maxLength, kind))
	{
		ps5::sound::cue(ps5::sound::Cue::Refuse);
		os_notify("The keyboard did not open", 4000, "flycast-boot.log in PSFlyCast's folder says why");
		return false;
	}
	keyboardFor = purpose;
	return true;
}

// Asked each frame by the screen that can open it: -1 while the keyboard is
// up (the pad is the keyboard's), what the text is for, once, when it was
// accepted (typed is the text), and 0 otherwise.
int keyboardState(std::string& typed)
{
	if (keyboardFor == KeyboardNone)
		return 0;
	switch (ps5::ime::poll())
	{
	case ps5::ime::State::Open:
		return -1;
	case ps5::ime::State::Accepted:
	{
		const int purpose = keyboardFor;
		keyboardFor = KeyboardNone;
		typed = ps5::ime::text();
		return purpose;
	}
	default:
		keyboardFor = KeyboardNone;
		return 0;
	}
}

// ---- leaving PSFlyCast, from the library

struct QuitUi
{
	bool open = false;
} quitUi;

void quitDialog(ImDrawList *dl, const Input& given)
{
	if (given.accept)
	{
		quitUi.open = false;
		dc_exit();
	}
	else if (given.back)
		quitUi.open = false;
	const float w = 700, h = 286, pad = 52;
	const ImVec2 at = dialogPanel(dl, w, h);
	text(dl, bold(), 38, at.x + pad, at.y + pad - 6, col::text, "Quit PSFlyCast?");
	text(dl, regular(), 23, at.x + pad, at.y + pad + 50, col::dim, "Back to the console's home screen");
	float x = at.x + pad;
	const float y = at.y + h - pad - 60 + 8;
	x += dialogButton(dl, x, y, Glyph::Cross, "Quit", true) + 20;
	dialogButton(dl, x, y, Glyph::Circle, "Stay", false);
}

// ---- another cover for a game (ps5_covers.cpp)
//
// The collection's three pictures of the game side by side, as the library
// would crop them, with the cover PSFlyCast finds by itself and, when there is
// one, the user's own picture. The one chosen is the game's cover from then on.

namespace
{

struct CoverUi
{
	bool open = false;
	std::string title;
	std::string base;					// the game's file name without its extension
	std::vector<std::string> bases;		// its own, and those of its set's other discs: they take the same cover
	std::string ownArt;					// the picture the game brings itself (its disc's, the scraper's)
	std::string cannot;					// why nothing is downloaded, when nothing is
	int focus = -1;						// -1: the cover the game has now, until another is moved to
	// What the files say, asked again when a download ends or a cover changes:
	// which the cover is, the user's own picture, and the cover PSFlyCast put
	// there by itself.
	int current = ps5::covers::Automatic;
	std::string own;
	std::string found;
	unsigned stamp = 0;
	std::string note;					// a choice that could not be made
} coverUi;

// A cover's file has changed: its picture is loaded again, and nothing that
// is drawn keeps the old one.
void forgetPicture(const std::string& file)
{
	if (imguiDriver != nullptr)
		imguiDriver->deleteTexture(file);
	ambient = Ambient{};
}

void openCover(Game& g)
{
	coverUi = CoverUi{};
	coverUi.open = true;
	coverUi.title = g.title;
	coverUi.base = get_file_basename(g.media.fileName);
	coverUi.bases.push_back(coverUi.base);
	const auto set = setsGrouped && !g.set.empty() ? discSets.find(g.set) : discSets.end();
	if (set != discSets.end())
		for (size_t i : set->second)
			if (&games[i] != &g)
				coverUi.bases.push_back(get_file_basename(games[i].media.fileName));
	coverUi.ownArt = g.art.boxartPath;
	if (!g.disc || g.media.arcade || coverUi.base.empty())
		coverUi.cannot = "The collection's pictures are of Dreamcast discs: there is none to download for this game";
	else if (!ps5::options().covers)
		coverUi.cannot = "Downloads are off: Settings > Library has Download covers and descriptions";
	else
		ps5::covers::fetchAlternatives(coverUi.base);
}

void coverDialog(ImDrawList *dl, const Input& given)
{
	using ps5::covers::Alternatives;
	CoverUi& ui = coverUi;
	const Alternatives found = ps5::covers::alternatives(ui.base);
	unsigned stamp = ps5::covers::generation();
	for (int i = 0; i < ps5::covers::PictureCount; i++)
		stamp = stamp * 8 + (unsigned)found.state[i] + 1;
	if (stamp != ui.stamp)
	{
		ui.stamp = stamp;
		ui.current = ps5::covers::current(ui.base);
		ui.own = ps5::covers::ownFile(ui.base);
		const std::string inPlace = ps5::rootDir + "covers/" + ui.base + ".png";
		ui.found = ui.current == ps5::covers::Automatic && file_exists(inPlace) ? inPlace : "";
	}
	// A picture in the covers folder that nothing is noted about is the
	// user's - unless an earlier build downloaded it, which kept no notes. The
	// box art tells (ps5_covers.cpp compares the two): until it is here, the
	// dialog does not say whose the cover is.
	const bool unsure = ui.current == ps5::covers::Own && found.state[ps5::covers::Boxart] == Alternatives::Waiting
			&& ui.own.rfind(ps5::rootDir + "covers/.yours/", 0) != 0;

	// The choices, left to right. state is Found for one that can be made.
	struct Tile { int choice; const char *name; std::string file; Alternatives::State state; };
	std::vector<Tile> tiles;
	// Automatic: the cover PSFlyCast put there itself, else the box art a
	// download brings, else what the game brings itself.
	const bool boxart = ui.cannot.empty() && found.state[ps5::covers::Boxart] == Alternatives::Found;
	tiles.push_back({ ps5::covers::Automatic, coverChoiceName(ps5::covers::Automatic),
			!ui.found.empty() ? ui.found : boxart ? found.file[ps5::covers::Boxart] : ui.ownArt, Alternatives::Found });
	for (int i = 0; i < ps5::covers::PictureCount; i++)
		tiles.push_back({ i, coverChoiceName(i), found.state[i] == Alternatives::Found ? found.file[i] : "", found.state[i] });
	if (!ui.own.empty() && !unsure)
		tiles.push_back({ ps5::covers::Own, coverChoiceName(ps5::covers::Own), ui.own, Alternatives::Found });
	const int n = (int)tiles.size();
	// The focus starts on the cover the game has.
	int focus = std::clamp(ui.focus, 0, n - 1);
	if (ui.focus < 0)
		for (int i = 0; i < n && !unsure; i++)
			if (tiles[i].choice == ui.current)
				focus = i;
	if (given.left && focus > 0)
		ui.focus = --focus;
	if (given.right && focus < n - 1)
		ui.focus = ++focus;
	const Tile& chosen = tiles[focus];
	if (given.accept && chosen.state != Alternatives::Found)
		ps5::sound::cue(ps5::sound::Cue::Refuse);
	else if (given.accept && chosen.choice == ui.current && !unsure)
		// The cover it has: nothing changes.
		ui.open = false;
	else if (given.accept)
	{
		bool done = true;
		for (const std::string& base : ui.bases)
		{
			// A disc of the set with no picture of the user's keeps the cover it has.
			if (chosen.choice == ps5::covers::Own && ps5::covers::ownFile(base).empty())
				continue;
			done = ps5::covers::choose(base, chosen.choice, ui.base) && done;
			for (const char *extension : { ".png", ".jpg", ".jpeg" })
				forgetPicture(ps5::rootDir + "covers/" + base + extension);
		}
		if (done)
			ui.open = false;
		else
		{
			ui.note = "The cover could not be written to the covers folder";
			ps5::sound::cue(ps5::sound::Cue::Refuse);
		}
	}
	else if (given.back)
		ui.open = false;
	bool waiting = false, nothing = true, failed = false;
	for (int i = 0; i < ps5::covers::PictureCount; i++)
	{
		waiting = waiting || found.state[i] == Alternatives::Waiting;
		nothing = nothing && found.state[i] == Alternatives::Missing;
		failed = failed || found.state[i] == Alternatives::Failed;
	}
	// What could not be downloaded is asked for again.
	if (given.square && failed && !waiting)
		ps5::covers::fetchAlternatives(ui.base);

	// As wide as its longest line needs, and wider for a fifth picture: the
	// pictures share the width.
	const float pad = 52, gap = 24;
	const float w = n > 4 ? 1200 : 1040, inner = w - 2 * pad;
	const float tile = (inner - (n - 1) * gap) / n, h = 420 + tile;
	const ImVec2 at = dialogPanel(dl, w, h);
	text(dl, bold(), 38, at.x + pad, at.y + pad - 6, col::text, "Change cover");
	const std::string whose = ui.title + (ui.bases.size() > 1 ? "  \xc2\xb7  every disc of it" : "");
	text(dl, regular(), 23, at.x + pad, at.y + pad + 50, col::dim, fit(regular(), 23, whose, inner).c_str());
	const float ty = at.y + pad + 104;
	for (int i = 0; i < n; i++)
	{
		const Tile& t = tiles[i];
		const float tx = at.x + pad + i * (tile + gap);
		const bool on = i == focus;
		if (on)
			glow(dl, tx, ty, tile, tile, 14, alpha(col::accent, 0.9f), 18);
		rect(dl, tx, ty, tile, tile, col::card, 14);
		ImTextureID id = ImTextureID();
		if (!t.file.empty())
		{
			ImguiFileTexture picture(t.file);
			id = picture.getId();
		}
		const float cx = tx + tile / 2, cy = ty + tile / 2;
		if (id != ImTextureID())
			imageFill(dl, id, tx, ty, tile, tile, 14);
		else if (t.state == Alternatives::Waiting)
		{
			// An arc going round while it is downloaded.
			const float turn = (float)timeNow * 3.2f;
			dl->PathClear();
			dl->PathArcTo(V(cx, cy), 34 * S, turn, turn + IM_PI * 0.7f, 24);
			dl->PathStroke(col::accent, 0, 6 * S);
		}
		else
		{
			const bool failed = t.state == Alternatives::Failed;
			const char *icon = t.choice == ps5::covers::Automatic ? ICON_FA_WAND_MAGIC_SPARKLES
					: failed ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_IMAGE;
			const ImVec2 is = textSize(regular(), 54, icon);
			text(dl, regular(), 54, cx - is.x / 2, cy - is.y / 2, failed ? alpha(col::warm, 0.8f) : col::faint, icon);
		}
		if (on)
			outline(dl, tx - 4, ty - 4, tile + 8, tile + 8, col::text, 17, 4);
		const ImVec2 ns = textSize(bold(), 24, t.name);
		text(dl, bold(), 24, tx + (tile - ns.x) / 2, ty + tile + 16, on ? col::text : col::dim, t.name);
		// Under its name: that it is the cover now, or why it cannot be chosen.
		std::string under;
		ImU32 colour = col::faint;
		if (t.choice == ui.current && !unsure)
		{
			under = ICON_FA_CIRCLE_CHECK "  The cover now";
			colour = col::good;
		}
		else if (t.state == Alternatives::Waiting)
			under = "Downloading";
		else if (t.state == Alternatives::Missing)
			under = "None found";
		else if (t.state == Alternatives::Failed)
			under = "Not downloaded";
		else if (t.state == Alternatives::None)
			under = "Not available";
		const ImVec2 us = textSize(regular(), 20, under.c_str());
		text(dl, regular(), 20, tx + (tile - us.x) / 2, ty + tile + 52, colour, under.c_str());
	}
	// One line: what went wrong, what is going on, or what the focused choice is.
	std::string line;
	bool bad = false;
	if (!ui.note.empty() || !found.error.empty())
	{
		line = !ui.note.empty() ? ui.note : found.error;
		bad = true;
	}
	else if (waiting)
		line = "Downloading from the covers' collection";
	else if (!ui.cannot.empty())
		line = ui.cannot;
	else if (nothing)
		line = "The covers' collection has no picture under this game's file name";
	else if (chosen.choice == ps5::covers::Automatic)
		line = "The cover PSFlyCast finds by itself: the downloaded box art, or the picture the game brings";
	else if (chosen.choice == ps5::covers::Own)
		line = "Your own picture, from the covers folder. It is kept whatever is chosen here";
	else
		line = "From the covers' collection, under this game's file name";
	text(dl, regular(), 22, at.x + pad, ty + tile + 100, bad ? col::warm : col::faint, fit(regular(), 22, line, inner).c_str());
	float bx = at.x + pad;
	const float by = at.y + h - pad - 60 + 8;
	bx += dialogButton(dl, bx, by, Glyph::Cross, "Use this cover", chosen.state == Alternatives::Found) + 20;
	bx += dialogButton(dl, bx, by, Glyph::Circle, "Cancel", false) + 20;
	bx += dialogButton(dl, bx, by, Glyph::DPadLR, "Choose", false) + 20;
	if (failed && !waiting)
		dialogButton(dl, bx, by, Glyph::Square, "Try again", false);
}

} // namespace

// ---- updating (ps5_update.cpp)

struct UpdateUi
{
	bool open = false;
} updateUi;

// Opens the dialog; asked for by hand, it also asks GitHub again unless an
// update is on its way.
void openUpdate(bool ask)
{
	using ps5::update::Phase;
	updateUi.open = true;
	const Phase phase = ps5::update::status().phase;
	if (ask && (phase == Phase::Idle || phase == Phase::UpToDate || phase == Phase::Available || phase == Phase::Failed))
		ps5::update::check();
}

std::string megabytes(uint64_t bytes)
{
	char amount[32];
	snprintf(amount, sizeof(amount), "%.1f MB", bytes / 1048576.0);
	return amount;
}

void updateDialog(ImDrawList *dl, const Input& given)
{
	using ps5::update::Phase;
	const ps5::update::Status now = ps5::update::status();
	enum Tone { Dim, Plain, Good };
	struct Line { std::string words; Tone tone; };
	struct Button { Glyph glyph; std::string label; bool primary; };
	std::string title, sub;
	std::vector<Line> lines;
	std::vector<Button> buttons;
	float bar = -1;		// none; -2 a light going along it; else the part done
	const std::string mine = ps5::update::thisBuild();

	switch (now.phase)
	{
	case Phase::Idle:
	case Phase::Checking:
		title = "Looking for a new version";
		sub = "Asking the project's GitHub page";
		bar = -2;
		buttons = { { Glyph::Circle, "Close", false } };
		if (given.back)
			updateUi.open = false;
		break;

	case Phase::UpToDate:
		title = "PSFlyCast is up to date";
		if (ps5::update::isRelease())
			sub = "You have " + mine + ", the newest release";
		else
		{
			sub = "The newest release is " + now.latest + ". This is " + mine + ", which is newer";
			lines.push_back({ "Installing " + now.latest + " would put the release in this test build's place", Dim });
		}
		buttons = { { Glyph::Circle, "Close", true }, { Glyph::Square, "Install " + now.latest + " anyway", false } };
		if (given.back)
			updateUi.open = false;
		else if (given.square)
			ps5::update::install();
		break;

	case Phase::Available:
		title = "PSFlyCast " + now.latest + " is out";
		sub = "You have " + mine + (now.size != 0 ? "   \xc2\xb7   " + megabytes(now.size) + " to download from the project's GitHub page"
				: "");
		for (const std::string& note : now.notes)
			lines.push_back({ note, Plain });
		lines.push_back({ "Your games, saves, settings and covers are kept", Dim });
		lines.push_back({ "The new version starts the next time you open PSFlyCast", Dim });
		buttons = { { Glyph::Cross, "Update now", true }, { Glyph::Circle, "Later", false },
				{ Glyph::Square, "Skip this version", false } };
		if (given.accept)
			ps5::update::install();
		else if (given.back)
			updateUi.open = false;
		else if (given.square)
		{
			ps5::update::skip();
			updateUi.open = false;
		}
		break;

	case Phase::Downloading:
	case Phase::Verifying:
	case Phase::Unpacking:
	case Phase::Swapping:
	{
		title = "Updating to " + now.latest;
		const int step = now.phase == Phase::Downloading ? 1 : now.phase == Phase::Verifying ? 2
				: now.phase == Phase::Unpacking ? 2 : 3;
		if (now.phase == Phase::Downloading)
		{
			sub = "Downloading   \xc2\xb7   " + megabytes(now.done) + (now.size != 0 ? " of " + megabytes(now.size) : "");
			bar = now.size != 0 ? std::clamp((float)((double)now.done / (double)now.size), 0.f, 1.f) : -2;
		}
		else
		{
			sub = now.phase == Phase::Verifying ? "Checking the download" : now.phase == Phase::Unpacking ? "Unpacking"
					: "Putting the files in place";
			bar = -2;
		}
		const char *steps[] = { "Found the release and its checksum", "Downloading the ZIP",
				"Check it against the checksum, then unpack it",
				"Swap the files in; this version is kept until the new one has started once" };
		for (int i = 0; i < 4; i++)
			lines.push_back({ steps[i], i < step ? Good : i == step ? Plain : Dim });
		if (now.phase == Phase::Downloading || now.phase == Phase::Unpacking)
		{
			buttons = { { Glyph::Circle, "Cancel", false } };
			if (given.back)
				ps5::update::cancel();
		}
		break;
	}

	case Phase::Installed:
		title = "PSFlyCast " + now.latest + " is installed";
		sub = "It starts when PSFlyCast is next started";
		lines.push_back({ "Restarting closes PSFlyCast and opens it again, as the new version", Plain });
		lines.push_back({ "If the console only closes it, open it again from the home screen", Dim });
		lines.push_back({ "This version's files are kept in the update folder until the new one has started once", Dim });
		buttons = { { Glyph::Cross, "Restart PSFlyCast now", true }, { Glyph::Circle, "Later", false } };
		if (given.accept)
		{
			ps5::restartOnExit = true;
			dc_exit();
		}
		else if (given.back)
			updateUi.open = false;
		break;

	case Phase::Failed:
		title = "The update did not go through";
		sub = now.error;
		lines.push_back({ now.restored ? "Nothing of the title was changed" : "Copy the release's ZIP over the title's folder to repair it",
				now.restored ? Dim : Plain });
		lines.push_back({ "Every step is in flycast-boot.log, in the title's folder", Dim });
		buttons = { { Glyph::Circle, "Close", true }, { Glyph::Square, "Try again", false } };
		if (given.back)
			updateUi.open = false;
		else if (given.square)
			ps5::update::check();
		break;
	}

	const float w = 940, pad = 52, inner = w - 2 * pad;
	const float subH = sub.empty() ? 0 : textSize(regular(), 23, sub.c_str(), inner).y + 14;
	const float h = pad + 50 + subH + (bar != -1 ? 46 : 0) + (lines.empty() ? 0 : 18 + lines.size() * 42)
			+ (buttons.empty() ? 0 : 30 + 60) + pad - 8;
	const ImVec2 at = dialogPanel(dl, w, h);
	float y = at.y + pad;
	text(dl, bold(), 38, at.x + pad, y - 6, col::text, fit(bold(), 38, title, inner).c_str());
	y += 50;
	if (!sub.empty())
	{
		text(dl, regular(), 23, at.x + pad, y, col::dim, sub.c_str(), inner);
		y += subH;
	}
	if (bar != -1)
	{
		y += 14;
		rect(dl, at.x + pad, y, inner, 12, col::rgba(255, 255, 255, 30), 6);
		if (bar >= 0)
			rect(dl, at.x + pad, y, std::max(12.f, inner * bar), 12, col::accent, 6);
		else
		{
			const float span = inner * 0.22f;
			const float lightAt = (float)std::fmod(timeNow * 0.7, 1.0) * (inner + span) - span;
			const float from = std::max(0.f, lightAt), to = std::min(inner, lightAt + span);
			if (to > from)
				rect(dl, at.x + pad + from, y, to - from, 12, alpha(col::accent, 0.85f), 6);
		}
		y += 32;
	}
	if (!lines.empty())
	{
		y += 18;
		for (const Line& line : lines)
		{
			const ImU32 colour = line.tone == Good ? col::good : line.tone == Plain ? col::text : col::dim;
			dl->AddCircleFilled(V(at.x + pad + 8, y + 15), 5 * S, line.tone == Dim ? col::faint : col::accent);
			text(dl, regular(), 23, at.x + pad + 30, y, colour, fit(regular(), 23, line.words, inner - 30).c_str());
			y += 42;
		}
	}
	if (!buttons.empty())
	{
		y += 30;
		float x = at.x + pad;
		for (const Button& button : buttons)
			x += dialogButton(dl, x, y, button.glyph, button.label.c_str(), button.primary) + 20;
	}
}

// ---- the other player's address, for netplay

struct AddressUi
{
	bool open = false;
	int field = 3;
	int part[5] = { 192, 168, 1, 2, 19713 };	// the address's four numbers, and the port
	std::string note;		// what came of typing it
	bool noteBad = false;
} addressUi;

// A name typed for the address, looked up off this thread: the numbers it
// stands for are what the setting holds (Flycast's netplay takes numbers).
struct AddressLookup
{
	std::mutex lock;
	int state = 0;			// 0 nothing, 1 asking, 2 found, 3 not found
	unsigned serial = 0;	// the lookup whose answer counts
	std::string name;
	int part[4] = {};
	int port = 0;
} addressLookup;

void lookUpAddress(const std::string& host, int port)
{
	unsigned serial;
	{
		std::lock_guard<std::mutex> hold(addressLookup.lock);
		addressLookup.state = 1;
		addressLookup.name = host;
		addressLookup.port = port;
		serial = ++addressLookup.serial;
	}
	std::thread([host, serial] {
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_DGRAM;
		addrinfo *found = nullptr;
		const int rc = getaddrinfo(host.c_str(), nullptr, &hints, &found);
		{
			std::lock_guard<std::mutex> hold(addressLookup.lock);
			if (serial == addressLookup.serial)
			{
				if (rc == 0 && found != nullptr && found->ai_family == AF_INET && found->ai_addr != nullptr)
				{
					const uint8_t *bytes = (const uint8_t *)&((const sockaddr_in *)found->ai_addr)->sin_addr;
					for (int i = 0; i < 4; i++)
						addressLookup.part[i] = bytes[i];
					addressLookup.state = 2;
				}
				else
					addressLookup.state = 3;
			}
		}
		if (found != nullptr)
			freeaddrinfo(found);
	}).detach();
}

// What the keyboard gave for the address: its numbers, or a name, each with
// or without a port after a colon.
void addressTyped(const std::string& given)
{
	std::string host = trimmed(given);
	if (host.empty())
		return;
	{
		// A name still being looked up no longer counts.
		std::lock_guard<std::mutex> hold(addressLookup.lock);
		addressLookup.serial++;
		addressLookup.state = 0;
	}
	int port = addressUi.part[4];
	const size_t colon = host.rfind(':');
	if (colon != std::string::npos && host.find(':') == colon)
	{
		const int asked = atoi(host.c_str() + colon + 1);
		if (asked >= 1 && asked <= 65535)
			port = asked;
		host.resize(colon);
	}
	unsigned a, b, c, d;
	char more;
	if (sscanf(host.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &more) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
	{
		const unsigned numbers[4] = { a, b, c, d };
		for (int i = 0; i < 4; i++)
			addressUi.part[i] = (int)numbers[i];
		addressUi.part[4] = port;
		addressUi.note = "Typed. Cross saves it";
		addressUi.noteBad = false;
		return;
	}
	if (host.find_first_of(" /") != std::string::npos || host.empty())
	{
		addressUi.note = "\"" + host + "\" is not an address or a name";
		addressUi.noteBad = true;
		return;
	}
	lookUpAddress(host, port);
}

constexpr int NetplayPort = 19713;		// Flycast's (core/network/ggpo.cpp)

void openAddress()
{
	addressUi = AddressUi{};
	addressUi.open = true;
	unsigned a, b, c, d, port = NetplayPort;
	if (sscanf(config::NetworkServer.get().c_str(), "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) >= 4)
	{
		const unsigned given[5] = { a, b, c, d, port };
		for (int i = 0; i < 5; i++)
			addressUi.part[i] = (int)std::min(given[i], i < 4 ? 255u : 65535u);
	}
	else if (sscanf(ps5::net::localAddress().c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4)
	{
		// Most likely on this console's own network: its numbers to start from.
		addressUi.part[0] = (int)a;
		addressUi.part[1] = (int)b;
		addressUi.part[2] = (int)c;
		addressUi.part[3] = 1;
	}
}

// What the setting holds, as the Settings show it.
std::string otherPlayerText()
{
	return config::NetworkServer.get().empty() ? "Not set" : config::NetworkServer.get();
}

void addressDialog(ImDrawList *dl, const Input& given)
{
	AddressUi& ui = addressUi;
	bool asking;
	{
		std::lock_guard<std::mutex> hold(addressLookup.lock);
		if (addressLookup.state == 2)
		{
			for (int i = 0; i < 4; i++)
				ui.part[i] = addressLookup.part[i];
			ui.part[4] = addressLookup.port;
			ui.note = addressLookup.name + " is this address. Cross saves it";
			ui.noteBad = false;
			addressLookup.state = 0;
		}
		else if (addressLookup.state == 3)
		{
			ui.note = "\"" + addressLookup.name + "\" was not found. Its numbers work without a name";
			ui.noteBad = true;
			addressLookup.state = 0;
		}
		asking = addressLookup.state == 1;
		if (asking)
		{
			ui.note = "Looking up " + addressLookup.name;
			ui.noteBad = false;
		}
	}
	if (given.square)
		openKeyboard(KeyboardAddress, "The other player's address", "192.168.1.20, or a name", "", 80);
	if (given.left && ui.field > 0)
		ui.field--;
	if (given.right && ui.field < 4)
		ui.field++;
	const int step = given.up ? 1 : given.down ? -1 : given.r2 ? 10 : given.l2 ? -10 : given.r1 ? 100 : given.l1 ? -100 : 0;
	if (step != 0)
	{
		int& value = ui.part[ui.field];
		if (ui.field < 4)
			value = ((value + step) % 256 + 256) % 256;		// round and round
		else
			value = std::clamp(value + step, 1, 65535);
		if (!asking)
			ui.note.clear();
	}
	if (given.accept && asking)
		ps5::sound::cue(ps5::sound::Cue::Refuse);
	else if (given.accept)
	{
		char address[48];
		snprintf(address, sizeof(address), "%d.%d.%d.%d", ui.part[0], ui.part[1], ui.part[2], ui.part[3]);
		std::string whole = address;
		if (ui.part[4] != NetplayPort)
			whole += ":" + std::to_string(ui.part[4]);
		config::NetworkServer.set(whole);
		SaveSettings();
		ui.open = false;
	}
	else if (given.triangle)
	{
		config::NetworkServer.set("");
		SaveSettings();
		ui.open = false;
	}
	else if (given.back)
		ui.open = false;

	const float w = 1080, h = 470, pad = 52;
	const ImVec2 at = dialogPanel(dl, w, h);
	text(dl, bold(), 38, at.x + pad, at.y + pad - 6, col::text, "The other player's address");
	if (given.back || given.triangle)
	{
		// Closed: a name still being looked up no longer counts.
		std::lock_guard<std::mutex> hold(addressLookup.lock);
		addressLookup.serial++;
		addressLookup.state = 0;
	}
	const std::string own = ps5::net::localAddress();
	const std::string sub = "Their console or PC on the network. They enter this console's"
			+ (own.empty() ? std::string("") : ": " + own);
	text(dl, regular(), 23, at.x + pad, at.y + pad + 50, col::dim, sub.c_str());

	// Four numbers and the port, the one being changed lit.
	const float boxW = 150, portW = 210, boxH = 108, gap = 34;
	const float total = 4 * boxW + 3 * gap + 56 + portW;
	float x = at.x + (w - total) / 2;
	const float by = at.y + 178;
	for (int i = 0; i < 5; i++)
	{
		const float bw = i < 4 ? boxW : portW;
		const bool on = i == ui.field;
		rect(dl, x, by, bw, boxH, on ? alpha(col::accent, 0.22f) : col::panelHi, 16);
		if (on)
		{
			outline(dl, x, by, bw, boxH, col::accent, 16, 3);
			dl->AddTriangleFilled(V(x + bw / 2, by - 26), V(x + bw / 2 - 13, by - 10), V(x + bw / 2 + 13, by - 10), col::accent);
			dl->AddTriangleFilled(V(x + bw / 2, by + boxH + 26), V(x + bw / 2 - 13, by + boxH + 10),
					V(x + bw / 2 + 13, by + boxH + 10), col::accent);
		}
		const std::string value = std::to_string(ui.part[i]);
		const ImVec2 vs = textSize(bold(), 54, value.c_str());
		text(dl, bold(), 54, x + (bw - vs.x) / 2, by + (boxH - vs.y) / 2, on ? col::text : col::dim, value.c_str());
		x += bw;
		if (i < 4)
		{
			const char *between = i < 3 ? "." : ":";
			const float space = i < 3 ? gap : 56;
			const ImVec2 bs = textSize(bold(), 54, between);
			text(dl, bold(), 54, x + (space - bs.x) / 2, by + (boxH - bs.y) / 2, col::faint, between);
			x += space;
		}
	}
	const std::string field = !ui.note.empty() ? fit(regular(), 22, ui.note, w - 2 * pad)
			: ui.field < 4 ? "Up and down change the number; L2 and R2 by 10, L1 and R1 by 100"
			: "The port: 19713 unless the other player says otherwise";
	const ImVec2 fs = textSize(regular(), 22, field.c_str());
	text(dl, regular(), 22, at.x + (w - fs.x) / 2, by + boxH + 44, ui.note.empty() ? col::faint : ui.noteBad ? col::warm : col::text,
			field.c_str());
	float bx = at.x + pad;
	const float buttonsY = at.y + h - pad - 60 + 8;
	bx += dialogButton(dl, bx, buttonsY, Glyph::Cross, "Save", true) + 20;
	bx += dialogButton(dl, bx, buttonsY, Glyph::Circle, "Cancel", false) + 20;
	bx += dialogButton(dl, bx, buttonsY, Glyph::Square, "Type it", false) + 20;
	dialogButton(dl, bx, buttonsY, Glyph::Triangle, "No address", false);
}

// ---- the memory card manager (ps5_vmu.cpp): Settings > System > Memory cards
#include "bigpicture_cards.inc"

// ---- the RetroAchievements account: Settings > Achievements > Account
#include "bigpicture_achievements.inc"

// ------------------------------------------------------------- the splash
//
// The start-up animations. Each start shows one: the one chosen in Settings >
// Interface, or one of the four by chance, never the one shown the last time.
// The start-up sound goes with it (ps5::sound, ps5_audio.cpp): its hit comes
// 2.49 s in, a beat every 0.6 s after it and a pulse four times a beat, and
// every animation has its mark whole on the hit and in the top bar on a beat.
// Any button ends it. With "Less" motion it is the mark and the name, still,
// for a second.
//
// The first, the birds, a little over seven seconds:
//   0.3 s  one line winds in to the middle of the screen and out again, and
//          is whole on the hit;
//   2.49 s the hit: its loops are a disc's grooves, and the app's name comes
//          up under it;
//   3.54 s the line lets its turns out until it is straight, and leaves to the left;
//   3.69 s the letters lift off as birds for the top left, one on each pulse,
//          the first on a beat and the last on a beat;
//   4.89 s on the beat the line comes in again in the top bar and coils up
//          there as the mark;
//   5.49 s on the next beat the first bird lands and the mark starts turning;
//          a bird lands on each pulse and they are the name in the top bar,
//          the last on the beat at 6.69 s;
//   6.4 s  the library comes in under them.
// The other three are in bigpicture_splash.inc.

namespace splashAt
{
// The sound: its hit, its beat, the pulse within a beat, and the beat three
// of the animations land their mark on in the top bar.
constexpr float Hit = 2.49f, Beat = 0.6f, Pulse = Beat / 4, Land = Hit + 6 * Beat;
constexpr float BarHeight = 96;
// The birds: the first leaves, the next ones, and how long one flies (it is
// there twelve pulses after it left).
constexpr float Lift = Hit + 2 * Beat, Gap = Pulse, Flight = 12 * Pulse - 0.12f;
constexpr float Loose = Lift - Pulse;						// the line starts letting its turns out
constexpr float Coil = Hit + 4 * Beat, Spin = Hit + 5 * Beat;	// the mark in the top bar coils up, and starts turning
constexpr float Library = 6.4f, Over = 7.4f;				// the library comes in
constexpr float Still = 1.2f;								// with less motion
constexpr float DiscX = 0, DiscY = 410, DiscRadius = 232;	// the disc: on the screen's middle line
constexpr float NameSize = 126, NameTop = 692;				// the name under it
constexpr float MarkX = 76, MarkY = 48, MarkRadius = 22;	// the top bar's mark and name (topBar)
constexpr float BarSize = 34, BarX = 112, BarTop = 28;
}

// How far t is between two moments, 0 to 1.
float between(float t, float from, float to)
{
	return std::clamp((t - from) / (to - from), 0.f, 1.f);
}

float easeIn(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return t * t * t;
}

// Slow, fast, slow: more so than easeInOut.
float swell(float t)
{
	t = std::clamp(t, 0.f, 1.f);
	return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2;
}

#include "bigpicture_splash.inc"

// A bird as a child draws one: two wings from where they meet. flap is how
// far up the wings are, 0 to 1.
void bird(ImDrawList *dl, float x, float y, float size, float flap, float tilt, ImU32 colour)
{
	if ((colour & IM_COL32_A_MASK) == 0)
		return;
	const float tip = -size * (0.10f + 0.50f * flap), bend = -size * (0.55f + 0.25f * flap);
	const float c = std::cos(tilt), s = std::sin(tilt);
	auto at = [&](float px, float py) { return V(x + px * c - py * s, y + px * s + py * c); };
	dl->PathClear();
	dl->PathLineTo(at(-size, tip));
	dl->PathBezierQuadraticCurveTo(at(-size * 0.45f, bend), at(0, 0), 10);
	dl->PathBezierQuadraticCurveTo(at(size * 0.45f, bend), at(size, tip), 10);
	dl->PathStroke(colour, 0, std::max(1.6f, size * 0.17f) * S);
}

// The birds at the moment t, without what is behind them.
void drawSplashBirds(ImDrawList *dl, float t)
{
	using namespace splashAt;
	const float cx = W / 2 + DiscX, cy = DiscY;
	static ImVec2 points[SpiralLine::Count];

	// Light where the disc is.
	const float lit = between(t, 0.2f, 1.4f) * (1 - between(t, Lift, Lift + 1));
	if (lit > 0)
		softDisc(dl, cx, cy, 760, alpha(col::accent, 0.17f * lit), 64);
	// The sound's hit, and the beat after it.
	if (t < Loose)
		splashHit(dl, t, cx, cy, Loose);

	// The line: drawn, whole on the hit, then the disc's grooves, then let out
	// straight, then gone to the left.
	const float sinceHit = t - Hit;
	const float drawn = swell(between(t, 0.3f, Hit));
	const float body = between(sinceHit, 0, 0.3f) * (1 - between(t, Loose - 0.15f, Loose + 0.1f));
	const float loose = swell(between(t, Loose, Loose + 0.8f));
	const float gone = easeIn(between(t, Loose + 0.7f, Coil));
	if (drawn > 0 && gone < 1)
	{
		const float x = cx - gone * 3900;
		const float bump = sinceHit < 0 ? 0 : 0.05f * (sinceHit < 0.04f ? sinceHit / 0.04f : std::exp(-(sinceHit - 0.04f) / 0.13f));
		const float radius = DiscRadius * (1 + bump + 0.012f * splashBeat(t, Loose));
		discBody(dl, x, cy, radius, col::text, col::accent, body);
		const ImVec2 *line = spiralLine().at;
		if (loose > 0)
		{
			uncoil(loose, points);
			line = points;
		}
		// Its own light under it, then the line.
		strokeSpiral(dl, line, x, cy, radius, 0, drawn, alpha(col::accent, 0.10f), 19, false);
		strokeSpiral(dl, line, x, cy, radius, 0, drawn, alpha(col::accent, 0.20f), 12, false);
		strokeSpiral(dl, line, x, cy, radius, 0, drawn, col::text, 7);
	}

	// The mark in the top bar: the line comes in straight, coils up, and turns.
	const float coil = swell(between(t, Coil, Coil + 0.85f));
	if (coil > 0)
	{
		// A picture of the user's own takes the mark's place as the library comes in.
		const float own = customLogo() != ImTextureID() ? between(t, Library, Library + 0.6f) : 0;
		const float angle = markAngle();
		discBody(dl, MarkX, MarkY, MarkRadius, col::text, col::accent, between(t, Spin + 0.05f, Spin + 0.45f) * (1 - own));
		const ImVec2 *line = spiralLine().at;
		if (coil < 1)
		{
			uncoil(1 - coil, points);
			line = points;
		}
		strokeSpiral(dl, line, MarkX, MarkY, MarkRadius, angle, 1, alpha(col::text, between(t, Coil, Coil + 0.2f) * (1 - own)),
				markWidth(MarkRadius));
		if (own > 0)
			spinMark(dl, MarkX, MarkY, MarkRadius, own);
	}

	// The name: letters, then birds, then letters again in the top bar.
	static const char name[] = "PSFlyCast";
	const float ascent = 0.79f;		// of a line's height, to the letters' baseline
	const float left = W / 2 - textSize(bold(), NameSize, name).x / 2;
	for (int i = 0; name[i] != 0; i++)
	{
		const std::string before(name, i), letter(1, name[i]);
		const float start = Lift + i * Gap;
		const float shown = easeOut(between(t, Hit + i * 0.03f, Hit + 0.3f + i * 0.03f));
		const float morph = between(t, start, start + 0.3f);
		const float fly = swell(between(t, start + 0.12f, start + 0.12f + Flight));
		const float land = between(t, start + Flight - 0.05f, start + Flight + 0.25f);
		// The middle of the letter, large and in the top bar.
		const float x0 = left + textSize(bold(), NameSize, before.c_str()).x + textSize(bold(), NameSize, letter.c_str()).x / 2;
		const float barLeft = BarX + textSize(bold(), BarSize, before.c_str()).x;
		const float x1 = barLeft + textSize(bold(), BarSize, letter.c_str()).x / 2;
		if (shown > 0 && morph < 1)
		{
			// It comes up into place; as a bird takes over, it shrinks and rises.
			const float size = std::round(NameSize * (1 - 0.5f * morph));
			const float base = NameTop + NameSize * ascent + (1 - shown) * 26 - morph * 30;
			text(dl, bold(), size, x0 - textSize(bold(), size, letter.c_str()).x / 2, base - size * ascent,
					alpha(col::text, shown * (1 - morph)), letter.c_str());
		}
		if (morph > 0 && land < 1)
		{
			// From the letter, up and over, to its place in the bar.
			const float y0 = NameTop + NameSize * 0.44f, y1 = BarTop + BarSize * 0.56f;
			const float mx = x0 + 90 - i * 14, my = 210 + i * 16, q = 1 - fly;
			const float bx = q * q * x0 + 2 * q * fly * mx + fly * fly * x1;
			const float by = q * q * y0 + 2 * q * fly * my + fly * fly * y1;
			const float vx = 2 * q * (mx - x0) + 2 * fly * (x1 - mx), vy = 2 * q * (my - y0) + 2 * fly * (y1 - my);
			const float tilt = std::clamp(std::atan2(vy, std::fabs(vx) + 1) * 0.5f, -0.5f, 0.5f) * (vx < 0 ? -1.f : 1.f);
			const float flap = 0.5f + 0.5f * std::sin(t * 15 + i * 1.7f);
			bird(dl, bx, by + std::sin(t * 5 + i) * 7 * std::sin(fly * IM_PI), 34 + (11 - 34) * fly * fly, flap, tilt,
					alpha(col::text, morph * (1 - land)));
		}
		if (land > 0)
			text(dl, bold(), BarSize, barLeft, BarTop, alpha(col::text, land), letter.c_str());
	}
	// The last one is down: the name is whole.
	splashRing(dl, MarkX, MarkY, 27, 70, t - (Lift + 8 * Gap + 12 * Pulse), 0.5f, 0.5f, 2.5f, col::text);
}

// For each animation: when the library begins to be drawn under it, when it
// is over, and when the top bar's mark starts turning.
struct SplashTimes
{
	float library, over, spin;
};
const SplashTimes splashTimes[SplashKinds] = {
	{ splashAt::Library, splashAt::Over, splashAt::Spin },
	{ 5.62f, 6.75f, splashAt::Hit },
	{ 5.85f, 7.0f, splashAt::Land },
	{ splashAt::Hit + 4 * splashAt::Beat, 6.75f, splashAt::Hit },
};
// As Settings > Interface > Start-up animation names its choices (Options::splash).
const char *const splashChoices[] = { "Off", "Random", "Birds", "Spin up", "Comet", "Sound line" };

// The animation shown at the moment t, without what is behind it.
void drawSplash(ImDrawList *dl, float t)
{
	switch (splash.kind)
	{
	case SplashDock: drawSplashDock(dl, t); break;
	case SplashComet: drawSplashComet(dl, t); break;
	case SplashShutter: drawSplashShutter(dl, t); break;
	default: drawSplashBirds(dl, t); break;
	}
}

// What is behind it at the moment t, over the library: the splash's own
// backdrop, which fades, or for the sound line what the shutter still covers.
void splashCover(ImDrawList *fg, float t)
{
	const SplashTimes& times = splashTimes[splash.kind];
	if (splash.kind == SplashShutter)
	{
		fg->PushClipRect(V(0, 0), V(W, shutterEdge(t)), true);
		const Layer cover = beginLayer(fg);
		drawBackdrop(fg);
		endLayer(cover, 1 - easeInOut(between(t, 5.8f, 6.4f)));
		fg->PopClipRect();
		return;
	}
	const float u = between(t, times.library, splash.kind == SplashBirds ? times.over : times.over - 0.15f);
	const Layer cover = beginLayer(fg);
	drawBackdrop(fg);
	endLayer(cover, 1 - (splash.kind == SplashBirds ? easeOut(u) : easeInOut(u)));
}

// Which one this start shows: the one chosen, or one by chance that is not
// the one shown the last time.
int chooseSplash()
{
	const int chosen = ps5::options().splash;
	if (chosen >= 2)
		return std::min(chosen - 2, (int)SplashKinds - 1);
	uint64_t seed = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count() ^ ((uint64_t)time(nullptr) << 20);
	seed = (seed ^ (seed >> 30)) * 0xbf58476d1ce4e5b9ull;
	seed = (seed ^ (seed >> 27)) * 0x94d049bb133111ebull;
	seed ^= seed >> 31;
	const int last = ps5::options().splashLast;
	int kind = (int)(seed % SplashKinds);
	if (last >= 0 && last < SplashKinds)
		kind = (last + 1 + (int)(seed % (SplashKinds - 1))) % SplashKinds;
	ps5::options().splashLast = kind;
	ps5::saveOptions();
	return kind;
}

// With less motion: the mark and the name, where the animation has them.
void drawSplashStill(ImDrawList *dl)
{
	using namespace splashAt;
	softDisc(dl, W / 2 + DiscX, DiscY, 760, alpha(col::accent, 0.17f), 64);
	spinMark(dl, W / 2 + DiscX, DiscY, DiscRadius);
	static const char name[] = "PSFlyCast";
	text(dl, bold(), NameSize, W / 2 - textSize(bold(), NameSize, name).x / 2, NameTop, col::text, name);
}

void libraryScreen(bool selectDisk);

void showSplash()
{
	splash = SplashState{};
}

void library(bool selectDisk)
{
	// The first time the library is on, the start-up animation is, before it.
	if (splash.state == SplashState::NotBegun)
	{
		const bool shown = !selectDisk && ps5::options().splash != 0 && motion() != MotionOff;
		if (shown)
			splash.kind = chooseSplash();
		splash.state = shown ? SplashState::Showing : SplashState::Over;
	}
	if (splash.state == SplashState::Over)
	{
		libraryScreen(selectDisk);
		return;
	}
	const bool lively = motion() == MotionFull;
	const SplashTimes& times = splashTimes[splash.kind];
	if (splash.state == SplashState::Showing)
	{
		ImDrawList *dl = beginScreen("##bp-splash", true);
		// The first frames wait for the display and the fonts: its clock,
		// and its sound, start after them.
		if (splash.frames++ < 3)
			splash.began = timeNow;
		else if (!splash.sounded)
		{
			splash.sounded = true;
			if (lively)
				spinMarkUpAt(splash.began + times.spin);
			if (ps5::options().splashSound)
			{
				// All of it, or with less motion the end of it, quieter.
				if (lively)
					ps5::sound::playStartup(0.05f, 0.6f);
				else
					ps5::sound::playStartup(6.4f, 0.4f);
			}
			else if (lively && splash.kind == SplashShutter)
				// The sound line draws the sound, heard or not.
				ps5::sound::prepareStartup();
		}
		const float t = (float)(timeNow - splash.began);
		// The games are looked for meanwhile.
		chooseSource();
		refreshGames(lib.source == Network);
		if (lively)
			drawSplash(dl, t);
		else
			drawSplashStill(dl);
		endScreen();
		const bool skipped = splash.skip && t > 0.25f;
		if (t >= (lively ? times.library : splashAt::Still) || skipped)
		{
			splash.state = SplashState::Leaving;
			splash.leaving = timeNow;
			splash.cut = skipped || !lively;
			splash.cutAt = t;
			if (skipped)
			{
				// The sound ends with it, and the mark turns as it always does.
				ps5::sound::stopStartup();
				markSpunAt = -1e9;
			}
		}
		return;
	}
	// Leaving: the library comes in under what the splash still covers
	// (splashCover). The animation goes on over it to its end, where its mark
	// and name are the top bar's; cut short, or with less motion, it fades
	// where it is instead.
	const double now = ImGui::GetTime();
	const float t = splash.cut ? (lively ? splash.cutAt : 0) : (float)(now - splash.began);
	const float u = splash.cut ? std::clamp((float)((now - splash.leaving) / 0.35), 0.f, 1.f)
			: between(t, times.library, times.over);
	splash.flight = splash.cut ? 1.f : std::min(u, 0.99f);
	libraryScreen(selectDisk);
	ImDrawList *fg = ImGui::GetForegroundDrawList();
	const Layer all = beginLayer(fg);
	splashCover(fg, t);
	if (lively)
		drawSplash(fg, t);
	else
		drawSplashStill(fg);
	endLayer(all, splash.cut ? 1 - u : 1);
	if (u >= 1)
	{
		splash.state = SplashState::Over;
		splash.flight = 1.f;
	}
}

void libraryScreen(bool selectDisk)
{
	if (discSwapScreen())
		return;
	ImDrawList *dl = beginScreen("##bp-library", true);
	// A newer release, found as the title started, is offered once; its
	// dialog takes the pad while it is open.
	if (!selectDisk && !game_started && ps5::update::offerAtStart())
		openUpdate(false);
	// The console's keyboard, for a search: the pad is its while it is up.
	{
		std::string typed;
		const int keyboard = keyboardState(typed);
		if (keyboard == KeyboardSearch)
			setSearch(typed);
		if (keyboard == -1)
			in = Input{};
	}
	if (updateUi.open)
	{
		const Input given = in;
		in = Input{};
		updateDialog(ImGui::GetForegroundDrawList(), given);
	}
	if (quitUi.open)
	{
		const Input given = in;
		in = Input{};
		quitDialog(ImGui::GetForegroundDrawList(), given);
	}
	if (coverUi.open)
	{
		const Input given = in;
		in = Input{};
		coverDialog(ImGui::GetForegroundDrawList(), given);
	}
	chooseSource();
	refreshGames(lib.source == Network);
	// A disc is to be chosen for the running game: its own tab, and the disc
	// after the one in the drive, if the game has several.
	static bool choosing = false;
	std::string nextDisc;
	if (selectDisk && !choosing)
		for (size_t i = 0; i < games.size(); i++)
			if (games[i].media.path == insertedPath)
			{
				switchSource(games[i].source);
				if (!games[i].set.empty())
				{
					const std::vector<size_t>& discs = discSets[games[i].set];
					const size_t at = std::find(discs.begin(), discs.end(), i) - discs.begin();
					nextDisc = games[discs[(at + 1) % discs.size()]].media.path;
				}
				break;
			}
	choosing = selectDisk;
	buildShelves(selectDisk);
	buildFlat(selectDisk);
	if (!nextDisc.empty())
	{
		focusPath(nextDisc);
		if (!lib.shelves.empty() && lib.shelves[0].name == "This game's discs")
		{
			lib.shelf = 0;
			for (size_t i = 0; i < lib.shelves[0].items.size(); i++)
				if (games[lib.shelves[0].items[i]].media.path == nextDisc)
					lib.shelves[0].focus = (int)i;
		}
	}
	const View v = view();

	auto focusedGame = [&]() -> Game * {
		if (v == Shelves)
		{
			if (lib.shelves.empty())
				return nullptr;
			Shelf& sh = lib.shelves[lib.shelf];
			return sh.items.empty() ? nullptr : &games[sh.items[std::clamp(sh.focus, 0, (int)sh.items.size() - 1)]];
		}
		return lib.flat.empty() ? nullptr : &games[lib.flat[std::clamp(lib.flatFocus, 0, (int)lib.flat.size() - 1)]];
	};
	Game *focused = focusedGame();

	// ---- input
	if (lib.details && focused == nullptr)
		lib.details = false;
	if (lib.details)
		detailsInput(*focused, selectDisk);
	else
	{
		if (v == Shelves && !lib.shelves.empty())
		{
			Shelf& sh = lib.shelves[lib.shelf];
			const int n = (int)sh.items.size();
			if (in.left && sh.focus > 0) sh.focus--;
			if (in.right && sh.focus < n - 1) sh.focus++;
			// By letter where the shelf is in the order of its titles; six
			// at a time on the others (the games last played, a game's discs).
			const bool byTitle = sh.name != "Recently played" && sh.name != "This game's discs";
			if (in.l2) sh.focus = byTitle ? letterJump(sh.items, sh.focus, -1) : std::max(0, sh.focus - 6);
			if (in.r2) sh.focus = byTitle ? letterJump(sh.items, sh.focus, 1) : std::min(n - 1, sh.focus + 6);
			if (in.up && lib.shelf > 0) lib.shelf--;
			if (in.down && lib.shelf < (int)lib.shelves.size() - 1) lib.shelf++;
		}
		else if (v != Shelves && !lib.flat.empty())
		{
			const int n = (int)lib.flat.size();
			const int step = v == Grid ? GridColumns : 1;
			int& f = lib.flatFocus;
			if (v == Grid)
			{
				if (in.left && f % GridColumns > 0) f--;
				if (in.right && f % GridColumns < GridColumns - 1 && f < n - 1) f++;
			}
			if (in.up && f - step >= 0) f -= step;
			if (in.down)
				f = f + step < n ? f + step : (v == Grid && f / GridColumns < (n - 1) / GridColumns ? n - 1 : f);
			if (in.l2) f = letterJump(lib.flat, f, -1);
			if (in.r2) f = letterJump(lib.flat, f, 1);
			lib.flatPath = games[lib.flat[f]].media.path;
		}
		if (in.accept && focused != nullptr)
			launch(*focused, selectDisk);
		if (in.triangle && focused != nullptr)
			openDetails(*focused);
		if (in.square)
		{
			// The open tab's games are looked for again.
			if (lib.source == Network)
				scanNetwork();
			else
			{
				scanner().refresh();
				if (lib.source == Usb)
					ps5::rescanUsb();
				scannedCount = (size_t)-1;
			}
		}
		// OPTIONS opens the Settings (not while a disc is chosen for a running
		// game: its options are in the quick menu).
		if (in.options && !selectDisk)
			gui_setState(GuiState::Settings);
		// R3 searches the titles, on the console's keyboard. Circle shows every
		// game again; with nothing searched for it leaves: the disc choice for
		// its game, the library for the console's home screen (after asking).
		if (in.r3)
			openKeyboard(KeyboardSearch, "Search the library", "A game's title, or part of it", librarySearch, 40);
		if (in.back)
		{
			if (!librarySearch.empty())
				setSearch("");
			else if (selectDisk)
				gui_setState(GuiState::Commands);
			else
				quitUi.open = true;
		}
		if (in.l1 || in.r1)
		{
			// The next tab, round and round.
			std::vector<int> sources;
			for (const TopTab& tab : topTabs())
				sources.push_back(tab.id);
			const int n = (int)sources.size();
			const int at = (int)(std::find(sources.begin(), sources.end(), lib.source) - sources.begin()) + (in.r1 ? 1 : -1);
			const int next = sources[(at + n) % n];
			switchSource(next);
			// It comes in from the side the button points to, also when it went round.
			entrance.next = in.r1 ? 1.f : -1.f;
		}
	}
	// The tab may have changed: its own shelves and list.
	buildShelves(selectDisk);
	buildFlat(selectDisk);
	focused = focusedGame();
	// A favourite made or a game hidden in its details changed the lists under
	// the panel: the focus stays on the panel's game, and the panel of a game
	// that has left the lists closes at once.
	if (lib.details && !selectDisk && (focused == nullptr || focused->media.path != det.path))
	{
		if (refocus(det.path, det.shelf))
			focused = focusedGame();
		else
		{
			lib.details = false;
			lib.detailsAnim = 0;
			lib.flatPath.clear();
		}
	}

	const bool empty = v == Shelves ? lib.shelves.empty() : lib.flat.empty();
	// The tab comes in from the side it was reached from; the backdrop takes
	// the focused cover's colours where it is asked to.
	const float e = entered(lib.source + (selectDisk ? 100 : 0));
	if (focused != nullptr && backdropChoice() == BackCover)
		ambientArt(coverTexture(*focused));
	// The view is chosen in the Settings: when it has changed, the focus stays on its game.
	{
		static int shownView = -1;
		if (shownView != (int)view())
		{
			if (shownView >= 0 && !lib.heroPath.empty())
			{
				focusPath(lib.heroPath);
				lib.flatScroll = (float)(view() == Grid ? lib.flatFocus / GridColumns : lib.flatFocus);
				focused = focusedGame();
			}
			shownView = (int)view();
		}
		if (focused != nullptr)
			lib.heroPath = v == Shelves ? lib.heroPath : focused->media.path;
	}
	const Layer content = beginLayer(dl);
	if (empty)
		emptyLibrary(dl, selectDisk);
	else if (view() == Shelves)
		drawShelves(dl, focused, selectDisk);
	else if (view() == Grid)
		drawGrid(dl, focused);
	else
		drawList(dl, focused, selectDisk);
	endEntrance(content, e);
	// The letter just jumped to, large, for a moment.
	if (const float since = (float)(timeNow - lib.letterAt); since < 0.9f && lib.letter != 0)
	{
		const float a = since < 0.6f ? 1.f : 1.f - (since - 0.6f) / 0.3f;
		const float size = 190, lx = W - 96 - size, ly = 150;
		glow(dl, lx, ly, size, size, 28, alpha(col::accent, 0.6f * a), 18);
		rect(dl, lx, ly, size, size, alpha(col::sheet, 0.94f * a), 28);
		outline(dl, lx, ly, size, size, alpha(col::accent, a), 28, 3);
		const char shown[2] = { lib.letter, 0 };
		const ImVec2 ls = textSize(bold(), 120, shown);
		text(dl, bold(), 120, lx + (size - ls.x) / 2, ly + (size - ls.y) / 2, alpha(col::text, a), shown);
	}

	// What goes on in the background, quietly, above the hint bar: the scan of
	// the share, why it found nothing new, cover downloads.
	std::vector<std::string> notes;
	if (lib.source == Network && !empty)
	{
		if (net.scanning || net.wanted)
		{
			const std::string doing = ps5::smb::status().text;
			notes.push_back(std::string(ICON_FA_ROTATE) + "   " + (doing.empty() ? "Looking for games on the share" : doing));
		}
		else if (net.unreachable)
			notes.push_back(std::string(ICON_FA_TRIANGLE_EXCLAMATION) + (net.known
					? "   The share did not answer: this is the list of the last scan"
					: "   The share stopped answering: the list is not complete"));
	}
	const std::string downloading = ps5::covers::status();
	if (!downloading.empty())
		notes.push_back(std::string(ICON_FA_DOWNLOAD) + "   " + downloading);
	// What is searched for and how many games have it, on the other side
	// (the shelves say it over the one shelf they are then).
	if (!librarySearch.empty() && !empty && !lib.details && v != Shelves)
	{
		const size_t count = lib.flat.size();
		const std::string found = std::string(ICON_FA_MAGNIFYING_GLASS) + "   " + fit(regular(), 20, librarySearch, 420) + "   \xc2\xb7   "
				+ std::to_string(count) + (count == 1 ? " game" : " games");
		const ImVec2 fs = textSize(regular(), 20, found.c_str());
		rect(dl, 96, H - 72 - 52, fs.x + 32, 36, alpha(col::accent, 0.85f), 18);
		text(dl, regular(), 20, 96 + 16, H - 72 - 52 + 18 - fs.y / 2, col::text, found.c_str());
	}
	float noteY = H - 72 - 52;
	for (const std::string& note : notes)
	{
		const ImVec2 ns = textSize(regular(), 20, note.c_str());
		rect(dl, W - 96 - ns.x - 32, noteY, ns.x + 32, 36, col::rgba(0, 0, 0, 150), 18);
		text(dl, regular(), 20, W - 96 - ns.x - 16, noteY + 18 - ns.y / 2, col::dim, note.c_str());
		noteY -= 44;
	}

	if (lib.details && focused != nullptr)
		lib.detailsAnim = approach(lib.detailsAnim, 1, 16);
	else
		lib.detailsAnim = approach(lib.detailsAnim, 0, 20);
	if (lib.detailsAnim > 0.01f && focused != nullptr)
		detailsPanel(dl, *focused, lib.detailsAnim, selectDisk);

	topBar(dl, lib.source, false, selectDisk ? Reach::Sources : Reach::All);
	const Glyph browse = view() == List ? Glyph::DPadUD : Glyph::DPadLR;
	const char *scanHint = lib.source == Network ? "Scan the share" : "Rescan";
	const bool searching = !librarySearch.empty();
	const char *leave = searching ? "Clear search" : selectDisk ? "Back" : "Quit";
	if (lib.details && selectDisk)
		hintBar(dl, { { Glyph::Cross, "Insert" }, { Glyph::Circle, "Back" } });
	else if (lib.details && det.page == PageActions && setsGrouped && focused != nullptr && focused->setSize > 1)
		hintBar(dl, { { Glyph::DPadLR, "Choose" }, { Glyph::Square, "Next disc" }, { Glyph::Cross, "Select" },
				{ Glyph::Circle, "Back" } });
	else if (lib.details && det.page == PageActions)
		hintBar(dl, { { Glyph::DPadLR, "Choose" }, { Glyph::Cross, "Select" }, { Glyph::Circle, "Back" } });
	else if (lib.details && det.page == PageStates)
		hintBar(dl, { { Glyph::DPadUD, "Slot" }, { Glyph::Cross, "Start from it" }, { Glyph::Circle, "Back" } });
	else if (lib.details && det.page == PageManage && det.row == ManageFavourite)
		hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::Cross, "On / off" }, { Glyph::Circle, "Back" } });
	else if (lib.details && det.page == PageManage && det.row <= ManageCover)
		hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::Cross, "Select" }, { Glyph::Circle, "Back" } });
	else if (lib.details && det.page == PageManage)
		hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::Circle, "Back" } });
	else if (lib.details)
		hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::DPadLR, "Change" }, { Glyph::Circle, "Back" } });
	else if (selectDisk && empty && searching)
		hintBar(dl, { { Glyph::L1R1, "Source" }, { Glyph::R3, "Search" }, { Glyph::Circle, "Clear search" } });
	else if (selectDisk)
		hintBar(dl, { { Glyph::L1R1, "Source" }, { browse, "Browse" }, { Glyph::L2R2, "Letter" }, { Glyph::R3, "Search" },
				{ Glyph::Cross, "Insert disc" }, { Glyph::Triangle, "Details" }, { Glyph::Circle, leave } });
	else if (empty && searching)
		hintBar(dl, { { Glyph::L1R1, "Tab" }, { Glyph::R3, "Search" }, { Glyph::Options, "Settings" }, { Glyph::Circle, leave } });
	else if (empty && lib.source == Network && (ps5::smb::gameFolders().empty() || net.scanning || net.wanted))
		hintBar(dl, { { Glyph::L1R1, "Tab" }, { Glyph::Options, "Settings" }, { Glyph::Circle, leave } });
	else if (empty)
		hintBar(dl, { { Glyph::L1R1, "Tab" }, { Glyph::Square, scanHint }, { Glyph::Options, "Settings" },
				{ Glyph::Circle, leave } });
	else
		hintBar(dl, { { Glyph::L1R1, "Tab" }, { browse, "Browse" }, { Glyph::L2R2, "Letter" }, { Glyph::R3, "Search" },
				{ Glyph::Square, scanHint }, { Glyph::Options, "Settings" }, { Glyph::Circle, leave },
				{ Glyph::Triangle, "Details" }, { Glyph::Cross, "Play" } });
	endScreen();
}

// ------------------------------------------------------------ loading screen

// The game being loaded, what the loader (or, for a network game, the share)
// is doing, and how far it is. True when the user asks to cancel.
bool loading(const char *label, float progress, bool cancelling)
{
	ImDrawList *dl = beginScreen("##bp-loading", true);
	Game *game = nullptr;
	for (Game& g : games)
		if (!loadingPath.empty() && g.media.path == loadingPath)
			game = &g;
	const bool network = ps5::smb::isNetworkPath(loadingPath);
	const ps5::smb::Status share = network ? ps5::smb::status() : ps5::smb::Status{};

	std::string line = label != nullptr ? label : "";
	float done = std::clamp(progress, 0.f, 1.f);
	if (cancelling)
	{
		line = "Cancelling...";
		done = 0;
	}
	else if (!share.text.empty())
	{
		line = share.text;
		done = std::max(0.f, share.progress);
	}

	const float cover = 420;
	const float w = cover + 64 + 760, x = (W - w) / 2, y = 96 + (H - 72 - 96 - cover) / 2;
	// The cover grows into place over a light of its own; the words come in beside it.
	const float e = entered(300);
	const Layer coverLayer = beginLayer(dl);
	if (motion() != MotionOff)
	{
		const float breath = motion() == MotionFull ? 0.75f + 0.25f * std::sin((float)timeNow * 1.8f) : 1.f;
		softDisc(dl, x + cover / 2, y + cover / 2, cover * 0.98f, alpha(col::accent, 0.20f * breath), 48);
	}
	if (game != nullptr)
		drawCover(dl, *game, x, y, cover, cover, 18, 1);
	else
	{
		rect(dl, x, y, cover, cover, col::card, 18);
		spinMark(dl, x + cover / 2, y + cover / 2, cover * 0.2f);
	}
	endLayer(coverLayer, e, 0, 0, 0.88f + 0.12f * e, x + cover / 2, y + cover / 2);
	const Layer words = beginLayer(dl);
	const float tx = x + cover + 64, tw = w - cover - 64;
	const std::string title = !loadingTitle.empty() ? loadingTitle : "PSFlyCast";
	const std::string source = network ? std::string(ICON_FA_NETWORK_WIRED) + "   From the network share"
			+ (ps5::options().ramCache ? ", into memory" : ", streamed") : "";
	text(dl, bold(), 20, tx, y + 44, col::accent, cancelling ? "STOPPING" : "LOADING");
	const float titleH = text(dl, bold(), 56, tx, y + 78, col::text, title.c_str(), tw).y;
	float ly = y + 78 + std::min(titleH, 140.f) + 22;
	if (!source.empty())
	{
		text(dl, regular(), 22, tx, ly, col::faint, source.c_str());
		ly += 40;
	}
	// The bar: the part done, or a light going along it while there is none to show.
	const float by = y + cover - 96;
	text(dl, regular(), 26, tx, by - 46, col::dim, fit(regular(), 26, line, tw).c_str());
	rect(dl, tx, by, tw, 12, col::rgba(255, 255, 255, 30), 6);
	if (done > 0.001f)
		rect(dl, tx, by, std::max(12.f, tw * done), 12, col::accent, 6);
	else
	{
		const float span = tw * 0.22f;
		const float at = (float)std::fmod(timeNow * 0.7, 1.0) * (tw + span) - span;
		const float from = std::max(0.f, at), to = std::min(tw, at + span);
		if (to > from)
			rect(dl, tx + from, by, to - from, 12, alpha(col::accent, 0.85f), 6);
	}
	if (done > 0.001f)
	{
		char percent[16];
		snprintf(percent, sizeof(percent), "%d%%", (int)(done * 100));
		const ImVec2 ps = textSize(bold(), 22, percent);
		text(dl, bold(), 22, tx + tw - ps.x, by + 24, col::dim, percent);
	}
	endLayer(words, e, (1 - e) * 44, 0);

	topBar(dl, game != nullptr ? (int)game->source : lib.source, false, Reach::None);
	if (cancelling)
		hintBar(dl, {});
	else
		hintBar(dl, { { Glyph::Circle, "Cancel" } });
	const bool cancel = !cancelling && in.back;
	endScreen();
	return cancel;
}

// GuiState::NetworkStart: the game is loaded and waits for the other player
// (netplay) or the other cabinets (an arcade game's link). `status` is what
// Flycast's network code last said. Returns what the user asked for.
int networkStart(const std::string& status, bool canStartNow)
{
	ImDrawList *dl = beginScreen("##bp-network", true);
	Game *game = nullptr;
	for (Game& g : games)
		if (!loadingPath.empty() && g.media.path == loadingPath)
			game = &g;

	const bool netplay = config::GGPOEnable;
	const bool host = config::ActAsServer;
	const std::string own = ps5::net::localAddress();
	std::string kicker = "NETPLAY", first, second;
	if (netplay && host)
	{
		first = "You are player 1. Waiting for player 2";
		second = "This console: " + (own.empty() ? std::string("not connected") : own + ", UDP port " + std::to_string(NetplayPort));
	}
	else if (netplay)
	{
		first = "You are player 2. Looking for player 1";
		second = "Player 1: " + otherPlayerText();
	}
	else
	{
		kicker = "ARCADE LINK";
		first = host ? "Waiting for the other cabinets" : "Looking for the main cabinet";
		second = own.empty() ? "" : "This console: " + own;
	}
	if (netplay && config::NetworkServer.get().empty())
		second += "      The other player's address is not set (Settings > Online)";

	const float cover = 420;
	const float w = cover + 64 + 760, x = (W - w) / 2, y = 96 + (H - 72 - 96 - cover) / 2;
	const float e = entered(310);
	const Layer coverLayer = beginLayer(dl);
	if (motion() != MotionOff)
	{
		const float breath = motion() == MotionFull ? 0.75f + 0.25f * std::sin((float)timeNow * 1.8f) : 1.f;
		softDisc(dl, x + cover / 2, y + cover / 2, cover * 0.98f, alpha(col::accent, 0.20f * breath), 48);
	}
	if (game != nullptr)
		drawCover(dl, *game, x, y, cover, cover, 18, 1);
	else
	{
		rect(dl, x, y, cover, cover, col::card, 18);
		spinMark(dl, x + cover / 2, y + cover / 2, cover * 0.2f);
	}
	endLayer(coverLayer, e, 0, 0, 0.88f + 0.12f * e, x + cover / 2, y + cover / 2);
	const Layer words = beginLayer(dl);
	const float tx = x + cover + 64, tw = w - cover - 64;
	const std::string title = !loadingTitle.empty() ? loadingTitle : "PSFlyCast";
	text(dl, bold(), 20, tx, y + 44, col::accent, kicker.c_str());
	const float titleH = text(dl, bold(), 56, tx, y + 78, col::text, title.c_str(), tw).y;
	float ly = y + 78 + std::min(titleH, 140.f) + 22;
	text(dl, regular(), 26, tx, ly, col::text, fit(regular(), 26, first, tw).c_str());
	ly += 42;
	if (!second.empty())
		text(dl, regular(), 22, tx, ly, col::faint, second.c_str(), tw);
	// What the network code says, over a light going along the bar.
	const float by = y + cover - 96;
	text(dl, regular(), 26, tx, by - 46, col::dim, fit(regular(), 26, status.empty() ? "Starting the network..." : status, tw).c_str());
	rect(dl, tx, by, tw, 12, col::rgba(255, 255, 255, 30), 6);
	{
		const float span = tw * 0.22f;
		const float at = (float)std::fmod(timeNow * 0.7, 1.0) * (tw + span) - span;
		const float from = std::max(0.f, at), to = std::min(tw, at + span);
		if (to > from)
			rect(dl, tx + from, by, to - from, 12, alpha(col::accent, 0.85f), 6);
	}
	endLayer(words, e, (1 - e) * 44, 0);

	topBar(dl, game != nullptr ? (int)game->source : lib.source, false, Reach::None);
	if (canStartNow)
		hintBar(dl, { { Glyph::Options, "Start now" }, { Glyph::Circle, "Cancel" } });
	else
		hintBar(dl, { { Glyph::Circle, "Cancel" } });
	const int asked = in.back ? 1 : canStartNow && in.options ? 2 : 0;
	endScreen();
	return asked;
}

void loadCancelled()
{
	ps5::smb::cancelLoad();
}

void loadEnded()
{
	ps5::smb::endLoad();
}

std::string loadFailed(const char *why)
{
	std::string message = why != nullptr ? why : "";
	// For a network game, what the share said is the useful half.
	if (ps5::smb::isNetworkPath(loadingPath) && !ps5::smb::lastError().empty())
		message += (message.empty() ? "" : "\n\n") + std::string("The network share: ") + ps5::smb::lastError();
	ps5::smb::endLoad();
	return message;
}

// ------------------------------------------------------------------ settings

namespace
{

struct Row
{
	enum Kind { Toggle, Choice, Slider, Action, Info } kind;
	std::string label;
	std::string desc;
	std::vector<std::string> choices;	// Choice: the names
	std::vector<int> values;			// Choice: the values they stand for
	int minValue = 0, maxValue = 100, step = 5;	// Slider
	std::string unit;
	std::function<int()> get;
	std::function<void(int)> set;
	std::function<std::string()> info;	// Info: the value shown
	std::function<void()> action;
};

struct Category
{
	const char *icon;
	const char *name;
	std::vector<Row> rows;
};

template<typename T>
Row toggle(const char *label, const char *desc, T& opt)
{
	Row r{ Row::Toggle, label, desc };
	r.get = [&opt] { return opt.get() ? 1 : 0; };
	r.set = [&opt](int v) { opt.set(v != 0); };
	return r;
}

template<typename T>
Row choice(const char *label, const char *desc, std::vector<std::string> names, std::vector<int> values, T& opt)
{
	Row r{ Row::Choice, label, desc, std::move(names), std::move(values) };
	r.get = [&opt] { return (int)opt.get(); };
	r.set = [&opt](int v) { opt.set((std::decay_t<decltype(opt.get())>)v); };
	return r;
}

std::shared_ptr<GamepadDevice> pad1()
{
	for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++)
	{
		auto pad = GamepadDevice::GetGamepad(i);
		if (pad && pad->api_name() == "PS5")
			return pad;
	}
	return nullptr;
}

std::vector<Category> buildCategories()
{
	std::vector<Category> cats;

	// The options a game can also have of its own come from their table
	// (gameOptions), so that the Settings never lack one of them.
	auto addOptions = [](Category& cat, GameOption::Category which) {
		for (const GameOption& option : gameOptions())
		{
			if (option.category != which)
				continue;
			Row r{ option.isBool ? Row::Toggle : option.step > 0 ? Row::Slider : Row::Choice, option.label, option.desc };
			if (option.step > 0)
			{
				r.minValue = option.values.front();
				r.maxValue = option.values.back();
				r.step = option.step;
				r.unit = option.unit;
			}
			else if (!option.isBool)
			{
				r.choices.assign(option.names.begin(), option.names.end());
				r.values = option.values;
			}
			r.get = option.get;
			r.set = option.set;
			cat.rows.push_back(r);
		}
	};

	Category video{ ICON_FA_DISPLAY, "Video" };
	addOptions(video, GameOption::Video);
	{
		// The front end's own: the driver takes the mode, or not, when Flycast starts.
		Row r{ Row::Toggle, "120 Hz output",
				"On a TV that takes 4K at 120 Hz. From the next start; About says what the display runs at" };
		r.get = [] { return ps5::options().hz120 ? 1 : 0; };
		r.set = [](int v) { ps5::options().hz120 = v != 0; ps5::saveOptions(); };
		video.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Variable refresh rate",
				"Experimental, on a VRR display with 120 Hz output: a frame shows as soon as it is ready. Next start" };
		r.get = [] { return ps5::options().vrr ? 1 : 0; };
		r.set = [](int v) { ps5::options().vrr = v != 0; ps5::saveOptions(); };
		video.rows.push_back(r);
	}
	cats.push_back(video);

	Category audio{ ICON_FA_VOLUME_HIGH, "Audio" };
	addOptions(audio, GameOption::Audio);
	cats.push_back(audio);

	Category controls{ ICON_FA_GAMEPAD, "Controls" };
	addOptions(controls, GameOption::Controls);
	{
		Row r{ Row::Action, "Restore the default layout", "Console games: the standard layout. Arcade games: their button order" };
		r.action = [] {
			for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++)
				if (auto p = GamepadDevice::GetGamepad(i); p && p->api_name() == "PS5")
				{
					p->resetMappingToDefault(::settings.platform.isArcade(), true);
					p->save_mapping();
				}
			os_notify("Controller layout restored", 2000);
		};
		controls.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Controllers",
				"Another player joins from a second DualSense, with its PS button" };
		r.info = [] {
			const std::string usb = ps5::usb::text();
			return ps5::pad::portsText() + (usb.empty() ? "" : "; USB " + usb);
		};
		controls.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Light bar in the player's colour",
				"Blue for player 1, red for 2, green for 3, pink for 4. Off: the console's own colour" };
		r.get = [] { return ps5::options().lightBar ? 1 : 0; };
		r.set = [](int v) { ps5::options().lightBar = v != 0; ps5::saveOptions(); };
		controls.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "USB keyboard and mouse",
				"Plugged into the console, they are the Dreamcast's: in the ports after the controllers', from a game's start" };
		r.get = [] { return ps5::options().usbInput ? 1 : 0; };
		r.set = [](int v) { ps5::options().usbInput = v != 0; ps5::saveOptions(); };
		controls.rows.push_back(r);
	}
	const char *layout[][2] = {
		{ "Cross / Circle / Square / Triangle", "A / B / X / Y" },
		{ "L2 / R2", "Analog triggers" },
		{ "L1 / R1", "Z / C" },
		{ "OPTIONS", "Start" },
		{ "Left stick / Right stick", "Analog stick / second stick" },
		{ "L3 / R3 (arcade)", "Insert coin / Service" },
		{ "Touch pad", "Quick menu" },
	};
	for (const auto& l : layout)
	{
		Row r{ Row::Info, l[0], "" };
		const std::string v = l[1];
		r.info = [v] { return v; };
		controls.rows.push_back(r);
	}
	cats.push_back(controls);

	Category system{ ICON_FA_MICROCHIP, "System" };
	addOptions(system, GameOption::System);
	{
		Row r{ Row::Toggle, "Rewind",
				"Keeps a game's last three minutes: the quick menu's Rewind goes back into them. Experimental" };
		r.get = [] { return ps5::options().rewind ? 1 : 0; };
		r.set = [](int v) {
			ps5::options().rewind = v != 0;
			ps5::saveOptions();
			ps5::rewind::setEnabled(v != 0);
		};
		system.rows.push_back(r);
	}
	{
		Row r{ Row::Action, "Memory cards", "The saves on each card: copy one to another card, delete it, or take it to and from a file" };
		r.action = [] {
			// Flycast holds a loaded game's cards open: changed under it, its saves would be lost.
			if (game_started)
			{
				ps5::sound::cue(ps5::sound::Cue::Refuse);
				os_notify("Quit the game first", 4000, "Its memory cards are in use while it is loaded");
			}
			else
				openCards();
		};
		system.rows.push_back(r);
	}
	cats.push_back(system);

	// Playing with someone else: Flycast's netplay (GGPO), and how a game's
	// own online mode reaches the internet.
	Category online{ ICON_FA_GLOBE, "Online" };
	{
		Row r{ Row::Choice, "Netplay",
				"Two players in one game, each on a console or PC of their own. Every game then waits for the other",
				{ "Off", "Host: player 1", "Join: player 2" }, { 0, 1, 2 } };
		r.get = [] { return !config::GGPOEnable ? 0 : config::ActAsServer ? 1 : 2; };
		r.set = [](int v) {
			config::GGPOEnable.set(v != 0);
			if (v != 0)
			{
				// One kind of link at a time.
				config::ActAsServer.set(v == 1);
				config::NetworkEnable.set(false);
				config::BattleCableEnable.set(false);
			}
		};
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Action, "Other player", "Their address on the network. Each player enters the other's" };
		r.info = [] { return otherPlayerText(); };
		r.action = [] { openAddress(); };
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Slider, "Input delay", "Frames a press waits, to hide a slow connection: 0 to 2 at home" };
		r.minValue = 0; r.maxValue = 20; r.step = 1; r.unit = " frames";
		r.get = [] { return (int)config::GGPODelay.get(); };
		r.set = [](int v) { config::GGPODelay.set(v); };
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Choice, "Left stick in netplay",
				"Dreamcast games: how much of the left stick is sent to the other player. Both choose the same",
				{ "Not sent", "Left and right", "Every direction" }, { 0, 1, 2 } };
		r.get = [] { return (int)config::GGPOAnalogAxes.get(); };
		r.set = [](int v) { config::GGPOAnalogAxes.set(v); };
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "This console", "What the other player enters. From outside your home: your router's address, UDP port 19713" };
		r.info = [] {
			const std::string own = ps5::net::localAddress();
			return own.empty() ? std::string("Not connected") : own + ", UDP port " + std::to_string(NetplayPort);
		};
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Both players need",
				"A Dreamcast game also needs one save state on both, as <game>.state.net in data/savestates" };
		r.info = [] { return std::string("The same game file and BIOS"); };
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Open the router's port (UPnP)",
				"Asks the router to let the other player in. Off: forward UDP port 19713 to this console yourself" };
		r.get = [] { return config::EnableUPnP ? 1 : 0; };
		r.set = [](int v) { config::EnableUPnP.set(v != 0); };
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Action, "Name online", "The name a game's own online mode signs in with: typed on the console's keyboard" };
		r.info = [] { return config::ISPUsername.get().empty() ? std::string("The console's own") : config::ISPUsername.get(); };
		r.action = [] { openKeyboard(KeyboardName, "Name online", "Letters and numbers, no spaces", config::ISPUsername.get(), 28); };
		online.rows.push_back(r);
	}
	{
		Row r{ Row::Choice, "Dreamcast online",
				"How a game's own online mode connects. DCNet is Flycast's service for games' revived servers",
				{ "Modem, through DCNet", "Broadband adapter, through DCNet", "Modem, direct", "Broadband adapter, direct" },
				{ 0, 1, 2, 3 } };
		r.get = [] { return (config::UseDCNet ? 0 : 2) + (config::EmulateBBA ? 1 : 0); };
		r.set = [](int v) { config::UseDCNet.set(v < 2); config::EmulateBBA.set((v & 1) != 0); };
		online.rows.push_back(r);
	}
	cats.push_back(online);

	// RetroAchievements: Flycast's own support for the site (core/achievements).
	Category trophies{ ICON_FA_TROPHY, "Achievements" };
	{
		Row r{ Row::Toggle, "RetroAchievements",
				"Earns the achievements of retroachievements.org as you play. Needs an account on the site" };
		r.get = [] { return config::EnableAchievements ? 1 : 0; };
		r.set = [](int v) { config::EnableAchievements.set(v != 0); };
		trophies.rows.push_back(r);
	}
	{
		Row r{ Row::Action, "Account", "Signs in with the console's keyboard. Kept afterwards: a key from the site, not the password" };
		r.info = [] { return accountText(); };
		r.action = [] {
			if (config::EnableAchievements)
				openAccount();
			else
			{
				ps5::sound::cue(ps5::sound::Cue::Refuse);
				os_notify("Turn RetroAchievements on first", 3000);
			}
		};
		trophies.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Mode", "Save states, cheats, rewind and fast forward stay as they are. Hardcore mode is not offered" };
		r.info = [] { return std::string("Softcore"); };
		trophies.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "In a game", "An achievement earned is shown over the game as it happens" };
		r.info = [] { return std::string("The quick menu lists its achievements"); };
		trophies.rows.push_back(r);
	}
	cats.push_back(trophies);

	Category look{ ICON_FA_PALETTE, "Interface" };
	{
		auto option = [](const char *label, const char *desc, std::vector<std::string> names, int& value) {
			Row r{ Row::Choice, label, desc, std::move(names) };
			for (int i = 0; i < (int)r.choices.size(); i++)
				r.values.push_back(i);
			int *at = &value;
			r.get = [at] { return std::clamp(*at, 0, 64); };
			r.set = [at](int v) { *at = v; ps5::saveOptions(); };
			return r;
		};
		std::vector<std::string> skinNames, accentNames, backNames{ "The skin's own" };
		for (const Skin& one : skins)
			skinNames.push_back(one.name);
		for (const Accent& one : accents)
			accentNames.push_back(one.name);
		for (const char *name : backdropNames)
			backNames.push_back(name);
		look.rows.push_back(option("Skin", "The colours of every screen, and what moves behind them. Midnight is the first look",
				skinNames, ps5::options().skin));
		look.rows.push_back(option("Accent colour", "The colour of what is focused or switched on", accentNames, ps5::options().accent));
		look.rows.push_back(option("Background", "What is behind the screens: still, or alive in one of six ways",
				backNames, ps5::options().backdrop));
		look.rows.push_back(option("Motion",
				"All: living background, screens that slide, cards that rise. Less: fades only. None: everything is at once",
				{ "All", "Less", "None" }, ps5::options().motion));
		look.rows.push_back(option("Start-up animation",
				"Random: one of the four each time PSFlyCast starts, never the same twice running. Any button ends it",
				std::vector<std::string>(std::begin(splashChoices), std::end(splashChoices)), ps5::options().splash));
		Row sound{ Row::Toggle, "Start-up sound",
				"The sound that goes with it: sounds/startup.wav in the title's folder, or a WAV file of your own there" };
		sound.get = [] { return ps5::options().splashSound ? 1 : 0; };
		sound.set = [](int v) { ps5::options().splashSound = v != 0; ps5::saveOptions(); };
		look.rows.push_back(sound);
		Row menu{ Row::Toggle, "Menu sounds", "A soft note for moving, choosing, going back and changing tab" };
		menu.get = [] { return ps5::options().menuSounds ? 1 : 0; };
		menu.set = [](int v) { ps5::options().menuSounds = v != 0; ps5::saveOptions(); };
		look.rows.push_back(menu);
	}
	cats.push_back(look);

	Category storage{ ICON_FA_FOLDER_OPEN, "Library" };
	{
		Row r{ Row::Action, "Scan for games", "Looks for new games in the games folder, on USB drives and on the network share" };
		r.action = [] {
			scanner().refresh();
			ps5::rescanUsb();
			scannedCount = (size_t)-1;
			scanNetwork();
			os_notify("Scanning for games", 2000);
		};
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Choice, "Library view", "How the games are laid out: shelves, a compact grid, or a list",
			{ "Shelves", "Grid", "List" }, { 0, 1, 2 } };
		r.get = [] { return ps5::options().view; };
		r.set = [](int v) { ps5::options().view = v; ps5::saveOptions(); };
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Download covers and descriptions",
			"Box art from the libretro thumbnails collection; descriptions and dates from TheGamesDB" };
		r.get = [] { return ps5::options().covers ? 1 : 0; };
		r.set = [](int v) {
			ps5::options().covers = v != 0;
			ps5::saveOptions();
			config::FetchBoxart.set(v != 0);
		};
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "USB drives",
			"A USB tab for games in a flycast or dreamcast folder on a drive (needs elfldr; restart to apply)" };
		r.get = [] { return ps5::options().usb ? 1 : 0; };
		r.set = [](int v) { ps5::options().usb = v != 0; ps5::saveOptions(); };
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "USB game folders", "" };
		r.info = [] {
			if (!ps5::options().usb)
				return std::string("Off");
			if (!ps5::elevated)
				return std::string("No access (is elfldr running?)");
			if (ps5::usbDirs.empty())
				return std::string("None found");
			std::string all;
			for (const std::string& dir : ps5::usbDirs)
				all += (all.empty() ? "" : ", ") + dir;
			return all;
		};
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Group the discs of a game",
			"A game with several discs is one entry; it starts from the disc last played, and Details chooses another" };
		r.get = [] { return ps5::options().groupDiscs ? 1 : 0; };
		r.set = [](int v) { ps5::options().groupDiscs = v != 0; ps5::saveOptions(); };
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Hidden games", "A game is hidden from its details, under Manage, and is then in none of the lists" };
		r.info = [] {
			return hiddenGames == 0 ? std::string("None") : std::to_string(hiddenGames) + (hiddenGames == 1 ? " game" : " games");
		};
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Show hidden games",
			"They are listed again, dimmed, so that a game's details, under Manage, can bring it back" };
		r.get = [] { return ps5::options().showHidden ? 1 : 0; };
		r.set = [](int v) { ps5::options().showHidden = v != 0; ps5::saveOptions(); };
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Load network games into memory",
			"On: the whole game is read before it starts and the NAS can sleep. Off: it is streamed while you play" };
		r.get = [] { return ps5::options().ramCache ? 1 : 0; };
		r.set = [](int v) { ps5::options().ramCache = v != 0; ps5::saveOptions(); };
		storage.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Network folders", "SMB shares, named in network.cfg in PSFlyCast's folder" };
		r.info = [] {
			const auto& folders = ps5::smb::gameFolders();
			if (folders.empty())
				return std::string("None");
			std::string all;
			for (const std::string& folder : folders)
				all += (all.empty() ? "" : ", ") + folder.substr(6);
			return all;
		};
		storage.rows.push_back(r);
	}
	for (const auto& [label, sub] : std::vector<std::pair<std::string, std::string>>{
			{ "Games folder", "games/" }, { "BIOS folder", "bios/" }, { "Covers folder", "covers/" },
			{ "Cheats folder", "cheats/" }, { "Saves and states", "data/" },
			{ "Save files in and out", "vmu/" } })
	{
		Row r{ Row::Info, label, "" };
		const std::string path = shownRoot() + sub;
		r.info = [path] { return path; };
		storage.rows.push_back(r);
	}
	cats.push_back(storage);

	Category about{ ICON_FA_CIRCLE_INFO, "About" };
	{
		// Which build this is, and a newer release when one was found.
		Row r{ Row::Action, "PSFlyCast", "" };
		r.info = [] {
			using ps5::update::Phase;
			const ps5::update::Status update = ps5::update::status();
			std::string build = ps5::update::thisBuild();
			if (update.phase == Phase::Available)
				build += "      " ICON_FA_CIRCLE_ARROW_UP "  Update: " + update.latest;
			else if (update.phase == Phase::Installed)
				build += "      " ICON_FA_CIRCLE_CHECK "  " + update.latest + " starts next time";
			return build;
		};
		r.action = [] { openUpdate(ps5::update::status().phase == ps5::update::Phase::Idle); };
		about.rows.push_back(r);
	}
	{
		Row r{ Row::Action, "Check for updates",
				"Asks the project's GitHub page for a newer release, which PSFlyCast can then download and install" };
		r.action = [] { openUpdate(true); };
		about.rows.push_back(r);
	}
	{
		Row r{ Row::Toggle, "Look for updates at start-up", "A release asks GitHub once as it starts. Off: only when you ask, above" };
		r.get = [] { return ps5::options().updateCheck ? 1 : 0; };
		r.set = [](int v) { ps5::options().updateCheck = v != 0; ps5::saveOptions(); };
		about.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Graphics", "" };
		r.info = [] { return std::string("Vulkan (RADV on PS5_Vulkan)"); };
		about.rows.push_back(r);
	}
	{
		// What the output really runs at: 119.88 Hz when the TV does it, else 59.94.
		Row r{ Row::Info, "Display", "" };
		r.info = [] {
			char mode[64];
			snprintf(mode, sizeof(mode), "%d x %d at %.2f Hz", ::settings.display.width, ::settings.display.height,
					::settings.display.refreshRate);
			return std::string(mode) + (ps5::variableRefresh ? ", variable refresh" : "");
		};
		about.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Dreamcast BIOS", "" };
		// Looked for once, when the list is made: it does not change while it shows.
		const bool found = ps5::games::biosFound();
		r.info = [found] {
			return std::string(found ? "Found in the bios folder"
					: "Built-in (put dc_boot.bin and dc_flash.bin in the bios folder)");
		};
		about.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Storage", "" };
		r.info = [] { return shownRoot(); };
		about.rows.push_back(r);
	}
	// What is in this build, and whose it is: the part on the left, its
	// source on the right.
	for (const auto& [part, whose] : std::vector<std::pair<const char *, const char *>>{
			{ "Emulator", "Flycast, by flyinghead and contributors" },
			{ "Vulkan driver", "Mesa RADV: PS5_Vulkan and PS5_Mesa, by Mihawk-99" },
			{ "Title start-up and SDK", "ps5-payload-sdk, by John Tornblom, and Mihawk-99's fork of it" },
			{ "USB drive access", "ps5-native-app-boilerplate, by BlackBearReloaded" },
			{ "Keyboard, USB input, home sound", "After BlackBearReloaded's Prospero projects; ps5-at9-converter" },
			{ "Network shares", "libsmb2, by Ronnie Sahlberg" },
			{ "Build foundation", "PS5_VulkanTemplate, by Mihawk-99" },
			{ "Upscaling", "FidelityFX Super Resolution 1.0, by AMD" },
			{ "Software renderer", "REFSW, by skmp, from nullDC-rust (MIT)" },
			{ "Achievements", "rcheevos, by RetroAchievements.org (MIT)" },
			{ "Cheats", "libretro-database (CC BY-SA 4.0)" },
			{ "Game patches", "Flycast widescreen and 60 FPS chart, by nexus382 and contributors" },
			{ "Covers", "libretro-thumbnails" },
			{ "Descriptions", "TheGamesDB, through Flycast's own scraper" } })
	{
		Row r{ Row::Info, part, "" };
		const std::string credit = whose;
		r.info = [credit] { return credit; };
		about.rows.push_back(r);
	}
	{
		Row r{ Row::Action, "Quit PSFlyCast", "Closes the app" };
		r.action = [] { dc_exit(); };
		about.rows.push_back(r);
	}
	cats.push_back(about);
	return cats;
}

struct SettingsState
{
	std::vector<Category> cats;
	int cat = 0;
	int row = 0;
	bool inRows = false;
	float catAnim = 0, rowAnim = 0, scroll = 0;
	bool dirty = false;
	// The row highlight's place and height, on their way to the focused row's;
	// the category the rows were last drawn for, and when it changed.
	float rowY = 0, rowH = 76;
	int shownCat = -1;
	double catAt = 0;
} st;

std::string valueText(const Row& r)
{
	switch (r.kind)
	{
	case Row::Choice:
	{
		const int v = r.get();
		for (size_t i = 0; i < r.values.size(); i++)
			if (r.values[i] == v)
				return r.choices[i];
		return std::to_string(v);
	}
	case Row::Slider:
		return std::to_string(r.get()) + r.unit;
	case Row::Info:
		return r.info();
	default:
		return "";
	}
}

void change(Row& r, int dir)
{
	switch (r.kind)
	{
	case Row::Toggle:
		r.set(r.get() ? 0 : 1);
		break;
	case Row::Choice:
	{
		const int v = r.get();
		int idx = 0;
		for (size_t i = 0; i < r.values.size(); i++)
			if (r.values[i] == v)
				idx = (int)i;
		idx = std::clamp(idx + dir, 0, (int)r.values.size() - 1);
		r.set(r.values[idx]);
		break;
	}
	case Row::Slider:
		r.set(std::clamp(r.get() + dir * r.step, r.minValue, r.maxValue));
		break;
	default:
		return;
	}
	st.dirty = true;
}

void leaveSettings()
{
	if (st.dirty)
	{
		SaveSettings();
		st.dirty = false;
	}
	st.inRows = false;
	gui_setState(game_started ? GuiState::Commands : GuiState::Main);
}

} // namespace

void settings()
{
	if (st.cats.empty())
		st.cats = buildCategories();
	ImDrawList *dl = beginScreen("##bp-settings", true);

	// The console's keyboard takes the pad while it is up; what was typed goes
	// where it was asked for.
	{
		std::string typed;
		const int keyboard = keyboardState(typed);
		if (keyboard == KeyboardAddress)
			addressTyped(typed);
		else if (keyboard == KeyboardName)
		{
			// As Flycast keeps it: printable characters, no spaces.
			typed.erase(std::remove_if(typed.begin(), typed.end(), [](char c) { return c <= ' ' || c > '~'; }), typed.end());
			config::ISPUsername.set(typed);
			st.dirty = true;
		}
		else if (keyboard == KeyboardRaUser || keyboard == KeyboardRaPassword)
		{
			accountTyped(keyboard, typed);
			// A password is in the dialog's keeping only.
			std::fill(typed.begin(), typed.end(), '\0');
		}
		if (keyboard == -1)
			in = Input{};
	}
	// A dialog over the Settings takes the pad.
	const Input given = in;
	const bool dialog = updateUi.open || addressUi.open || cardsUi.open || accountUi.open;
	if (dialog)
		in = Input{};
	if (updateUi.open)
		updateDialog(ImGui::GetForegroundDrawList(), given);
	else if (addressUi.open)
	{
		addressDialog(ImGui::GetForegroundDrawList(), given);
		st.dirty = true;
	}
	else if (cardsUi.open)
		cardsDialog(ImGui::GetForegroundDrawList(), given);
	else if (accountUi.open)
	{
		accountDialog(ImGui::GetForegroundDrawList(), given);
		st.dirty = true;
	}

	Category& cat = st.cats[st.cat];
	// ---- input
	if (!st.inRows)
	{
		if (in.up && st.cat > 0) { st.cat--; st.row = 0; }
		if (in.down && st.cat < (int)st.cats.size() - 1) { st.cat++; st.row = 0; }
		if (in.accept || in.right) st.inRows = true;
		if (in.back)
			leaveSettings();
	}
	else
	{
		Row& r = cat.rows[st.row];
		if (in.up && st.row > 0) st.row--;
		if (in.down && st.row < (int)cat.rows.size() - 1) st.row++;
		if (in.left)
		{
			if (r.kind == Row::Choice || r.kind == Row::Slider || r.kind == Row::Toggle)
				change(r, -1);
			else
				st.inRows = false;
		}
		if (in.right)
			change(r, 1);
		if (in.accept)
		{
			if (r.kind == Row::Action && r.action)
				r.action();
			else
				change(r, 1);
		}
		if (in.back)
			st.inRows = false;
	}
	// OPTIONS, which opened the Settings, closes them from anywhere in them.
	if (in.options && gui_state == GuiState::Settings)
		leaveSettings();
	if (gui_state != GuiState::Settings)
	{
		endScreen();
		return;
	}

	// ---- categories
	const float listX = 96, listW = 440, top = 150;
	const float e = entered(200);
	const Layer content = beginLayer(dl);
	st.catAnim = motion() == MotionOff ? (float)st.cat : approach(st.catAnim, (float)st.cat, 18);
	text(dl, bold(), 54, listX, top - 10, col::text, "Settings");
	// The highlight slides from one category to the next.
	rect(dl, listX - 16, top + 90 + st.catAnim * 76, listW, 64, st.inRows ? col::panelHi : col::accent, 12);
	for (int i = 0; i < (int)st.cats.size(); i++)
	{
		const float y = top + 90 + i * 76;
		const bool on = i == st.cat;
		const std::string label = std::string(st.cats[i].icon) + "     " + st.cats[i].name;
		text(dl, bold(), 28, listX + 12, y + 16, on ? col::text : col::dim, label.c_str());
	}

	// ---- rows
	const float px = listX + listW + 48, pw = W - px - 96;
	const float rowsTop = top + 90;
	const float viewH = H - 72 - rowsTop - 24;
	rect(dl, px, rowsTop - 24, pw, viewH + 24, col::panel, 18);
	// Row heights: a description adds a line.
	std::vector<float> heights;
	float total = 0;
	for (const auto& r : cat.rows)
	{
		const float h = r.desc.empty() ? 76.f : 104.f;
		heights.push_back(h);
		total += h;
	}
	float focusY = 0;
	for (int i = 0; i < st.row; i++)
		focusY += heights[i];
	float target = std::clamp(focusY - viewH * 0.4f, 0.f, std::max(0.f, total - viewH + 24));
	st.scroll = approach(st.scroll, target, 14);
	dl->PushClipRect(V(px, rowsTop - 12), V(px + pw, rowsTop + viewH), true);
	// Another category's rows come up in place of the last one's.
	if (st.shownCat != st.cat)
	{
		st.shownCat = st.cat;
		st.catAt = timeNow;
		st.rowY = focusY;
	}
	const float rowsIn = motion() == MotionOff ? 1.f : easeOut((float)(timeNow - st.catAt) / 0.22f);
	const Layer rowsLayer = beginLayer(dl);
	float y = rowsTop - st.scroll;
	// The highlight slides from row to row.
	if (st.inRows && !cat.rows.empty())
	{
		const float h = heights[std::clamp(st.row, 0, (int)heights.size() - 1)];
		st.rowY = motion() == MotionOff ? focusY : approach(st.rowY, focusY, 20);
		st.rowH = motion() == MotionOff ? h : approach(st.rowH, h, 20);
		rect(dl, px + 16, y + st.rowY, pw - 32, st.rowH - 8, alpha(col::accent, 0.16f), 12);
		rect(dl, px + 16, y + st.rowY + 10, 6, st.rowH - 28, col::accent, 3);
	}
	for (int i = 0; i < (int)cat.rows.size(); i++)
	{
		const Row& r = cat.rows[i];
		const float h = heights[i];
		const bool on = st.inRows && i == st.row;
		const float tx = px + 52;
		text(dl, bold(), 28, tx, y + 16, r.kind == Row::Info ? col::dim : col::text, r.label.c_str());
		if (!r.desc.empty())
			text(dl, regular(), 22, tx, y + 56, col::faint, fit(regular(), 22, r.desc, pw * 0.74f).c_str());

		const float right = px + pw - 52;
		const float cy = y + (h - 8) / 2;
		switch (r.kind)
		{
		case Row::Toggle:
		{
			const bool v = r.get() != 0;
			const float sw = 84, sh = 44;
			rect(dl, right - sw, cy - sh / 2, sw, sh, v ? col::accent : col::rgba(255, 255, 255, 40), sh / 2);
			dl->AddCircleFilled(V(right - sw + (v ? sw - sh / 2 : sh / 2), cy), (sh / 2 - 5) * S, col::text);
			break;
		}
		case Row::Choice:
		case Row::Slider:
		{
			const std::string v = valueText(r);
			const ImVec2 vs = textSize(bold(), 26, v.c_str());
			float x = right - vs.x;
			if (r.kind == Row::Slider)
			{
				const float bw = 260;
				const float frac = (float)(r.get() - r.minValue) / std::max(1, r.maxValue - r.minValue);
				x = right - bw;
				rect(dl, x, cy - 4, bw, 8, col::rgba(255, 255, 255, 40), 4);
				rect(dl, x, cy - 4, bw * frac, 8, col::accent, 4);
				dl->AddCircleFilled(V(x + bw * frac, cy), 13 * S, col::text);
				x -= vs.x + 28;
			}
			text(dl, bold(), 26, x, cy - vs.y / 2, on ? col::text : col::dim, v.c_str());
			if (on && r.kind == Row::Choice)
			{
				const ImU32 a = col::accent;
				dl->AddTriangleFilled(V(x - 30, cy), V(x - 18, cy - 10), V(x - 18, cy + 10), a);
				dl->AddTriangleFilled(V(right + 30, cy), V(right + 18, cy - 10), V(right + 18, cy + 10), a);
			}
			break;
		}
		case Row::Info:
		{
			const std::string v = fit(regular(), 24, valueText(r), pw * 0.55f);
			const ImVec2 vs = textSize(regular(), 24, v.c_str());
			text(dl, regular(), 24, right - vs.x, cy - vs.y / 2, col::text, v.c_str());
			break;
		}
		case Row::Action:
		{
			const char *s = ICON_FA_CHEVRON_RIGHT;
			const ImVec2 vs = textSize(regular(), 24, s);
			text(dl, regular(), 24, right - vs.x, cy - vs.y / 2, on ? col::accent : col::faint, s);
			if (r.info)
			{
				// What it is now, before the arrow.
				const std::string v = fit(regular(), 24, r.info(), pw * 0.55f);
				const ImVec2 is = textSize(regular(), 24, v.c_str());
				text(dl, regular(), 24, right - vs.x - 22 - is.x, cy - is.y / 2, on ? col::text : col::dim, v.c_str());
			}
			break;
		}
		}
		if (i + 1 < (int)cat.rows.size() && !on && !(st.inRows && i + 1 == st.row))
			rect(dl, px + 40, y + h - 4, pw - 80, 1, col::line);
		y += h;
	}
	endLayer(rowsLayer, rowsIn, 0, (1 - rowsIn) * 16);
	dl->PopClipRect();
	endEntrance(content, e);

	// Over the library's tab, which stays lit: the Settings are not a tab.
	topBar(dl, lib.source, game_started, Reach::None);
	if (!st.inRows)
		hintBar(dl, { { Glyph::DPadUD, "Category" }, { Glyph::Cross, "Select" }, { Glyph::Options, "Close" },
				{ Glyph::Circle, "Back" } });
	else
	{
		const Row& r = cat.rows[st.row];
		if (r.kind == Row::Action)
			hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::Cross, "Run" }, { Glyph::Circle, "Back" } });
		else if (r.kind == Row::Info)
			hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::Circle, "Back" } });
		else
			hintBar(dl, { { Glyph::DPadUD, "Move" }, { Glyph::DPadLR, "Change" }, { Glyph::Circle, "Back" } });
	}
	endScreen();
}

// ---------------------------------------------------------------- quick menu

namespace
{

struct QuickItem
{
	const char *icon;
	std::string label;
	std::string detail;
	bool enabled = true;
	std::function<void()> run;
	std::function<void(int)> side;	// left/right
	bool stays = false;				// running it keeps the menu open
};

struct QuickState
{
	int focus = 0;
	float open = 0;
	float focusAnim = 0;
	// The Cheats page.
	bool cheats = false;
	float cheatsAnim = 0;
	int cheatFocus = 0;				// 0 is the file row
	float cheatScroll = 0;
	std::vector<std::string> cheatFiles;	// best match first
	int cheatFile = -1;				// index in cheatFiles, -1 for none
	// The Game options page.
	bool options = false;
	float optionsAnim = 0;
	int optionFocus = 0;
	float optionScroll = 0;
	bool optionsDirty = false;
	// The Controls page.
	bool controls = false;
	float controlsAnim = 0;
	int controlFocus = 0;
	float controlScroll = 0;
	int listening = -1;				// the row waiting for a button; -1 for none
	bool listenArmed = false;		// every button was let go since it began to wait
	double listenAt = 0;
	// "Restart game" was chosen once: until then, choosing it again restarts.
	double restartAsked = -10;
	// The Rewind page: the moments there were when it was opened.
	bool rewind = false;
	float rewindAnim = 0;
	int rewindFocus = 0;
	float rewindScroll = 0;
	std::vector<ps5::rewind::Point> rewindPoints;
	std::string rewindNote;
	// The Achievements page: the game's, asked for once when it is opened
	// (each asking has the pictures still missing downloaded).
	bool trophies = false;
	float trophiesAnim = 0;
	int trophyFocus = 0;
	float trophyScroll = 0;
	achievements::Game trophyGame;
	std::vector<achievements::Achievement> trophyList;
	// What the menu's own row says, asked for once as the menu opens.
	bool trophiesActive = false;
	std::string trophiesDetail;
} qm;

void openCheats()
{
	qm.cheats = true;
	qm.cheatFocus = cheatManager.cheatCount() != 0 ? 1 : 0;
	qm.cheatScroll = 0;
	qm.cheatFiles = ps5::cheats::candidates(::settings.content.fileName);
	const std::string current = ps5::cheats::current();
	qm.cheatFile = -1;
	for (size_t i = 0; i < qm.cheatFiles.size(); i++)
		if (qm.cheatFiles[i] == current)
			qm.cheatFile = (int)i;
}

std::string cheatFileLabel()
{
	if (qm.cheatFile < 0 || qm.cheatFile >= (int)qm.cheatFiles.size())
		return "None";
	const std::string& file = qm.cheatFiles[qm.cheatFile];
	return file.substr(0, file.size() - 4);
}

// The Cheats page, in the quick menu's panel: the cheat file on top (left and
// right change it, best match first), then each cheat with its switch.
void cheatsPage(ImDrawList *dl, float px, float pw, float top)
{
	const int rows = 1 + (int)cheatManager.cheatCount();
	if (in.up && qm.cheatFocus > 0) qm.cheatFocus--;
	if (in.down && qm.cheatFocus < rows - 1) qm.cheatFocus++;
	if (in.l2) qm.cheatFocus = std::max(0, qm.cheatFocus - 8);
	if (in.r2) qm.cheatFocus = std::min(rows - 1, qm.cheatFocus + 8);
	if (qm.cheatFocus == 0)
	{
		int next = qm.cheatFile;
		if (in.left) next--;
		if (in.right || in.accept) next++;
		next = std::clamp(next, -1, (int)qm.cheatFiles.size() - 1);
		if (next != qm.cheatFile)
		{
			qm.cheatFile = next;
			ps5::cheats::select(next < 0 ? "" : qm.cheatFiles[next]);
		}
	}
	else if (in.accept || in.left || in.right)
	{
		const size_t index = (size_t)qm.cheatFocus - 1;
		if (index < cheatManager.cheatCount())
			cheatManager.enableCheat(index, !cheatManager.cheatEnabled(index));
	}
	if (in.back || in.options)
	{
		ps5::cheats::saveStates();
		qm.cheats = false;
	}
	qm.cheatFocus = std::clamp(qm.cheatFocus, 0, std::max(0, (int)cheatManager.cheatCount()));

	text(dl, bold(), 34, px + 48, top - 62, col::text, ICON_FA_BOLT "   Cheats");
	const float rowH = 66;
	const float viewH = H - 96 - top;
	const float visible = viewH / rowH;
	const float target = std::clamp((float)qm.cheatFocus - (visible - 1) / 2, 0.f, std::max(0.f, (float)rows - visible));
	qm.cheatScroll = approach(qm.cheatScroll, target, 18);
	dl->PushClipRect(V(px, top), V(px + pw, top + viewH), true);
	for (int i = 0; i < rows; i++)
	{
		const float y = top + (i - qm.cheatScroll) * rowH;
		if (y > H || y + rowH < top - rowH)
			continue;
		const bool on = i == qm.cheatFocus;
		if (on)
			rect(dl, px + 32, y + 2, pw - 64, rowH - 6, col::accent, 12);
		const float right = px + pw - 60;
		const float cy = y + rowH / 2 - 1;
		if (i == 0)
		{
			text(dl, bold(), 26, px + 56, y + 16, on ? col::text : col::dim, "Cheat file");
			const std::string label = fit(regular(), 24, cheatFileLabel(), pw - 330);
			const ImVec2 ls = textSize(regular(), 24, label.c_str());
			text(dl, regular(), 24, right - 22 - ls.x, cy - ls.y / 2, on ? col::text : col::faint, label.c_str());
			if (on)
			{
				dl->AddTriangleFilled(V(right - 42 - ls.x, cy), V(right - 30 - ls.x, cy - 10), V(right - 30 - ls.x, cy + 10), col::text);
				dl->AddTriangleFilled(V(right, cy), V(right - 12, cy - 10), V(right - 12, cy + 10), col::text);
			}
			continue;
		}
		const size_t index = (size_t)i - 1;
		const bool enabled = cheatManager.cheatEnabled(index);
		text(dl, regular(), 24, px + 56, y + 17, on ? col::text : enabled ? col::text : col::dim,
				fit(regular(), 24, cheatManager.cheatDescription(index), pw - 240).c_str());
		const float sw = 68, sh = 36;
		rect(dl, right - sw, cy - sh / 2, sw, sh, enabled ? (on ? col::text : col::accent) : col::rgba(255, 255, 255, 40), sh / 2);
		dl->AddCircleFilled(V(right - sw + (enabled ? sw - sh / 2 : sh / 2), cy), (sh / 2 - 5) * S,
				enabled && on ? col::accent : col::text);
	}
	dl->PopClipRect();
	if (cheatManager.cheatCount() == 0)
		text(dl, regular(), 22, px + 56, top + rowH + 24, col::faint,
				qm.cheatFiles.empty() ? "No cheat files in the cheats folder."
				: "No cheats loaded. Choose this game's file above:\nthe closest names come first.", pw - 112);
}

// How many options the running game has of its own.
int ownOptions()
{
	int own = 0;
	for (const GameOption& option : optionsFor(::settings.content.fileName, false))
		own += option.get() != settingsValue(option) ? 1 : 0;
	return own;
}

void openGameOptions()
{
	qm.options = true;
	qm.optionFocus = 0;
	qm.optionScroll = 0;
	qm.optionsDirty = false;
	// From here on what is saved is saved for this game: each option that
	// differs from the Settings goes to the game's section of emu.cfg, and
	// one that is the same again leaves it.
	config::Settings::instance().setPerGameConfig(true);
}

// The Game options page, in the quick menu's panel: the options of the
// running game. A change is in effect when the game resumes (a few when it is
// next started) and is kept for this game only; the Settings are not touched.
void optionsPage(ImDrawList *dl, float px, float pw, float top)
{
	const std::vector<GameOption> options = optionsFor(::settings.content.fileName, false);
	const int count = (int)options.size();
	const int rows = count + 1;		// and "As in Settings, all of them"
	if (in.up && qm.optionFocus > 0) qm.optionFocus--;
	if (in.down && qm.optionFocus < rows - 1) qm.optionFocus++;
	if (in.l2) qm.optionFocus = std::max(0, qm.optionFocus - 8);
	if (in.r2) qm.optionFocus = std::min(rows - 1, qm.optionFocus + 8);
	if (qm.optionFocus < count)
	{
		const GameOption& option = options[qm.optionFocus];
		const int choice = choiceOf(option, option.get());
		const int last = (int)option.values.size() - 1;
		int next = choice;
		if (in.left && choice > 0) next = choice - 1;
		else if (in.right && choice < last) next = choice + 1;
		else if (in.accept) next = choice >= last ? 0 : choice + 1;
		if (next != choice)
		{
			option.set(option.values[next]);
			qm.optionsDirty = true;
		}
		else if (in.square && option.get() != settingsValue(option))
		{
			option.set(settingsValue(option));
			qm.optionsDirty = true;
		}
	}
	else if (in.accept)
	{
		for (const GameOption& option : options)
			option.set(settingsValue(option));
		qm.optionsDirty = true;
	}
	if (in.back || in.options)
	{
		if (qm.optionsDirty)
			SaveSettings();
		qm.optionsDirty = false;
		qm.options = false;
	}

	text(dl, bold(), 34, px + 48, top - 62, col::text, ICON_FA_SLIDERS "   Game options");
	{
		const char *scope = "For this game only";
		const ImVec2 ss = textSize(regular(), 22, scope);
		text(dl, regular(), 22, px + pw - 60 - ss.x, top - 52, col::faint, scope);
	}
	const float rowH = 62;
	const float noteH = 92;			// the focused option, described, under the list
	const float viewH = H - 96 - noteH - top;
	const float visible = viewH / rowH;
	const float target = std::clamp((float)qm.optionFocus - (visible - 1) / 2, 0.f, std::max(0.f, (float)rows - visible));
	qm.optionScroll = approach(qm.optionScroll, target, 18);
	dl->PushClipRect(V(px, top), V(px + pw, top + viewH), true);
	for (int i = 0; i < rows; i++)
	{
		const float y = top + (i - qm.optionScroll) * rowH;
		if (y > top + viewH || y + rowH < top - rowH)
			continue;
		const bool on = i == qm.optionFocus;
		if (on)
			rect(dl, px + 32, y + 2, pw - 64, rowH - 6, col::accent, 12);
		const float right = px + pw - 60;
		const float cy = y + rowH / 2 - 1;
		if (i == count)
		{
			text(dl, bold(), 24, px + 72, y + 16, on ? col::text : col::dim, ICON_FA_ROTATE_LEFT "   As in Settings, all of them");
			continue;
		}
		const GameOption& option = options[i];
		const int value = option.get();
		const bool own = value != settingsValue(option);
		// A dot marks an option this game has of its own.
		if (own)
			dl->AddCircleFilled(V(px + 54, cy), 5 * S, on ? col::text : col::accent);
		text(dl, bold(), 24, px + 72, y + 16, on || own ? col::text : col::dim, option.label);
		const int choice = choiceOf(option, value);
		const std::string label = fit(regular(), 24, choice < 0 ? oddValue(option, value) : option.names[choice], pw * 0.42f);
		const ImVec2 ls = textSize(regular(), 24, label.c_str());
		text(dl, regular(), 24, right - 22 - ls.x, cy - ls.y / 2, on || own ? col::text : col::faint, label.c_str());
		if (on)
		{
			dl->AddTriangleFilled(V(right - 42 - ls.x, cy), V(right - 30 - ls.x, cy - 10), V(right - 30 - ls.x, cy + 10), col::text);
			dl->AddTriangleFilled(V(right, cy), V(right - 12, cy - 10), V(right - 12, cy + 10), col::text);
		}
	}
	dl->PopClipRect();
	if (qm.optionFocus < count)
	{
		const GameOption& option = options[qm.optionFocus];
		std::string note = option.desc;
		if (option.nextStart)
			note += ".\nTakes effect the next time this game starts.";
		else if (option.key == nullptr)
			note += ".\nThe controller's own: the same for every game.";
		if (option.patch != nullptr)
			note = std::string(option.desc) + ".\nFrom the community chart's " + ps5::patches::label(::settings.content.fileName, option.patch)
					+ "; from the next start of the game.";
		text(dl, regular(), 20, px + 56, top + viewH + 14, option.nextStart ? col::dim : col::faint, note.c_str(), pw - 112);
	}
	else
		text(dl, regular(), 20, px + 56, top + viewH + 14, col::faint,
				"This game goes back to the Settings for every option here.", pw - 112);
}

// ---- the Controls page: which DualSense button is which Dreamcast one
//
// For the running game. The layout every game has is the pad's (Settings >
// Controls restores it); a button changed here makes a layout of the game's
// own, Flycast's per-game mapping, kept beside the pad's and loaded whenever
// the game starts. The touch pad stays the quick menu's and the sticks stay
// the sticks.

struct PadControl
{
	const char *console;	// what the control is on a Dreamcast pad
	const char *arcade;		// and on an arcade board (null: not there)
	DreamcastKey key;
	// The emulator's own, not the Dreamcast's: on no button until it is given
	// one, and Square takes it off again.
	bool emulator = false;
};

const PadControl padControls[] = {
	{ "A", "Button 1", DC_BTN_A }, { "B", "Button 2", DC_BTN_B }, { "X", "Button 4", DC_BTN_X },
	{ "Y", "Button 5", DC_BTN_Y }, { "Left trigger", "Left trigger", DC_AXIS_LT },
	{ "Right trigger", "Right trigger", DC_AXIS_RT }, { "Start", "Start", DC_BTN_START },
	{ "D-pad up", "Up", DC_DPAD_UP }, { "D-pad down", "Down", DC_DPAD_DOWN },
	{ "D-pad left", "Left", DC_DPAD_LEFT }, { "D-pad right", "Right", DC_DPAD_RIGHT },
	{ "C (six-button pad)", "Button 3", DC_BTN_C }, { "Z (six-button pad)", "Button 6", DC_BTN_Z },
	{ nullptr, "Insert coin", DC_BTN_D }, { nullptr, "Service", DC_DPAD2_UP },
	// Flycast's own (core/input/gamepad_device.cpp): a press turns fast forward
	// on and the next one off. (Its screenshot control is not offered: the
	// console's Create button takes them.)
	{ "Fast forward", "Fast forward", EMU_BTN_FFORWARD, true },
};

std::vector<PadControl> controlsShown()
{
	std::vector<PadControl> rows;
	const bool arcade = ::settings.platform.isArcade();
	for (const PadControl& control : padControls)
		if ((arcade ? control.arcade : control.console) != nullptr)
			rows.push_back(control);
	if (arcade)
		// In the order of the buttons' numbers.
		std::stable_sort(rows.begin(), rows.end(), [](const PadControl& a, const PadControl& b) {
			const bool an = !strncmp(a.arcade, "Button", 6), bn = !strncmp(b.arcade, "Button", 6);
			return an != bn ? an : an && strcmp(a.arcade, b.arcade) < 0;
		});
	return rows;
}

// The DualSense button a control is on.
std::string boundTo(GamepadDevice& pad, DreamcastKey key)
{
	const std::shared_ptr<InputMapping> mapping = pad.get_input_mapping();
	if (mapping == nullptr)
		return "";
	const u32 button = mapping->get_button_code(0, key);
	if (button != InputMapping::INVALID_CODE)
	{
		const char *name = pad.get_button_name(button);
		return name != nullptr ? name : "Button " + std::to_string(button);
	}
	const u32 axis = mapping->get_axis_code(0, key).first;
	if (axis != InputMapping::INVALID_CODE)
	{
		const char *name = pad.get_axis_name(axis);
		return name != nullptr ? name : "Axis " + std::to_string(axis);
	}
	return "Not set";
}

// Puts a control on a DualSense button, in the game's own layout (made now if
// the game had none). What the button did before, it no longer does.
void bindControl(GamepadDevice& pad, DreamcastKey key, u32 bit)
{
	if (!pad.isPerGameMapping())
		pad.setPerGameMapping(true);
	const std::shared_ptr<InputMapping> mapping = pad.get_input_mapping();
	if (mapping == nullptr)
		return;
	const bool onTrigger = bit == ps5::pad::L2 || bit == ps5::pad::R2;
	const u32 axis = bit == ps5::pad::L2 ? ps5::pad::AxisCodeL2 : ps5::pad::AxisCodeR2;
	if (onTrigger)
	{
		// L2 and R2 are an axis as well as a button: whatever was on the axis leaves it.
		const DreamcastKey before = mapping->get_axis_id(0, axis, true);
		if (before != EMU_AXIS_NONE)
			pad.clearAxisMapping(0, before);
	}
	// The button (this also takes it from the control that had it).
	mapping->set_button(0, key, ps5::pad::buttonCode(bit));
	pad.clearAxisMapping(0, key);
	if (onTrigger && (key == DC_AXIS_LT || key == DC_AXIS_RT))
	{
		// A Dreamcast trigger on L2 or R2 is pressed as far as they are.
		pad.clearButtonMapping(0, key);
		mapping->set_axis(0, key, axis, true);
	}
	mapping->set_dirty();
	pad.save_mapping();
}

// Takes a control off its button, in the game's own layout.
void unbindControl(GamepadDevice& pad, DreamcastKey key)
{
	if (!pad.isPerGameMapping())
		pad.setPerGameMapping(true);
	const std::shared_ptr<InputMapping> mapping = pad.get_input_mapping();
	if (mapping == nullptr)
		return;
	pad.clearButtonMapping(0, key);
	pad.clearAxisMapping(0, key);
	mapping->set_dirty();
	pad.save_mapping();
}

void openControls()
{
	qm.controls = true;
	qm.controlFocus = 0;
	qm.controlScroll = 0;
	qm.listening = -1;
}

void controlsPage(ImDrawList *dl, float px, float pw, float top)
{
	const std::shared_ptr<GamepadDevice> pad = pad1();
	const std::vector<PadControl> controls = controlsShown();
	const bool arcade = ::settings.platform.isArcade();
	const int count = (int)controls.size();
	const int rows = count + 1;		// the first row: whose layout this is
	const bool own = pad != nullptr && pad->isPerGameMapping();
	// The focused row is one of the emulator's own controls, and is on a button.
	const bool removable = pad != nullptr && qm.listening < 0 && qm.controlFocus > 0 && qm.controlFocus <= count
			&& controls[qm.controlFocus - 1].emulator && boundTo(*pad, controls[qm.controlFocus - 1].key) != "Not set";

	if (qm.listening >= 0)
	{
		// Waiting for the button: the first one pressed once all were let go.
		const ps5::pad::State& state = ps5::pad::player1();
		const u32 held = state.buttons & ~ps5::pad::Intercepted;
		if (!qm.listenArmed)
			qm.listenArmed = held == 0;
		else if (state.pressed & ps5::pad::TouchPad)
			qm.listening = -1;
		else if (const u32 pressed = state.pressed & ~(ps5::pad::Intercepted | ps5::pad::TouchPad); pressed != 0 && pad != nullptr)
		{
			bindControl(*pad, controls[qm.listening - 1].key, pressed & (0u - pressed));
			qm.listening = -1;
		}
		if (qm.listening >= 0 && timeNow > qm.listenAt + 8)
			qm.listening = -1;
	}
	else
	{
		if (in.up && qm.controlFocus > 0) qm.controlFocus--;
		if (in.down && qm.controlFocus < rows - 1) qm.controlFocus++;
		if (in.l2) qm.controlFocus = std::max(0, qm.controlFocus - 8);
		if (in.r2) qm.controlFocus = std::min(rows - 1, qm.controlFocus + 8);
		if (pad != nullptr && qm.controlFocus == 0 && own && (in.accept || in.left || in.right || in.square))
		{
			// Back to the layout every game has: the game's own is deleted.
			pad->setPerGameMapping(false);
			GamepadDevice::load_system_mappings();
		}
		else if (pad != nullptr && qm.controlFocus > 0 && in.accept)
		{
			qm.listening = qm.controlFocus;
			qm.listenArmed = false;
			qm.listenAt = timeNow;
		}
		else if (removable && in.square)
			unbindControl(*pad, controls[qm.controlFocus - 1].key);
		else if (in.back || in.options)
			qm.controls = false;
	}

	text(dl, bold(), 34, px + 48, top - 62, col::text, ICON_FA_GAMEPAD "   Controls");
	{
		const char *scope = "For this game only";
		const ImVec2 ss = textSize(regular(), 22, scope);
		text(dl, regular(), 22, px + pw - 60 - ss.x, top - 52, col::faint, scope);
	}
	const float rowH = 62, noteH = 92;
	const float viewH = H - 96 - noteH - top;
	const float visible = viewH / rowH;
	const float target = std::clamp((float)qm.controlFocus - (visible - 1) / 2, 0.f, std::max(0.f, (float)rows - visible));
	qm.controlScroll = approach(qm.controlScroll, target, 18);
	dl->PushClipRect(V(px, top), V(px + pw, top + viewH), true);
	for (int i = 0; i < rows; i++)
	{
		const float y = top + (i - qm.controlScroll) * rowH;
		if (y > top + viewH || y + rowH < top - rowH)
			continue;
		const bool on = i == qm.controlFocus;
		const bool waiting = i == qm.listening;
		if (on)
			rect(dl, px + 32, y + 2, pw - 64, rowH - 6, waiting ? col::warm : col::accent, 12);
		const float right = px + pw - 60, cy = y + rowH / 2 - 1;
		std::string label, value;
		if (i == 0)
		{
			label = "Layout";
			value = own ? "This game's own" : "As for every game";
		}
		else
		{
			const PadControl& control = controls[i - 1];
			label = arcade ? control.arcade : control.console;
			value = waiting ? "Press a button..." : pad != nullptr ? boundTo(*pad, control.key) : "No controller";
		}
		if (i == 0 && own)
			dl->AddCircleFilled(V(px + 54, cy), 5 * S, on ? col::text : col::accent);
		text(dl, bold(), 24, px + 72, y + 16, on || (i == 0 && own) ? col::text : col::dim, label.c_str());
		const ImVec2 vs = textSize(regular(), 24, value.c_str());
		const float pulse = waiting ? 0.6f + 0.4f * std::sin((float)timeNow * 6.f) : 1.f;
		// A Dreamcast control on no button is amiss; one of the emulator's own is not.
		const bool amiss = value == "Not set" && !(i > 0 && controls[i - 1].emulator);
		text(dl, regular(), 24, right - vs.x, cy - vs.y / 2,
				alpha(on ? col::text : amiss ? col::warm : col::faint, pulse), value.c_str());
	}
	dl->PopClipRect();
	const bool emulators = qm.controlFocus > 0 && qm.controlFocus <= count && controls[qm.controlFocus - 1].emulator;
	const char *note = qm.listening >= 0 ? "Press the DualSense button for it. The touch pad leaves it as it is."
			: qm.controlFocus == 0 ? (own ? "This game has a layout of its own. Cross puts it back to the one every game has."
					: "This game uses the layout every game has. Changing a button below gives it one of its own.")
			: emulators ? "A press turns fast forward on, the next one off: not in netplay. Cross, then the button for it;\n"
					"Square takes it off its button. L3 and R3 are free in Dreamcast games."
			: "Cross, then the DualSense button for it. A button that did something else stops doing it.\n"
					"L2 and R2 are analog for the triggers. The sticks and the touch pad stay as they are.";
	text(dl, regular(), 20, px + 56, top + viewH + 14, col::faint, note, pw - 112);
}

// ---- the Rewind page: the moments of the last minutes, the newest first

std::string agoText(double seconds)
{
	const int whole = std::max(1, (int)std::lround(seconds));
	if (whole < 60)
		return std::to_string(whole) + (whole == 1 ? " second ago" : " seconds ago");
	const int minutes = whole / 60, rest = whole % 60;
	return std::to_string(minutes) + " min" + (rest == 0 ? "" : " " + std::to_string(rest) + " s") + " ago";
}

void openRewind()
{
	qm.rewind = true;
	qm.rewindFocus = 0;
	qm.rewindScroll = 0;
	qm.rewindNote.clear();
	// The emulator is stopped while the menu is up: the list stays as it is.
	qm.rewindPoints = ps5::rewind::points();
}

// True when the game went back and the menu closes.
bool rewindPage(ImDrawList *dl, float px, float pw, float top)
{
	const int rows = (int)qm.rewindPoints.size();
	bool went = false;
	if (in.up && qm.rewindFocus > 0) qm.rewindFocus--;
	if (in.down && qm.rewindFocus < rows - 1) qm.rewindFocus++;
	if (in.l2) qm.rewindFocus = std::max(0, qm.rewindFocus - 8);
	if (in.r2) qm.rewindFocus = std::min(std::max(0, rows - 1), qm.rewindFocus + 8);
	if (in.accept && rows > 0)
	{
		using ps5::rewind::Result;
		switch (ps5::rewind::restore((size_t)qm.rewindFocus))
		{
		case Result::Ok:
			went = true;
			break;
		case Result::NoMemory:
			qm.rewindNote = "Not enough memory to go back now.";
			break;
		case Result::LoadFailed:
			// As a state that did not load: the machine is part one moment and part another.
			qm.rewindNote = "Going back failed, and the game may no longer run right: load a state, or restart it.";
			qm.rewindPoints.clear();
			break;
		case Result::Unavailable:
			qm.rewindNote = "This game cannot be rewound now.";
			break;
		case Result::Corrupt:
			qm.rewindNote = "That moment could not be read back. Nothing was changed.";
			break;
		case Result::EmulatorRunning:
			qm.rewindNote = "The game was not stopped. Nothing was changed.";
			break;
		default:
			qm.rewindNote = "That moment is no longer kept.";
			qm.rewindPoints = ps5::rewind::points();
			break;
		}
		if (!went)
			ps5::sound::cue(ps5::sound::Cue::Refuse);
		qm.rewindFocus = std::clamp(qm.rewindFocus, 0, std::max(0, (int)qm.rewindPoints.size() - 1));
	}
	if (in.back || in.options)
		qm.rewind = false;

	text(dl, bold(), 34, px + 48, top - 62, col::text, ICON_FA_BACKWARD "   Rewind");
	const float rowH = 62;
	const float noteH = 92;
	const float viewH = H - 96 - noteH - top;
	const float visible = viewH / rowH;
	const int count = (int)qm.rewindPoints.size();
	const float target = std::clamp((float)qm.rewindFocus - (visible - 1) / 2, 0.f, std::max(0.f, (float)count - visible));
	qm.rewindScroll = approach(qm.rewindScroll, target, 18);
	dl->PushClipRect(V(px, top), V(px + pw, top + viewH), true);
	for (int i = 0; i < count; i++)
	{
		const float y = top + (i - qm.rewindScroll) * rowH;
		if (y > top + viewH || y + rowH < top - rowH)
			continue;
		const bool on = i == qm.rewindFocus;
		if (on)
			rect(dl, px + 32, y + 2, pw - 64, rowH - 6, col::accent, 12);
		text(dl, regular(), 24, px + 64, y + 17, on ? col::text : col::dim, ICON_FA_CLOCK_ROTATE_LEFT);
		text(dl, bold(), 24, px + 112, y + 16, on ? col::text : col::dim, agoText(qm.rewindPoints[i].secondsAgo).c_str());
	}
	dl->PopClipRect();
	if (count == 0 && qm.rewindNote.empty())
		text(dl, regular(), 22, px + 56, top + 24, col::faint,
				"Nothing to go back to yet: a moment is kept every few seconds of play.", pw - 112);
	text(dl, regular(), 20, px + 56, top + viewH + 14, qm.rewindNote.empty() ? col::faint : col::warm,
			!qm.rewindNote.empty() ? qm.rewindNote.c_str()
			: "The whole machine goes back to that moment, its memory cards too: what the game saved since is undone.\nThe moments after it are forgotten.",
			pw - 112);
	return went;
}

// ---- the Achievements page: the running game's, as the site groups them

void openAchievements()
{
	qm.trophies = true;
	qm.trophyFocus = 0;
	qm.trophyScroll = 0;
	qm.trophyGame = achievements::getCurrentGame();
	qm.trophyList = achievements::getAchievementList();
}

void achievementsPage(ImDrawList *dl, float px, float pw, float top)
{
	const int count = (int)qm.trophyList.size();
	if (in.up && qm.trophyFocus > 0) qm.trophyFocus--;
	if (in.down && qm.trophyFocus < count - 1) qm.trophyFocus++;
	if (in.l2) qm.trophyFocus = std::max(0, qm.trophyFocus - 6);
	if (in.r2) qm.trophyFocus = std::min(std::max(0, count - 1), qm.trophyFocus + 6);
	if (in.back || in.options)
		qm.trophies = false;

	text(dl, bold(), 34, px + 48, top - 62, col::text, ICON_FA_TROPHY "   Achievements");
	{
		const achievements::Game& game = qm.trophyGame;
		const std::string sum = std::to_string(game.unlockedAchievements) + " of " + std::to_string(game.totalAchievements)
				+ ", " + std::to_string(game.points) + " of " + std::to_string(game.totalPoints) + " points";
		const ImVec2 ss = textSize(regular(), 22, sum.c_str());
		text(dl, regular(), 22, px + pw - 60 - ss.x, top - 52, col::dim, sum.c_str());
	}
	// Each achievement is a row; where the site's group changes, its name is a
	// line above the row.
	const float rowH = 92, headH = 46;
	const float noteH = 92;
	const float viewH = H - 96 - noteH - top;
	std::vector<float> at(count + 1, 0.f);
	float total = 0;
	for (int i = 0; i < count; i++)
	{
		if (i == 0 || qm.trophyList[i].category != qm.trophyList[i - 1].category)
			total += headH;
		at[i] = total;
		total += rowH;
	}
	const float focusY = count > 0 ? at[std::clamp(qm.trophyFocus, 0, count - 1)] : 0;
	const float target = std::clamp(focusY - (viewH - rowH) / 2, 0.f, std::max(0.f, total - viewH));
	qm.trophyScroll = approach(qm.trophyScroll, target, 18);
	dl->PushClipRect(V(px, top), V(px + pw, top + viewH), true);
	for (int i = 0; i < count; i++)
	{
		const achievements::Achievement& one = qm.trophyList[i];
		const float y = top + at[i] - qm.trophyScroll;
		if (y > top + viewH || y + rowH < top - headH)
			continue;
		if (i == 0 || one.category != qm.trophyList[i - 1].category)
		{
			const bool open = one.category.find("Unlocked") != std::string::npos;
			const std::string head = std::string(open ? ICON_FA_LOCK_OPEN : ICON_FA_LOCK) + "   " + one.category;
			text(dl, bold(), 20, px + 56, y - headH + 14, col::faint, head.c_str());
		}
		const bool on = i == qm.trophyFocus;
		if (on)
			rect(dl, px + 32, y + 2, pw - 64, rowH - 6, col::accent, 12);
		const float side = 68;
		ImguiFileTexture picture(one.image);
		const ImTextureID id = picture.getId();
		if (id != ImTextureID())
			imageFill(dl, id, px + 48, y + (rowH - 4 - side) / 2, side, side, 8);
		else
		{
			rect(dl, px + 48, y + (rowH - 4 - side) / 2, side, side, on ? alpha(col::text, 0.2f) : col::card, 8);
			const ImVec2 is = textSize(regular(), 26, ICON_FA_TROPHY);
			text(dl, regular(), 26, px + 48 + (side - is.x) / 2, y + (rowH - 4 - is.y) / 2, on ? col::text : col::faint, ICON_FA_TROPHY);
		}
		const float tx = px + 48 + side + 20;
		float tw = pw - (tx - px) - 64;
		if (!one.status.empty())
		{
			// How far along it is ("12/50"), on the right.
			const ImVec2 ps = textSize(regular(), 22, one.status.c_str());
			text(dl, regular(), 22, px + pw - 60 - ps.x, y + 16, on ? col::text : col::dim, one.status.c_str());
			tw -= ps.x + 24;
		}
		text(dl, bold(), 24, tx, y + 12, on ? col::text : col::dim, fit(bold(), 24, one.title, tw).c_str());
		text(dl, regular(), 20, tx, y + 48, on ? col::text : col::faint,
				fit(regular(), 20, one.description, pw - (tx - px) - 64).c_str());
	}
	dl->PopClipRect();
	if (count == 0)
		text(dl, regular(), 22, px + 56, top + 24, col::faint, "The site has no achievements for this game.", pw - 112);
	else
	{
		// The focused one's description in full, when its row cut it short.
		const achievements::Achievement& one = qm.trophyList[std::clamp(qm.trophyFocus, 0, count - 1)];
		if (fit(regular(), 20, one.description, pw - (48 + 68 + 20) - 64) != one.description)
			text(dl, regular(), 20, px + 56, top + viewH + 14, col::dim, one.description.c_str(), pw - 112);
	}
}

} // namespace

void quickMenu()
{
	ImDrawList *dl = beginScreen("##bp-quick", false);
	{
		// Not drawn the frame before: the menu is being opened, whichever way it
		// was closed. (The touch pad, which is the emulator's own menu button,
		// resumes the game from any page without the menu hearing of it; its
		// pages would come back as they were, the Rewind page with a list of
		// moments that are older by now.)
		static int lastFrame = -2;
		const int frame = ImGui::GetFrameCount();
		if (frame != lastFrame + 1)
		{
			if (qm.optionsDirty)
			{
				SaveSettings();
				qm.optionsDirty = false;
			}
			if (qm.cheats)
				ps5::cheats::saveStates();
			qm.open = 0;
		}
		lastFrame = frame;
	}
	if (qm.open < 0.01f)
	{
		qm.focus = 0;
		qm.cheats = false;
		qm.cheatsAnim = 0;
		qm.options = false;
		qm.optionsAnim = 0;
		qm.controls = false;
		qm.controlsAnim = 0;
		qm.listening = -1;
		qm.rewind = false;
		qm.rewindAnim = 0;
		qm.trophies = false;
		qm.trophiesAnim = 0;
		// Asked once as the menu opens: every asking has the game's picture
		// downloaded again while it is missing.
		qm.trophiesActive = config::EnableAchievements && achievements::isActive();
		if (qm.trophiesActive)
		{
			const achievements::Game game = achievements::getCurrentGame();
			qm.trophiesDetail = game.totalAchievements == 0 ? "None for this game"
					: std::to_string(game.unlockedAchievements) + " of " + std::to_string(game.totalAchievements);
		}
		else
			// Signed in and none here: the site has none for this game, or they
			// are still being fetched, or could not be.
			qm.trophiesDetail = !config::EnableAchievements ? "" : !achievements::isLoggedOn() ? "Not signed in" : "None loaded";
	}
	qm.open = approach(qm.open, 1, 14);
	const float t = qm.open;

	const bool canState = dc_savestateAllowed();
	const time_t stateDate = dc_getStateCreationDate(config::SavestateSlot);
	std::vector<QuickItem> items;
	items.push_back({ ICON_FA_PLAY, "Resume", "", true, [] {
		GamepadDevice::load_system_mappings();
		gui_setState(GuiState::Closed);
	} });
	items.push_back({ ICON_FA_DOWNLOAD, "Save state", "", canState, [] {
		gui_setState(GuiState::Closed);
		saveState();
	} });
	items.push_back({ ICON_FA_CLOCK_ROTATE_LEFT, "Load state", stateDate == 0 ? "Empty slot" : "", canState && stateDate != 0, [] {
		gui_setState(GuiState::Closed);
		dc_loadstate(config::SavestateSlot);
	} });
	{
		QuickItem slot{ ICON_FA_LAYER_GROUP, "State slot", "", true, nullptr };
		slot.side = [](int d) {
			config::SavestateSlot = (config::SavestateSlot + 10 + d) % 10;
			SaveSettings();
		};
		slot.run = [slot] { slot.side(1); };
		slot.stays = true;
		items.push_back(slot);
	}
	if (ps5::options().rewind)
	{
		// Settings > System > Rewind is on: back to a moment of the last minutes.
		const bool any = ps5::rewind::available();
		QuickItem rewind{ ICON_FA_BACKWARD, "Rewind", any ? "" : "Nothing yet", any, [] { openRewind(); } };
		rewind.stays = true;
		items.push_back(rewind);
	}
	{
		// Flycast's fast forward: the game is no longer held to its own pace,
		// and has no sound. On this console it is still held to the display's:
		// the driver shows every frame for a refresh (twice a 60 fps game's
		// speed with 120 Hz output, and none gained at 60 Hz). Its rules are the
		// button's (core/input/gamepad_device.cpp): not in netplay, not on
		// linked arcade boards. A game that starts has it off.
		const bool allowed = !::settings.network.online && !::settings.naomi.multiboard;
		const bool on = ::settings.input.fastForwardMode;
		QuickItem fast{ ICON_FA_FORWARD, "Fast forward", !allowed ? (::settings.network.online ? "Not while online"
				: "Not on linked boards") : on ? "On" : "Off", allowed, [on] {
			::settings.input.fastForwardMode = !on;
			if (!on)
			{
				// Turned on: straight back to the game.
				GamepadDevice::load_system_mappings();
				gui_setState(GuiState::Closed);
			}
		} };
		fast.stays = on;
		items.push_back(fast);
	}
	if (::settings.platform.isConsole())
		items.push_back({ ICON_FA_COMPACT_DISC, gdr::isOpen() ? "Insert disc" : "Open disc lid", "", true, [] {
			if (gdr::isOpen())
				gui_setState(GuiState::SelectDisk);
			else
			{
				emu.openGdrom();
				gui_setState(GuiState::Closed);
			}
		} });
	{
		int on = 0;
		for (size_t i = 0; i < cheatManager.cheatCount(); i++)
			on += cheatManager.cheatEnabled(i) ? 1 : 0;
		QuickItem cheats{ ICON_FA_BOLT, "Cheats", cheatManager.cheatCount() == 0 ? "None" : on == 0 ? "Off"
				: std::to_string(on) + " on", true, [] { openCheats(); } };
		cheats.stays = true;
		items.push_back(cheats);
	}
	if (config::EnableAchievements)
	{
		QuickItem trophies{ ICON_FA_TROPHY, "Achievements", qm.trophiesDetail, qm.trophiesActive, [] { openAchievements(); } };
		trophies.stays = true;
		items.push_back(trophies);
	}
	{
		// This game's own options. (The Settings, for every game, are in the
		// library.)
		const bool hasId = !::settings.content.gameId.empty();
		const int own = hasId ? ownOptions() : 0;
		QuickItem options{ ICON_FA_SLIDERS, "Game options", own == 0 ? "" : std::to_string(own) + " of its own", hasId,
				[] { openGameOptions(); } };
		options.stays = true;
		items.push_back(options);
	}
	{
		// Which DualSense button is which, for this game.
		const std::shared_ptr<GamepadDevice> pad = pad1();
		QuickItem controls{ ICON_FA_GAMEPAD, "Controls", pad != nullptr && pad->isPerGameMapping() ? "Its own" : "", true,
				[] { openControls(); } };
		controls.stays = true;
		items.push_back(controls);
	}
	{
		// The console's reset button, and more: the game is loaded again from
		// its beginning, with the options that wait for a start. Asked twice,
		// as what was not saved is lost.
		const bool asked = timeNow < qm.restartAsked + 4;
		QuickItem restart{ ICON_FA_POWER_OFF, asked ? "Restart: press again" : "Restart game", "", true, [asked] {
			if (!asked)
			{
				qm.restartAsked = timeNow;
				return;
			}
			qm.restartAsked = -10;
			ps5::net::gameEnding(true);
			const std::string path = !insertedPath.empty() ? insertedPath : ::settings.content.path;
			ps5::games::startFresh();
			for (Game& g : games)
				if (g.media.path == path)
				{
					launch(g, false);
					return;
				}
			loadingPath = path;
			loadingTitle = ::settings.content.title;
			gui_start_game(path);
		} };
		restart.stays = !asked;
		items.push_back(restart);
	}
	items.push_back({ ICON_FA_RIGHT_FROM_BRACKET, "Quit game", "", true, [] {
		::settings.input.fastForwardMode = false;
		// The achievements' pictures still to come are not waited for.
		ps5::net::gameEnding(true);
		gui_stop_game();
		ps5::net::gameEnding(false);
	} });

	// ---- input
	const int n = (int)items.size();
	QuickItem& cur = items[std::clamp(qm.focus, 0, n - 1)];
	bool closing = false;
	const bool onCheats = qm.cheats;	// the page handles its own input, below
	const bool onOptions = qm.options;
	const bool onControls = qm.controls;
	const bool onRewind = qm.rewind;
	const bool onTrophies = qm.trophies;
	const bool onPage = onCheats || onOptions || onControls || onRewind || onTrophies;
	if (!onPage)
	{
		if (in.up) qm.focus = (qm.focus + n - 1) % n;
		if (in.down) qm.focus = (qm.focus + 1) % n;
		QuickItem& now = items[std::clamp(qm.focus, 0, n - 1)];
		if (now.side && (in.left || in.right))
			now.side(in.left ? -1 : 1);
		if (in.accept && now.enabled && now.run)
		{
			now.run();
			closing = !now.stays;
		}
		else if (in.back || in.options)
		{
			GamepadDevice::load_system_mappings();
			gui_setState(GuiState::Closed);
			closing = true;
		}
	}
	if (closing)
		qm.open = 0;
	qm.cheatsAnim = approach(qm.cheatsAnim, qm.cheats ? 1.f : 0.f, 16);
	qm.optionsAnim = approach(qm.optionsAnim, qm.options ? 1.f : 0.f, 16);
	qm.controlsAnim = approach(qm.controlsAnim, qm.controls ? 1.f : 0.f, 16);
	qm.rewindAnim = approach(qm.rewindAnim, qm.rewind ? 1.f : 0.f, 16);
	qm.trophiesAnim = approach(qm.trophiesAnim, qm.trophies ? 1.f : 0.f, 16);

	// ---- the paused game, dimmed
	dl->AddRectFilled(V(0, 0), V(W, H), col::rgba(0, 0, 0, (int)(140 * t)));

	// ---- the panel, sliding in from the right
	// The pages are wider; the Rewind page, a short list, is not.
	const float pw = 640 + 260 * std::max({ qm.cheatsAnim, qm.optionsAnim, qm.controlsAnim, qm.trophiesAnim });
	const float px = W - pw * (0.15f + 0.85f * t) + (1 - t) * 0;
	gradientH(dl, px - 120, 0, 120, H, col::rgba(0, 0, 0, 0), col::rgba(0, 0, 0, (int)(120 * t)));
	rect(dl, px, 0, pw, H, alpha(col::sheet, 0.97f * t));
	rect(dl, px, 0, 1, H, col::line);

	// The game.
	GameMedia media;
	media.path = ::settings.content.path;
	media.fileName = ::settings.content.fileName;
	GameBoxart art = boxart().getBoxart(media);
	Game g;
	g.media = media;
	g.source = sourceOf(media.path);
	g.art = art;
	g.title = !::settings.content.title.empty() ? ::settings.content.title : art.name;
	const float gx = px + 48;
	drawCover(dl, g, gx, 56, 150, 150, 12, 0.5f);
	text(dl, bold(), 34, gx + 176, 72, col::text, fit(bold(), 34, g.title, pw - 48 - 176 - 40).c_str());
	text(dl, regular(), 22, gx + 176, 124, col::dim, ::settings.platform.isArcade() ? "Arcade" : "Dreamcast");
	text(dl, regular(), 22, gx + 176, 156, col::good, ICON_FA_PAUSE "  Paused");

	// The items, or the Cheats page in their place.
	// A row is 78 high, and lower when that many would not fit above the hints.
	const float itemsTop = 250, ih = std::min(78.f, std::floor((H - 100 - itemsTop) / n));
	if (onCheats)
		cheatsPage(dl, px, pw, itemsTop + 70);
	else if (onOptions)
		optionsPage(dl, px, pw, itemsTop + 70);
	else if (onControls)
		controlsPage(dl, px, pw, itemsTop + 70);
	else if (onRewind)
	{
		if (rewindPage(dl, px, pw, itemsTop + 70))
		{
			// As "Load state": the game goes on from where it now is.
			GamepadDevice::load_system_mappings();
			gui_setState(GuiState::Closed);
			qm.open = 0;
		}
	}
	else if (onTrophies)
		achievementsPage(dl, px, pw, itemsTop + 70);
	else
	{
	qm.focusAnim = approach(qm.focusAnim, (float)qm.focus, 22);
	rect(dl, px + 32, itemsTop + qm.focusAnim * ih, pw - 64, ih - 8, col::accent, 12);
	for (int i = 0; i < n; i++)
	{
		const QuickItem& it = items[i];
		// The offsets below are for a row 78 high: a lower row is moved up by half the difference.
		const float y = itemsTop + i * ih + (ih - 78) / 2;
		const bool on = i == qm.focus;
		const ImU32 c = !it.enabled ? col::faint : on ? col::text : col::dim;
		text(dl, regular(), 28, px + 64, y + 20, c, it.icon);
		text(dl, bold(), 28, px + 120, y + 19, c, it.label.c_str());
		std::string right = it.detail;
		if (it.side)
			right = "Slot " + std::to_string((int)config::SavestateSlot + 1);
		if (!right.empty())
		{
			const ImVec2 rs = textSize(regular(), 24, right.c_str());
			float rx = px + pw - 64 - rs.x;
			if (it.side && on)
			{
				dl->AddTriangleFilled(V(rx - 26, y + 35), V(rx - 14, y + 25), V(rx - 14, y + 45), col::text);
				dl->AddTriangleFilled(V(px + pw - 44, y + 35), V(px + pw - 56, y + 25), V(px + pw - 56, y + 45), col::text);
			}
			text(dl, regular(), 24, rx, y + 23, on ? col::text : col::faint, right.c_str());
		}
	}
	}

	// The state in the slot: a card beside the panel while a state row is focused.
	if (!onPage && (cur.label == "Save state" || cur.label == "Load state" || cur.side))
	{
		const float cw = 432, chh = 320;
		const float cx = px - 32 - cw;
		const float rowY = itemsTop + qm.focusAnim * ih + (ih - 8) / 2;
		const float cy = std::clamp(rowY - chh / 2, 40.f, H - chh - 40);
		glow(dl, cx, cy, cw, chh, 18, col::rgba(0, 0, 0, 200), 24);
		rect(dl, cx, cy, cw, chh, alpha(col::sheetHi, 0.98f), 18);
		const float iw = cw - 32, ihh = iw * 9 / 16;
		const std::string slotName = "Slot " + std::to_string((int)config::SavestateSlot + 1);
		if (stateDate != 0)
		{
			ImguiStateTexture pic;
			const ImTextureID id = pic.getId();
			if (id != ImTextureID())
				imageFill(dl, id, cx + 16, cy + 16, iw, ihh, 12);
			else
				rect(dl, cx + 16, cy + 16, iw, ihh, col::card, 12);
		}
		else
		{
			rect(dl, cx + 16, cy + 16, iw, ihh, col::card, 12);
			const char *empty = ICON_FA_BOX_OPEN "   Empty slot";
			const ImVec2 es = textSize(regular(), 26, empty);
			text(dl, regular(), 26, cx + 16 + (iw - es.x) / 2, cy + 16 + (ihh - es.y) / 2, col::faint, empty);
		}
		text(dl, bold(), 26, cx + 20, cy + ihh + 32, col::text, slotName.c_str());
		if (stateDate != 0)
		{
			const std::string when = timeToShortDateTimeString(stateDate);
			const ImVec2 ws = textSize(regular(), 22, when.c_str());
			text(dl, regular(), 22, cx + cw - 20 - ws.x, cy + ihh + 36, col::dim, when.c_str());
		}
	}

	// Hints, in the panel.
	{
		float x = px + 48;
		const float y = H - 52;
		auto hint = [&](Glyph gl, const char *label) {
			x += glyph(dl, gl, x, y, 30) + 10;
			x += text(dl, regular(), 22, x, y - 13, col::text, label).x + 30;
		};
		if (onCheats)
		{
			hint(Glyph::Cross, qm.cheatFocus == 0 ? "Next file" : "On / off");
			if (qm.cheatFocus == 0)
				hint(Glyph::DPadLR, "File");
			hint(Glyph::Circle, "Back");
		}
		else if (onOptions)
		{
			hint(Glyph::DPadLR, "Change");
			hint(Glyph::Square, "As in Settings");
			hint(Glyph::Circle, "Back");
		}
		else if (onRewind)
		{
			if (!qm.rewindPoints.empty())
				hint(Glyph::Cross, "Go back to it");
			hint(Glyph::Circle, "Back");
		}
		else if (onTrophies)
		{
			hint(Glyph::DPadUD, "Move");
			hint(Glyph::Circle, "Back");
		}
		else if (onControls && qm.listening >= 0)
			hint(Glyph::TouchPad, "Cancel");
		else if (onControls)
		{
			// As controlsPage has it: Square takes one of the emulator's own
			// controls off its button.
			const std::shared_ptr<GamepadDevice> pad = pad1();
			const std::vector<PadControl> controls = controlsShown();
			const int row = qm.controlFocus - 1;
			hint(Glyph::Cross, qm.controlFocus == 0 ? "As for every game" : "Change");
			if (pad != nullptr && row >= 0 && row < (int)controls.size() && controls[row].emulator
					&& boundTo(*pad, controls[row].key) != "Not set")
				hint(Glyph::Square, "No button");
			hint(Glyph::Circle, "Back");
		}
		else
		{
			hint(Glyph::Cross, "Select");
			if (cur.side)
				hint(Glyph::DPadLR, "Slot");
			hint(Glyph::Circle, "Resume");
		}
	}
	endScreen();
}

// ----------------------------------------------------------------- the rest

void applyTheme()
{
	ImGuiStyle& s = ImGui::GetStyle();
	s.WindowRounding = 16.f;
	s.ChildRounding = 12.f;
	s.FrameRounding = 10.f;
	s.PopupRounding = 12.f;
	s.GrabRounding = 10.f;
	s.TabRounding = 10.f;
	s.WindowBorderSize = 0.f;
	s.FrameBorderSize = 0.f;
	ImVec4 *c = s.Colors;
	auto f = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
	c[ImGuiCol_WindowBg] = f(col::rgba(18, 23, 31, 250));
	c[ImGuiCol_ChildBg] = f(col::rgba(30, 38, 49, 255));
	c[ImGuiCol_PopupBg] = f(col::rgba(24, 30, 40, 252));
	c[ImGuiCol_ModalWindowDimBg] = f(col::rgba(0, 0, 0, 170));
	c[ImGuiCol_Text] = f(col::text);
	c[ImGuiCol_TextDisabled] = f(col::faint);
	c[ImGuiCol_Border] = f(col::line);
	c[ImGuiCol_FrameBg] = f(col::rgba(255, 255, 255, 20));
	c[ImGuiCol_FrameBgHovered] = f(col::rgba(255, 255, 255, 34));
	c[ImGuiCol_FrameBgActive] = f(col::rgba(255, 255, 255, 44));
	c[ImGuiCol_Button] = f(col::panelHi);
	c[ImGuiCol_ButtonHovered] = f(col::accent);
	c[ImGuiCol_ButtonActive] = f(col::accentDark);
	c[ImGuiCol_Header] = f(alpha(col::accent, 0.35f));
	c[ImGuiCol_HeaderHovered] = f(col::accent);
	c[ImGuiCol_HeaderActive] = f(col::accentDark);
	c[ImGuiCol_CheckMark] = f(col::accent);
	c[ImGuiCol_SliderGrab] = f(col::accent);
	c[ImGuiCol_SliderGrabActive] = f(col::accentDark);
	c[ImGuiCol_PlotHistogram] = f(col::accent);
	c[ImGuiCol_NavCursor] = f(col::text);
	c[ImGuiCol_TitleBg] = f(col::bar);
	c[ImGuiCol_TitleBgActive] = f(col::bar);
}

void feedNav(ImGuiIO& io)
{
	const auto& p = ps5::pad::player1();
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, p.buttons & ps5::pad::Cross);
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, p.buttons & ps5::pad::Circle);
	io.AddKeyEvent(ImGuiKey_GamepadFaceUp, p.buttons & ps5::pad::Triangle);
	io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, p.buttons & ps5::pad::Square);
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, p.buttons & ps5::pad::Up);
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, p.buttons & ps5::pad::Down);
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, p.buttons & ps5::pad::Left);
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, p.buttons & ps5::pad::Right);
	io.AddKeyEvent(ImGuiKey_GamepadL1, p.buttons & ps5::pad::L1);
	io.AddKeyEvent(ImGuiKey_GamepadR1, p.buttons & ps5::pad::R1);
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, p.lx < -0.1f, std::max(0.f, -p.lx));
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, p.lx > 0.1f, std::max(0.f, p.lx));
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, p.ly < -0.1f, std::max(0.f, -p.ly));
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, p.ly > 0.1f, std::max(0.f, p.ly));
}

}
