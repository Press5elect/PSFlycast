/*
	PSFlyCast - a game disc in a USB CD or DVD drive (see ps5_disc.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	The reading, the retries and the slowing of a drive that struggles follow
	PSSwanStation's disc drive (build 21 and 22, Press5elect); the disc, as
	Flycast's Dreamcast drive sees it, is made the way Flycast's own reader of
	real drives (core/imgread/cdio.cpp, flyinghead) makes one.
*/
#include "types.h"
#include "imgread/common.h"
#include "ps5_disc.h"
#include "ps5_drive.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "ps5_priority.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>
#include <cstdio>
#include <functional>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace ps5::disc
{
namespace
{

constexpr uint32_t Pregap = 150;			// track 1's two seconds before the drive's address 0
constexpr uint32_t SessionGap = 11400;		// session 1's lead-out, session 2's lead-in and pregap
constexpr uint32_t Block = 26;				// sectors read at once: 26 x 2352 bytes stay under the 64 KiB a USB bus moves
constexpr size_t BudgetBytes = 900u * 1024 * 1024;	// a whole CD (80 minutes, 847 MB), at most, is kept in memory
const char Scheme[] = "ps5disc:";

// ---- the drive's speed: as chosen, and a step slower after failures

constexpr unsigned SpeedMax = 0xFFFF;
constexpr unsigned SpeedSteps[] = { SpeedMax, 4234, 2822, 1411, 706 };	// max, 24x, 16x, 8x, 4x (176.4 KB/s a 1x)
std::mutex speedMutex;
std::map<std::string, unsigned> driveSpeeds;

unsigned chosenSpeed()
{
	switch (options().discSpeed)
	{
	case 0: return 1411;
	case 1: return 4234;
	default: return SpeedMax;
	}
}

unsigned speedOf(const std::string& device)
{
	std::lock_guard<std::mutex> lock(speedMutex);
	const auto it = driveSpeeds.find(device);
	return it != driveSpeeds.end() ? std::min(it->second, chosenSpeed()) : chosenSpeed();
}

bool slowDown(const std::string& device)
{
	std::lock_guard<std::mutex> lock(speedMutex);
	const auto it = driveSpeeds.find(device);
	const unsigned now = it != driveSpeeds.end() ? std::min(it->second, chosenSpeed()) : chosenSpeed();
	for (unsigned step : SpeedSteps)
		if (step < now)
		{
			driveSpeeds[device] = step;
			return true;
		}
	return false;
}

std::string speedText(unsigned speed)
{
	if (speed >= SpeedMax)
		return "as fast as it goes";
	return std::to_string((speed + 88) / 176) + "x (" + std::to_string(speed) + " KB/s)";
}

bool syncIsRight(const uint8_t *sector)
{
	static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
	return std::memcmp(sector, sync, sizeof(sync)) == 0;
}

std::string trimmed(const uint8_t *text, size_t size)
{
	std::string s;
	for (size_t i = 0; i < size && text[i] != 0; i++)
		s += (text[i] >= 0x20 && text[i] < 0x7F) ? (char)text[i] : ' ';
	while (!s.empty() && s.back() == ' ')
		s.pop_back();
	while (!s.empty() && s.front() == ' ')
		s.erase(s.begin());
	// Runs of spaces that the header pads its fields with.
	std::string out;
	for (char c : s)
		if (!(c == ' ' && !out.empty() && out.back() == ' '))
			out += c;
	return out;
}

// ---- the disc as the drive gives it

// Sectors by their address on the drive (0 is where track 1 begins), kept in memory as they are read.
class Source
{
public:
	std::string device;
	std::vector<drive::TocEntry> toc;	// the tracks, then the lead-out
	uint32_t sectors = 0;				// the lead-out's address
	std::atomic<bool> stopping{ false };

	~Source() { stopFill(); }

	bool open(const std::string& name, std::string& error)
	{
		device = name;
		dr = drive::openDevice(name, error);
		if (!dr || !dr->readToc(toc, error))
			return false;
		sectors = toc.back().lba;
		if (sectors == 0 || sectors > 450000)
		{
			error = "the disc is not a CD";
			return false;
		}
		// Where sectors can be read: the tracks, but for the gap between sessions.
		for (size_t i = 0; i + 1 < toc.size(); i++)
		{
			uint32_t end = toc[i + 1].lba;
			const bool data = (toc[i + 1].control & 4) != 0 && toc[i + 1].track != 0xAA;
			if (i + 2 < toc.size() + 0 && data && (toc[i].control & 4) == 0)
				end = end > SessionGap ? end - SessionGap : end;
			ranges.emplace_back(toc[i].lba, end);
		}
		return chooseMode(error);
	}

	bool raw() const { return mode == Mode::Raw; }
	uint32_t stride() const { return raw() ? drive::RawSector : drive::DataSector; }
	bool isData(uint32_t lba) const
	{
		for (size_t i = toc.size() - 1; i-- > 0;)
			if (lba >= toc[i].lba)
				return (toc[i].control & 0x04) != 0;
		return true;
	}
	bool readable(uint32_t lba) const
	{
		for (const auto& r : ranges)
			if (lba >= r.first && lba < r.second)
				return true;
		return false;
	}
	drive::Drive& drv() { return *dr; }
	const std::vector<std::pair<uint32_t, uint32_t>>& trackRanges() const { return ranges; }
	void keepNothing() { keepBlocks = false; }

	// The sector at `lba`, `stride()` bytes into `out`.
	bool readSector(uint32_t lba, uint8_t *out)
	{
		if (lba >= sectors || !readable(lba))
			return false;
		const uint32_t b = lba / Block;
		for (int pass = 0; pass < 2; pass++)
		{
			{
				std::lock_guard<std::mutex> lock(cacheMutex);
				if (const uint8_t *data = cached(b))
				{
					std::memcpy(out, data + (size_t)(lba % Block) * stride(), stride());
					lastBlock = b;
					return true;
				}
				if (bad.count(lba) != 0)
					return false;
			}
			if (pass == 1)
				break;
			// Another thread may be reading it: this one waits for the drive and looks again.
			waiting++;
			std::lock_guard<std::mutex> drive(driveMutex);
			waiting--;
			{
				std::lock_guard<std::mutex> lock(cacheMutex);
				if (cached(b) != nullptr)
					continue;
			}
			fetch(b, true);
		}
		return false;
	}

	// The rest of the disc, in memory, behind the game.
	void startFill()
	{
		if (filler.joinable())
			return;
		filler = std::thread([this] { fillLoop(); });
	}

	void stopFill()
	{
		stopping = true;
		if (filler.joinable())
			filler.join();
	}

	float share()
	{
		const uint32_t total = blockCount();
		if (total == 0)
			return 0;
		return std::min(1.f, (float)used.load() / (float)total);
	}

	bool complete() { return used.load() >= blockCount(); }

	// A sector at `lba` is in memory: what the log says about it.
	const char *modeName() const { return mode == Mode::Raw ? "whole sectors (READ CD)" : "2048 bytes a sector"; }

private:
	enum class Mode { Raw, Data };
	std::unique_ptr<drive::Drive> dr;
	Mode mode = Mode::Raw;
	std::vector<std::pair<uint32_t, uint32_t>> ranges;		// [begin, end) of the sectors a track has
	std::mutex driveMutex;
	std::mutex cacheMutex;
	std::vector<std::unique_ptr<uint8_t[]>> blocks;
	std::vector<char> have;
	std::vector<uint8_t> scratch;			// the block last read that was not kept
	uint32_t scratchBlock = ~0u;
	std::set<uint32_t> bad;					// sectors the drive would not give
	std::atomic<uint32_t> used{ 0 };
	std::atomic<int> waiting{ 0 };
	std::atomic<uint32_t> lastBlock{ 0 };
	std::thread filler;
	unsigned appliedSetting = ~0u;
	std::string appliedFor;
	bool told = false;
	bool keepBlocks = true;

	uint32_t blockCount() const { return (sectors + Block - 1) / Block; }

	// Under cacheMutex.
	const uint8_t *cached(uint32_t b)
	{
		if (b < have.size() && have[b])
			return blocks[b].get();
		if (b == scratchBlock)
			return scratch.data();
		return nullptr;
	}

	bool chooseMode(std::string& error)
	{
		// Whole sectors if the drive gives them (a data track's sync pattern is the proof), else 2048 bytes.
		size_t data = toc.size();
		for (size_t i = 0; i + 1 < toc.size(); i++)
			if (toc[i].control & 4)
			{
				data = i;
				break;
			}
		std::vector<uint8_t> buffer(drive::RawSector);
		if (data == toc.size())
		{
			mode = Mode::Raw;
		}
		else if (dr->readCd(toc[data].lba, 1, drive::Sub::None, buffer.data()) && syncIsRight(buffer.data()))
		{
			mode = Mode::Raw;
		}
		else if (dr->readData(toc[data].lba, 1, buffer.data()))
		{
			mode = Mode::Data;
		}
		else
		{
			error = "the drive did not give the disc's sectors in any way";
			return false;
		}
		blocks.resize(blockCount());
		have.assign(blockCount(), 0);
		scratch.assign((size_t)Block * stride(), 0);
		diag::mark("disc: %s: %u sectors, %d track(s), read as %s (commands through %s)", device.c_str(), sectors,
				(int)toc.size() - 1, modeName(), dr->route().c_str());
		applySpeed();
		return true;
	}

	void applySpeed()
	{
		appliedSetting = chosenSpeed();
		const unsigned speed = speedOf(device);
		const bool ok = dr->setSpeed(speed);
		diag::mark("disc: %s: read %s%s", device.c_str(), speedText(speed).c_str(), ok ? "" : "; the drive did not take the speed");
	}

	bool rest(int ms)
	{
		for (int waited = 0; waited < ms && !stopping; waited += 50)
			usleep(50000);
		return !stopping;
	}

	// One try at a stretch of sectors.
	bool readOnce(uint32_t lba, uint32_t count, uint8_t *out)
	{
		const uint32_t each = stride();
		uint32_t i = 0;
		while (i < count)
		{
			uint32_t run = 1;
			const bool d = isData(lba + i);
			while (i + run < count && isData(lba + i + run) == d && readable(lba + i + run) == readable(lba + i))
				run++;
			uint8_t *at = out + (size_t)i * each;
			if (!readable(lba + i))
				std::memset(at, 0, (size_t)run * each);
			else if (mode == Mode::Raw)
			{
				// Audio asked for as CD audio, and as "any type" if the drive refuses that.
				if (!(!d && dr->readCd(lba + i, run, drive::Sub::None, at, true)) && !dr->readCd(lba + i, run, drive::Sub::None, at, false))
					return false;
			}
			else if (d)
			{
				if (!dr->readData(lba + i, run, at))
					return false;
			}
			else
				std::memset(at, 0, (size_t)run * each);
			i += run;
		}
		return true;
	}

	// Under driveMutex: block `b`, into the cache (or the scratch block when memory is spent).
	void fetch(uint32_t b, bool tell)
	{
		if (appliedSetting != chosenSpeed() && appliedSetting != ~0u)
			applySpeed();
		const uint32_t first = b * Block;
		const uint32_t count = std::min(Block, sectors - first);
		const uint32_t each = stride();
		std::vector<uint8_t> data((size_t)Block * each, 0);
		bool whole = false;
		for (int attempt = 0; attempt < 3 && !stopping; attempt++)
		{
			if (readOnce(first, count, data.data()))
			{
				whole = true;
				break;
			}
			// After the second failure in a row the drive is slowed a step.
			if (attempt == 1 && slowDown(device))
			{
				diag::mark("disc: %s: reads at sector %u failed twice; slowing the drive", device.c_str(), first);
				applySpeed();
			}
			if (!rest(300 * (attempt + 1)))
				break;
		}
		std::vector<uint32_t> failed;
		if (!whole && !stopping)
		{
			// One at a time, so a scratch costs only the sectors under it.
			for (uint32_t i = 0; i < count && !stopping; i++)
				if (!readOnce(first + i, 1, data.data() + (size_t)i * each))
				{
					std::memset(data.data() + (size_t)i * each, 0, each);
					failed.push_back(first + i);
				}
		}
		if (stopping)
			return;
		if (!failed.empty() && !told)
		{
			told = true;
			diag::mark("disc: %s: %zu sector(s) from %u could not be read; they are silence or zeros", device.c_str(), failed.size(), failed[0]);
		}
		std::lock_guard<std::mutex> lock(cacheMutex);
		for (uint32_t lba : failed)
			bad.insert(lba);
		// A block with a sector that will not read is not kept as if it were whole.
		if (keepBlocks && failed.empty() && (size_t)used.load() * Block * each < BudgetBytes)
		{
			std::unique_ptr<uint8_t[]> kept(new (std::nothrow) uint8_t[(size_t)Block * each]);
			if (kept)
			{
				std::memcpy(kept.get(), data.data(), (size_t)Block * each);
				blocks[b] = std::move(kept);
				have[b] = 1;
				used++;
				return;
			}
		}
		scratch = std::move(data);
		scratchBlock = b;
	}

	void fillLoop()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
		uint32_t cursor = 0;
		const uint32_t total = blockCount();
		bool told = false;
		while (!stopping)
		{
			if ((size_t)used.load() * Block * stride() >= BudgetBytes || used.load() >= total)
				break;
			// The next block that is not in memory, from just after the one the game read last.
			uint32_t b = ~0u;
			{
				std::lock_guard<std::mutex> lock(cacheMutex);
				cursor = std::max(cursor, (uint32_t)lastBlock.load() + 1) % total;
				for (uint32_t k = 0; k < total; k++)
				{
					const uint32_t c = (cursor + k) % total;
					if (!have[c] && !(c * Block < sectors && !readable(c * Block) && !readable(std::min(c * Block + Block, sectors) - 1)))
					{
						b = c;
						break;
					}
				}
			}
			if (b == ~0u)
				break;
			cursor = b + 1;
			if (waiting.load() > 0)
			{
				usleep(2000);
				continue;
			}
			std::lock_guard<std::mutex> drive(driveMutex);
			{
				std::lock_guard<std::mutex> lock(cacheMutex);
				if (have[b])
					continue;
			}
			fetch(b, false);
			if (!told && used.load() >= total)
			{
				told = true;
				diag::mark("disc: %s: the whole disc is in memory", device.c_str());
			}
			usleep(0);
		}
	}
};

// ---- the disc, as Flycast's drive sees it

struct DriveTrack final : TrackFile
{
	DriveTrack(std::shared_ptr<Source> source, bool audio) : source(std::move(source)), audio(audio) {}

	bool Read(u32 FAD, u8 *dst, SectorFormat *sectorType, u8 *subcode, SubcodeFormat *subcodeType) override
	{
		if (FAD < Pregap)
			return false;
		const uint32_t lba = FAD - Pregap;
		if (lba >= source->sectors)
			return false;
		*subcodeType = SUBFMT_NONE;
		if (!source->readSector(lba, dst))
			return false;
		if (source->raw())
			*sectorType = SECFMT_2352;
		else if (audio)
		{
			// Audio the drive gave no sectors for is silence.
			std::memset(dst, 0, 2352);
			*sectorType = SECFMT_2352;
		}
		else
			*sectorType = SECFMT_2048_MODE2_FORM1;
		return true;
	}

	std::shared_ptr<Source> source;
	bool audio;
};

struct DriveDisc final : Disc
{
	explicit DriveDisc(std::shared_ptr<Source> source) : source(std::move(source)) {}

	~DriveDisc() override
	{
		source->stopFill();
	}

	bool build()
	{
		const std::vector<drive::TocEntry>& toc = source->toc;
		type = CdDA;
		for (size_t i = 0; i + 1 < toc.size(); i++)
		{
			const bool data = (toc[i].control & 4) != 0;
			Track t;
			t.ADR = 1;
			t.CTRL = data ? 4 : 0;
			t.StartFAD = toc[i].lba + Pregap;
			t.EndFAD = toc[i + 1].lba + Pregap - 1;
			if (i == 0)
			{
				Session session;
				session.StartFAD = t.StartFAD;
				session.FirstTrack = toc[i].track;
				sessions.push_back(session);
				type = data ? CdRom : CdDA;
			}
			else if (data && !tracks.back().isDataTrack())
			{
				// Session 1's lead-out and session 2's lead-in and pregap lie before this track.
				tracks.back().EndFAD -= SessionGap;
				type = CdRom_XA;
				Session session;
				session.StartFAD = t.StartFAD;
				session.FirstTrack = toc[i].track;
				sessions.push_back(session);
			}
			t.file = new DriveTrack(source, !data);
			tracks.push_back(t);
		}
		if (tracks.empty())
			return false;
		LeadOut.StartFAD = toc.back().lba + Pregap;
		LeadOut.ADR = 1;
		LeadOut.CTRL = 4;
		EndFAD = LeadOut.StartFAD;
		return true;
	}

	std::shared_ptr<Source> source;
	std::shared_ptr<void> lease;		// keeps the drive marked as in use
};

// ---- what the library lists

std::mutex listMutex;
std::vector<Info> infos;
std::atomic<unsigned> listGeneration{ 1 };
std::string noDrive;
std::map<std::string, int> inUse;			// drives a game is played from: left alone by the watcher
std::atomic<Source *> playing{ nullptr };
std::thread watcher;
std::atomic<bool> watching{ false };
std::atomic<bool> stopWatching{ false };

std::string idPart(const std::string& id)
{
	std::string out;
	for (char c : id)
		out += (c == '#' || c == ':' || c == '/' || c == ' ') ? '-' : c;
	return out;
}

std::string deviceOf(const std::string& path)
{
	std::string rest = path.substr(sizeof(Scheme) - 1);
	const size_t hash = rest.find('#');
	return hash == std::string::npos ? rest : rest.substr(0, hash);
}

// What the disc is, from its header: a Dreamcast disc starts its data track with "SEGA SEGAKATANA".
void identify(const std::shared_ptr<Source>& source, Info& info)
{
	auto disc = std::make_unique<DriveDisc>(source);
	if (!disc->build())
	{
		info.state = "Not a Dreamcast disc";
		info.problem = "The disc has no tracks.";
		return;
	}
	// The game's data is the last session's first data track; a plain data CD has it first.
	const Track *data = nullptr;
	for (const Track& track : disc->tracks)
		if (track.isDataTrack())
		{
			data = &track;
			if (disc->type == CdRom_XA)
				break;
		}
	if (data == nullptr)
	{
		info.state = "Not a Dreamcast disc";
		info.problem = "It has no data track: it is an audio CD.";
		return;
	}
	u8 header[2048] = {};
	if (disc->ReadSectors(data->StartFAD, 1, header, 2048) != 1 || std::memcmp(header, "SEGA SEGAKATANA", 15) != 0)
	{
		info.state = "Not a Dreamcast disc";
		info.problem = "Its data does not start with a Dreamcast header: insert a Dreamcast disc.";
		return;
	}
	info.id = trimmed(header + 0x40, 10);
	info.title = trimmed(header + 0x80, 128);
	if (info.title.empty())
		info.title = info.id.empty() ? "Dreamcast disc" : info.id;
	info.playable = true;
	info.state = "Ready";
	info.problem.clear();
	info.path = std::string(Scheme) + info.device + "#" + idPart(info.id);
}

struct Watched
{
	std::unique_ptr<drive::Drive> drive;
	std::string signature;
	std::string tocError;
	Info info;
	bool unlocked = false;
	bool told = false;
};

std::string signatureOf(const std::vector<drive::TocEntry>& toc)
{
	std::string s;
	for (const drive::TocEntry& e : toc)
		s += std::to_string(e.track) + ":" + std::to_string(e.lba) + ":" + std::to_string(e.control) + ";";
	return s;
}

void watch()
{
	PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
	std::map<std::string, Watched> drives;
	std::string lastNoDrive;
	while (!stopWatching)
	{
		const std::vector<std::string> names = drive::deviceNames();
		bool changed = false;
		for (const std::string& name : names)
		{
			{
				std::lock_guard<std::mutex> lock(listMutex);
				if (inUse[name] > 0)
					continue;
			}
			Watched& w = drives[name];
			w.info.device = name;
			if (!w.drive)
			{
				std::string error;
				w.drive = drive::openDevice(name, error);
				if (!w.drive)
				{
					if (w.info.problem != error)
						changed = true;
					w.info.present = false;
					w.info.state = "Drive not opened";
					w.info.problem = error;
					continue;
				}
				w.info.drive = w.drive->identity();
				const int usb = w.drive->onUsb();
				w.info.onUsb = usb != 0;
				diag::mark("disc: %s: %s%s", name.c_str(), w.info.drive.empty() ? "a drive" : w.info.drive.c_str(),
						usb == 0 ? " (the console's own drive: left alone)" : "");
				changed = true;
			}
			// The console's own drive is not offered: its discs are the console's.
			if (w.drive->onUsb() == 0)
			{
				if (w.info.state != "The console's own drive")
					changed = true;
				w.info.state = "The console's own drive";
				w.info.present = false;
				continue;
			}
			if (!w.unlocked)
			{
				// A console started with a disc in a USB drive locks the tray.
				std::string how;
				const bool ok = w.drive->allowRemoval(how);
				diag::mark("disc: %s: tray %s (%s)", name.c_str(), ok ? "unlocked" : "left locked", how.c_str());
				w.unlocked = true;
			}
			std::vector<drive::TocEntry> toc;
			std::string error;
			if (!w.drive->readToc(toc, error))
			{
				if (w.info.present || w.info.problem != error || w.info.state != "No disc")
					changed = true;
				w.signature.clear();
				w.info.present = w.info.playable = false;
				w.info.title.clear();
				w.info.id.clear();
				w.info.path.clear();
				w.info.state = "No disc";
				w.info.problem = error;
				continue;
			}
			const std::string signature = signatureOf(toc);
			if (signature == w.signature)
				continue;
			// A disc: what is it.
			w.signature = signature;
			w.info.present = true;
			w.info.playable = false;
			w.info.title.clear();
			w.info.id.clear();
			w.info.path.clear();
			auto source = std::make_shared<Source>();
			std::string why;
			if (source->open(name, why))
				identify(source, w.info);
			else
			{
				w.info.state = "Disc not readable";
				w.info.problem = why;
			}
			source.reset();
			diag::mark("disc: %s: %s%s%s%s", name.c_str(), w.info.state.c_str(), w.info.title.empty() ? "" : ": ",
					w.info.title.c_str(), w.info.id.empty() ? "" : (" (" + w.info.id + ")").c_str());
			changed = true;
		}
		// A drive that is gone.
		for (auto it = drives.begin(); it != drives.end();)
		{
			if (std::find(names.begin(), names.end(), it->first) == names.end())
			{
				it = drives.erase(it);
				changed = true;
			}
			else
				++it;
		}
		std::string reason;
		if (names.empty())
		{
			if (!ps5::elevated)
				reason = "PSFlyCast is inside its sandbox, which shows no drives. Run the ELF loader on port 9021 and start PSFlyCast again.";
			else
				reason = "The console lists no disc drive. A full-size drive needs its own power adapter, plugged in and switched on. "
						"Try another USB port and the drive's own cable, and plug it in while PSFlyCast is running: it is looked for every "
						"two seconds. The boot log's \"disc:\", \"usb drive:\" and \"usb bus:\" lines say what the console lists.";
		}
		{
			std::lock_guard<std::mutex> lock(listMutex);
			if (reason != noDrive)
			{
				noDrive = reason;
				changed = true;
			}
			if (changed)
			{
				infos.clear();
				for (auto& entry : drives)
				{
					// A drive in use by a game keeps the entry it had.
					infos.push_back(entry.second.info);
				}
				// Drives in use are not in `drives` updated; keep them listed.
			}
		}
		if (changed)
			listGeneration++;
		for (int waited = 0; waited < 2000 && !stopWatching; waited += 100)
			usleep(100000);
	}
}


// ---- copying the disc to an image file (CDI or CHD)

struct CopyState
{
	std::mutex mutex;
	std::thread thread;
	std::atomic<bool> running{ false };
	std::atomic<bool> cancel{ false };
	CopyStatus status;
};
CopyState copyState;

// A sector as a drive that gives only the 2048 bytes of user data would have it: sync, header and
// subheader are rebuilt (Mode 2 Form 1); the error codes at the end are not.
void rebuildSector(uint8_t *out, const uint8_t *user, uint32_t lba)
{
	static const uint8_t sync[12] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
	std::memcpy(out, sync, 12);
	const uint32_t fad = lba + Pregap;
	auto bcd = [](uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); };
	out[12] = bcd(fad / 75 / 60);
	out[13] = bcd(fad / 75 % 60);
	out[14] = bcd(fad % 75);
	out[15] = 2;
	static const uint8_t sub[8] = { 0, 0, 8, 0, 0, 0, 8, 0 };
	std::memcpy(out + 16, sub, 8);
	std::memcpy(out + 24, user, 2048);
	std::memset(out + 2072, 0, 280);
}

