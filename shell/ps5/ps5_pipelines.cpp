/*
	PSFlyCast - the store behind pipeline warm-up.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	core/rend/vulkan/pipeline_warm.h: the pipelines each game has used are kept
	in <root>/data/pipelines/<game id>.txt, one line of fourteen numbers each.
	When a game starts its list is loaded and the generation changes, so the
	pipeline managers build the listed pipelines at their next draw; new ones
	are added as the game makes them, and the list is saved when the game is
	paused or closed. The graphics driver's own cache (radv-shader-cache/)
	makes each of those builds a read from disk after the first time.
*/
#include "rend/vulkan/pipeline_warm.h"
#include "emulator.h"
#include "log/Log.h"
#include "ps5_diag.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>

namespace ps5
{
extern std::string rootDir;
}

namespace pipelinewarm
{
namespace
{
std::mutex mutex;
std::set<Record> records;
std::string currentFile;
bool dirty;
std::atomic<u32> currentGeneration{1};

std::string fileFor(const std::string& gameId)
{
	std::string name;
	for (char c : gameId)
		name += (isalnum((unsigned char)c) || c == '-' || c == '_') ? c : '_';
	if (name.empty())
		name = "unknown";
	return ps5::rootDir + "data/pipelines/" + name + ".txt";
}

void saveLocked()
{
	if (!dirty || currentFile.empty())
		return;
	const std::string temporary = currentFile + ".new";
	FILE *f = fopen(temporary.c_str(), "w");
	if (f == nullptr)
		return;
	for (const Record& r : records)
		fprintf(f, "%x %x %x %x %x %x %x %x %x %x %x %x %x %x\n", r.kind, r.listType, r.sort, r.pass, r.gpuPalette,
				r.dithering, r.tsp, r.tcw, r.pcw, r.isp, r.tileclip, r.tsp1, r.tcw1, r.naomi2);
	fclose(f);
	rename(temporary.c_str(), currentFile.c_str());
	dirty = false;
}

void onStart(Event, void *)
{
	std::lock_guard<std::mutex> lock(mutex);
	const std::string file = fileFor(settings.content.gameId);
	if (file == currentFile)
		return;		// resumed, or the same game again: the set in memory is current
	saveLocked();
	records.clear();
	currentFile = file;
	if (FILE *f = fopen(file.c_str(), "r"))
	{
		Record r;
		while (fscanf(f, "%x %x %x %x %x %x %x %x %x %x %x %x %x %x", &r.kind, &r.listType, &r.sort, &r.pass,
				&r.gpuPalette, &r.dithering, &r.tsp, &r.tcw, &r.pcw, &r.isp, &r.tileclip, &r.tsp1, &r.tcw1, &r.naomi2) == 14)
			if (records.size() < 4096)
				records.insert(r);
		fclose(f);
	}
	NOTICE_LOG(RENDERER, "Pipeline warm-up: %d pipelines known for %s", (int)records.size(), settings.content.gameId.c_str());
	// A new generation, never 0 (a manager's "not warmed").
	u32 next = currentGeneration.load() + 1;
	if (next == 0)
		next = 1;
	currentGeneration = next;
}

void onSave(Event, void *)
{
	std::lock_guard<std::mutex> lock(mutex);
	saveLocked();
}
} // namespace

void record(const Record& r)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (currentFile.empty() || records.size() >= 4096)
		return;
	if (records.insert(r).second)
		dirty = true;
}

u32 generation()
{
	return currentGeneration.load(std::memory_order_relaxed);
}

std::vector<Record> snapshot(Kind kind)
{
	std::lock_guard<std::mutex> lock(mutex);
	std::vector<Record> out;
	for (const Record& r : records)
		if (r.kind == kind)
			out.push_back(r);
	return out;
}

void report(Kind kind, unsigned count, double ms)
{
	if (count != 0)
		NOTICE_LOG(RENDERER, "Pipeline warm-up: built %u pipelines (kind %u) in %.0f ms", count, (unsigned)kind, ms);
}

} // namespace pipelinewarm

namespace ps5
{
void pipelineWarmInit()
{
	mkdir((rootDir + "data/pipelines").c_str(), 0777);
	EventManager::listen(Event::Start, pipelinewarm::onStart);
	EventManager::listen(Event::Pause, pipelinewarm::onSave);
	EventManager::listen(Event::Terminate, pipelinewarm::onSave);
}
}
