/*
	PSFlyCast - objects from further away, for the Sonic Adventure games.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	"Object draw distance" in a game's options (ps5.drawdist in its section
	of emu.cfg: 100 is off, 200 twice as far). While the game runs, its
	memory is searched for the tables that say from how far each kind of
	object exists, and the distances in them are multiplied
	(ps5_drawdist_scan.h says how they are recognised, and where the idea is
	from). A game loads a stage's tables when it loads the stage, so the
	search goes on for as long as the game runs: a third of a millisecond of
	it at each vertical blank, the whole of memory every second or so.

	Experimental, and for the two games it is made for: the tables are
	recognised by their shape, not known by address, and the game's own
	limits on how many objects are alive are not raised. flycast-boot.log
	says what was found ("drawdist: ...").
*/
#include "ps5_frontend.h"
#include "ps5_drawdist_scan.h"

#include "cfg/cfg.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "ps5_diag.h"
#include "stdclass.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace ps5::drawdist
{
namespace
{
constexpr u32 CHUNK = 16 * 1024;

std::unique_ptr<Scanner> scanner;
std::string scanned;		// the game it is for
u32 position;
int tablesLogged, tablesFound, distancesScaled;
bool listening;

void pass(Event, void *)
{
	if (scanner == nullptr)
		return;
	// A third of a millisecond of this frame, a megabyte at the most: where
	// memory is full of pointers the search is slow, elsewhere it flies.
	std::vector<Table> found;
	const auto begin = std::chrono::steady_clock::now();
	for (int chunk = 0; chunk < 64; chunk++)
	{
		scanner->scan(position, CHUNK, found);
		position = position + CHUNK >= RAM_SIZE ? 0 : position + CHUNK;
		if (std::chrono::steady_clock::now() - begin > std::chrono::microseconds(300))
			break;
	}
	for (const Table& table : found)
	{
		tablesFound++;
		distancesScaled += table.scaled + table.given;
		if (tablesLogged < 40)
		{
			tablesLogged++;
			ps5::diag::mark("drawdist: a table at %08X, %d objects of %d bytes (%s...): %d distances multiplied, %d objects given one",
					table.address, table.entries, table.stride, table.names.c_str(), table.scaled, table.given);
			// What the table held (name=use:distance), for the first few: how
			// what is assumed about it is checked.
			if (tablesLogged <= 4)
				for (size_t at = 0; at < table.detail.size(); )
				{
					size_t end = std::min(table.detail.size(), at + 380);
					if (end < table.detail.size())
						end = table.detail.rfind(' ', end) + 1;
					if (end <= at)
						break;
					ps5::diag::mark("drawdist:   %s", table.detail.substr(at, end - at).c_str());
					at = end;
				}
		}
		else if (tablesLogged == 40)
		{
			tablesLogged++;
			ps5::diag::mark("drawdist: more tables are not listed");
		}
	}
}

void stop()
{
	if (listening)
		EventManager::unlisten(Event::VBlank, pass);
	listening = false;
	if (scanner != nullptr)
		ps5::diag::mark("drawdist: %d table(s) found in all, %d distance(s) multiplied", tablesFound, distancesScaled);
	scanner.reset();
	scanned.clear();
}

// A game starts, or goes on after the menu. What the option says is taken
// once for the game loaded (the option says so: "the next time this game
// starts"): distances already multiplied stay as they are until then.
void started()
{
	if (settings.content.path == scanned)
		return;
	stop();
	scanned = settings.content.path;
	const std::string id = settings.content.gameId;
	const int percent = settings.platform.isConsole() && !id.empty() && !settings.network.online
			? config::loadInt(id, "ps5.drawdist", 100) : 100;
	if (percent <= 100)
		return;
	const float times = percent / 100.f;
	scanner = std::make_unique<Scanner>(&mem_b[0], RAM_SIZE, 0x8c000000u, times * times);
	position = 0;
	tablesLogged = tablesFound = distancesScaled = 0;
	EventManager::listen(Event::VBlank, pass);
	listening = true;
	ps5::diag::mark("drawdist: objects from %.1f times as far, for %s", times, id.c_str());
}
} // namespace

void init()
{
	EventManager::listen(Event::Start, [](Event, void *) { started(); });
	EventManager::listen(Event::Terminate, [](Event, void *) { stop(); });
}

} // namespace ps5::drawdist
