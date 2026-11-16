/*
	PSFlyCast - where the player's files are (ps5_folders.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
	Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect".
*/
#include "ps5_folders.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5::folders
{
namespace
{
const KindInfo kinds[KindCount] = {
	{ "game", "Game Folders",
		"Folders with games. Their games, and those in the folders inside them, join the library where they are: nothing is moved or copied.",
		"games", false },
	{ "bios", "BIOS Folders", "Folders containing BIOS files (e.g. dc_boot.bin or dc_bios.bin) and arcade BIOS.", "bios", false },
	{ "state", "Savestate Folders",
		"Folders for save states. The first one is used for new states; all of them are searched when loading.", "data/savestates", true },
	{ "cover", "Box Art Folder", "Where covers are kept and looked for; new covers are saved in the first folder.", "covers", true },
	{ "texture", "Texture Pack Folders", "Folders containing textures/<gameId> or <gameId> under a textures subfolder.", "data/textures", false },
	{ "cheat", "Cheat Folders", "Folders containing cheat files (.cht/.txt) named with the game ID.", "cheats", false },
	{ "video", "Game Video Folders", "Folders with each game's video, named after the game (.mp4). Looked in before the places PSFlyCast guesses.", "videos", false },
};

Lister theLister;
Settings current;
std::string theRoot;
// Where each kind is written now, kept so that a lookup every frame costs no file call. The covers
// downloader reads it from its own thread.
std::mutex cacheMutex;
std::string cache[KindCount];

std::string lower(std::string text)
{
	for (char& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

// A path without the slashes at its end (the root "/" and "smb://" keep theirs).
std::string norm(std::string path)
{
	while (path.size() > 1 && path.back() == '/' && path != "smb://")
		path.pop_back();
	return path;
}

bool endsWith(const std::string& text, const char *tail)
{
	const size_t n = std::strlen(tail);
	return text.size() >= n && text.compare(text.size() - n, n, tail) == 0;
}

bool posixList(const std::string& folder, std::vector<Entry>& out)
{
	DIR *dir = opendir(folder.c_str());
	if (dir == nullptr)
		return false;
	while (const dirent *entry = readdir(dir))
	{
		if (!std::strcmp(entry->d_name, ".") || !std::strcmp(entry->d_name, ".."))
			continue;
		Entry e;
		e.name = entry->d_name;
		if (entry->d_type == DT_DIR)
			e.folder = true;
		else if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK)
		{
			struct stat st;
			e.folder = stat(join(folder, e.name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
		}
		out.push_back(std::move(e));
	}
	closedir(dir);
	return true;
}

bool localExists(const std::string& folder)
{
	if (isShare(folder))
		return true;		// not asked at the start: the network is not woken for it
	struct stat st;
	return stat(folder.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isBiosName(const std::string& low)
{
	return low == "dc_boot.bin" || low == "dc_bios.bin" || low == "dc_flash.bin" || low == "naomi.zip" || low == "naomi2.zip"
			|| endsWith(low, "bios.zip");
}

bool oneOf(const std::string& low, std::initializer_list<const char *> names)
{
	for (const char *n : names)
		if (low == n)
			return true;
	return false;
}

bool skipped(const std::string& name)
{
	return name.empty() || name[0] == '.' || name == "System Volume Information" || name == "$RECYCLE.BIN" || name == "lost+found";
}
}	// namespace

const KindInfo& info(Kind kind)
{
	return kinds[kind];
}

void setLister(Lister lister)
{
	theLister = std::move(lister);
}

bool isShare(const std::string& path)
{
	return path.rfind("smb://", 0) == 0;
}

bool list(const std::string& folder, std::vector<Entry>& out)
{
	out.clear();
	if (isShare(folder))
		return theLister ? theLister(folder, out) : false;
	return posixList(folder, out);
}

std::string join(const std::string& folder, const std::string& name)
{
	if (folder.empty())
		return name;
	return folder.back() == '/' ? folder + name : folder + "/" + name;
}

std::vector<Place> places(const PlaceSources& sources)
{
	std::vector<Place> out;
	for (const std::string& usb : sources.usb)
	{
		const size_t digit = usb.find_last_not_of("0123456789");
		const std::string number = digit == std::string::npos || digit + 1 >= usb.size() ? "" : usb.substr(digit + 1);
		out.push_back({ number.empty() ? "USB drive (" + usb + ")" : "USB drive " + std::to_string(atoi(number.c_str()) + 1) + " (" + usb + ")",
				norm(usb) });
	}
	for (const std::string& share : sources.shares)
		out.push_back({ "Network share " + (isShare(share) ? share.substr(6) : share), norm(share) });
	if (!sources.console.empty())
		out.push_back({ "The console's storage (" + sources.console + ")", norm(sources.console) });
	return out;
}

std::vector<std::string> subfolders(const std::string& folder)
{
	std::vector<Entry> entries;
	std::vector<std::string> names;
	if (!list(folder, entries))
		return names;
	for (const Entry& e : entries)
		if (e.folder && !e.name.empty() && e.name[0] != '.' && e.name.find('/') == std::string::npos)
			names.push_back(e.name);
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
	if (names.size() > 1000)
		names.resize(1000);
	return names;
}

std::string parentOf(const std::string& folder, const std::vector<Place>& places)
{
	const std::string path = norm(folder);
	size_t best = 0;
	for (const Place& place : places)
	{
		const std::string& p = place.path;
		if (path == p)
			return "";
		if (p.size() > best && path.size() > p.size() && path.compare(0, p.size(), p) == 0 && (path[p.size()] == '/' || p.back() == '/'))
			best = p.size();
	}
	if (best == 0)
		return "";
	const size_t slash = path.rfind('/');
	if (slash == std::string::npos || slash < best)
		return path.substr(0, best);
	return path.substr(0, slash);
}

bool canWriteIn(const std::string& folder)
{
	if (folder.empty() || isShare(folder))
		return false;
	struct stat st;
	if (stat(folder.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))
		return false;
	const std::string probe = join(folder, ".psflycast-write-test");
	FILE *f = fopen(probe.c_str(), "wb");
	if (f == nullptr)
		return false;
	fclose(f);
	::remove(probe.c_str());
	return true;
}

Scan lookFor(const std::string& rootFolder, int depth, int maxFolders, const std::function<bool()>& stop)
{
	Scan scan;
	struct Item { std::string path; int level; };
	std::vector<Item> queue{ { norm(rootFolder), 0 } };
	std::vector<Found> raw;
	for (size_t at = 0; at < queue.size(); at++)
	{
		if (scan.visited >= maxFolders || (stop && stop()))
		{
			scan.partial = true;
			break;
		}
		const Item item = queue[at];
		std::vector<Entry> entries;
		scan.visited++;
		if (!list(item.path, entries))
			continue;
		const size_t slash = item.path.find_last_of('/');
		const std::string here = lower(slash == std::string::npos ? item.path : item.path.substr(slash + 1));
		int games = 0, bios = 0, states = 0, pictures = 0, cheats = 0, videos = 0, textureFolders = 0;
		for (const Entry& e : entries)
		{
			if (skipped(e.name))
				continue;
			const std::string low = lower(e.name);
			if (e.folder)
			{
				if (item.level < depth)
					queue.push_back({ join(item.path, e.name), item.level + 1 });
				textureFolders++;
				continue;
			}
			const size_t dot = low.find_last_of('.');
			const std::string ext = dot == std::string::npos ? "" : low.substr(dot + 1);
			if (isBiosName(low))
				bios++;
			else if (oneOf(ext, { "chd", "gdi", "cdi", "cue", "zip", "7z", "rar" }))
				games++;
			else if (ext == "state" || endsWith(low, ".state.net"))
				states++;
			else if (ext == "cht")
				cheats++;
			else if (ext == "png" || ext == "jpg" || ext == "jpeg")
				pictures++;
			else if (ext == "mp4" || ext == "mkv" || ext == "avi")
				videos++;
		}
		if (games > 0)
			raw.push_back({ Games, item.path, games });
		if (bios > 0)
			raw.push_back({ Bios, item.path, bios });
		if (states > 0)
			raw.push_back({ States, item.path, states });
		if (cheats > 0)
			raw.push_back({ Cheats, item.path, cheats });
		if (pictures >= 3 && oneOf(here, { "covers", "cover", "boxart", "artwork", "images", "thumbnails", "box2d", "box3d", "miximages" }))
			raw.push_back({ Covers, item.path, pictures });
		if (videos > 0 && oneOf(here, { "videos", "video", "movies" }))
			raw.push_back({ Videos, item.path, videos });
		if (here == "textures" && textureFolders > 0)
			raw.push_back({ Textures, item.path, textureFolders });
	}
	// Games are looked for under every folder added, so a folder inside another one counts for it.
	std::vector<Found> games;
	std::vector<Found> others;
	for (const Found& f : raw)
		(f.kind == Games ? games : others).push_back(f);
	std::sort(games.begin(), games.end(), [](const Found& a, const Found& b) { return a.folder < b.folder; });
	std::vector<Found> merged;
	for (const Found& f : games)
	{
		bool absorbed = false;
		for (Found& kept : merged)
			if (f.folder.size() > kept.folder.size() && f.folder.compare(0, kept.folder.size(), kept.folder) == 0
					&& (f.folder[kept.folder.size()] == '/' || kept.folder.back() == '/'))
			{
				kept.count += f.count;
				absorbed = true;
				break;
			}
		if (!absorbed)
			merged.push_back(f);
	}
	std::sort(merged.begin(), merged.end(), [](const Found& a, const Found& b) { return a.count != b.count ? a.count > b.count : a.folder < b.folder; });
	scan.found = merged;
	if (scan.found.size() > 20)
		scan.found.resize(20);
	for (int k = Bios; k < KindCount; k++)
	{
		std::vector<Found> of;
		for (const Found& f : others)
			if (f.kind == k)
				of.push_back(f);
		std::sort(of.begin(), of.end(), [](const Found& a, const Found& b) { return a.count != b.count ? a.count > b.count : a.folder < b.folder; });
		if (of.size() > 5)
			of.resize(5);
		scan.found.insert(scan.found.end(), of.begin(), of.end());
	}
	return scan;
}

Settings parse(const std::string& text)
{
	Settings out;
	size_t from = 0;
	while (from <= text.size())
	{
		size_t end = text.find('\n', from);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(from, end - from);
		from = end + 1;
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
			line.pop_back();
		const size_t first = line.find_first_not_of(" \t");
		if (first == std::string::npos || line[first] == '#')
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos)
			continue;
		std::string key = line.substr(first, eq - first);
		while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
			key.pop_back();
		const size_t value = line.find_first_not_of(" \t", eq + 1);
		if (value == std::string::npos)
			continue;
		const std::string path = norm(line.substr(value));
		for (int k = 0; k < KindCount; k++)
		{
			if (key != kinds[k].key)
				continue;
			std::vector<std::string>& to = k == Games ? out.games : out.kept[k];
			if (std::find(to.begin(), to.end(), path) == to.end())
				to.push_back(path);
		}
	}
	return out;
}

std::string serialise(const Settings& settings)
{
	std::string text = "# The folders PSFlyCast reads and writes (Settings > Games and saves > Folders).\n"
			"# One line for each folder; the first of a kind is where PSFlyCast writes.\n";
	for (int k = 0; k < KindCount; k++)
		for (const std::string& path : k == Games ? settings.games : settings.kept[k])
			text += std::string(kinds[k].key) + " = " + path + "\n";
	return text;
}

Settings& settings()
{
	return current;
}

std::string rootFolder()
{
	return theRoot;
}

void load(const std::string& root)
{
	theRoot = norm(root);
	current = Settings();
	if (FILE *f = fopen((theRoot + "/folders.cfg").c_str(), "rb"))
	{
		std::string text;
		char buffer[4096];
		size_t n;
		while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0)
			text.append(buffer, n);
		fclose(f);
		current = parse(text);
	}
	refresh();
}

bool save(std::string& why)
{
	const std::string file = theRoot + "/folders.cfg";
	const std::string temp = file + ".tmp";
	const std::string text = serialise(current);
	FILE *f = fopen(temp.c_str(), "wb");
	if (f == nullptr)
	{
		why = "folders.cfg cannot be written";
		return false;
	}
	const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	fclose(f);
	if (!ok || ::rename(temp.c_str(), file.c_str()) != 0)
	{
		::remove(temp.c_str());
		why = "folders.cfg cannot be written";
		return false;
	}
	return true;
}

Resolved resolve(const Settings& settings, Kind kind, const std::string& defaultFolder, const std::function<bool(const std::string&)>& exists)
{
	Resolved out;
	const std::vector<std::string>& chosen = kind == Games ? settings.games : settings.kept[kind];
	for (size_t i = 0; i < chosen.size(); i++)
	{
		if (exists(chosen[i]))
			out.use.push_back(chosen[i]);
		else
		{
			out.missing.push_back(chosen[i]);
			if (i == 0 && kinds[kind].written)
				out.askFirst = true;
		}
	}
	if (std::find(out.use.begin(), out.use.end(), defaultFolder) == out.use.end())
		out.use.push_back(defaultFolder);
	return out;
}

std::string defaultFolder(Kind kind)
{
	return join(theRoot, kinds[kind].fallback);
}

Resolved resolveNow(Kind kind)
{
	return resolve(current, kind, defaultFolder(kind), localExists);
}

void refresh()
{
	std::lock_guard<std::mutex> lock(cacheMutex);
	for (int k = 0; k < KindCount; k++)
		cache[k] = resolve(current, (Kind)k, defaultFolder((Kind)k), localExists).use.front();
}

std::string writeFolder(Kind kind)
{
	{
		std::lock_guard<std::mutex> lock(cacheMutex);
		if (!cache[kind].empty())
			return cache[kind];
	}
	refresh();
	std::lock_guard<std::mutex> lock(cacheMutex);
	return cache[kind];
}

std::string findIn(Kind kind, const std::string& name)
{
	for (const std::string& folder : resolveNow(kind).use)
	{
		struct stat st;
		if (!isShare(folder) && stat(join(folder, name).c_str(), &st) == 0)
			return folder;
	}
	return "";
}

std::string coversPrefix()
{
	return writeFolder(Covers) + "/";
}

std::string stateFile(const std::string& name)
{
	const std::string at = findIn(States, name);
	return join(at.empty() ? writeFolder(States) : at, name);
}

bool add(Kind kind, const std::string& folder, std::string& why)
{
	const std::string path = norm(folder);
	std::vector<std::string>& to = kind == Games ? current.games : current.kept[kind];
	if (path.empty() || std::find(to.begin(), to.end(), path) != to.end())
	{
		why = "That folder is already in the list.";
		return false;
	}
	if (kinds[kind].written)
	{
		if (isShare(path))
		{
			why = "PSFlyCast writes there, and a share is read only here: choose a folder on a USB drive or the console.";
			return false;
		}
		if (!canWriteIn(path))
		{
			why = "PSFlyCast cannot write in that folder.";
			return false;
		}
	}
	const std::vector<std::string> before = to;
	if (kind == Games)
		to.push_back(path);
	else if (kind == Covers)
		to = { path };		// one covers folder: PSFlyCast keeps notes about its covers in it
	else
		to.insert(to.begin(), path);
	std::string saved;
	if (!save(saved))
	{
		to = before;
		why = saved;
		return false;
	}
	refresh();
	return true;
}

void remove(Kind kind, const std::string& folder)
{
	std::vector<std::string>& from = kind == Games ? current.games : current.kept[kind];
	from.erase(std::remove(from.begin(), from.end(), norm(folder)), from.end());
	std::string why;
	save(why);
	refresh();
}

void reset(Kind kind)
{
	(kind == Games ? current.games : current.kept[kind]).clear();
	std::string why;
	save(why);
	refresh();
}
}
