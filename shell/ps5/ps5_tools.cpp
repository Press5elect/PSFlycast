/*
	PSFlyCast - tools for a running game: find in memory, and a speedrun timer.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Find in memory is the way cheats are found: a search starts with every
	place in the Dreamcast's main memory (16 MB, 32 MB in the arcade boards)
	as a candidate, and each step keeps those that compare as asked, against a
	number or against what they were at the step before: the lives went down
	by one, so the place that holds them decreased while most did not. A
	handful left, each can be held at a value (frozen) or made a GameShark
	code in the game's own cheat file. Addresses are offsets from the start of
	the memory; the console shows them as 8C000000 and up.

	The speedrun timer counts the emulator's frames (game time: the same on
	every console and display, and stopped while a menu is open) or the
	clock (real time). A run is split by hand or by itself, when a value in
	the game's memory becomes what a split's condition says. A run done
	without loading a state, rewinding or fast forward that ends faster than
	the personal best becomes it; the fastest each segment was ever done is
	kept too. The splits of a game are <root>/data/splits/<game id>.txt, and
	can be written by hand:

		start when 0x8C01A2B4 u8 == 1       (optional: the run starts by itself)
		split First level | when 0x8C07A3B4 u8 == 2
		split Boss
		best 83450 165000                   (the title's: ms at each split)
		gold 83450 80000                    (the title's: each segment's fastest)

	A condition is an address in the main memory, a size (u8, u16, u32), a
	comparison (== != > < >= <=) and a number; it acts when it becomes true.
*/
#include "ps5_frontend.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/spg.h"
#include "imgui/imgui.h"
#include "oslib/oslib.h"
#include "ps5_diag.h"
#include "ps5_clock.h"
#include "stdclass.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

