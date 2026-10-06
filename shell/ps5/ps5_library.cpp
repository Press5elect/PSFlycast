/*
	PSFlyCast - what the library keeps about each game.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Beside the games themselves the library keeps, for each one, whether it is
	a favourite, whether it is hidden, how long it has been played and when it
	was last played. It also keeps which covers in <root>/covers PSFlyCast put
	there itself (a download, or a picture chosen in a game's details), so that
	a picture of the user's own is never taken for one of them and removed.

	All of it is in <root>/data/library.txt, beside the list of the games last
	played: a line for each game and for each cover, tabs between its fields.
	  game <tab> path <tab> favourite <tab> hidden <tab> seconds played <tab> last played
	  cover <tab> file name without extension <tab> which <tab> size <tab> time
	Favourite and hidden are 0 or 1 and last played is a Unix time, 0 for a
	game that was never played. A cover's size and time are the file's own when
	it was put there: a file that differs from them is not that file any more.
	A game is in the file by its path, read under this run's root whichever
	root it was written under; a game with nothing to say has no line. Lines
	that begin with anything else are left out when the file is written again.

	The interface's thread and the cover downloads' both come here.
*/
#include "ps5_frontend.h"
#include "types.h"
#include "emulator.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ps5
{

std::string underRoot(const std::string& path)
{
	for (const char *prefix : { "/app0/", "/data/homebrew/PPSA99247/", "/mnt/sandbox/PPSA99247_000/app0/" })
		if (path.rfind(prefix, 0) == 0)
			return rootDir + path.substr(strlen(prefix));
	return path;
}

namespace library
{
namespace
{

struct CoverMark
{
	std::string which;
	uint64_t size = 0;
	int64_t time = 0;
};

std::mutex mutex;
bool loaded;
bool unreadable;		// the file is there and could not be opened: it is not written over
std::map<std::string, Entry> entries;		// by path
std::map<std::string, CoverMark> coverMarks;	// by file name without extension
unsigned currentGeneration = 1;

// The game the emulator runs, and what of its time is not in its entry yet.
struct Clock
{
	std::string path;
	bool wasRunning = false;
	bool dated = false;			// "last played" was written for this start
	std::chrono::steady_clock::time_point last;
	double pending = 0;			// seconds
	double sinceKept = 0;
} playClock;

std::string fileName()
{
	return rootDir + "data/library.txt";
}

std::string coverFile(const std::string& base)
{
	return rootDir + "covers/" + base + ".png";
}

std::vector<std::string> splitTabs(const std::string& line)
{
	std::vector<std::string> fields;
	size_t start = 0;
	for (;;)
	{
		const size_t tab = line.find('\t', start);
		fields.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
		if (tab == std::string::npos)
			break;
		start = tab + 1;
	}
	return fields;
}

void load()
{
	if (loaded)
		return;
	loaded = true;
	FILE *f = fopen(fileName().c_str(), "r");
	if (f == nullptr)
	{
		// No file yet is an empty library. A file that is there and cannot be
		// read is left alone for this run: saving would replace it with less.
		unreadable = errno != ENOENT;
		return;
	}
	char line[4096];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		std::string s(line);
		while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
			s.pop_back();
		const std::vector<std::string> fields = splitTabs(s);
		if (fields[0] == "game" && fields.size() >= 6 && !fields[1].empty())
		{
			// Two roots' lines for one game (a run in the sandbox, one outside
			// it) are one game: what either says, and the time of both.
			Entry& e = entries[underRoot(fields[1])];
			e.favourite = e.favourite || fields[2] == "1";
			e.hidden = e.hidden || fields[3] == "1";
			e.seconds += strtoull(fields[4].c_str(), nullptr, 10);
			e.lastPlayed = std::max(e.lastPlayed, (int64_t)strtoll(fields[5].c_str(), nullptr, 10));
		}
		else if (fields[0] == "cover" && fields.size() >= 5 && !fields[1].empty())
			coverMarks[fields[1]] = { fields[2], strtoull(fields[3].c_str(), nullptr, 10),
					(int64_t)strtoll(fields[4].c_str(), nullptr, 10) };
	}
	fclose(f);
}

// Written beside the file and moved over it: a title closed half way through
// (it is written while a game runs) leaves the file of a minute ago.
void save()
{
	if (unreadable)
		return;
	const std::string file = fileName(), temporary = file + ".new";
	FILE *f = fopen(temporary.c_str(), "w");
	if (f == nullptr)
		return;
	bool written = true;
	for (const auto& [path, e] : entries)
		if (e.favourite || e.hidden || e.seconds != 0 || e.lastPlayed != 0)
			written = fprintf(f, "game\t%s\t%d\t%d\t%llu\t%lld\n", path.c_str(), e.favourite ? 1 : 0, e.hidden ? 1 : 0,
					(unsigned long long)e.seconds, (long long)e.lastPlayed) > 0 && written;
	for (const auto& [base, mark] : coverMarks)
		written = fprintf(f, "cover\t%s\t%s\t%llu\t%lld\n", base.c_str(), mark.which.c_str(),
				(unsigned long long)mark.size, (long long)mark.time) > 0 && written;
	written = fclose(f) == 0 && written;
	if (!written || rename(temporary.c_str(), file.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return;
	}
	// For the console's FTP server, which is another process.
	chmod(file.c_str(), 0666);
}

// The clock's time goes to its game's entry; kept when asked, or when there
// was a whole second to keep.
void bank(bool keep)
{
	const uint64_t whole = (uint64_t)playClock.pending;
	if (whole != 0 && !playClock.path.empty())
	{
		Entry& e = entries[playClock.path];
		e.seconds += whole;
		e.lastPlayed = (int64_t)time(nullptr);
		playClock.pending -= (double)whole;
		keep = true;
	}
	playClock.sinceKept = 0;
	if (keep)
		save();
}

// The same while a game runs, each minute: the file is written on a thread of
// its own, so that the frame being drawn does not wait for the disk.
std::atomic<bool> keeping;

void bankLater()
{
	const uint64_t whole = (uint64_t)playClock.pending;
	playClock.sinceKept = 0;
	if (whole == 0 || playClock.path.empty())
		return;
	Entry& e = entries[playClock.path];
	e.seconds += whole;
	e.lastPlayed = (int64_t)time(nullptr);
	playClock.pending -= (double)whole;
	if (keeping.exchange(true))
		return;		// the last one is still being written: the next minute has this one's time too
	std::thread([] {
		{
			std::lock_guard<std::mutex> lock(mutex);
			save();
		}
		keeping = false;
	}).detach();
}

} // namespace

Entry entry(const std::string& path)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	const auto it = entries.find(path);
	return it != entries.end() ? it->second : Entry();
}

void setFavourite(const std::vector<std::string>& paths, bool on)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	for (const std::string& path : paths)
		entries[path].favourite = on;
	currentGeneration++;
	save();
}

