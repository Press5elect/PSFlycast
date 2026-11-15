// Crux2 - the memory card sync's engine, on two folders kept in memory.
// PSFlyCast: from Crux2 f0efa95 (ps5/tests/cardsync), with a check of Io::is_card on the names of
// Flycast's VMU images.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_card_sync.h"

#include <cstdio>
#include <map>
#include <string>

namespace
{
int g_checks = 0, g_failed = 0;
void Expect(bool ok, const char* what)
{
	g_checks++;
	if (!ok)
	{
		g_failed++;
		std::fprintf(stderr, "FAILED: %s\n", what);
	}
}

// A file system in memory: a path to its bytes and its time. Each write is a new time.
struct Files
{
	std::map<std::string, std::pair<std::string, int64_t>> files;
	int64_t clock = 1000;
	bool there_answers = true;
	bool stop = false;
	bool fail_writes_there = false;

	void Put(const std::string& path, const std::string& data) { files[path] = {data, ++clock}; }
	std::string Get(const std::string& path) const
	{
		const auto f = files.find(path);
		return f == files.end() ? std::string("<none>") : f->second.first;
	}
	size_t CountUnder(const std::string& folder) const
	{
		size_t n = 0;
		for (const auto& [p, v] : files)
			n += (p.rfind(folder + "/", 0) == 0) ? 1 : 0;
		return n;
	}

	PS5CardSync::Io Io()
	{
		PS5CardSync::Io io;
		io.list = [this](const std::string& folder, std::vector<std::string>* names) {
			if (folder.rfind("there", 0) == 0 && !there_answers)
				return false;
			for (const auto& [p, v] : files)
			{
				if (p.rfind(folder + "/", 0) == 0 && p.find('/', folder.size() + 1) == std::string::npos)
					names->push_back(p.substr(folder.size() + 1));
			}
			return true;
		};
		io.stat = [this](const std::string& path) {
			PS5CardSync::FileInfo info;
			const auto f = files.find(path);
			if (f != files.end())
			{
				info.exists = true;
				info.size = f->second.first.size();
				info.mtime = f->second.second;
			}
			return info;
		};
		io.read = [this](const std::string& path, std::string* data) {
			const auto f = files.find(path);
			if (f == files.end())
				return false;
			*data = f->second.first;
			return true;
		};
		io.write = [this](const std::string& path, const std::string& data) {
			if (fail_writes_there && path.rfind("there", 0) == 0)
				return false;
			Put(path, data);
			return true;
		};
		io.make_folder = [](const std::string&) { return true; };
		io.stop = [this] { return stop; };
		return io;
	}
};

const PS5CardSync::Folders kFolders{"here", "there", "older-here", "there/.older"};
} // namespace