struct DumpTrack
{
	uint32_t first = 0;		// the first address
	uint32_t count = 0;		// sectors
	bool data = false;
};

// Reads the sectors of one track, whole (2352 bytes), to `sink`.
bool readTrack(Source& source, const DumpTrack& track, const std::function<bool(const uint8_t *)>& sink, uint32_t& unreadable)
{
	std::vector<uint8_t> in(source.stride());
	std::vector<uint8_t> out(drive::RawSector);
	for (uint32_t i = 0; i < track.count; i++)
	{
		if (copyState.cancel)
			return false;
		const uint32_t lba = track.first + i;
		if (!source.readSector(lba, in.data()))
		{
			std::fill(out.begin(), out.end(), 0);
			unreadable++;
		}
		else if (source.raw())
			std::memcpy(out.data(), in.data(), drive::RawSector);
		else
			rebuildSector(out.data(), in.data(), lba);
		if (!sink(out.data()))
			return false;
	}
	return true;
}

// ---- SHA-1, for the CHD's checksums
struct Sha1
{
	uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
	uint8_t buffer[64];
	uint64_t length = 0;
	size_t used = 0;

	static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
	void block(const uint8_t *p)
	{
		uint32_t w[80];
		for (int i = 0; i < 16; i++)
			w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
		for (int i = 16; i < 80; i++)
			w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
		for (int i = 0; i < 80; i++)
		{
			uint32_t f, k;
			if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
			else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
			else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
			else { f = b ^ c ^ d; k = 0xCA62C1D6; }
			const uint32_t t = rol(a, 5) + f + e + k + w[i];
			e = d; d = c; c = rol(b, 30); b = a; a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
	}
	void add(const void *data, size_t size)
	{
		const uint8_t *p = (const uint8_t *)data;
		length += size;
		while (size > 0)
		{
			const size_t n = std::min(size, sizeof(buffer) - used);
			std::memcpy(buffer + used, p, n);
			used += n; p += n; size -= n;
			if (used == sizeof(buffer))
			{
				block(buffer);
				used = 0;
			}
		}
	}
	void finish(uint8_t out[20])
	{
		const uint64_t bits = length * 8;
		const uint8_t one = 0x80;
		add(&one, 1);
		const uint8_t zero = 0;
		while (used != 56)
			add(&zero, 1);
		uint8_t len[8];
		for (int i = 0; i < 8; i++)
			len[i] = (uint8_t)(bits >> (56 - 8 * i));
		add(len, 8);
		for (int i = 0; i < 5; i++)
		{
			out[i * 4] = (uint8_t)(h[i] >> 24);
			out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
			out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
			out[i * 4 + 3] = (uint8_t)h[i];
		}
	}
};

void put32(std::vector<uint8_t>& v, uint32_t x, bool big)
{
	for (int i = 0; i < 4; i++)
		v.push_back((uint8_t)(x >> (big ? 24 - 8 * i : 8 * i)));
}

void put16(std::vector<uint8_t>& v, uint16_t x)
{
	v.push_back((uint8_t)x);
	v.push_back((uint8_t)(x >> 8));
}

void zeros(std::vector<uint8_t>& v, size_t n)
{
	v.insert(v.end(), n, 0);
}

// DiscJuggler (3.5) image: the tracks' sectors one after the other, then the descriptor Flycast's reader
// (core/deps/chdpsr/cdipsr.cpp) takes the tracks from. Sessions: a data track after audio starts one.
bool writeCdi(FILE *f, Source& source, const std::vector<DumpTrack>& tracks, const std::function<void(uint32_t)>& progress, uint32_t& unreadable, std::string& error)
{
	uint32_t done = 0;
	for (const DumpTrack& t : tracks)
	{
		const bool ok = readTrack(source, t, [&](const uint8_t *sector) {
			if (fwrite(sector, 1, drive::RawSector, f) != drive::RawSector)
			{
				error = "the file could not be written (is the disk full?)";
				return false;
			}
			progress(++done);
			return true;
		}, unreadable);
		if (!ok)
			return false;
	}
	std::vector<std::vector<size_t>> sessions;
	for (size_t i = 0; i < tracks.size(); i++)
	{
		if (i == 0 || (tracks[i].data && !tracks[i - 1].data))
			sessions.emplace_back();
		sessions.back().push_back(i);
	}
	std::vector<uint8_t> d;
	put16(d, (uint16_t)sessions.size());
	static const uint8_t mark[10] = { 0, 0, 0x01, 0, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF };
	for (const auto& session : sessions)
	{
		put16(d, (uint16_t)session.size());
		for (size_t index : session)
		{
			const DumpTrack& t = tracks[index];
			put32(d, 0, false);
			d.insert(d.end(), mark, mark + 10);
			d.insert(d.end(), mark, mark + 10);
			zeros(d, 4);
			d.push_back(0);				// the file name's length
			zeros(d, 11);
			zeros(d, 4);
			zeros(d, 4);
			put32(d, 0, false);
			zeros(d, 2);
			put32(d, 0, false);			// pregap
			put32(d, t.count, false);	// length
			zeros(d, 6);
			put32(d, t.data ? 2 : 0, false);	// mode: audio, or Mode 2
			zeros(d, 12);
			put32(d, t.first + Pregap, false);	// start address
			put32(d, t.count, false);	// total length
			zeros(d, 16);
			put32(d, 2, false);			// 2352 bytes a sector
			zeros(d, 29);
			zeros(d, 5);
			put32(d, 0, false);
		}
		zeros(d, 4 + 8 + 1);
	}
	const uint32_t offset = (uint32_t)d.size() + 8;
	put32(d, 0x80000006, false);
	put32(d, offset, false);
	if (fwrite(d.data(), 1, d.size(), f) != d.size())
	{
		error = "the file could not be written (is the disk full?)";
		return false;
	}
	return true;
}

// CHD version 5, not compressed (every hunk is eight frames of 2352 bytes of sector and 96 of subcode, the
// way chdman lays out a CD; chdman can compress it afterwards).
bool writeChd(FILE *f, Source& source, const std::vector<DumpTrack>& tracks, const std::function<void(uint32_t)>& progress, uint32_t& unreadable, std::string& error)
{
	constexpr uint32_t FrameBytes = 2352 + 96;
	constexpr uint32_t FramesPerHunk = 8;
	constexpr uint32_t HunkBytes = FrameBytes * FramesPerHunk;
	uint32_t totalFrames = 0;
	for (const DumpTrack& t : tracks)
		totalFrames += (t.count + 3) / 4 * 4;
	const uint32_t hunks = (totalFrames + FramesPerHunk - 1) / FramesPerHunk;
	const uint64_t logical = (uint64_t)totalFrames * FrameBytes;

	// Metadata: one CHT2 entry a track.
	struct Meta { std::string text; uint8_t hash[20]; };
	std::vector<Meta> meta;
	for (size_t i = 0; i < tracks.size(); i++)
	{
		char line[200];
		snprintf(line, sizeof(line), "TRACK:%zu TYPE:%s SUBTYPE:NONE FRAMES:%u PREGAP:0 PGTYPE:MODE1 PGSUB:NONE POSTGAP:0", i + 1,
				tracks[i].data ? "MODE2_RAW" : "AUDIO", tracks[i].count);
		Meta m;
		m.text = line;
		Sha1 sha;
		sha.add(m.text.c_str(), m.text.size() + 1);
		sha.finish(m.hash);
		meta.push_back(m);
	}
	const uint64_t mapOffset = 124;
	const uint64_t metaOffset = mapOffset + (uint64_t)hunks * 4;
	uint64_t metaEnd = metaOffset;
	for (const Meta& m : meta)
		metaEnd += 16 + m.text.size() + 1;
	const uint64_t dataStart = (metaEnd + HunkBytes - 1) / HunkBytes * HunkBytes;

	// The map and the metadata first (their places are known), the hunks after, the header last.
	std::vector<uint8_t> front;
	for (uint32_t i = 0; i < hunks; i++)
		put32(front, (uint32_t)(dataStart / HunkBytes + i), true);
	uint64_t at = metaOffset;
	for (size_t i = 0; i < meta.size(); i++)
	{
		const Meta& m = meta[i];
		front.push_back('C'); front.push_back('H'); front.push_back('T'); front.push_back('2');
		front.push_back(1);		// checksummed
		const uint32_t len = (uint32_t)m.text.size() + 1;
		front.push_back((uint8_t)(len >> 16)); front.push_back((uint8_t)(len >> 8)); front.push_back((uint8_t)len);
		const uint64_t next = i + 1 < meta.size() ? at + 16 + len : 0;
		for (int b = 0; b < 8; b++)
			front.push_back((uint8_t)(next >> (56 - 8 * b)));
		front.insert(front.end(), m.text.begin(), m.text.end());
		front.push_back(0);
		at += 16 + len;
	}
	if (fseek(f, 124, SEEK_SET) != 0 || fwrite(front.data(), 1, front.size(), f) != front.size())
	{
		error = "the file could not be written (is the disk full?)";
		return false;
	}
	if (fseek(f, (long)dataStart, SEEK_SET) != 0)
	{
		error = "the file could not be written";
		return false;
	}

	Sha1 raw;
	std::vector<uint8_t> hunk;
	hunk.reserve(HunkBytes);
	uint64_t written = 0;
	auto flushFrames = [&](bool last) -> bool {
		while (hunk.size() >= HunkBytes || (last && !hunk.empty()))
		{
			hunk.resize(std::max<size_t>(hunk.size(), HunkBytes), 0);
			const uint64_t take = std::min<uint64_t>(HunkBytes, logical - written);
			raw.add(hunk.data(), (size_t)take);
			written += take;
			if (fwrite(hunk.data(), 1, HunkBytes, f) != HunkBytes)
			{
				error = "the file could not be written (is the disk full?)";
				return false;
			}
			hunk.erase(hunk.begin(), hunk.begin() + HunkBytes);
		}
		return true;
	};
	auto addFrame = [&](const uint8_t *sector, bool swap) -> bool {
		const size_t at = hunk.size();
		hunk.resize(at + FrameBytes, 0);
		std::memcpy(hunk.data() + at, sector, 2352);
		if (swap)
			for (size_t i = 0; i < 2352; i += 2)
				std::swap(hunk[at + i], hunk[at + i + 1]);
		return flushFrames(false);
	};
	uint32_t done = 0;
	const uint8_t blank[2352] = {};
	for (const DumpTrack& t : tracks)
	{
		if (!readTrack(source, t, [&](const uint8_t *sector) {
				progress(++done);
				return addFrame(sector, !t.data);
			}, unreadable))
			return false;
		for (uint32_t pad = t.count; pad % 4 != 0; pad++)
			if (!addFrame(blank, false))
				return false;
	}
	if (!flushFrames(true))
		return false;

	// The checksums: of the data, and of that with the metadata's.
	uint8_t rawSha[20], overall[20];
	raw.finish(rawSha);
	std::vector<std::pair<std::string, int>> order;
	Sha1 all;
	all.add(rawSha, 20);
	std::vector<std::vector<uint8_t>> hashes;
	for (const Meta& m : meta)
	{
		std::vector<uint8_t> entry{ 'C', 'H', 'T', '2' };
		entry.insert(entry.end(), m.hash, m.hash + 20);
		hashes.push_back(entry);
	}
	std::sort(hashes.begin(), hashes.end());
	for (const auto& entry : hashes)
		all.add(entry.data(), entry.size());
	all.finish(overall);

	std::vector<uint8_t> header;
	const char tag[8] = { 'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D' };
	header.insert(header.end(), tag, tag + 8);
	put32(header, 124, true);
	put32(header, 5, true);
	zeros(header, 16);	// no compressors
	for (int b = 0; b < 8; b++) header.push_back((uint8_t)(logical >> (56 - 8 * b)));
	for (int b = 0; b < 8; b++) header.push_back((uint8_t)(mapOffset >> (56 - 8 * b)));
	for (int b = 0; b < 8; b++) header.push_back((uint8_t)(metaOffset >> (56 - 8 * b)));
	put32(header, HunkBytes, true);
	put32(header, FrameBytes, true);
	header.insert(header.end(), rawSha, rawSha + 20);
	header.insert(header.end(), overall, overall + 20);
	zeros(header, 20);	// no parent
	if (fseek(f, 0, SEEK_SET) != 0 || fwrite(header.data(), 1, header.size(), f) != header.size())
	{
		error = "the file could not be written";
		return false;
	}
	return true;
}

std::string safeFileName(const std::string& s)
{
	std::string out;
	for (char c : s)
		out += (strchr("<>:\"/\\|?*", c) != nullptr || (unsigned char)c < 0x20) ? '-' : c;
	while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
		out.pop_back();
	return out.empty() ? std::string("Dreamcast disc") : out;
}

void copyWork(std::string device, CopyFormat format, std::string title, std::string id)
{
	PS5Priority::SetCurrentThread(PS5Priority::Role::Background);
	auto finish = [&](bool ok, const std::string& text, const std::string& result) {
		std::lock_guard<std::mutex> lock(copyState.mutex);
		copyState.status.running = false;
		copyState.status.finished = true;
		copyState.status.ok = ok;
		copyState.status.text = text;
		copyState.status.result = result;
		copyState.status.progress = ok ? 1.f : copyState.status.progress;
		copyState.running = false;
	};
	{
		std::lock_guard<std::mutex> lock(listMutex);
		inUse[device]++;
	}
	struct Release
	{
		std::string device;
		~Release()
		{
			std::lock_guard<std::mutex> lock(listMutex);
			inUse[device]--;
			listGeneration++;
		}
	} release{ device };

	auto source = std::make_shared<Source>();
	std::string error;
	if (!source->open(device, error))
		return finish(false, "The disc could not be read: " + error, "");
	source->keepNothing();
	std::vector<DumpTrack> tracks;
	const auto& ranges = source->trackRanges();
	for (size_t i = 0; i < ranges.size(); i++)
	{
		DumpTrack t;
		t.first = ranges[i].first;
		t.count = ranges[i].second - ranges[i].first;
		t.data = (source->toc[i].control & 4) != 0;
		if (t.count > 0)
			tracks.push_back(t);
	}
	if (tracks.empty())
		return finish(false, "The disc has no tracks to copy.", "");
	bool audio = false;
	uint32_t total = 0;
	for (const DumpTrack& t : tracks)
	{
		audio = audio || !t.data;
		total += t.count;
	}
	if (audio && !source->raw())
		return finish(false, "This drive gives no audio tracks (only the 2048 bytes of data sectors), so the disc cannot be copied whole.", "");

	const std::string folder = ps5::rootDir + "games/";
	::mkdir(folder.c_str(), 0777);
	std::string base = safeFileName(title);
	if (!id.empty())
		base += " [" + safeFileName(id) + "]";
	const char *ext = format == CopyFormat::Cdi ? ".cdi" : ".chd";
	const std::string finalPath = folder + base + ext;
	const std::string tempPath = finalPath + ".part";
	struct statvfs vfs{};
	const uint64_t need = (uint64_t)total * (format == CopyFormat::Chd ? 2448 : 2352) + (1u << 20);
	if (statvfs(folder.c_str(), &vfs) == 0 && (uint64_t)vfs.f_bavail * vfs.f_frsize < need)
		return finish(false, "There is not enough free space: the copy needs " + std::to_string(need >> 20) + " MB, and "
				+ std::to_string(((uint64_t)vfs.f_bavail * vfs.f_frsize) >> 20) + " MB are free.", "");
	FILE *f = fopen(tempPath.c_str(), "wb");
	if (f == nullptr)
		return finish(false, "The file " + base + ext + " could not be created in the games folder.", "");

	diag::mark("disc: copying %s to %s (%u sectors)", device.c_str(), finalPath.c_str(), total);
	uint32_t unreadable = 0;
	auto progress = [&](uint32_t done) {
		if ((done & 0x3F) == 0 || done == total)
		{
			std::lock_guard<std::mutex> lock(copyState.mutex);
			copyState.status.progress = total ? (float)done / (float)total : 0.f;
		}
	};
	bool ok = format == CopyFormat::Cdi ? writeCdi(f, *source, tracks, progress, unreadable, error)
			: writeChd(f, *source, tracks, progress, unreadable, error);
	fclose(f);
	if (!ok)
	{
		unlink(tempPath.c_str());
		return finish(false, copyState.cancel ? std::string("Cancelled.") : "The copy stopped: " + error, "");
	}
	if (rename(tempPath.c_str(), finalPath.c_str()) != 0)
	{
		unlink(tempPath.c_str());
		return finish(false, "The finished copy could not be renamed.", "");
	}
	std::string text = "Copied to the games folder as " + base + ext + ".";
	if (!source->raw())
		text += " The drive gave only the data of each sector: the error-correction codes were not rebuilt.";
	if (unreadable > 0)
		text += " " + std::to_string(unreadable) + " sector(s) could not be read and are zeros.";
	diag::mark("disc: copy done: %s (%u unreadable)", finalPath.c_str(), unreadable);
	finish(true, text, finalPath);
}

}	// namespace

