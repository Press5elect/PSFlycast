/*
	PSFlyCast - cheats from RetroArch .cht files.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Flycast's cheat manager reads RetroArch's cheat files (address, value,
	type). <root>/cheats holds them: the title ships the Dreamcast set of the
	libretro database (github.com/libretro/libretro-database, cht/Sega -
	Dreamcast, CC BY-SA 4.0), and the user can add more.

	Those files are named after the game ("Re-Volt.cht", "Crazy Taxi 2 - Crazy
	Taxi 2 (Japanese).cht"), not after the disc image, so the file for a game is
	found by comparing the words of both names; a close match is loaded when the
	game starts, with every cheat off, and the in-game menu lists the other
	files for when the match is wrong. Which cheats are on is saved per game in
	<root>/data/cheats/<game id>.txt and comes back the next time.
*/
#include "ps5_frontend.h"

#include "cheats.h"
#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/sh4/sh4_if.h"
#include "log/Log.h"
#include "oslib/oslib.h"
#include "ps5_diag.h"
#include "stdclass.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ps5::cheats
{
namespace
{

std::string cheatsDir()
{
	return rootDir + "cheats/";
}

std::string idFile(const std::string& gameId)
{
	std::string name;
	for (char c : gameId)
		name += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
	return rootDir + "data/cheats/" + name + ".txt";
}

bool exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

struct Name
{
	std::set<std::string> words;
	std::string joined;	// the words run together: "Soulcalibur" is "Soul Calibur"
	char region = 0;	// 'J', 'E', or 0 for USA / unspecified
};

// The words of a title, lower case, without bracketed tags, articles and
// version numbers; the region from its tags.
Name parse(const std::string& text)
{
	Name name;
	std::string lower;
	for (char c : text)
		lower += (char)tolower((unsigned char)c);
	if (lower.find("japan") != std::string::npos)
		name.region = 'J';
	else if (lower.find("europe") != std::string::npos || lower.find("(pal") != std::string::npos)
		name.region = 'E';
	// Drop (...) and [...] groups.
	std::string plain;
	int depth = 0;
	for (char c : lower)
	{
		if (c == '(' || c == '[')
			depth++;
		else if (c == ')' || c == ']')
			depth = std::max(0, depth - 1);
		else if (depth == 0)
			plain += c;
	}
	std::vector<std::string> words;
	std::string word;
	for (char c : plain + " ")
	{
		if (isalnum((unsigned char)c))
			word += c;
		else if (c == '\'')
			continue;	// "Tony Hawk's" is one word
		else if (!word.empty())
		{
			words.push_back(word);
			word.clear();
		}
	}
	for (size_t i = 0; i < words.size(); i++)
	{
		const std::string& w = words[i];
		if (w == "the" || w == "a" || w == "an" || w == "of" || w == "and")
			continue;
		if (w == "version" || w == "ver" || w == "rev")
		{
			// "Version 1.1": the number belongs to it.
			while (i + 1 < words.size() && isdigit((unsigned char)words[i + 1][0]))
				i++;
			continue;
		}
		name.words.insert(w);
		name.joined += w;
	}
	return name;
}

bool isNumber(const std::string& word)
{
	return !word.empty() && std::all_of(word.begin(), word.end(), [](char c) { return isdigit((unsigned char)c); });
}

// 0..1: how alike two titles are; sequels ("2") must agree.
float similarity(const Name& a, const Name& b)
{
	if (a.words.empty() || b.words.empty())
		return 0.f;
	size_t common = 0;
	for (const std::string& w : a.words)
		common += b.words.count(w);
	float score = (float)common / (float)(a.words.size() + b.words.size() - common);
	if (a.joined == b.joined)
		score = 1.f;
	for (const Name *one : { &a, &b })
	{
		const Name& other = one == &a ? b : a;
		for (const std::string& w : one->words)
			if (isNumber(w) && other.words.count(w) == 0)
				score *= 0.5f;
	}
	if (a.region == b.region)
		score += 0.05f;
	else if (b.region != 0)
		score -= 0.1f;
	return score;
}

// A cheat file serves the versions its name lists, joined by " - ".
float fileScore(const Name& game, const std::string& fileBase)
{
	float best = similarity(game, parse(fileBase));
	size_t start = 0;
	for (;;)
	{
		const size_t sep = fileBase.find(" - ", start);
		const std::string part = fileBase.substr(start, sep == std::string::npos ? sep : sep - start);
		best = std::max(best, similarity(game, parse(part)));
		if (sep == std::string::npos)
			break;
		start = sep + 3;
	}
	return best;
}

std::vector<std::pair<float, std::string>> ranked(const std::string& gameFileName)
{
	std::vector<std::pair<float, std::string>> files;
	const std::string base = get_file_basename(gameFileName);
	const Name game = parse(base);
	// Also without its subtitle: "Marvel vs. Capcom 2 - New Age of Heroes".
	Name title = parse(base.substr(0, base.find(" - ")));
	title.region = game.region;
	if (DIR *dir = opendir(cheatsDir().c_str()))
	{
		while (const dirent *entry = readdir(dir))
		{
			const std::string file = entry->d_name;
			if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".cht") != 0)
				continue;
			const std::string name = file.substr(0, file.size() - 4);
			files.emplace_back(std::max(fileScore(game, name), fileScore(title, name) - 0.02f), file);
		}
		closedir(dir);
	}
	std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
		return a.first != b.first ? a.first > b.first : a.second < b.second;
	});
	return files;
}

} // namespace

