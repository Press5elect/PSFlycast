/*
	PSFlyCast - the DualSense, read through libScePad.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Up to four pads, one per logged-in user. The first is player 1 and drives
	the menus. Each pad is also a Flycast GamepadDevice, so the emulated
	controller, remapping and rumble go through Flycast's own input code.
*/
#pragma once
#include "types.h"

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
	float l2 = 0, r2 = 0;	// 0..1
};

// Flycast's input codes for the pad (its mapping files hold them): a button's
// is the index of its bit, and the two triggers are also axes.
constexpr u32 buttonCode(u32 bit) { return (u32)__builtin_ctz(bit); }
constexpr u32 AxisCodeL2 = 4, AxisCodeR2 = 5;

// Opens the pads and registers them with Flycast. Safe to call once.
void init();
void term();
// Reads every pad and feeds Flycast. Called once per frame by os_UpdateInputState.
void poll();
// Player 1's pad (or an all-released state when none is connected).
const State& player1();
// Number of connected pads.
int connectedCount();

}
