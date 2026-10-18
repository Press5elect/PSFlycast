/*
	PSFlyCast - dumping the game's textures, for making a texture pack.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Flycast's own dumper decodes each texture the game loads and writes it as
	a PNG named by its hash, which is the name a replacement texture is found
	by (Settings > Video > Custom textures). This hands the picture on instead
	of writing it, so that it can be:
	  - sorted: 2D (drawn flat on the screen: the HUD, menus, sprites) or 3D
	    (everything else), by how the polygons that use it sit on the screen;
	  - put where it is wanted: the console's own storage, a USB drive, or the
	    network share (smb://server/share/PSFlyCast/Texture dumps);
	  - written by a thread of its own, so the game does not wait for a disk
	    or a share.
	The folders are <where>/<game ID>/2D and /3D.
*/
#pragma once
#include "types.h"
#include "cfg/option.h"

#include <string>

struct rend_context;

namespace ps5::texdump
{

// What to dump: nothing, 2D and 3D textures, only 2D, only 3D.
enum { ModeOff, ModeAll, Mode2D, Mode3D };
// Where: the console's own storage, the first USB drive, the network share.
enum { ToInternal, ToUsb, ToNetwork };
extern config::Option<int> Mode;
extern config::Option<int> Where;

// As a game starts: Flycast's dumper is switched on or off by the setting.
void apply();
// Flycast decoded a texture (RGBA, w x h): to be sorted and written. Takes a copy.
void submit(const std::string& gameId, u32 hash, int w, int h, const u8 *rgba);
// Once a frame, after the game's polygons were read: sorts what was submitted.
void frame(const rend_context& rend);
// The game ended: what is waiting is written, and the counts are kept for the screen.
void stop();
// For the screen: how many were saved and where, e.g. "212 saved (2D 80, 3D 132)"; empty when none.
std::string status();

}