int main()
{
	using namespace PS5CardSync;
	Expect(IsCard("Mcd001.ps2") && IsCard("SLUS-21059.PS2") && !IsCard(".hidden.ps2") && !IsCard("notes.txt") && !IsCard(".ps2"), "which files are cards");

	{
		// The first sync: cards on one side only go to the other; the same bytes on both are remembered.
		Files fs;
		Records records;
		fs.Put("here/Mcd001.ps2", "AAA");
		fs.Put("there/Mcd002.ps2", "BBB");
		fs.Put("here/Same.ps2", "SSS");
		fs.Put("there/Same.ps2", "SSS");
		fs.Put("here/readme.txt", "not a card");
		const Result r = Sync(fs.Io(), kFolders, &records, 0);
		Expect(r.listed && r.sent == 1 && r.brought == 1 && r.unchanged == 1 && r.failed == 0, "first sync: one sent, one brought, one the same");
		Expect(fs.Get("there/Mcd001.ps2") == "AAA" && fs.Get("here/Mcd002.ps2") == "BBB", "first sync: both sides have both");
		Expect(fs.Get("there/readme.txt") == "<none>", "first sync: what is not a card stays");
		Expect(records.size() == 3, "first sync: three cards remembered");

		// Nothing changed: nothing is copied.
		const Result again = Sync(fs.Io(), kFolders, &records, 0);
		Expect(again.sent == 0 && again.brought == 0 && again.unchanged == 3, "a second sync with no change copies nothing");

		// Changed here only: sent. Changed there only: brought.
		fs.Put("here/Mcd001.ps2", "AAA2");
		fs.Put("there/Mcd002.ps2", "BBB2");
		const Result changed = Sync(fs.Io(), kFolders, &records, 0);
		Expect(changed.sent == 1 && changed.brought == 1 && changed.both_changed == 0, "one changed on each side: one each way");
		Expect(fs.Get("there/Mcd001.ps2") == "AAA2" && fs.Get("here/Mcd002.ps2") == "BBB2", "the changes went across");

		// Changed on both sides: the newer is kept on both, the older put away on its side.
		fs.Put("there/Same.ps2", "THERE");
		fs.Put("here/Same.ps2", "HERE-NEWER");
		const Result both = Sync(fs.Io(), kFolders, &records, 1760000000);
		Expect(both.both_changed == 1 && both.sent == 1, "changed on both: the console's newer one is sent");
		Expect(fs.Get("there/Same.ps2") == "HERE-NEWER" && fs.Get("here/Same.ps2") == "HERE-NEWER", "changed on both: both sides have the newer");
		Expect(fs.CountUnder("there/.older") == 1, "changed on both: the older one is put away there");

		fs.Put("here/Same.ps2", "HERE-OLDER");
		fs.Put("there/Same.ps2", "THERE-NEWER");
		const Result both2 = Sync(fs.Io(), kFolders, &records, 1760000100);
		Expect(both2.both_changed == 1 && both2.brought == 1, "changed on both: the other side's newer one is brought");
		Expect(fs.Get("here/Same.ps2") == "THERE-NEWER" && fs.CountUnder("older-here") == 1, "changed on both: the console's older one is put away here");

		// Deleted on one side: it comes back from the other.
		fs.files.erase("there/Mcd002.ps2");
		const Result back = Sync(fs.Io(), kFolders, &records, 0);
		Expect(back.sent == 1 && fs.Get("there/Mcd002.ps2") == "BBB2", "a card deleted there comes back from the console");
	}
	{
		// Never synced, different bytes on both sides: handled as changed on both; nothing lost.
		Files fs;
		Records records;
		fs.Put("there/Mcd001.ps2", "PC");
		fs.Put("here/Mcd001.ps2", "CONSOLE");
		const Result r = Sync(fs.Io(), kFolders, &records, 1760000000);
		Expect(r.both_changed == 1 && fs.Get("there/Mcd001.ps2") == "CONSOLE" && fs.CountUnder("there/.older") == 1,
			"different cards of one name, never synced: the newer wins and the other is kept");
	}
	{
		// The other side does not answer: nothing is done.
		Files fs;
		Records records;
		fs.there_answers = false;
		fs.Put("here/Mcd001.ps2", "AAA");
		const Result r = Sync(fs.Io(), kFolders, &records, 0);
		Expect(!r.listed && r.sent == 0 && records.empty(), "no answer: nothing done, nothing remembered");
		Expect(Summary(r, "Share: games").find("did not answer") != std::string::npos, "no answer: said");
	}
	{
		// A write that fails is not remembered, so the next sync tries again.
		Files fs;
		Records records;
		fs.fail_writes_there = true;
		fs.Put("here/Mcd001.ps2", "AAA");
		const Result r = Sync(fs.Io(), kFolders, &records, 0);
		Expect(r.failed == 1 && records.empty(), "a failed copy is not remembered");
		fs.fail_writes_there = false;
		const Result r2 = Sync(fs.Io(), kFolders, &records, 0);
		Expect(r2.sent == 1 && fs.Get("there/Mcd001.ps2") == "AAA", "and is copied the next time");
	}
	{
		// A game starting stops it before the console's cards are written.
		Files fs;
		Records records;
		fs.Put("there/Mcd001.ps2", "PC");
		fs.stop = true;
		const Result r = Sync(fs.Io(), kFolders, &records, 0);
		Expect(r.stopped && fs.Get("here/Mcd001.ps2") == "<none>", "a game started: the console's cards are not written");
	}
	{
		// The list survives its text.
		Records records;
		records["SLUS-21059.ps2"] = Record{8650752, 1760000000, 8650752, -5};
		records["Mcd 001.ps2"] = Record{1, 2, 3, 4};
		const Records back = ParseRecords(FormatRecords(records) + "garbage line\n\t1\t2\n");
		Expect(back.size() == 2 && back.at("SLUS-21059.ps2").there_mtime == -5 && back.at("Mcd 001.ps2").there_size == 3, "the list is read back as written");
	}

	{
		// PSFlyCast: the caller says which files are cards (the VMU images Flycast opens, ps5_vmu.h's
		// isCardFile); nothing else is copied either way, a half-written ".part" neither.
		Files fs;
		fs.Put("here/vmu_save_A1.bin", "A1");
		fs.Put("here/notes.txt", "N");
		fs.Put("there/T1234N_vmu_save_A1.bin", "GAME");
		fs.Put("there/vmu_save_B1.bin.part", "HALF");
		Records records;
		Io io = fs.Io();
		io.is_card = [](const std::string& name) {
			const std::string tail = "vmu_save_A1.bin";
			return (name.size() == 15 && name.rfind("vmu_save_", 0) == 0 && name.compare(11, 4, ".bin") == 0)
					|| (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0);
		};
		const Result r = Sync(io, kFolders, &records, 1760000000);
		Expect(r.sent == 1 && r.brought == 1 && fs.Get("there/vmu_save_A1.bin") == "A1" && fs.Get("here/T1234N_vmu_save_A1.bin") == "GAME",
				"is_card: the VMU images go both ways");
		Expect(fs.Get("there/notes.txt") == "<none>" && fs.Get("here/vmu_save_B1.bin.part") == "<none>" && records.size() == 2,
				"is_card: nothing else is copied, a half-written file neither");
	}

	if (g_failed)
	{
		std::fprintf(stderr, "cardsync: %d of %d checks FAILED\n", g_failed, g_checks);
		return 1;
	}
	std::printf("cardsync: %d checks passed\n", g_checks);
	return 0;
}