void setHidden(const std::vector<std::string>& paths, bool on)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	for (const std::string& path : paths)
		entries[path].hidden = on;
	currentGeneration++;
	save();
}

unsigned generation()
{
	std::lock_guard<std::mutex> lock(mutex);
	return currentGeneration;
}

void playing(const std::string& path)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	// What the game before it still had to its name stays that game's.
	bank(false);
	playClock.path = path;
	playClock.pending = 0;
	playClock.dated = false;
	playClock.wasRunning = false;
}

void tick()
{
	const bool running = emu.running();
	// The clock is this thread's alone (playing() and flush() are called on it
	// too): nothing is locked while no game runs.
	if (!running && !playClock.wasRunning)
		return;
	// Held by the covers' thread noting a download, or while the last minute is
	// written: the next frame counts this one's time as well.
	std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
	if (!lock.owns_lock())
		return;
	const auto now = std::chrono::steady_clock::now();
	if (running)
	{
		load();
		// A game started by another way than the library.
		if (playClock.path.empty())
			playClock.path = ::settings.content.path;
		if (playClock.wasRunning)
		{
			// A frame that took seconds (the console's own menu over the
			// title, a disc that stalled) was not all play.
			const double passed = std::min(std::chrono::duration<double>(now - playClock.last).count(), 2.0);
			playClock.pending += passed;
			playClock.sinceKept += passed;
		}
		if (!playClock.dated && !playClock.path.empty())
		{
			// It runs: played today, however short.
			playClock.dated = true;
			entries[playClock.path].lastPlayed = (int64_t)time(nullptr);
			bank(true);
		}
		else if (playClock.sinceKept >= 60)
			bankLater();
	}
	else
		// Paused (the quick menu) or stopped.
		bank(false);
	playClock.last = now;
	playClock.wasRunning = running;
}

void flush()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (loaded)
		bank(false);
}

void played(const std::string& path, uint64_t seconds, int64_t when)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	Entry& e = entries[path];
	e.seconds += seconds;
	e.lastPlayed = std::max(e.lastPlayed, when);
	save();
}

void coverPut(const std::string& base, const std::string& which)
{
	struct stat st;
	if (stat(coverFile(base).c_str(), &st) != 0)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	load();
	coverMarks[base] = { which, (uint64_t)st.st_size, (int64_t)st.st_mtime };
	save();
}

void coverGone(const std::string& base)
{
	std::lock_guard<std::mutex> lock(mutex);
	load();
	if (coverMarks.erase(base) != 0)
		save();
}

std::string coverOurs(const std::string& base)
{
	struct stat st;
	if (stat(coverFile(base).c_str(), &st) != 0)
		return "";
	std::lock_guard<std::mutex> lock(mutex);
	load();
	const auto it = coverMarks.find(base);
	if (it == coverMarks.end() || it->second.size != (uint64_t)st.st_size || it->second.time != (int64_t)st.st_mtime)
		return "";
	return it->second.which;
}

} // namespace library
} // namespace ps5
