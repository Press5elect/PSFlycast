/*
	PSFlyCast - mods: a game changed a feature at a time, as text.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A mod is a text file: a folder with a mod.txt in it, or a <name>.mod file,
	in <root>/mods (and PSFlyCast/mods on a USB drive). It names the games it
	is for and lists features, each switched on and off by itself among the
	game's options (Details > Options, or the quick menu's game options); what
	is on is kept in the game's own section of emu.cfg.

		name = Faster start
		version = 1.0
		author = Someone
		description = What the mod does.
		games = Game Name (USA), Another Game

		[feature]
		name = Skip the logos
		description = What this part does.
		patch = 8C010000: 2a 00 02 24 > 00 00 00 00
		write = 8C010010: 00 00 80 3f

	A patch writes the new bytes only where the old ones are found, so another
	version of the game is left alone. A write sets the bytes at each vertical
	blank, as a cheat does. Addresses are in the Dreamcast's 16 MB of memory
	(8C000000 to 8CFFFFFF; 0C000000 and a bare offset are read the same way).
	A mod carries no game data and can be shared. Mods are off online.
	Every feature is off until it is switched on.
*/
#include "ps5_frontend.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "oslib/oslib.h"
#include "ps5_diag.h"
#include "stdclass.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <dirent.h>
#include <mutex>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <vector>

