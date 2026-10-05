/*
	PSFlyCast - a USB keyboard and a USB mouse, as the Dreamcast's.

	Copyright 2026 the PSFlyCast contributors
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
#include "hw/maple/maple_cfg.h"

#include <algorithm>
#include <atomic>
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
	if (keyboards > 0 && !keyboard)
	{
		keyboard = std::make_shared<UsbKeyboard>();
		GamepadDevice::Register(keyboard);
	}
	if (mice > 0 && !mouse)
	{
		// Flycast keeps a pointer on the screen for each mouse and moves it
		// within the screen's size, which it learns from a pointer's place:
		// the middle of the display, once.
		const int width = ::settings.display.width > 0 ? ::settings.display.width : 1920;
		const int height = ::settings.display.height > 0 ? ::settings.display.height : 1080;
		SetMousePosition(width / 2, height / 2, width, height, 0);
		mouse = std::make_shared<UsbMouse>();
		GamepadDevice::Register(mouse);
	}
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
	if (options().usbInput && (keyboardThere || mouseThere))
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
				if (keyboardThere && keyboardAt < 0)
				{
					config::MapleMainDevices[port].override(MDT_Keyboard);
					keyboardAt = port;
				}
				else if (mouseThere && mouseAt < 0)
				{
					config::MapleMainDevices[port].override(MDT_Mouse);
					mouseAt = port;
				}
			}
			static const char *const letters[] = { "none", "A", "B", "C", "D" };
			diag::mark("usb: the Dreamcast's keyboard is in port %s, its mouse in port %s",
					letters[keyboardThere ? keyboardAt + 1 : 0], letters[mouseThere ? mouseAt + 1 : 0]);
		}
		else
		{
			// An arcade game with a keyboard or a mouse reads the first port's.
			keyboardAt = keyboardThere ? 0 : -1;
			mouseAt = mouseThere ? 0 : -1;
		}
	}
	keyboardPort = keyboardThere ? keyboardAt : -1;
	mousePort = mouseThere ? mouseAt : -1;
	if (keyboard)
		keyboard->set_maple_port(keyboardPort);
	if (mouse)
		mouse->set_maple_port(mousePort);
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
