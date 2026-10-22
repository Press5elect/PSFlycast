/*
	PSFlyCast - the DualSense, read through libScePad.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Up to four pads, one per signed-in user. The first is player 1 and drives
	the menus. Each pad is also a Flycast GamepadDevice, so the emulated
	controller, remapping and rumble go through Flycast's own input code.

	A pad is a controller port: player 1 is port A, and each other signed-in
	user whose controller is on gets the next free one, also while the title
	runs (the PS button on another DualSense signs its user in). A user who
	is signed in with the controller off takes no port. The Dreamcast's ports
	B to D are empty in Flycast's settings; while a pad is theirs, a
	controller with a memory card and a rumble pack is plugged into them, for
	that game and not into the settings.
*/
#pragma once
#include "types.h"
#include "cfg/option.h"

#include <string>
#include <vector>

namespace ps5::pad
{

// libScePad's button bits, as both console-proven PS5 projects read them
// (PS5 RetroArch src/input_ps5.cpp, PS5SX2 main-boot.cpp).
enum : u32
{
	L3 = 0x000002,
	R3 = 0x000004,
	Options = 0x000008,
	Up = 0x000010,
	Right = 0x000020,
	Down = 0x000040,
	Left = 0x000080,
	L2 = 0x000100,
	R2 = 0x000200,
	L1 = 0x000400,
	R1 = 0x000800,
	Triangle = 0x001000,
	Circle = 0x002000,
	Cross = 0x004000,
	Square = 0x008000,
	TouchPad = 0x100000,
	// Set while the system intercepts the pad (its own menu is up).
	Intercepted = 0x80000000,
};

struct State
{
	bool connected = false;
	u32 buttons = 0;		// held now
	u32 pressed = 0;		// went down since the previous poll
	u32 released = 0;		// went up since the previous poll
	float lx = 0, ly = 0;	// -1..1, dead zone applied
	float rx = 0, ry = 0;
	// The right stick as it is used: past its own dead zone (Settings > Controls), shaped by the
	// response curve, smoothed, and with each direction normal, inverted or off. -1..1.
	float px = 0, py = 0;
	float l2 = 0, r2 = 0;	// 0..1
};

// Flycast's input codes for the pad (its mapping files hold them): a button's
// is the index of its bit, and the two triggers are also axes.
constexpr u32 buttonCode(u32 bit) { return (u32)__builtin_ctz(bit); }
constexpr u32 AxisCodeL2 = 4, AxisCodeR2 = 5;

// Whether the left stick is also the d-pad: a setting, and a game can have its
// own. Automatic is on in the arcade games that read no analog stick, whose
// joystick is the d-pad, and off everywhere else.
enum { StickDpadAuto, StickDpadOn, StickDpadOff };
extern config::Option<int> StickAsDpad;

// What aims a light-gun game's gun: the left stick (Flycast's own), a finger
// on the touch pad, or the pad's motion sensor; and for motion, which way it
// goes (bit 0: left and right the other way round, bit 1: up and down).
// Settings, and a game can have its own.
enum { AimStick, AimTouch, AimMotion };
extern config::Option<int> LightGunAim;
extern config::Option<int> MotionAimDirection;

// Turbo: the buttons (libScePad bits) that are pressed again and again while
// held, and how fast: 0 slow (7 a second), 1 normal (10), 2 fast (15).
// Settings, and a game can have its own.
extern config::Option<int> TurboButtons;
extern config::Option<int> TurboRate;

// The pad as the Dreamcast's mouse and keyboard, for the games that read those (a first-person
// game's mouse look): a profile says whether the right stick moves a mouse and what each button,
// trigger and direction of the left stick does: stays the controller's, is blocked, a mouse
// button, the wheel, or a key. Which profile a game uses is a setting, and a game can have its
// own. Dreamcast games only. The profiles are in bind:: below.
// How a push of the right stick becomes movement: as it is; fine near the centre (square);
// quick near the centre (square root); slow at both ends (smoothstep).
enum { CurveLinear, CurvePrecise, CurveAggressive, CurveS };
enum { AxisNormal, AxisInverted, AxisOff };
extern config::Option<int> PadProfile;			// the profile in use: 0 to bind::Profiles - 1
extern config::Option<int> PadMouseSpeed;		// 1 to 20
extern config::Option<int> PadMouseDeadZone;	// 0 to 40 percent, beyond the stick's own
extern config::Option<int> PadMouseCurve;		// the right stick's response: CurveLinear, ...
extern config::Option<int> PadSmoothing;		// 0 to 90 percent of the last frame's push kept
extern config::Option<int> PadAxisX;			// left and right: AxisNormal, AxisInverted, AxisOff
extern config::Option<int> PadAxisY;			// up and down
// The controller the Dreamcast sees: the standard one (one stick), or the one
// with a second stick (Flycast's "Sega Controller XL", six axes), which only
// software that reads a second stick makes use of.
enum { PadTypeStandard, PadTypeSecondStick };
extern config::Option<int> PadType;
// The right stick left and right adds to the left stick's (the game turns with it as it does with the
// left stick), for games that read no second stick and take a mouse badly. Dreamcast games only.
extern config::Option<int> PadRightTurns;
// The right stick, left and right, presses the Dreamcast's L and R triggers, as far as it is pushed
// (a camera turned with the triggers). Before Right stick turns like the left stick, and not while
// the right stick is the mouse. Dreamcast games only.
extern config::Option<int> PadRightTriggers;

// The profiles. Four are built in and cannot change; four are the player's own, kept in the
// settings for every game. An input with anything but Controller on it is not the Dreamcast
// controller's as well: nothing is pressed twice, and a profile that gives nothing away leaves
// the controller whole.
namespace bind
{
enum { Inputs = 20 };
// The action of an entry: the input stays the Dreamcast controller's; it does nothing at all;
// a mouse button or the wheel; or Key + a USB usage code.
enum { Controller = 0, Blocked = 1, MouseLeft = 2, MouseRight = 3, MouseMiddle = 4, WheelUp = 5, WheelDown = 6, Key = 256 };
struct Profile
{
	bool mouse = false;			// the right stick moves the mouse
	int action[Inputs] = {};
};
enum { BuiltIn = 4, Own = 4, Profiles = BuiltIn + Own };
struct KeyName
{
	const char *name;
	int code;				// USB usage code
};
const char *profileName(int index);
bool profileIsOwn(int index);
Profile profile(int index);
// Changes one of the player's own; a built-in one is left as it is.
void setProfile(int index, const Profile& profile);
// The profile the setting chooses.
int chosen();
Profile current();
// Whether a profile makes the pad a mouse, or a keyboard, in a Dreamcast port.
bool wantsMouse(const Profile& profile);
bool wantsKeyboard(const Profile& profile);
const char *inputName(int input);
// The pad's button for an entry (libScePad bit), 0 for a trigger or a stick direction.
uint32_t inputButton(int input);
// Whether an entry is held on this pad state.
bool inputHeld(int input, const State& s);
const std::vector<KeyName>& keys();	// every key that can be given, in the order a list shows
std::string actionName(int action);
}

// Opens the pads and registers them with Flycast. Safe to call once.
void init();
void term();
// Reads every pad and feeds Flycast. Called once per frame by os_UpdateInputState.
void poll();
// Player 1's pad (or an all-released state when none is connected).
const State& player1();
// Number of connected pads.
int connectedCount();
// Looks, about once a second, for users who signed in or out and for
// controllers that were switched on, and gives each a port; a pad that joins
// while a Dreamcast game runs is plugged into it. Called once per frame from
// the interface's thread.
void hotplug();
// Before the emulated controllers are made: a controller, a memory card and
// a rumble pack for each of the ports B to D that a pad is on and the
// settings leave empty.
void plugPorts();
// Which ports have a pad, for the Settings: "2: ports A and B".
std::string portsText();
// The user whose pad is on a port (0 to 3); the first pad's user for a port without one; -1 for none.
int userOfPort(int port);

}

namespace ps5::usb
{
// The first pad as the Dreamcast's mouse and keyboard (ps5_usbinput.cpp), once
// a frame, when the game's settings ask for it.
void padInput(const ps5::pad::State& pad);
// What the pad gives to the mouse and keyboard in the Dreamcast's ports, and so does not give to
// its controller: no input is bound twice.
struct Taken
{
	uint32_t buttons = 0;
	bool l2 = false, r2 = false, lx = false, ly = false;
};
Taken padTaken();
}