namespace ps5::mods
{
namespace
{

struct Change
{
	bool conditional;	// a patch: only where the old bytes are found
	u32 offset;			// into the 16 MB
	std::vector<u8> oldBytes;
	std::vector<u8> newBytes;
};

struct Feature
{
	std::string key;		// in the game's section: ps5.mod.<mod>.<feature>
	std::string name;
	std::string description;
	std::string mod;		// the mod's label
	std::vector<Change> changes;
};

struct Mod
{
	std::string path;
	std::string name, version, author, description;
	std::vector<std::string> games;
	std::vector<Feature> features;
	std::vector<std::string> problems;
};

std::vector<Mod> mods;
bool loaded;
std::string summaryText;

std::mutex activeMutex;
std::vector<Change> active;
bool listening;

bool exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

std::string trim(const std::string& text)
{
	size_t a = 0, b = text.size();
	while (a < b && isspace((unsigned char)text[a]))
		a++;
	while (b > a && isspace((unsigned char)text[b - 1]))
		b--;
	return text.substr(a, b - a);
}

// "Soul Calibur", "SoulCalibur" and "soul-calibur" are one title.
std::string plain(const std::string& text)
{
	std::string out;
	for (char c : text)
		if (isalnum((unsigned char)c))
			out += (char)tolower((unsigned char)c);
	return out;
}

std::string slug(const std::string& text)
{
	std::string out;
	for (char c : text)
		out += isalnum((unsigned char)c) ? (char)tolower((unsigned char)c) : '_';
	return out;
}

// A file's name as a title: no folder, extension or disc number.
std::string titleOf(const std::string& fileName)
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

// A game in a mod's list is a game file's whole name ("Game (USA)"), or its
// title without the tags.
bool forGame(const Mod& mod, const std::string& fileName)
{
	if (fileName.empty())
		return false;
	const std::string name = titleOf(fileName);
	const std::string bare = plain(name.substr(0, name.find_first_of("([")));
	for (const std::string& game : mod.games)
	{
		if (strcasecmp(game.c_str(), name.c_str()) == 0)
			return true;
		if (game.find_first_of("([") == std::string::npos && plain(game) == bare && !bare.empty())
			return true;
	}
	return false;
}

bool parseAddress(const std::string& text, u32& offset)
{
	char *end = nullptr;
	const unsigned long value = strtoul(text.c_str(), &end, 16);
	if (end == text.c_str() || *end != '\0')
		return false;
	offset = (u32)(value & 0x00ffffff);
	return true;
}

bool parseBytes(const std::string& text, std::vector<u8>& out)
{
	out.clear();
	size_t at = 0;
	while (at < text.size())
	{
		while (at < text.size() && isspace((unsigned char)text[at]))
			at++;
		if (at >= text.size())
			break;
		size_t end = at;
		while (end < text.size() && isxdigit((unsigned char)text[end]))
			end++;
		if (end == at || end - at != 2 || (end < text.size() && !isspace((unsigned char)text[end])))
			return false;
		out.push_back((u8)strtoul(text.substr(at, 2).c_str(), nullptr, 16));
		at = end;
	}
	return !out.empty();
}

// "8C010000: 2a 00 02 24 > 00 00 00 00" (patch) or "8C010010: 00 00 80 3f" (write).
bool parseChange(const std::string& value, bool patch, Change& change, std::string& why)
{
	const size_t colon = value.find(':');
	if (colon == std::string::npos || !parseAddress(trim(value.substr(0, colon)), change.offset))
	{
		why = "an address and bytes are \"8C010000: 00 11 22 33\"";
		return false;
	}
	std::string rest = value.substr(colon + 1);
	change.conditional = patch;
	if (patch)
	{
		const size_t arrow = rest.find('>');
		if (arrow == std::string::npos || !parseBytes(rest.substr(0, arrow), change.oldBytes)
				|| !parseBytes(rest.substr(arrow + 1), change.newBytes) || change.oldBytes.size() != change.newBytes.size())
		{
			why = "a patch is \"8C010000: old bytes > new bytes\", as many on each side";
			return false;
		}
	}
	else if (!parseBytes(rest, change.newBytes))
	{
		why = "bytes are two hexadecimal digits each, as \"00 11 22 33\"";
		return false;
	}
	if ((u64)change.offset + change.newBytes.size() > 16_MB)
	{
		why = "it reaches past the Dreamcast's 16 MB of memory";
		return false;
	}
	return true;
}

Mod readMod(const std::string& path, const std::string& text)
{
	Mod mod;
	mod.path = path;
	Feature *feature = nullptr;
	int number = 0;
	size_t from = 0;
	while (from <= text.size())
	{
		size_t end = text.find('\n', from);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = trim(text.substr(from, end - from));
		from = end + 1;
		number++;
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line[0] == '[')
		{
			if (strcasecmp(line.c_str(), "[feature]") == 0)
			{
				mod.features.emplace_back();
				feature = &mod.features.back();
			}
			else
				mod.problems.push_back("line " + std::to_string(number) + ": only [feature] is a section");
			continue;
		}
		const size_t equals = line.find('=');
		if (equals == std::string::npos)
		{
			mod.problems.push_back("line " + std::to_string(number) + " is not \"name = value\"");
			continue;
		}
		std::string key = trim(line.substr(0, equals));
		for (char& c : key)
			c = (char)tolower((unsigned char)c);
		const std::string value = trim(line.substr(equals + 1));
		if (feature == nullptr)
		{
			if (key == "name") mod.name = value;
			else if (key == "version") mod.version = value;
			else if (key == "author") mod.author = value;
			else if (key == "description") mod.description += (mod.description.empty() ? "" : " ") + value;
			else if (key == "games")
			{
				size_t at = 0;
				while (at <= value.size())
				{
					size_t comma = value.find(',', at);
					if (comma == std::string::npos)
						comma = value.size();
					const std::string game = trim(value.substr(at, comma - at));
					if (!game.empty())
						mod.games.push_back(game);
					at = comma + 1;
				}
			}
			else
				mod.problems.push_back("line " + std::to_string(number) + ": \"" + key + "\" is not a mod's own field");
			continue;
		}
		if (key == "name") feature->name = value;
		else if (key == "description") feature->description += (feature->description.empty() ? "" : " ") + value;
		else if (key == "patch" || key == "write")
		{
			Change change;
			std::string why;
			if (parseChange(value, key == "patch", change, why))
				feature->changes.push_back(change);
			else
				mod.problems.push_back("line " + std::to_string(number) + ": " + why);
		}
		else
			mod.problems.push_back("line " + std::to_string(number) + ": \"" + key + "\" is not a feature's field");
	}
	if (mod.name.empty())
		mod.problems.push_back("it has no name");
	if (mod.games.empty())
		mod.problems.push_back("it names no game (games = Game Name, ...)");
	const std::string label = mod.version.empty() ? mod.name : mod.name + " " + mod.version;
	std::vector<Feature> kept;
	for (Feature& f : mod.features)
	{
		if (f.name.empty())
		{
			mod.problems.push_back("a feature has no name");
			continue;
		}
		if (f.changes.empty())
		{
			mod.problems.push_back("feature \"" + f.name + "\" has no patch or write");
			continue;
		}
		f.mod = label;
		f.key = "ps5.mod." + slug(mod.name) + "." + slug(f.name);
		if (f.description.empty())
			f.description = mod.description;
		kept.push_back(f);
	}
	mod.features = kept;
	return mod;
}

std::vector<std::string> folders()
{
	std::vector<std::string> out{ rootDir + "mods" };
	if (ps5::options().usb)
		for (int i = 0; i < 8; i++)
			out.push_back("/mnt/usb" + std::to_string(i) + "/PSFlyCast/mods");
	return out;
}

bool readFile(const std::string& path, std::string& text)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	char buffer[4096];
	size_t n;
	text.clear();
	while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0 && text.size() < (1u << 20))
		text.append(buffer, n);
	fclose(f);
	return true;
}