namespace
{
std::string format(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string format(const char *fmt, ...)
{
	char buffer[256];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	va_end(args);
	return buffer;
}
}

namespace ps5::memsearch
{
namespace
{
int searchSize = 1;
bool begun = false;
int stepCount = 0;
std::vector<u8> alive;		// one flag for each place of searchSize
std::vector<u8> before;		// the memory at the last step
u64 aliveCount = 0;

struct Held
{
	u32 address;
	int size;
	u32 value;
};
std::vector<Held> held;
std::mutex heldMutex;
bool listening;

const u8 *ramBase(u32& size)
{
	size = settings.platform.ram_size;
	return size == 0 ? nullptr : &mem_b[0];
}

u32 readFrom(const u8 *memory, u32 limit, u32 address, int bytes)
{
	if (memory == nullptr || address + (u32)bytes > limit)
		return 0;
	u32 value = 0;
	for (int i = bytes - 1; i >= 0; i--)
		value = (value << 8) | memory[address + (u32)i];
	return value;
}

bool matches(Compare how, u32 now, u32 was, u32 value)
{
	switch (how)
	{
	case Equal: return now == value;
	case NotEqual: return now != value;
	case Greater: return now > value;
	case Less: return now < value;
	case Changed: return now != was;
	case Unchanged: return now == was;
	case Increased: return now > was;
	case Decreased: return now < was;
	default: return false;
	}
}

// At each vertical blank: what is frozen is written again.
void hold(Event, void *)
{
	std::lock_guard<std::mutex> lock(heldMutex);
	for (const Held& h : held)
		write(h.address, h.size, h.value);
}

void listenIfNeeded()
{
	if (!held.empty() && !listening)
		EventManager::listen(Event::VBlank, hold);
	else if (held.empty() && listening)
		EventManager::unlisten(Event::VBlank, hold);
	listening = !held.empty();
}
}

const char *compareName(Compare how)
{
	static const char *const names[CompareCount] = { "Equal to the value", "Not equal to the value",
		"Greater than the value", "Less than the value", "Changed", "Unchanged", "Increased", "Decreased" };
	return how >= 0 && how < CompareCount ? names[how] : "";
}

bool compareNeedsValue(Compare how)
{
	return how == Equal || how == NotEqual || how == Greater || how == Less;
}

void setSize(int bytes)
{
	bytes = bytes >= 4 ? 4 : bytes >= 2 ? 2 : 1;
	if (bytes == searchSize)
		return;
	searchSize = bytes;
	begun = false;
	alive.clear();
	before.clear();
	aliveCount = 0;
	stepCount = 0;
}

int size()
{
	return searchSize;
}

void begin()
{
	u32 ramSize = 0;
	const u8 *ram = ramBase(ramSize);
	if (ram == nullptr)
	{
		begun = false;
		return;
	}
	const u32 places = ramSize / (u32)searchSize;
	alive.assign(places, 1);
	before.assign(ram, ram + ramSize);
	aliveCount = places;
	begun = true;
	stepCount = 0;
}

bool searching()
{
	return begun;
}

void narrow(Compare how, u32 value)
{
	if (!begun)
		begin();
	u32 ramSize = 0;
	const u8 *ram = ramBase(ramSize);
	if (ram == nullptr || ramSize != before.size())
	{
		// The memory changed size (another game): anew.
		begin();
		return;
	}
	const u32 step = (u32)searchSize;
	u64 left = 0;
	for (size_t i = 0; i < alive.size(); i++)
	{
		if (!alive[i])
			continue;
		const u32 address = (u32)i * step;
		if (matches(how, readFrom(ram, ramSize, address, searchSize),
				readFrom(before.data(), (u32)before.size(), address, searchSize), value))
			left++;
		else
			alive[i] = 0;
	}
	aliveCount = left;
	before.assign(ram, ram + ramSize);
	stepCount++;
}

u64 count()
{
	return begun ? aliveCount : 0;
}

int steps()
{
	return stepCount;
}

std::vector<Result> results(size_t most)
{
	std::vector<Result> out;
	if (!begun)
		return out;
	u32 ramSize = 0;
	const u8 *ram = ramBase(ramSize);
	const u32 step = (u32)searchSize;
	for (size_t i = 0; i < alive.size() && out.size() < most; i++)
	{
		if (!alive[i])
			continue;
		const u32 address = (u32)i * step;
		out.push_back({ address, readFrom(ram, ramSize, address, searchSize),
			readFrom(before.data(), (u32)before.size(), address, searchSize) });
	}
	return out;
}

u32 read(u32 address, int bytes)
{
	u32 ramSize = 0;
	const u8 *ram = ramBase(ramSize);
	return readFrom(ram, ramSize, address, bytes);
}

void write(u32 address, int bytes, u32 value)
{
	u32 ramSize = 0;
	if (ramBase(ramSize) == nullptr || address + (u32)bytes > ramSize)
		return;
	// Through the emulated bus: the recompiler is told about code that changes.
	for (int i = 0; i < bytes; i++)
		WriteMem8_nommu(0x8C000000 + address + (u32)i, (u8)(value >> (8 * i)));
}

bool frozen(u32 address, int bytes)
{
	std::lock_guard<std::mutex> lock(heldMutex);
	for (const Held& h : held)
		if (h.address == address && h.size == bytes)
			return true;
	return false;
}

void freeze(u32 address, int bytes, bool on)
{
	{
		std::lock_guard<std::mutex> lock(heldMutex);
		held.erase(std::remove_if(held.begin(), held.end(),
				[&](const Held& h) { return h.address == address && h.size == bytes; }), held.end());
		if (on)
			held.push_back({ address, bytes, read(address, bytes) });
	}
	listenIfNeeded();
}

int frozenCount()
{
	std::lock_guard<std::mutex> lock(heldMutex);
	return (int)held.size();
}

void unload()
{
	{
		std::lock_guard<std::mutex> lock(heldMutex);
		held.clear();
	}
	listenIfNeeded();
	begun = false;
	alive.clear();
	before.clear();
	aliveCount = 0;
	stepCount = 0;
}

std::string gameSharkCode(u32 address, int bytes, u32 value)
{
	const u32 at = address & 0x00ffffff;
	if (bytes == 1)
		return format("00%06X %08X", at, value & 0xff);
	if (bytes == 2)
		return format("01%06X %08X", at, value & 0xffff);
	return format("02%06X %08X", at, value);
}

std::string addressText(u32 address)
{
	return format("%08X", 0x8C000000u + address);
}

void init()
{
	EventManager::listen(Event::Terminate, [](Event, void *) { unload(); });
}

} // namespace ps5::memsearch

// ------------------------------------------------------------------ speedrun

namespace ps5::speedrun
{
namespace
{
config::Option<int> Clock("SpeedrunClock", 0, "ps5");		// 0 game time, 1 real time
config::Option<int> Show("SpeedrunShow", 1, "ps5");		// the timer on the picture

std::vector<Split> list;
std::string serial_, title_, path_, startCondition, practiceWhy;
State runState = Idle;
size_t at = 0;
u64 frames = 0;
// Emulated vblanks per second (spg_GetFieldRate(): ~59.94 NTSC, 50 PAL),
// not the output's refresh rate, which on a 119.88 Hz display is twice
// the game's and made game time run at half speed.
double fpsNow = 59.94;
u64 startedAt = 0;
s64 finishedAt = 0;
bool isPractice = false;
bool startHeld = false, splitHeld = false;
std::mutex mutex;

struct Condition
{
	u32 address = 0;
	int bytes = 1;
	int op = 0;		// 0 ==, 1 !=, 2 >, 3 <, 4 >=, 5 <=
	u32 value = 0;
};

std::string trim(const std::string& text)
{
	size_t a = 0, b = text.size();
	while (a < b && isspace((unsigned char)text[a]))
		a++;
	while (b > a && isspace((unsigned char)text[b - 1]))
		b--;
	return text.substr(a, b - a);
}

bool parse(const std::string& text, Condition& out)
{
	char size[8] = {}, op[4] = {};
	char addressText[32] = {}, valueText[32] = {};
	if (sscanf(text.c_str(), "%31s %7s %3s %31s", addressText, size, op, valueText) != 4)
		return false;
	char *end = nullptr;
	const unsigned long address = strtoul(addressText, &end, 0);
	if (end == addressText)
		return false;
	out.address = (u32)(address & 0x00ffffffu);
	if (!strcmp(size, "u8"))
		out.bytes = 1;
	else if (!strcmp(size, "u16"))
		out.bytes = 2;
	else if (!strcmp(size, "u32"))
		out.bytes = 4;
	else
		return false;
	static const char *const ops[6] = { "==", "!=", ">", "<", ">=", "<=" };
	out.op = -1;
	for (int i = 0; i < 6; i++)
		if (!strcmp(op, ops[i]))
			out.op = i;
	if (out.op < 0)
		return false;
	const unsigned long value = strtoul(valueText, &end, 0);
	if (end == valueText)
		return false;
	out.value = (u32)value;
	return true;
}

bool evaluate(const Condition& c)
{
	const u32 now = memsearch::read(c.address, c.bytes);
	switch (c.op)
	{
	case 0: return now == c.value;
	case 1: return now != c.value;
	case 2: return now > c.value;
	case 3: return now < c.value;
	case 4: return now >= c.value;
	case 5: return now <= c.value;
	default: return false;
	}
}

s64 now_()
{
	if (runState == Finished)
		return finishedAt;
	if (runState == Idle)
		return 0;
	if (Clock == 1)
		return (s64)(getTimeMs() - startedAt);
	return (s64)((double)frames * 1000.0 / (fpsNow > 1.0 ? fpsNow : 59.94));
}

void save()
{
	if (path_.empty())
		return;
	std::string text = "# PSFlyCast's speedrun splits for " + serial_ + " (" + title_ + ")\n"
			"# split NAME [| when ADDRESS u8|u16|u32 ==|!=|>|<|>=|<= VALUE]; start when CONDITION; best and gold are the "
			"title's\n";
	if (!startCondition.empty())
		text += "start when " + startCondition + "\n";
	bool anyBest = false, anyGold = false;
	for (const Split& split : list)
	{
		text += "split " + split.name + (split.when.empty() ? std::string() : " | when " + split.when) + "\n";
		anyBest = anyBest || split.best >= 0;
		anyGold = anyGold || split.gold >= 0;
	}
	if (anyBest)
	{
		text += "best";
		for (const Split& split : list)
			text += " " + std::to_string(split.best);
		text += "\n";
	}
	if (anyGold)
	{
		text += "gold";
		for (const Split& split : list)
			text += " " + std::to_string(split.gold);
		text += "\n";
	}
	mkdir((rootDir + "data").c_str(), 0777);
	mkdir((rootDir + "data/splits").c_str(), 0777);
	FILE *f = fopen(path_.c_str(), "w");
	if (f == nullptr)
		return;
	fwrite(text.data(), 1, text.size(), f);
	fclose(f);
	chmod(path_.c_str(), 0666);
}

void finish()
{
	finishedAt = now_();
	runState = Finished;
	at = list.size();
	if (isPractice)
	{
		ps5::diag::mark("speedrun: finished in %s (practice: %s)", timeText(finishedAt).c_str(), practiceWhy.c_str());
		return;
	}
	// Each segment's fastest.
	s64 previous = 0;
	bool changed = false;
	for (Split& split : list)
	{
		if (split.time < 0)
			continue;
		const s64 segment = split.time - previous;
		previous = split.time;
		if (segment > 0 && (split.gold < 0 || segment < split.gold))
		{
			split.gold = segment;
			changed = true;
		}
	}
	// A new personal best: faster at the end, every split done.
	const bool complete = !list.empty() && std::all_of(list.begin(), list.end(), [](const Split& s) { return s.time >= 0; });
	const s64 bestEnd = list.empty() ? -1 : list.back().best;
	const bool record = complete && (bestEnd < 0 || finishedAt < bestEnd);
	if (record)
	{
		for (Split& split : list)
			split.best = split.time;
		changed = true;
	}
	ps5::diag::mark("speedrun: finished in %s%s", timeText(finishedAt).c_str(), record ? " (a personal best)" : "");
	os_notify(record ? ("A personal best: " + timeText(finishedAt)).c_str() : ("Finished in " + timeText(finishedAt)).c_str(),
			5000);
	if (changed)
		save();
}

void begin()
{
	if (list.empty())
		list.push_back({ "The end", "", -1, -1, -1 });
	for (Split& split : list)
		split.time = -1;
	runState = Running;
	at = 0;
	frames = 0;
	startedAt = getTimeMs();
	finishedAt = 0;
	isPractice = false;
	practiceWhy.clear();
	// A split that holds already waits for its condition to become true again.
	Condition c;
	splitHeld = !list.empty() && parse(list[0].when, c) && evaluate(c);
	ps5::diag::mark("speedrun: started");
}

void advance()
{
	if (at >= list.size())
		return;
	list[at].time = now_();
	at++;
	if (at >= list.size())
	{
		finish();
		return;
	}
	Condition c;
	splitHeld = parse(list[at].when, c) && evaluate(c);
}

// Once an emulated frame: game time, and the conditions that start and split.
void vblank(Event, void *)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (serial_.empty())
		return;
	// The game's own field rate (this event comes once per emulated vblank).
	const float fieldRate = spg_GetFieldRate();
	fpsNow = fieldRate > 1.f ? fieldRate : 59.94;
	// Fast forward does not count: a run with it is practice.
	if (runState == Running && settings.input.fastForwardMode)
		markPractice("fast forward");
	if (runState == Running)
		frames++;
	Condition c;
	if (runState != Running && !startCondition.empty() && parse(startCondition, c))
	{
		const bool holdsNow = evaluate(c);
		if (holdsNow && !startHeld)
			begin();
		startHeld = holdsNow;
	}
	if (runState == Running && at < list.size() && !list[at].when.empty() && parse(list[at].when, c))
	{
		const bool holdsNow = evaluate(c);
		if (holdsNow && !splitHeld)
			advance();
		else
			splitHeld = holdsNow;
	}
}

void loadFor(const std::string& serial, const std::string& title)
{
	unload();
	std::lock_guard<std::mutex> lock(mutex);
	serial_ = serial;
	title_ = title;
	if (serial.empty())
		return;
	std::string safe;
	for (char c : serial)
		safe += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
	path_ = rootDir + "data/splits/" + safe + ".txt";
	FILE *f = fopen(path_.c_str(), "r");
	if (f == nullptr)
		return;
	std::vector<s64> best, gold;
	char buffer[2048];
	while (fgets(buffer, sizeof(buffer), f) != nullptr)
	{
		const std::string line = trim(buffer);
		if (line.empty() || line[0] == '#')
			continue;
		if (line.compare(0, 11, "start when ") == 0)
			startCondition = trim(line.substr(11));
		else if (line.compare(0, 6, "split ") == 0)
		{
			Split split;
			std::string rest = line.substr(6);
			const size_t bar = rest.find("| when ");
			if (bar != std::string::npos)
			{
				split.when = trim(rest.substr(bar + 7));
				rest = rest.substr(0, bar);
			}
			split.name = trim(rest);
			list.push_back(split);
		}
		else if (line.compare(0, 5, "best ") == 0 || line.compare(0, 5, "gold ") == 0)
		{
			std::vector<s64>& into = line[0] == 'b' ? best : gold;
			const char *p = line.c_str() + 5;
			while (*p != '\0')
			{
				char *end = nullptr;
				const long long value = strtoll(p, &end, 10);
				if (end == p)
					break;
				into.push_back((s64)value);
				p = end;
			}
		}
	}
	fclose(f);
	for (size_t i = 0; i < list.size(); i++)
	{
		list[i].best = i < best.size() ? best[i] : -1;
		list[i].gold = i < gold.size() ? gold[i] : -1;
	}
	Condition c;
	startHeld = parse(startCondition, c) && evaluate(c);
}
}

void unload()
{
	list.clear();
	serial_.clear();
	title_.clear();
	path_.clear();
	startCondition.clear();
	runState = Idle;
	at = 0;
	frames = 0;
	isPractice = false;
}

void init()
{
	// What the clock found, for the boot log, when a game closes.
	EventManager::listen(Event::Terminate, [](Event, void *) {
		char line[256];
		ps5px_clock_line(line, sizeof(line));
		if (line[0] != '\0')
			ps5::diag::mark("%s", line);
	});
	EventManager::listen(Event::Start, [](Event, void *) { loadFor(settings.content.gameId, settings.content.title); });
	EventManager::listen(Event::VBlank, vblank);
	EventManager::listen(Event::Terminate, [](Event, void *) {
		std::lock_guard<std::mutex> lock(mutex);
		unload();
	});
	// A state loaded in the middle of a run is not a run.
	EventManager::listen(Event::LoadState, [](Event, void *) { markPractice("a state was loaded"); });
}

void startOrSplit()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (runState == Running)
		advance();
	else
		begin();
}

