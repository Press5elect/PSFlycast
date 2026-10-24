/*
	PSFlyCast - dumping the game's textures. See ps5_texdump.h.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Nothing here has run on a console yet: every result is a line of
	flycast-boot.log ("texdump: ...").
*/
#include "ps5_texdump.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "hw/pvr/ta_ctx.h"
#include "rend/TexCache.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "cfg/option.h"

#include <stb_image_write.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps5::texdump
{

config::Option<int> Mode("TextureDump", ModeOff, "ps5");
config::Option<int> Where("TextureDumpTo", ToInternal, "ps5");

namespace
{

struct Pending
{
	std::string gameId;
	int w, h;
	std::vector<u8> rgba;
	u32 flat = 0, total = 0;	// polygons that drew it flat on the screen, and all that drew it
	u32 age = 0;				// frames waiting
};

struct Job
{
	std::string gameId, folder, name;	// folder: "2D" or "3D"
	int w, h;
	std::vector<u8> rgba;
};

constexpr size_t PendingLimit = 96u << 20;		// bytes waiting to be sorted
constexpr size_t QueueLimit = 192u << 20;		// bytes waiting to be written
constexpr u32 PatienceFrames = 240;				// a texture not drawn by then goes with 3D

std::mutex lock;
std::unordered_map<u32, Pending> pending;
std::unordered_set<u64> seen;					// game and hash: dumped already
size_t pendingBytes = 0;
std::deque<Job> queue;
size_t queuedBytes = 0;
std::condition_variable wake;
bool workerStarted = false;
bool workerStop = false;
std::atomic<unsigned> saved{0}, saved2D{0}, saved3D{0}, failed{0}, dropped{0};
std::string firstFailure;
std::string target;								// where it goes, for the log and the screen
std::string lastGame;

u64 key(const std::string& gameId, u32 hash)
{
	return (u64)std::hash<std::string>()(gameId) * 1000003ull ^ hash;
}

void encode(void *context, void *data, int size)
{
	auto *out = static_cast<std::vector<u8> *>(context);
	out->insert(out->end(), (const u8 *)data, (const u8 *)data + size);
}

bool makeFolders(const std::string& path)
{
	// Each level on its own: /a, /a/b, /a/b/c.
	for (size_t at = 1; at <= path.size(); at++)
		if (at == path.size() || path[at] == '/')
		{
			const std::string part = path.substr(0, at);
			if (!file_exists(part) && !make_directory(part) && !file_exists(part))
				return false;
		}
	return true;
}

// The first USB drive that can be written to: /mnt/usbN/PSFlyCast/Texture dumps.
std::string usbBase()
{
	for (int i = 0; i < 8; i++)
	{
		const std::string drive = "/mnt/usb" + std::to_string(i);
		if (!file_exists(drive))
			continue;
		const std::string base = drive + "/PSFlyCast/Texture dumps";
		if (makeFolders(base))
			return base;
	}
	return "";
}

std::string networkBase()
{
	std::string root = smb::backupFolder();
	const size_t cut = root.rfind("/Memory cards");
	if (cut == std::string::npos)
		return "";
	return root.substr(0, cut) + "/Texture dumps";
}

void fail(const std::string& why)
{
	if (failed++ == 0)
	{
		firstFailure = why;
		diag::mark("texdump: a texture could not be saved: %s", why.c_str());
		os_notify("A texture could not be saved", 4000, why.c_str());
	}
}

void run()
{
	PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
	for (;;)
	{
		Job job;
		std::string base;
		int to;
		{
			std::unique_lock<std::mutex> l(lock);
			wake.wait(l, [] { return workerStop || !queue.empty(); });
			if (queue.empty())
				return;
			job = std::move(queue.front());
			queue.pop_front();
			queuedBytes -= job.rgba.size();
			base = target;
			to = Where;
		}
		std::vector<u8> png;
		stbi_flip_vertically_on_write(1);
		stbi_write_png_to_func(encode, &png, job.w, job.h, 4, job.rgba.data(), 0);
		job.rgba = std::vector<u8>();
		if (png.empty() || base.empty())
		{
			fail(base.empty() ? "there is nowhere to save to" : "the picture could not be made");
			continue;
		}
		if (to == ToNetwork)
		{
			std::string why;
			if (smb::putFile(base + "/" + job.gameId + "/" + job.folder, job.name, png.data(), png.size(), why))
			{
				saved++;
				(job.folder == "2D" ? saved2D : saved3D)++;
			}
			else
				fail(why);
		}
		else
		{
			const std::string folder = base + "/" + job.gameId + "/" + job.folder;
			const std::string path = folder + "/" + job.name;
			if (file_exists(path))
				continue;
			if (!makeFolders(folder))
			{
				fail("the folder " + folder + " cannot be made");
				continue;
			}
			FILE *f = fopen(path.c_str(), "wb");
			if (f == nullptr)
			{
				fail("cannot write " + path);
				continue;
			}
			const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
			fclose(f);
			if (ok)
			{
				saved++;
				(job.folder == "2D" ? saved2D : saved3D)++;
			}
			else
				fail("the disk is full or cannot be written: " + path);
		}
	}
}

bool wants(bool flat)
{
	return Mode == ModeAll || (Mode == Mode2D && flat) || (Mode == Mode3D && !flat);
}

// Called with the lock held.
void finish(u32 hash, Pending& p, bool flat)
{
	pendingBytes -= p.rgba.size();
	if (wants(flat) && queuedBytes + p.rgba.size() <= QueueLimit)
	{
		char name[32];
		snprintf(name, sizeof(name), "%x.png", hash);
		queuedBytes += p.rgba.size();
		queue.push_back(Job{ p.gameId, flat ? "2D" : "3D", name, p.w, p.h, std::move(p.rgba) });
		wake.notify_one();
	}
	else if (wants(flat))
		dropped++;
}

}

void apply()
{
	const bool on = Mode != ModeOff && settings.platform.isConsole();
	if (config::DumpTextures != on)
		config::DumpTextures.override(on);
	if (!on)
		return;
	{
		std::lock_guard<std::mutex> l(lock);
		if (!workerStarted)
		{
			workerStarted = true;
			std::thread(run).detach();
		}
		workerStop = false;
		if (lastGame != settings.content.gameId)
		{
			lastGame = settings.content.gameId;
			saved = saved2D = saved3D = failed = dropped = 0;
			firstFailure.clear();
		}
	}
	std::string base;
	switch (Where)
	{
	case ToUsb:
		base = usbBase();
		if (base.empty())
			diag::mark("texdump: no USB drive that can be written to");
		break;
	case ToNetwork:
		base = networkBase();
		if (base.empty())
			diag::mark("texdump: there is no network share in network.cfg");
		break;
	default:
		base = hostfs::getTextureDumpPath();
		while (!base.empty() && base.back() == '/')
			base.pop_back();
		makeFolders(base);
		break;
	}
	{
		std::lock_guard<std::mutex> l(lock);
		target = base;
	}
	diag::mark("texdump: mode %d, to %s", (int)Mode, base.empty() ? "(nowhere)" : base.c_str());
	if (base.empty())
		os_notify("Texture dump: nowhere to save to", 4000, Where == ToUsb ? "No USB drive that can be written to" : "No network share");
}

void submit(const std::string& gameId, u32 hash, int w, int h, const u8 *rgba)
{
	if (w <= 0 || h <= 0)
		return;
	const size_t bytes = (size_t)w * h * 4;
	std::lock_guard<std::mutex> l(lock);
	if (Mode == ModeOff || target.empty())
		return;
	if (!seen.insert(key(gameId, hash)).second)
		return;
	if (pendingBytes + bytes > PendingLimit)
	{
		dropped++;
		return;
	}
	Pending& p = pending[hash];
	p.gameId = gameId;
	p.w = w;
	p.h = h;
	p.rgba.assign(rgba, rgba + bytes);
	pendingBytes += bytes;
}

void frame(const rend_context& rend)
{
	std::lock_guard<std::mutex> l(lock);
	if (pending.empty())
		return;
	auto scan = [&](const std::vector<PolyParam>& list) {
		for (const PolyParam& pp : list)
		{
			if (pp.pcw.Texture == 0 || pp.texture == nullptr || pp.count < 3)
				continue;
			auto it = pending.find(pp.texture->texture_hash);
			if (it == pending.end())
				continue;
			// Flat on the screen: every vertex of the polygons has the same depth.
			float lowest = 1e30f, highest = -1e30f;
			const u32 end = std::min<u32>(pp.first + pp.count, (u32)rend.idx.size());
			for (u32 i = pp.first; i < end; i++)
			{
				if (rend.idx[i] >= rend.verts.size())
					continue;
				const float z = rend.verts[rend.idx[i]].z;
				lowest = std::min(lowest, z);
				highest = std::max(highest, z);
			}
			if (highest < lowest)
				continue;
			it->second.total++;
			if (highest - lowest <= 1e-5f * std::max(1.f, std::fabs(highest)))
				it->second.flat++;
		}
	};
	scan(rend.global_param_op);
	scan(rend.global_param_pt);
	scan(rend.global_param_tr);
	for (auto it = pending.begin(); it != pending.end(); )
	{
		Pending& p = it->second;
		if (p.total > 0 || ++p.age >= PatienceFrames)
		{
			finish(it->first, p, p.total > 0 && p.flat * 2 > p.total);
			it = pending.erase(it);
		}
		else
			++it;
	}
}

void stop()
{
	std::lock_guard<std::mutex> l(lock);
	for (auto& [hash, p] : pending)
		finish(hash, p, false);
	pending.clear();
	pendingBytes = 0;
	seen.clear();
	if (saved + failed + dropped > 0)
		diag::mark("texdump: %u saved (2D %u, 3D %u), %u not saved%s%s, %u dropped for lack of room", saved.load(),
				saved2D.load(), saved3D.load(), failed.load(), firstFailure.empty() ? "" : ": ", firstFailure.c_str(),
				dropped.load());
}

std::string status()
{
	if (Mode == ModeOff)
		return "Off";
	std::string s = std::to_string(saved.load()) + " saved (2D " + std::to_string(saved2D.load()) + ", 3D "
			+ std::to_string(saved3D.load()) + ")";
	if (failed > 0)
		s += ", " + std::to_string(failed.load()) + " failed: " + firstFailure;
	if (dropped > 0)
		s += ", " + std::to_string(dropped.load()) + " dropped for lack of room";
	return s;
}

}