// ---- the public interface

void start()
{
	if (!options().discDrive || watching.exchange(true))
		return;
	if (!elevated)
		diag::mark("disc: the sandbox was not left; no drive can be seen");
	stopWatching = false;
	watcher = std::thread(watch);
}

void stop()
{
	copyState.cancel = true;
	if (copyState.thread.joinable())
		copyState.thread.join();
	if (!watching.exchange(false))
		return;
	stopWatching = true;
	if (watcher.joinable())
		watcher.join();
}

std::vector<Info> list()
{
	std::lock_guard<std::mutex> lock(listMutex);
	return infos;
}

unsigned generation()
{
	return listGeneration.load();
}

bool isPath(const std::string& path)
{
	return path.rfind(Scheme, 0) == 0;
}

std::string fileName(const std::string& path)
{
	const size_t hash = path.find('#');
	const std::string id = hash == std::string::npos ? std::string() : path.substr(hash + 1);
	return (id.empty() ? "disc" : id) + ".cdr";
}

bool exists(const std::string& path)
{
	if (!isPath(path))
		return false;
	const std::string device = deviceOf(path);
	std::lock_guard<std::mutex> lock(listMutex);
	if (inUse[device] > 0)
		return true;
	for (const Info& info : infos)
		if (info.device == device && info.present)
			return true;
	return false;
}

