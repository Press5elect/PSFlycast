/*
	PSFlyCast - a USB keyboard and a USB mouse, as the Dreamcast's.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A keyboard or a mouse plugged into the console is read through
	libSceKeyboard and libSceMouse and handed to Flycast's own keyboard and
	mouse devices, which the games that know them read: typing, chat and
	browsers, and a first-person game's keyboard and mouse.

	The Dreamcast has four ports. The pads have the first ones (ps5_pad.cpp);
	when a game starts, a keyboard that is connected takes the next free one
	and a mouse the one after it, for that game and not in the settings. An
	arcade game that has a keyboard or a mouse reads the first port's.

	In an arcade game that reads no keyboard, the keys are Flycast's keyboard
	layout for player 1's controls (arrows, X C S D, Enter; Tab opens the
	quick menu). The interface itself is driven by the pad only.

	The calls and the layout of what they return are the ones ProsperoLight
	(BlackBearReloaded, GPL-3.0-or-later) reads on the console: a keyboard
	state of 96 bytes with up to 16 keys held, as USB usage codes, which are
	also Flycast's key codes; a mouse report of 40 bytes with the buttons and
	how far it moved. Nothing here has run on a console yet: every result is
	a line of flycast-boot.log ("usb: ...").
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "ps5_pad.h"
#include "cfg/option.h"
#include "input/keyboard_device.h"
#include "input/mouse.h"
#include "emulator.h"
#include "ui/settings.h"
#include "ui/gui.h"
#include "hw/maple/maple_cfg.h"

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

extern "C"
{
int sceSysmoduleLoadModule(uint32_t id);
int sceUserServiceGetInitialUser(int32_t *userId);
int sceKeyboardInit(void);
int sceKeyboardOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int sceKeyboardRead(int32_t handle, void *data, int32_t capacity);
int sceMouseInit(void);
int sceMouseOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int sceMouseRead(int32_t handle, void *data, int32_t capacity);
}

namespace ps5::usb
{
namespace
{

constexpr uint32_t KeyboardModule = 0x0106, MouseModule = 0x00a9, KeyboardDialogModule = 0x0096;
constexpr int Slots = 4;

struct KeyboardState
{
	uint64_t timestamp;
	uint8_t intercepted;		// the system has the keyboard
	uint8_t pad0[7];
	uint8_t connected;
	uint8_t pad1[3];
	int32_t length;
	uint32_t leds;
	uint32_t modifiers;			// bit 0 left Ctrl, then Shift, Alt, GUI; bits 4 to 7 the right ones
	uint16_t keys[16];
	uint8_t pad2[32];
};
static_assert(sizeof(KeyboardState) == 96, "a keyboard state is 96 bytes");
static_assert(offsetof(KeyboardState, connected) == 0x10, "connected at 0x10");
static_assert(offsetof(KeyboardState, modifiers) == 0x1c, "modifiers at 0x1c");
static_assert(offsetof(KeyboardState, keys) == 0x20, "keys at 0x20");

struct MouseData
{
	uint64_t timestamp;
	uint8_t connected;
	uint8_t pad0[3];
	uint32_t buttons;			// bit 0 left, 1 right, 2 middle, then two more; bit 31: the system has it
	int32_t x, y, wheel, tilt;
	uint8_t pad1[8];
};
static_assert(sizeof(MouseData) == 40, "a mouse report is 40 bytes");
static_assert(offsetof(MouseData, buttons) == 0x0c, "buttons at 0x0c");
static_assert(offsetof(MouseData, x) == 0x10, "movement at 0x10");

class UsbKeyboard : public KeyboardDevice
{
public:
	UsbKeyboard() : KeyboardDevice(-1, "PS5-USB")
	{
		_unique_id = "ps5_usb_keyboard";
		if (!find_mapping())
			input_mapper = getDefaultMapping();
	}
	void key(uint8_t code, bool pressed) {
		input(code, pressed, 0);
	}
};

class UsbMouse : public Mouse
{
public:
	UsbMouse() : Mouse("PS5-USB", -1)
	{
		_unique_id = "ps5_usb_mouse";
		loadMapping();
	}
};

int moduleResult[3] = { -1, -1, -1 };
int keyboardInit = -1, mouseInit = -1;
bool started, said;
std::chrono::steady_clock::time_point nextScan;
int keyboardHandle[Slots] = { -1, -1, -1, -1 }, mouseHandle[Slots] = { -1, -1, -1, -1 };
KeyboardState keyboardLast[Slots];
uint32_t mouseLast[Slots];
std::shared_ptr<UsbKeyboard> keyboard;
std::shared_ptr<UsbMouse> mouse;
// Read by the emulator's thread when it makes the game's devices.
std::atomic<bool> keyboardThere, mouseThere;
std::atomic<int> keyboardPort{ -1 }, mousePort{ -1 };

bool holds(const KeyboardState& state, uint16_t key)
{
	return std::find(std::begin(state.keys), std::end(state.keys), key) != std::end(state.keys);
}

// What changed between two states of a keyboard, as key presses and releases.
void feedKeyboard(KeyboardState& before, const KeyboardState& given)
{
	const KeyboardState none{};
	const KeyboardState& now = given.connected != 0 && given.intercepted == 0 ? given : none;
	// The modifier keys are usage codes 0xe0 to 0xe7, in the order of their bits.
	for (int bit = 0; bit < 8; bit++)
		if (((before.modifiers ^ now.modifiers) >> bit) & 1)
			keyboard->key((uint8_t)(0xe0 + bit), ((now.modifiers >> bit) & 1) != 0);
	for (const uint16_t key : before.keys)
		if (key != 0 && key < 0xe0 && !holds(now, key))
			keyboard->key((uint8_t)key, false);
	for (const uint16_t key : now.keys)
		if (key != 0 && key < 0xe0 && !holds(before, key))
			keyboard->key((uint8_t)key, true);
	before = now;
}

void feedMouse(uint32_t& before, const MouseData& report)
{
	const bool usable = report.connected != 0 && (report.buttons & 0x80000000u) == 0;
	const uint32_t now = usable ? report.buttons & 0x1f : 0;
	static const Mouse::Button buttons[5] = { Mouse::LEFT_BUTTON, Mouse::RIGHT_BUTTON, Mouse::MIDDLE_BUTTON, Mouse::BUTTON_4,
			Mouse::BUTTON_5 };
	for (int bit = 0; bit < 5; bit++)
		if (((before ^ now) >> bit) & 1)
			mouse->setButton(buttons[bit], ((now >> bit) & 1) != 0);
	before = now;
	if (!usable)
		return;
	if (report.x != 0 || report.y != 0)
		mouse->setRelPos((float)std::clamp(report.x, -2000, 2000), (float)std::clamp(report.y, -2000, 2000));
	if (report.wheel != 0)
		mouse->setWheel(std::clamp(report.wheel, -100, 100));
}

// Flycast keeps a device for each: made when a real one is plugged in, or when
// the pad is asked to be one.
void ensureKeyboard()
{
	if (keyboard)
		return;
	keyboard = std::make_shared<UsbKeyboard>();
	GamepadDevice::Register(keyboard);
}

void ensureMouse()
{
	if (mouse)
		return;
	// Flycast keeps a pointer on the screen for each mouse and moves it
	// within the screen's size, which it learns from a pointer's place:
	// the middle of the display, once.
	const int width = ::settings.display.width > 0 ? ::settings.display.width : 1920;
	const int height = ::settings.display.height > 0 ? ::settings.display.height : 1080;
	SetMousePosition(width / 2, height / 2, width, height, 0);
	mouse = std::make_shared<UsbMouse>();
	GamepadDevice::Register(mouse);
}

// Whether the game asks for the pad to be a mouse or a keyboard: a Dreamcast
// game only, as the arcade ones have their own controls.
bool padMouseWanted()
{
	return ::settings.platform.isConsole() && ps5::pad::PadMouse == ps5::pad::PadMouseOn;
}

bool padKeysWanted()
{
	return ::settings.platform.isConsole() && ps5::pad::PadKeys != ps5::pad::PadKeysOff;
}

// Opens what is not open yet: at the first poll, and every three seconds
// after it for a keyboard or a mouse that is plugged in later.
void open()
{
	if (!started)
	{
		started = true;
		keyboardInit = moduleResult[0] >= 0 ? sceKeyboardInit() : -1;
		mouseInit = moduleResult[1] >= 0 ? sceMouseInit() : -1;
	}
	int32_t user = -1;
	sceUserServiceGetInitialUser(&user);
	int keyboards = 0, mice = 0, opened = 0;
	if (keyboardInit >= 0)
		for (int i = 0; i < Slots; i++)
		{
			if (keyboardHandle[i] < 0)
			{
				const uint64_t param = 0;
				keyboardHandle[i] = sceKeyboardOpen(user, 0, i, &param);
				opened += keyboardHandle[i] >= 0;
			}
			keyboards += keyboardHandle[i] >= 0;
		}
	if (mouseInit >= 0)
		for (int i = 0; i < Slots; i++)
		{
			if (mouseHandle[i] < 0)
			{
				const uint8_t param[8] = {};
				mouseHandle[i] = sceMouseOpen(user, 0, i, param);
				opened += mouseHandle[i] >= 0;
			}
			mice += mouseHandle[i] >= 0;
		}
	if (!said || opened > 0)
		diag::mark("usb: keyboard module %x, init %x, %d of %d opened; mouse module %x, init %x, %d of %d opened",
				(unsigned)moduleResult[0], (unsigned)keyboardInit, keyboards, Slots, (unsigned)moduleResult[1],
				(unsigned)mouseInit, mice, Slots);
	said = true;
	if (keyboards > 0)
		ensureKeyboard();
	if (mice > 0)
		ensureMouse();
}

} // namespace

void preload()
{
	// Before the sandbox is left (USB drives): a module is found by a path
	// inside it.
	moduleResult[0] = sceSysmoduleLoadModule(KeyboardModule);
	moduleResult[1] = sceSysmoduleLoadModule(MouseModule);
	moduleResult[2] = sceSysmoduleLoadModule(KeyboardDialogModule);
}

int dialogModule()
{
	return moduleResult[2];
}

void poll()
{
	if (!options().usbInput)
		return;
	if (const auto now = std::chrono::steady_clock::now(); now >= nextScan)
	{
		nextScan = now + std::chrono::seconds(3);
		open();
	}
	bool anyKeyboard = false, anyMouse = false;
	if (keyboard)
		for (int i = 0; i < Slots; i++)
		{
			if (keyboardHandle[i] < 0)
				continue;
			KeyboardState states[16];
			const int count = std::min(sceKeyboardRead(keyboardHandle[i], states, 16), 16);
			for (int n = 0; n < count; n++)
				feedKeyboard(keyboardLast[i], states[n]);
			anyKeyboard |= keyboardLast[i].connected != 0;
		}
	if (mouse)
		for (int i = 0; i < Slots; i++)
		{
			if (mouseHandle[i] < 0)
				continue;
			MouseData reports[64];
			const int count = std::min(sceMouseRead(mouseHandle[i], reports, 64), 64);
			for (int n = 0; n < count; n++)
			{
				feedMouse(mouseLast[i], reports[n]);
				anyMouse |= reports[n].connected != 0;
			}
			// A mouse that sends nothing is still there.
			static bool seen[Slots];
			if (count > 0)
				seen[i] = reports[count - 1].connected != 0;
			anyMouse |= seen[i];
		}
	if (anyKeyboard != keyboardThere || anyMouse != mouseThere)
		diag::mark("usb: keyboard %s, mouse %s", anyKeyboard ? "connected" : "not connected",
				anyMouse ? "connected" : "not connected");
	keyboardThere = anyKeyboard;
	mouseThere = anyMouse;
}

void plugPorts(uint32_t taken)
{
	int keyboardAt = -1, mouseAt = -1;
	// What the Dreamcast gets: the real ones plugged into the console, and the
	// pad's own when the game's settings ask for them.
	const bool wantKeyboard = (options().usbInput && keyboardThere) || padKeysWanted();
	const bool wantMouse = (options().usbInput && mouseThere) || padMouseWanted();
	if (wantKeyboard || wantMouse)
	{
		if (settings.platform.isConsole())
		{
			// A port the settings already give a keyboard or a mouse is theirs.
			for (int port = 0; port < 4; port++)
			{
				if (config::MapleMainDevices[port] == MDT_Keyboard && keyboardAt < 0)
					keyboardAt = port;
				if (config::MapleMainDevices[port] == MDT_Mouse && mouseAt < 0)
					mouseAt = port;
			}
			for (int port = 1; port < 4; port++)
			{
				if ((taken & (1u << port)) != 0 || config::MapleMainDevices[port] != MDT_None)
					continue;
				if (wantKeyboard && keyboardAt < 0)
				{
					config::MapleMainDevices[port].override(MDT_Keyboard);
					keyboardAt = port;
				}
				else if (wantMouse && mouseAt < 0)
				{
					config::MapleMainDevices[port].override(MDT_Mouse);
					mouseAt = port;
				}
			}
			static const char *const letters[] = { "none", "A", "B", "C", "D" };
			diag::mark("usb: the Dreamcast's keyboard is in port %s, its mouse in port %s (the pad as a keyboard: %s, as a mouse: %s)",
					letters[wantKeyboard ? keyboardAt + 1 : 0], letters[wantMouse ? mouseAt + 1 : 0],
					padKeysWanted() ? "yes" : "no", padMouseWanted() ? "yes" : "no");
		}
		else
		{
			// An arcade game with a keyboard or a mouse reads the first port's.
			keyboardAt = keyboardThere ? 0 : -1;
			mouseAt = mouseThere ? 0 : -1;
		}
	}
	if (wantKeyboard && settings.platform.isConsole())
		ensureKeyboard();
	if (wantMouse && settings.platform.isConsole())
		ensureMouse();
	keyboardPort = (wantKeyboard || keyboardThere) ? keyboardAt : -1;
	mousePort = (wantMouse || mouseThere) ? mouseAt : -1;
	if (keyboard)
		keyboard->set_maple_port(keyboardPort);
	if (mouse)
		mouse->set_maple_port(mousePort);
}

// The first pad as a mouse and a keyboard. Called once a frame, and does
// nothing unless a game is running and its settings ask for it.
void padInput(const ps5::pad::State& pad)
{
	using namespace ps5::pad;
	static uint32_t mouseHeld;			// the buttons the pad holds down on the mouse
	static std::bitset<256> keysHeld;	// the keys it holds down
	static float carryX, carryY;
	static int sentCounts, sentFrames, biggestStep;
	static auto lastReport = std::chrono::steady_clock::now();
	// Not while the quick menu or the settings are up: the pad drives those.
	const bool active = game_started && pad.connected && !gui_is_open();
	const bool mouseOn = active && padMouseWanted() && mouse && mouse->maple_port() >= 0;
	const bool keysOn = active && padKeysWanted() && keyboard && keyboard->maple_port() >= 0;

	if (mouse)
	{
		uint32_t now = 0;
		if (mouseOn)
			now = (pad.r2 > 0.5f ? 1u : 0u) | (pad.l2 > 0.5f ? 2u : 0u) | ((pad.buttons & R3) ? 4u : 0u);
		static const Mouse::Button buttons[3] = { Mouse::LEFT_BUTTON, Mouse::RIGHT_BUTTON, Mouse::MIDDLE_BUTTON };
		for (int bit = 0; bit < 3; bit++)
			if (((mouseHeld ^ now) >> bit) & 1)
				mouse->setButton(buttons[bit], ((now >> bit) & 1) != 0);
		mouseHeld = now;
		if (mouseOn)
		{
			// The stick has its own dead zone already; this one is on top of it, so a stick
			// that rests off the centre does not move the mouse. Past it the push starts from zero.
			const float raw = std::min(1.f, std::hypot(pad.rx, pad.ry));
			const float zone = std::clamp((int)PadMouseDeadZone, 0, 60) / 100.f;
			const float magnitude = raw > zone ? (raw - zone) / (1.f - zone) : 0.f;
			if (magnitude > 0.f)
			{
				// Counts a frame at full push. The game takes whole counts and some turn a long way
				// for each, so the low end is slow: speed 1 is a count every ten frames, speed 10 one
				// a frame, speed 20 five a frame. The curve makes a small push slower still.
				const int steps = std::clamp((int)PadMouseSpeed, 1, 20);
				const float full = steps <= 10 ? 0.1f * steps : 1.f + 0.4f * (steps - 10);
				const float scale = full * (PadMouseCurve != 0 ? magnitude : 1.f) * magnitude / raw;
				carryX += pad.rx * scale;
				carryY += (PadMouseInvertY != 0 ? -pad.ry : pad.ry) * scale;
				const float dx = std::trunc(carryX), dy = std::trunc(carryY);
				carryX -= dx;
				carryY -= dy;
				if (dx != 0.f || dy != 0.f)
				{
					mouse->setRelPos(dx, dy);
					sentCounts += (int)(std::fabs(dx) + std::fabs(dy));
					sentFrames++;
					biggestStep = std::max(biggestStep, (int)std::max(std::fabs(dx), std::fabs(dy)));
				}
			}
			else
				carryX = carryY = 0.f;
			// L1 and R1 turn the wheel: the weapon before and after.
			if ((pad.pressed & R1) != 0 && PadKeys != PadKeysOff)
				mouse->setWheel(1);
			if ((pad.pressed & L1) != 0 && PadKeys != PadKeysOff)
				mouse->setWheel(-1);
		}
	}
	if (keyboard)
	{
		std::bitset<256> want;
		if (keysOn)
		{
			// USB usage codes: A 04, D 07, E 08, S 16, W 1a, 1 to 4 1e to 21, Enter 28, Escape 29,
			// Space 2c, C 06, right, left, down, up arrows 4f to 52, Control e0, Shift e1, Alt e2.
			const bool wasd = PadKeys == PadKeysWasd;
			if (pad.ly < -0.5f) want[wasd ? 0x1a : 0x52] = true;
			if (pad.ly > 0.5f) want[wasd ? 0x16 : 0x51] = true;
			if (pad.lx < -0.5f) want[wasd ? 0x04 : 0x50] = true;
			if (pad.lx > 0.5f) want[wasd ? 0x07 : 0x4f] = true;
			if (pad.buttons & Cross) want[0x2c] = true;				// Space: jump
			if (pad.buttons & Circle) want[wasd ? 0x06 : 0xe0] = true;	// C: crouch (Control)
			if (pad.buttons & Square) want[wasd ? 0x08 : 0xe2] = true;	// E: use (Alt)
			if (pad.buttons & Triangle) want[0x15] = true;			// R: reload
			if (pad.buttons & L3) want[0xe1] = true;				// Shift: walk
			if (pad.buttons & ps5::pad::Options) want[0x29] = true;			// Escape
			if (pad.buttons & Up) want[0x1e] = true;				// 1 to 4: weapons
			if (pad.buttons & Right) want[0x1f] = true;
			if (pad.buttons & Down) want[0x20] = true;
			if (pad.buttons & Left) want[0x21] = true;
		}
		const std::bitset<256> changed = keysHeld ^ want;
		if (changed.any())
		{
			for (int code = 1; code < 256; code++)
				if (changed[code])
					keyboard->key((uint8_t)code, want[code]);
			keysHeld = want;
		}
	}
	// What the stick sent, every few seconds while it moves: to tell, from a log, whether a
	// mouse that is too fast is too fast here or in the game.
	if (sentFrames > 0)
	{
		const auto now = std::chrono::steady_clock::now();
		if (now - lastReport >= std::chrono::seconds(4))
		{
			diag::mark("mouse: speed %d, dead zone %d%%, %s; sent %d counts in %d frames, the biggest step %d, the stick %.2f %.2f",
					(int)PadMouseSpeed, (int)PadMouseDeadZone, PadMouseCurve != 0 ? "fine aim" : "even", sentCounts, sentFrames,
					biggestStep, pad.rx, pad.ry);
			lastReport = now;
			sentCounts = sentFrames = biggestStep = 0;
		}
	}
}

std::string text()
{
	if (!options().usbInput)
		return "";
	std::string out;
	if (keyboardThere)
		out += "keyboard";
	if (mouseThere)
		out += out.empty() ? "mouse" : " and mouse";
	return out;
}

} // namespace ps5::usb

// What the running game read from its mouse: every 300 reads that were not zero (maple_cfg.cpp).
void ps5_mouseRead(int reads, long long total, int biggest)
{
	ps5::diag::mark("mouse: the game read %d moves, %.1f counts each on average, the biggest %d", reads,
			(double)total / std::max(1, reads), biggest);
}