std::vector<std::string> candidates(const std::string& gameFileName)
{
	std::vector<std::string> names;
	for (const auto& [score, file] : ranked(gameFileName))
		names.push_back(file);
	return names;
}

std::string current()
{
	const std::string name = config::loadStr("ps5-cheats", settings.content.gameId);
	return name == "-" ? "" : name;
}

// Turns on the cheats the user had on for this game.
void applyStates()
{
	FILE *f = fopen(idFile(settings.content.gameId).c_str(), "r");
	if (f == nullptr)
		return;
	std::set<std::string> on;
	char line[512];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		if (!s.empty())
			on.insert(s);
	}
	fclose(f);
	for (size_t i = 0; i < cheatManager.cheatCount(); i++)
		if (on.count(cheatManager.cheatDescription(i)) != 0)
			cheatManager.enableCheat(i, true);
}

void select(const std::string& name)
{
	const std::string gameId = settings.content.gameId;
	if (gameId.empty())
		return;
	// "-" is "none, by choice": a close match is not loaded again.
	config::saveStr("ps5-cheats", gameId, name.empty() ? "-" : name);
	unlink(idFile(gameId).c_str());
	if (name.empty())
	{
		// No file: forget the game's cheats and let the manager start over
		// (it keeps the fixes it applies to a few games by itself).
		config::saveStr("cheats", gameId, "");
		cheatManager.reset("");
		cheatManager.reset(gameId);
		return;
	}
	cheatManager.loadCheatFile(cheatsDir() + name);
}

void autoLoad()
{
	const std::string gameId = settings.content.gameId;
	if (gameId.empty())
		return;
	if (cheatManager.cheatCount() == 0)
	{
		std::string name = config::loadStr("ps5-cheats", gameId);
		if (name == "-")
			return;		// the user chose "none" for this game
		if (name.empty())
		{
			const auto files = ranked(settings.content.fileName);
			if (files.empty() || files[0].first < 0.8f)
				return;
			name = files[0].second;
			NOTICE_LOG(COMMON, "Cheats: %s for %s (match %.2f)", name.c_str(), settings.content.fileName.c_str(),
					files[0].first);
			config::saveStr("ps5-cheats", gameId, name);
		}
		if (!exists(cheatsDir() + name))
			return;
		cheatManager.loadCheatFile(cheatsDir() + name);
	}
	applyStates();
}

// Which cheats are on, by description, in <root>/data/cheats/<game id>.txt
// (Flycast's own cheat files always start with every cheat off).
void saveStates()
{
	const std::string gameId = settings.content.gameId;
	if (gameId.empty())
		return;
	FILE *f = fopen(idFile(gameId).c_str(), "w");
	if (f == nullptr)
		return;
	for (size_t i = 0; i < cheatManager.cheatCount(); i++)
		if (cheatManager.cheatEnabled(i))
			fprintf(f, "%s\n", cheatManager.cheatDescription(i).c_str());
	fclose(f);
}

