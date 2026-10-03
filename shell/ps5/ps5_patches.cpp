/*
	PSFlyCast - game patches: a 16:9 picture, 60 frames a second.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	<root>/patches/patches.txt lists, for a game and a region, the memory
	writes that make the game draw a 16:9 picture or run at 60 frames a second:
	the codes of the Flycast widescreen and 60 FPS chart
	(github.com/nexus382/Flycast-Widescreen-Compatability-And-Cheat-Chart),
	turned into writes by shell/ps5/patches/make_patches.py. They are patches,
	not cheats: each kind has its own switch among a game's options (off unless
	turned on), whatever cheat file the game has, and they are applied the way
	the cheat manager applies its own built-in widescreen codes, at each
	vertical blank.

	A game file is found by its name - the Redump names of the game in the
	patch's region - else by the game's title when the file's name carries no
	more than "(USA)", "(Europe)" or "(Japan)" (a file with no region in its
	name takes the console's). A demo, a beta or a release in one language is
	another build with addresses of its own, and gets a patch only where the
	list names it.
*/
#include "ps5_frontend.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "ps5_diag.h"
#include "stdclass.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <strings.h>
#include <vector>

namespace ps5::patches
{
namespace
{

struct Write
{
	enum Type { Set, IfEq, IfNe, IfLt, IfGt } type;
	u32 bits;
	u32 address;
	u32 value;
};

struct Patch
{
	std::string kind;		// "60fps", "widescreen"
	std::string region;		// "NA", "EU", "JP", or one release ("EU (Fr)")
	std::string title;
	std::vector<std::string> names;
	std::vector<Write> writes;
};

std::vector<Patch> patches;
bool loaded;

std::mutex activeMutex;
std::vector<Write> active;		// the running game's
bool listening;

// "Soul Calibur", "SoulCalibur" and "soul-calibur" are one title.
std::string plain(const std::string& text)
{
	std::string out;
	for (char c : text)
	{
		if (c == '\'')
			continue;
		if (c == '&')
			out += " and ";
		else if (isalnum((unsigned char)c))
			out += (char)tolower((unsigned char)c);
		else if (!out.empty() && out.back() != ' ')
			out += ' ';
	}
	while (!out.empty() && out.back() == ' ')
		out.pop_back();
	std::string joined;
	for (char c : out)
		if (c != ' ')
			joined += c;
	return joined;
}

void load()
{
	if (loaded)
		return;
	loaded = true;
	FILE *f = fopen((rootDir + "patches/patches.txt").c_str(), "r");
	if (f == nullptr)
	{
		ps5::diag::mark("patches: no patches/patches.txt");
		return;
	}
	char line[4096];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		if (line[0] == '#')
			continue;
		std::vector<std::string> fields(1);
		for (const char *c = line; *c != '\0' && *c != '\n' && *c != '\r'; c++)
		{
			if (*c == '\t')
				fields.emplace_back();
			else
				fields.back() += *c;
		}
		if (fields.size() < 5)
			continue;
		Patch patch;
		patch.kind = fields[0];
		patch.region = fields[1];
		patch.title = fields[2];
		size_t from = 0;
		while (from <= fields[3].size() && !fields[3].empty())
		{
			const size_t bar = std::min(fields[3].find('|', from), fields[3].size());
			patch.names.push_back(fields[3].substr(from, bar - from));
			from = bar + 1;
		}
		bool good = true;
		const char *at = fields[4].c_str();
		while (*at != '\0')
		{
			char type[8];
			unsigned bits, address, value;
			int used = 0;
			if (sscanf(at, " %7[a-z]:%u:%x:%x%n", type, &bits, &address, &value, &used) != 4)
			{
				good = false;
				break;
			}
			Write write;
			if (!strcmp(type, "set")) write.type = Write::Set;
			else if (!strcmp(type, "ifeq")) write.type = Write::IfEq;
			else if (!strcmp(type, "ifne")) write.type = Write::IfNe;
			else if (!strcmp(type, "iflt")) write.type = Write::IfLt;
			else if (!strcmp(type, "ifgt")) write.type = Write::IfGt;
			else good = false;
			// The Dreamcast's 16 MB: the patches are for its games.
			good = good && (bits == 8 || bits == 16 || bits == 32) && address <= 16_MB - 4;
			write.bits = bits;
			write.address = address;
			write.value = value;
			patch.writes.push_back(write);
			at += used;
			while (*at == ' ')
				at++;
		}
		if (good && !patch.writes.empty())
			patches.push_back(patch);
	}
	fclose(f);
	ps5::diag::mark("patches: %d in patches/patches.txt", (int)patches.size());
}

// A game file's name as the list has names: no extension, no disc number.
std::string gameName(const std::string& fileName)
{
	std::string name = get_file_basename(fileName);
	for (const char *word : { " (Disc ", " (Disk ", " (GD-ROM " })
	{
		const size_t at = name.find(word);
		if (at == std::string::npos)
			continue;
		const size_t end = name.find(')', at);
		if (end != std::string::npos)
			name.erase(at, end + 1 - at);
	}
	return name;
}

// The region of a file whose name says no more than it: "Game (USA)" is NA,
// "Game" the console's, "Game (USA) (Rev A)" or "Game (France)" none.
std::string plainRegion(const std::string& name)
{
	const size_t open = name.find_first_of("([");
	if (open == std::string::npos)
	{
		switch ((int)config::Region)
		{
		case 0: return "JP";
		case 1: return "NA";
		case 2: return "EU";
		default: return "";
		}
	}
	const std::string tags = name.substr(open);
	if (tags == "(USA)") return "NA";
	if (tags == "(Europe)") return "EU";
	if (tags == "(Japan)") return "JP";
	return "";
}

const Patch *find(const std::string& fileName, const std::string& kind)
{
	load();
	if (fileName.empty())
		return nullptr;
	const std::string name = gameName(fileName);
	for (const Patch& patch : patches)
		if (patch.kind == kind)
			for (const std::string& known : patch.names)
				if (strcasecmp(known.c_str(), name.c_str()) == 0)
					return &patch;
	const std::string region = plainRegion(name);
	if (region.empty())
		return nullptr;
	const std::string title = plain(name.substr(0, name.find_first_of("([")));
	for (const Patch& patch : patches)
	{
		if (patch.kind != kind || patch.region != region)
			continue;
		if (plain(patch.title) == title)
			return &patch;
		for (const std::string& known : patch.names)
			if (plain(known.substr(0, known.find_first_of("(["))) == title)
				return &patch;
	}
	return nullptr;
}

u32 read(const Write& w)
{
	const u32 address = 0x8C000000 + w.address;
	return w.bits == 8 ? ReadMem8_nommu(address) : w.bits == 16 ? ReadMem16_nommu(address) : ReadMem32_nommu(address);
}

// At each vertical blank, as the cheat manager applies its cheats: a write
// where the value is another, a test that lets the next write through.
void apply(Event, void *)
{
	std::lock_guard<std::mutex> lock(activeMutex);
	bool skip = false;
	for (const Write& w : active)
	{
		if (skip)
		{
			skip = false;
			continue;
		}
		const u32 now = read(w);
		switch (w.type)
		{
		case Write::Set:
			if (now != w.value)
			{
				const u32 address = 0x8C000000 + w.address;
				if (w.bits == 8) WriteMem8_nommu(address, (u8)w.value);
				else if (w.bits == 16) WriteMem16_nommu(address, (u16)w.value);
				else WriteMem32_nommu(address, w.value);
			}
			break;
		case Write::IfEq: skip = now != w.value; break;
		case Write::IfNe: skip = now == w.value; break;
		case Write::IfGt: skip = now <= w.value; break;
		case Write::IfLt: skip = now >= w.value; break;
		}
	}
}

// A game starts: the patches turned on for it.
void started()
{
	std::vector<Write> writes;
	const std::string id = settings.content.gameId;
	if (settings.platform.isConsole() && !id.empty() && !settings.network.online)
		for (const char *kind : { "60fps", "widescreen" })
		{
			if (!config::loadBool(id, optionKey(kind), false))
				continue;
			const Patch *patch = find(settings.content.fileName, kind);
			if (patch == nullptr)
				continue;
			writes.insert(writes.end(), patch->writes.begin(), patch->writes.end());
			ps5::diag::mark("patches: %s on for %s [%s], %d write(s)", kind, patch->title.c_str(), patch->region.c_str(),
					(int)patch->writes.size());
			// The game draws 16:9 into its 4:3 frame: stretched to the screen,
			// as Flycast does for its own widescreen codes.
			if (!strcmp(kind, "widescreen"))
				config::ScreenStretching.override(134);
		}
	std::lock_guard<std::mutex> lock(activeMutex);
	active = std::move(writes);
	if (!active.empty() && !listening)
		EventManager::listen(Event::VBlank, apply);
	else if (active.empty() && listening)
		EventManager::unlisten(Event::VBlank, apply);
	listening = !active.empty();
}

} // namespace

std::string optionKey(const char *kind)
{
	return std::string("ps5.patch.") + kind;
}

std::string label(const std::string& fileName, const char *kind)
{
	const Patch *patch = find(fileName, kind);
	return patch == nullptr ? "" : patch->title + " [" + patch->region + "]";
}

bool has(const std::string& fileName, const char *kind)
{
	return find(fileName, kind) != nullptr;
}

void init()
{
	load();
	EventManager::listen(Event::Start, [](Event, void *) { started(); });
	EventManager::listen(Event::Terminate, [](Event, void *) {
		std::lock_guard<std::mutex> lock(activeMutex);
		active.clear();
		if (listening)
			EventManager::unlisten(Event::VBlank, apply);
		listening = false;
	});
}

} // namespace ps5::patches
