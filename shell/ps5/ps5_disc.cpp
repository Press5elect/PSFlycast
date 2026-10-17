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

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unistd.h>

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
		if (failed.empty() && (size_t)used.load() * Block * each < BudgetBytes)
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
		info.problem = "Its data does not start with a Dreamcast header. PSFlyCast starts Dreamcast games burned to a CD-R "
				"(a self-boot disc or a game on a data CD); a Dreamcast's own GD-ROM cannot be read by a CD or DVD drive.";
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