void undo()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (runState == Finished && !list.empty())
	{
		runState = Running;
		at = list.size() - 1;
		list[at].time = -1;
		return;
	}
	if (runState == Running && at > 0)
	{
		at--;
		list[at].time = -1;
	}
}

void skip()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (runState != Running || at >= list.size())
		return;
	list[at].time = -1;
	at++;
	if (at >= list.size())
		finish();
}

void reset()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (runState == Running)
		ps5::diag::mark("speedrun: reset at %s", timeText(now_()).c_str());
	runState = Idle;
	at = 0;
	frames = 0;
	for (Split& split : list)
		split.time = -1;
	isPractice = false;
	practiceWhy.clear();
}

void markPractice(const char *why)
{
	if (runState != Running || isPractice)
		return;
	isPractice = true;
	practiceWhy = why;
	ps5::diag::mark("speedrun: practice from here (%s)", why);
}

State state() { return runState; }
s64 elapsed() { std::lock_guard<std::mutex> lock(mutex); return now_(); }
size_t current() { return at; }
bool practice() { return isPractice; }
const std::string& practiceReason() { return practiceWhy; }
std::vector<Split>& splits() { return list; }
int clockMode() { return Clock; }
void setClock(int v) { Clock.set(v != 0 ? 1 : 0); SaveSettings(); }
bool shown() { return Show != 0; }
void setShown(bool on) { Show.set(on ? 1 : 0); SaveSettings(); }
bool loaded() { return !serial_.empty(); }