float cached()
{
	Source *s = playing.load();
	return s != nullptr ? s->share() : -1.f;
}

std::string noDriveReason()
{
	std::lock_guard<std::mutex> lock(listMutex);
	return noDrive;
}

bool eject(const std::string& device, std::string& why)
{
	{
		std::lock_guard<std::mutex> lock(listMutex);
		if (inUse[device] > 0)
		{
			why = "A game is playing from it.";
			return false;
		}
	}
	std::string error;
	std::unique_ptr<drive::Drive> d = drive::openDevice(device, error);
	if (!d)
	{
		why = error;
		return false;
	}
	std::string how;
	const bool ok = d->eject(how);
	diag::mark("disc: %s: eject %s (%s)", device.c_str(), ok ? "done" : "failed", how.c_str());
	why = ok ? "" : how;
	return ok;
}

bool copyStart(const std::string& device, CopyFormat format, const std::string& title, const std::string& id, std::string& why)
{
	if (copyState.running)
	{
		why = "A copy is already running.";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(listMutex);
		if (inUse[device] > 0)
		{
			why = "A game is playing from this drive.";
			return false;
		}
	}
	if (copyState.thread.joinable())
		copyState.thread.join();
	{
		std::lock_guard<std::mutex> lock(copyState.mutex);
		copyState.status = CopyStatus();
		copyState.status.running = true;
		copyState.status.text = "Copying the disc";
	}
	copyState.cancel = false;
	copyState.running = true;
	copyState.thread = std::thread(copyWork, device, format, title, id);
	return true;
}