std::vector<std::string> descriptions(const std::string& file)
{
	// cheatN_desc = "..." lines, in the order of N.
	std::vector<std::pair<int, std::string>> found;
	FILE *f = fopen((cheatsDir() + file).c_str(), "r");
	if (f == nullptr)
		return {};
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		int number = 0, consumed = 0;
		if (sscanf(line, " cheat%d_desc %n", &number, &consumed) != 1 || consumed == 0)
			continue;
		const char *first = strchr(line + consumed, '"');
		const char *last = first != nullptr ? strrchr(first + 1, '"') : nullptr;
		if (first == nullptr || last == nullptr)
			continue;
		found.emplace_back(number, std::string(first + 1, last));
	}
	fclose(f);
	std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<std::string> names;
	for (auto& [number, name] : found)
		names.push_back(std::move(name));
	return names;
}

std::string chosen(const std::string& gameId)
{
	return gameId.empty() ? "" : config::loadStr("ps5-cheats", gameId);
}

std::string closest(const std::string& gameFileName)
{
	const auto files = ranked(gameFileName);
	return files.empty() || files[0].first < 0.8f ? "" : files[0].second;
}

void choose(const std::string& gameId, const std::string& name)
{
	if (gameId.empty())
		return;
	config::saveStr("ps5-cheats", gameId, name.empty() ? "-" : name);
	// Flycast loads this file when the game starts.
	config::saveStr("cheats", gameId, name.empty() ? "" : cheatsDir() + name);
	unlink(idFile(gameId).c_str());
}

std::vector<std::string> enabled(const std::string& gameId)
{
	std::vector<std::string> on;
	FILE *f = gameId.empty() ? nullptr : fopen(idFile(gameId).c_str(), "r");
	if (f == nullptr)
		return on;
	char line[512];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		if (!s.empty())
			on.push_back(s);
	}
	fclose(f);
	return on;
}

void setEnabled(const std::string& gameId, const std::vector<std::string>& on)
{
	if (gameId.empty())
		return;
	FILE *f = fopen(idFile(gameId).c_str(), "w");
	if (f == nullptr)
		return;
	for (const std::string& name : on)
		fprintf(f, "%s\n", name.c_str());
	fclose(f);
	chmod(idFile(gameId).c_str(), 0666);
}

void init()
{
	EventManager::listen(Event::Start, [](Event, void *) { autoLoad(); });
}

} // namespace ps5::cheats

// ------------------------------------------------------------ game IDs, states