void addSplit(const std::string& name)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (trim(name).empty())
		return;
	list.push_back({ trim(name), "", -1, -1, -1 });
	save();
}

void removeLast()
{
	std::lock_guard<std::mutex> lock(mutex);
	if (list.empty())
		return;
	list.pop_back();
	runState = Idle;
	at = 0;
	frames = 0;
	for (Split& split : list)
		split.time = -1;
	save();
}

void setWhen(size_t index, const std::string& condition)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (index >= list.size())
		return;
	list[index].when = trim(condition);
	save();
}

void setStartWhen(const std::string& condition)
{
	std::lock_guard<std::mutex> lock(mutex);
	startCondition = trim(condition);
	Condition c;
	startHeld = parse(startCondition, c) && evaluate(c);
	save();
}

const std::string& startWhen() { return startCondition; }

void forgetBest()
{
	std::lock_guard<std::mutex> lock(mutex);
	for (Split& split : list)
		split.best = split.gold = -1;
	save();
}

bool validCondition(const std::string& condition)
{
	Condition c;
	return parse(condition, c);
}

std::string timeText(s64 ms)
{
	if (ms < 0)
		return "\xe2\x80\x94";
	const s64 hundredths = ms / 10;
	const s64 seconds = hundredths / 100, minutes = seconds / 60, hours = minutes / 60;
	if (hours > 0)
		return format("%lld:%02lld:%02lld.%02lld", (long long)hours, (long long)(minutes % 60), (long long)(seconds % 60),
				(long long)(hundredths % 100));
	return format("%lld:%02lld.%02lld", (long long)minutes, (long long)(seconds % 60), (long long)(hundredths % 100));
}

