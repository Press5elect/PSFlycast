// PSFlyCast - the display's variable refresh, found while the title runs (ps5_videoout.cpp).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast from Crux2's PS5SystemModules (ps5/frontend/ps5_keyboard.h, Crux2 by Press5elect).
#pragma once

#include <cstdint>

namespace ps5::modules
{
// A function of a module this process holds, found while the title runs: moduleName is the start
// of the module's file name ("libSceVideoOut" matches "libSceVideoOut.sprx"); the function is
// looked up by its name, then by its NID (the console's loader names exports by NID), then with
// dlsym, in that module, then in every module the process lists, then by the handles the system's
// modules have. A function found in no module is said in the boot log, with the modules listed.
void* findFunction(const char* moduleName, const char* function, const char* nid);
}

// Called by the graphics driver (PS5_Mesa patch 0017) to leave the output's fixed rate.
extern "C" int ps5_title_videoout_vrr_unpeg(int32_t handle);