CopyStatus copyStatus()
{
	std::lock_guard<std::mutex> lock(copyState.mutex);
	return copyState.status;
}

void copyCancel()
{
	copyState.cancel = true;
}

void copyAcknowledge()
{
	std::lock_guard<std::mutex> lock(copyState.mutex);
	if (!copyState.status.running)
		copyState.status = CopyStatus();
}
}	// namespace ps5::disc

// Flycast's disc drivers (core/imgread/common.cpp) ask for the path first.
Disc *ps5disc_parse(const char *path, std::vector<u8> *digest)
{
	using namespace ps5::disc;
	if (path == nullptr || !isPath(path))
		return nullptr;
	const std::string device = deviceOf(path);
	struct Lease
	{
		std::string device;
		explicit Lease(std::string d) : device(std::move(d))
		{
			std::lock_guard<std::mutex> lock(listMutex);
			inUse[device]++;
		}
		~Lease()
		{
			std::lock_guard<std::mutex> lock(listMutex);
			inUse[device]--;
			playing = nullptr;
			listGeneration++;
		}
	};
	auto lease = std::make_shared<Lease>(device);
	auto source = std::make_shared<Source>();
	std::string error;
	if (!source->open(device, error))
	{
		ps5::diag::mark("disc: %s: cannot be opened: %s", device.c_str(), error.c_str());
		return nullptr;
	}
	auto disc = std::make_unique<DriveDisc>(source);
	if (!disc->build())
		return nullptr;
	disc->lease = lease;
	playing = source.get();
	source->startFill();
	if (digest != nullptr)
	{
		digest->clear();
		for (char c : std::string(path))
			digest->push_back((u8)c);
	}
	return disc.release();
}