std::string deltaText(s64 ms)
{
	const s64 magnitude = ms < 0 ? -ms : ms;
	const s64 hundredths = magnitude / 10;
	const s64 seconds = hundredths / 100;
	if (seconds >= 60)
		return std::string(ms < 0 ? "-" : "+") + format("%lld:%02lld.%01lld", (long long)(seconds / 60),
				(long long)(seconds % 60), (long long)((hundredths % 100) / 10));
	return std::string(ms < 0 ? "-" : "+") + format("%lld.%02lld", (long long)seconds, (long long)(hundredths % 100));
}

// The timer at the top right of the picture, while a run is under way or the
// game has splits: time, the split being run and how far ahead or behind the
// best run it is.
void drawOsd(float fontSize, float inset)
{
	if (!Show)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	if (list.empty() && runState == Idle)
		return;
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImVec2 display = ImGui::GetIO().DisplaySize;
	const s64 elapsedMs = now_();
	std::string line1 = timeText(elapsedMs);
	if (isPractice)
		line1 += "  (practice)";
	std::string line2;
	u32 colour = 0xFFFFFFFF;
	if (runState == Finished)
		line2 = "Finished";
	else if (runState == Running && at < list.size())
	{
		line2 = list[at].name;
		if (list[at].best >= 0)
		{
			const s64 delta = elapsedMs - list[at].best;
			line2 += "   " + deltaText(delta);
			colour = delta <= 0 ? 0xFF78C85C : 0xFF5A5AFF;	// ahead: green; behind: red
		}
	}
	else
		line2 = "Ready";
	const float small = fontSize * 0.7f;
	const ImVec2 s1 = ImGui::GetFont()->CalcTextSizeA(fontSize, FLT_MAX, 0.f, line1.c_str());
	const ImVec2 s2 = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0.f, line2.c_str());
	const float w = std::max(s1.x, s2.x) + 24.f, h = s1.y + s2.y + 12.f;
	const ImVec2 pos(display.x - inset - w, inset * 0.5f + 8.f);
	dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), 0xA0202020, 6.f);
	dl->AddText(nullptr, fontSize, ImVec2(pos.x + w - 12.f - s1.x, pos.y + 4.f), 0xFFFFFFFF, line1.c_str());
	dl->AddText(nullptr, small, ImVec2(pos.x + w - 12.f - s2.x, pos.y + 8.f + s1.y), colour, line2.c_str());
}

} // namespace ps5::speedrun