void load()
{
	if (loaded)
		return;
	loaded = true;
	int problems = 0;
	for (const std::string& folder : folders())
	{
		DIR *dir = opendir(folder.c_str());
		if (dir == nullptr)
			continue;
		std::vector<std::string> files;
		while (const dirent *entry = readdir(dir))
		{
			if (entry->d_name[0] == '.')
				continue;
			const std::string name = entry->d_name;
			const std::string path = folder + "/" + name;
			if (name.size() > 4 && !strcasecmp(name.c_str() + name.size() - 4, ".mod"))
				files.push_back(path);
			else if (exists(path + "/mod.txt"))
				files.push_back(path + "/mod.txt");
		}
		closedir(dir);
		std::sort(files.begin(), files.end());
		for (const std::string& file : files)
		{
			std::string text;
			if (!readFile(file, text))
			{
				ps5::diag::mark("mods: %s: could not be read", file.c_str());
				problems++;
				continue;
			}
			Mod mod = readMod(file, text);
			for (const std::string& problem : mod.problems)
				ps5::diag::mark("mods: %s: %s", file.c_str(), problem.c_str());
			problems += (int)mod.problems.size();
			if (!mod.name.empty() && !mod.games.empty())
				mods.push_back(std::move(mod));
		}
	}
	summaryText = mods.empty() && problems == 0 ? std::string()
			: std::to_string(mods.size()) + " mod" + (mods.size() == 1 ? "" : "s")
					+ (problems == 0 ? "" : ", " + std::to_string(problems) + " problem" + (problems == 1 ? "" : "s")
							+ " (boot log, \"mods:\")");
	ps5::diag::mark("mods: %d found, %d problem(s)", (int)mods.size(), problems);
}

bool isOn(const std::string& gameId, const Feature& feature)
{
	return config::loadBool(gameId, feature.key, false);
}

void apply(Event, void *)
{
	std::lock_guard<std::mutex> lock(activeMutex);
	for (const Change& change : active)
	{
		const u32 base = 0x8C000000 + change.offset;
		if (change.conditional)
		{
			// The old bytes, all of them, or the game is another version.
			bool found = true;
			bool done = true;
			for (size_t i = 0; i < change.oldBytes.size() && found; i++)
			{
				const u8 now = ReadMem8_nommu(base + (u32)i);
				found = now == change.oldBytes[i] || now == change.newBytes[i];
				done = done && now == change.newBytes[i];
			}
			if (!found || done)
				continue;
		}
		for (size_t i = 0; i < change.newBytes.size(); i++)
			if (ReadMem8_nommu(base + (u32)i) != change.newBytes[i])
				WriteMem8_nommu(base + (u32)i, change.newBytes[i]);
	}
}

void started()
{
	std::vector<Change> changes;
	const std::string id = settings.content.gameId;
	int features = 0;
	if (settings.platform.isConsole() && !id.empty() && !settings.network.online)
	{
		load();
		for (const Mod& mod : mods)
			if (forGame(mod, settings.content.fileName))
				for (const Feature& feature : mod.features)
					if (isOn(id, feature))
					{
						features++;
						changes.insert(changes.end(), feature.changes.begin(), feature.changes.end());
					}
	}
	if (features != 0)
		ps5::diag::mark("mods: %d feature(s) on for %s", features, id.c_str());
	std::lock_guard<std::mutex> lock(activeMutex);
	active = std::move(changes);
	if (!active.empty() && !listening)
		EventManager::listen(Event::VBlank, apply);
	else if (active.empty() && listening)
		EventManager::unlisten(Event::VBlank, apply);
	listening = !active.empty();
}

} // namespace

std::vector<Entry> forFile(const std::string& fileName)
{
	load();
	// The rows point at these, so they are kept, once for each feature.
	static std::map<std::string, std::array<std::string, 3>> kept;
	std::vector<Entry> out;
	for (const Mod& mod : mods)
		if (forGame(mod, fileName))
			for (const Feature& feature : mod.features)
			{
				auto& text = kept[feature.key];
				text[0] = feature.key;
				text[1] = feature.mod + ": " + feature.name;
				text[2] = feature.description.empty() ? "A mod's feature" : feature.description;
				out.push_back({ text[0].c_str(), text[1].c_str(), text[2].c_str() });
			}
	return out;
}

std::string summary()
{
	load();
	return summaryText;
}

void init()
{
	EventManager::listen(Event::Start, [](Event, void *) { started(); });
	EventManager::listen(Event::Terminate, [](Event, void *) {
		std::lock_guard<std::mutex> lock(activeMutex);
		active.clear();
		if (listening)
			EventManager::unlisten(Event::VBlank, apply);
		listening = false;
	});
}

} // namespace ps5::mods