namespace ps5::games
{
namespace
{

std::mutex idsMutex;
std::map<std::string, std::string> ids;		// game path -> ID
bool idsLoaded;
std::atomic<int> stateSlot{-1};
std::atomic<bool> fresh{false};

std::string idsFile()
{
	return rootDir + "data/game-ids.txt";
}

void loadIds()
{
	if (idsLoaded)
		return;
	idsLoaded = true;
	FILE *f = fopen(idsFile().c_str(), "r");
	if (f == nullptr)
		return;
	char line[2048];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		const size_t tab = s.find('\t');
		if (tab != std::string::npos && tab > 0 && tab + 1 < s.size())
			ids[s.substr(0, tab)] = s.substr(tab + 1);
	}
	fclose(f);
}

// The first minute of a game, in the boot log: what it was started with, and
// every five seconds how far it got. A game that shows nothing is then either
// drawing frames that come out black (the frame count goes up), or not drawing
// (the count stands still, and the program counter says where the game waits),
// or has the picture turned off.
std::atomic<int> watchRun{0};		// one more for each start
std::string watched;				// the path of the game being watched

void watch(int run)
{
	using namespace std::chrono;
	const auto begin = steady_clock::now();
	u32 lastFrames = FrameCount;
	for (int tick = 1; tick <= 12; tick++)
	{
		std::this_thread::sleep_until(begin + seconds(tick * 5));
		if (watchRun != run)
			return;
		if (!emu.running())
			continue;
		// Four looks at the program counter, a moment apart: the same address
		// (or a few close ones) each time is a loop the game does not leave.
		u32 pc[4];
		for (u32& value : pc)
		{
			value = p_sh4rcb->cntx.pc;
			std::this_thread::sleep_for(milliseconds(3));
		}
		const u32 frames = FrameCount;
		const char *picture = !FB_R_CTRL.fb_enable ? "off" : VO_CONTROL.blank_video ? "blanked" : "on";
		ps5::diag::mark("game: %2d s: %u frames drawn (%u in all), picture %s, program at %08x %08x %08x %08x",
				tick * 5, frames - lastFrames, frames, picture, pc[0], pc[1], pc[2], pc[3]);
		lastFrames = frames;
	}
}

void describeStart(const std::string& path, const std::string& id)
{
	const size_t slash = path.find_last_of('/');
	const bool builtIn = settings.platform.isConsole() && (config::UseReios || !biosFound());
	ps5::diag::mark("game: %s [%s]", slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1, id.c_str());
	ps5::diag::mark("game: BIOS %s, transparency %s, %d lines, cable %d, region %d, broadcast %d, recompiler %s",
			builtIn ? "built-in" : settings.platform.isConsole() ? "from the bios folder" : "of the arcade board",
			config::RendererType == RenderType::Vulkan_OIT ? "per-pixel" : config::PerStripSorting ? "per-strip" : "per-triangle",
			(int)config::RenderResolution, (int)config::Cable, (int)config::Region, (int)config::Broadcast,
			config::DynarecEnabled ? "on" : "off");
	ps5::diag::mark("game: native depth %s, framebuffer emulation %s, rendered textures to VRAM %s, mipmaps %s,"
			" fast disc loading %s, own options %s",
			config::NativeDepthInterpolation ? "on" : "off", config::EmulateFramebuffer ? "on" : "off",
			config::RenderToTextureBuffer ? "on" : "off", config::UseMipmaps ? "on" : "off",
			config::FastGDRomLoad ? "on" : "off", config::Settings::instance().hasPerGameConfig() ? "yes" : "no");
}

// A game has started: its ID is known now, and the state it was asked to
// start from is loaded (where Flycast itself loads one, with "Auto load
// state").
void started()
{
	// A restart: Flycast looks at "Auto load state" right after this event.
	if (fresh.exchange(false))
		config::AutoLoadState.override(false);
	const std::string path = settings.content.path, id = settings.content.gameId;
	// Start is also what leaving the in-game menu does: one description and
	// one watch for each game loaded.
	const bool loaded = !path.empty() && path != watched;
	if (loaded)
	{
		watched = path;
		describeStart(path, id);
		std::thread(watch, ++watchRun).detach();
	}
	if (!path.empty() && !id.empty())
	{
		std::lock_guard<std::mutex> lock(idsMutex);
		loadIds();
		if (ids[path] != id)
		{
			ids[path] = id;
			if (FILE *f = fopen(idsFile().c_str(), "w"))
			{
				for (const auto& [gamePath, gameId] : ids)
					fprintf(f, "%s\t%s\n", gamePath.c_str(), gameId.c_str());
				fclose(f);
				chmod(idsFile().c_str(), 0666);
			}
		}
	}
	const int slot = stateSlot.exchange(-1);
	if (slot >= 0 && !path.empty())
	{
		config::SavestateSlot = slot;
		dc_loadstate(slot);
	}
}

} // namespace

std::string knownId(const std::string& path)
{
	std::lock_guard<std::mutex> lock(idsMutex);
	loadIds();
	const auto it = ids.find(path);
	return it == ids.end() ? "" : it->second;
}

void loadStateAtStart(int slot)
{
	stateSlot = slot;
}

void startFresh()
{
	fresh = true;
}

bool biosFound()
{
	return !hostfs::findFlash("dc_", "%boot.bin;%boot.bin.bin;%bios.bin;%bios.bin.bin").empty();
}

void init()
{
	EventManager::listen(Event::Start, [](Event, void *) { started(); });
	EventManager::listen(Event::Terminate, [](Event, void *) {
		watched.clear();
		++watchRun;
	});
}

} // namespace ps5::games
