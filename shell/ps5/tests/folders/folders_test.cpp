// PSFlyCast - the folders engine (ps5_folders.cpp) on folders made in a temporary directory.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ps5_folders.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

using namespace ps5::folders;

namespace
{
int g_checks = 0, g_failed = 0;
void Expect(bool ok, const char *what)
{
	g_checks++;
	if (!ok)
	{
		g_failed++;
		std::fprintf(stderr, "FAILED: %s\n", what);
	}
}

std::string tmp;
void Dir(const std::string& path)
{
	std::string so_far;
	size_t at = 0;
	while (at != std::string::npos)
	{
		at = path.find('/', at + 1);
		so_far = path.substr(0, at);
		mkdir(so_far.c_str(), 0777);
	}
}
void File(const std::string& path, const std::string& data = "x")
{
	Dir(path.substr(0, path.find_last_of('/')));
	std::ofstream(path, std::ios::binary) << data;
}
bool Has(const Scan& scan, Kind kind, const std::string& folder, int count = -1)
{
	for (const Found& f : scan.found)
		if (f.kind == kind && f.folder == folder && (count < 0 || f.count == count))
			return true;
	return false;
}
int Of(const Scan& scan, Kind kind)
{
	int n = 0;
	for (const Found& f : scan.found)
		n += f.kind == kind;
	return n;
}
}	// namespace

int main()
{
	char pattern[] = "/tmp/psflycast-folders-XXXXXX";
	tmp = mkdtemp(pattern);

	// ---- browsing
	Dir(tmp + "/drive/Zeta");
	Dir(tmp + "/drive/alpha");
	Dir(tmp + "/drive/.hidden");
	Dir(tmp + "/drive/Beta");
	File(tmp + "/drive/a-file.txt");
	const std::vector<std::string> subs = subfolders(tmp + "/drive");
	Expect(subs.size() == 3, "hidden folders and files are left out");
	Expect(subs.size() == 3 && subs[0] == "alpha" && subs[1] == "Beta" && subs[2] == "Zeta", "folders are sorted by name, any case");
	Expect(subfolders(tmp + "/nowhere").empty(), "a folder that is not there has none");
	Expect(join("/a/b", "c") == "/a/b/c" && join("/a/b/", "c") == "/a/b/c" && join("smb://s/x", "c") == "smb://s/x/c", "join");

	PlaceSources sources;
	sources.usb = { "/mnt/usb0", "/mnt/usb2" };
	sources.shares = { "smb://nas/Multimedia" };
	sources.console = "/data";
	const std::vector<Place> places0 = places(sources);
	Expect(places0.size() == 4, "a place for each drive, share and the console");
	Expect(places0[0].label.find("USB drive 1") == 0 && places0[1].label.find("USB drive 3") == 0, "drives are numbered from 1");
	Expect(places0[2].label == "Network share nas/Multimedia", "a share's label");
	Expect(parentOf("/mnt/usb0", places0).empty(), "above a drive is the list of places");
	Expect(parentOf("/mnt/usb0/Games", places0) == "/mnt/usb0", "above a folder in a drive is the drive");
	Expect(parentOf("/mnt/usb0/Games/Sega", places0) == "/mnt/usb0/Games", "above a deeper folder");
	Expect(parentOf("smb://nas/Multimedia/ISO", places0) == "smb://nas/Multimedia", "above a folder on a share");
	Expect(parentOf("smb://nas/Multimedia", places0).empty(), "above a share is the list of places");
	Expect(parentOf("/elsewhere/x", places0).empty(), "a folder in no place goes to the list");
	Expect(parentOf("/mnt/usb00/x", places0).empty(), "a drive's name is not a prefix of another's");

	// ---- writing
	Expect(canWriteIn(tmp + "/drive"), "a folder that can be written in");
	Expect(access((tmp + "/drive/.psflycast-write-test").c_str(), F_OK) != 0, "the probe file is taken away");
	Expect(!canWriteIn(tmp + "/nowhere"), "a folder that is not there");
	Expect(!canWriteIn(tmp + "/drive/a-file.txt"), "a file is not a folder");
	Expect(!canWriteIn("smb://nas/Multimedia"), "never on a share");
	Expect(!canWriteIn(""), "no folder");

	// ---- looking for files
	const std::string usb = tmp + "/usb";
	File(usb + "/BIOS/dc_boot.bin");
	File(usb + "/BIOS/dc_flash.bin");
	File(usb + "/BIOS/naomi.zip");
	File(usb + "/BIOS/awbios.zip");
	File(usb + "/Games/Sega/Dreamcast/One.chd");
	File(usb + "/Games/Sega/Dreamcast/Two.gdi");
	File(usb + "/Games/Sega/Dreamcast/Two/track01.bin");
	File(usb + "/Games/Sega/Dreamcast/Sub/Three.cdi");
	File(usb + "/Games/Sega/Naomi/Four.zip");
	File(usb + "/flycast/data/One.state");
	File(usb + "/flycast/data/One_2.state");
	File(usb + "/flycast/data/One.state.net");
	File(usb + "/flycast/boxart/a.png");
	File(usb + "/flycast/boxart/b.jpg");
	File(usb + "/flycast/boxart/c.jpeg");
	File(usb + "/photos/p1.png");
	File(usb + "/photos/p2.png");
	File(usb + "/photos/p3.png");
	File(usb + "/cheats/x.cht");
	File(usb + "/emu/textures/MK-51035/a.png");
	File(usb + "/emu/textures/HDR-0001/a.png");
	File(usb + "/videos/One-video.mp4");
	File(usb + "/.Trash/hidden.chd");
	File(usb + "/System Volume Information/x.chd");
	const Scan scan = lookFor(usb);
	Expect(!scan.partial, "a small drive is looked through whole");
	Expect(Has(scan, Bios, usb + "/BIOS", 4), "dc_boot, dc_flash and the two arcade BIOS are found");
	Expect(Has(scan, Games, usb + "/Games/Sega/Dreamcast", 3), "games count for the folder above their subfolders: 3, the track file is not a game");
	Expect(Has(scan, Games, usb + "/Games/Sega/Naomi", 1), "a folder of arcade zips is games");
	Expect(Of(scan, Games) == 2, "the subfolder of a game folder is not listed again");
	Expect(Has(scan, States, usb + "/flycast/data", 3), "states, numbered ones and netplay ones");
	Expect(Has(scan, Covers, usb + "/flycast/boxart", 3), "covers in a folder named for them");
	Expect(!Has(scan, Covers, usb + "/photos"), "pictures in other folders are not covers");
	Expect(Has(scan, Cheats, usb + "/cheats", 1), "cheat files");
	Expect(Has(scan, Textures, usb + "/emu/textures", 2), "a textures folder holding a folder for each game");
	Expect(Has(scan, Videos, usb + "/videos", 1), "videos in a folder named for them");
	Expect(!Has(scan, Games, usb + "/.Trash") && !Has(scan, Games, usb + "/System Volume Information"), "hidden and system folders are skipped");
	Expect(scan.visited > 10, "folders were counted");

	// The limits.
	const Scan shallow = lookFor(usb, 1);
	Expect(!Has(shallow, Games, usb + "/Games/Sega/Dreamcast"), "depth stops the walk");
	const Scan few = lookFor(usb, 4, 3);
	Expect(few.partial && few.visited == 3, "the folder limit stops it and says so");
	int asked = 0;
	const Scan stopped = lookFor(usb, 4, 800, [&] { return ++asked > 2; });
	Expect(stopped.partial, "a stop request ends it and says so");

	// ---- a share is looked into through the lister
	std::map<std::string, std::vector<Entry>> share;
	share["smb://nas/Media"] = { { "ISO", true }, { "notes.txt", false } };
	share["smb://nas/Media/ISO"] = { { "A.chd", false }, { "B.chd", false } };
	setLister([&](const std::string& folder, std::vector<Entry>& out) {
		const auto at = share.find(folder);
		if (at == share.end())
			return false;
		out = at->second;
		return true;
	});
	const Scan onShare = lookFor("smb://nas/Media/");
	Expect(Has(onShare, Games, "smb://nas/Media/ISO", 2), "games on a share are found through the title's lister");
	Expect(subfolders("smb://nas/Media").size() == 1, "a share's folders are listed through it");
	Expect(subfolders("smb://nas/Missing").empty(), "a share that does not answer has none");
	setLister(nullptr);

	// ---- the choice, kept in folders.cfg
	Dir(tmp + "/root");
	load(tmp + "/root/");
	Expect(settings().games.empty() && settings().kept[Bios].empty(), "nothing chosen at first");
	std::string why;
	Expect(add(Games, usb + "/Games/Sega/Dreamcast/", why), "a game folder is added");
	Expect(settings().games.size() == 1 && settings().games[0] == usb + "/Games/Sega/Dreamcast", "its trailing slash is taken off");
	Expect(!add(Games, usb + "/Games/Sega/Dreamcast", why) && !why.empty(), "the same folder twice is refused, with the reason");
	Expect(add(Bios, usb + "/BIOS", why), "a BIOS folder is added");
	Expect(add(States, usb + "/flycast/data", why), "a folder to write states in is added");
	Expect(!add(States, "smb://nas/Media/ISO", why) && why.find("read only") != std::string::npos, "states never go on a share");
	Expect(!add(Covers, tmp + "/nowhere", why), "a covers folder that cannot be written is refused");
	Expect(add(Cheats, "smb://nas/Media/ISO", why), "a cheat folder on a share is fine: it is only read");
	Dir(tmp + "/second");
	Expect(add(States, tmp + "/second", why) && settings().kept[States][0] == tmp + "/second", "the newest written folder is the first");
	load(tmp + "/root");
	Expect(settings().games.size() == 1 && settings().kept[Bios].size() == 1 && settings().kept[States].size() == 2, "it is read again from folders.cfg");
	Expect(access((tmp + "/root/folders.cfg.tmp").c_str(), F_OK) != 0, "no temporary file is left");
	remove(States, tmp + "/second");
	load(tmp + "/root");
	Expect(settings().kept[States].size() == 1, "a folder taken off stays off");
	reset(Bios);
	load(tmp + "/root");
	Expect(settings().kept[Bios].empty(), "back to the default");

	// ---- parse
	const Settings p = parse("# a comment\r\nbios = /a/b/ \n\nbios=/a/b\nbogus = /x\nstate =\nstate = /s\ngame = smb://nas/x/\nnonsense\n");
	Expect(p.kept[Bios].size() == 1 && p.kept[Bios][0] == "/a/b", "duplicates and trailing slashes are one folder");
	Expect(p.kept[States].size() == 1 && p.games.size() == 1 && p.games[0] == "smb://nas/x", "an empty value and unknown keys are ignored");
	Expect(parse(serialise(p)).kept[States] == p.kept[States] && parse(serialise(p)).games == p.games, "serialise then parse is the same");

	// ---- which folders are in use
	Settings s;
	s.kept[States] = { "/mnt/usb0/states", "/other" };
	s.kept[Bios] = { "/mnt/usb0/bios" };
	const auto there = [](const std::string& f) { return f == "/other" || f == "/root/bios"; };
	Resolved r = resolve(s, States, "/root/data/savestates", there);
	Expect(r.askFirst, "a written kind whose first folder is gone asks before a game starts");
	Expect(r.use.size() == 2 && r.use[0] == "/other" && r.use[1] == "/root/data/savestates", "the folders there, then the default");
	Expect(r.missing.size() == 1 && r.missing[0] == "/mnt/usb0/states", "the one that is gone is named");
	r = resolve(s, Bios, "/root/bios", there);
	Expect(!r.askFirst && r.use.size() == 1 && r.use[0] == "/root/bios" && r.missing.size() == 1, "a read kind just goes on without the folder that is gone");
	r = resolve(s, States, "/root/data/savestates", [](const std::string&) { return true; });
	Expect(!r.askFirst && r.use.size() == 3 && r.use[0] == "/mnt/usb0/states", "all there: the chosen first, the default last");
	r = resolve(Settings(), Covers, "/root/covers", there);
	Expect(!r.askFirst && r.use.size() == 1 && r.use[0] == "/root/covers", "nothing chosen is the default alone, as before");

	// ---- finding a file in the folders in use
	load(tmp + "/root");
	File(tmp + "/root/covers/Game.png");
	Expect(findIn(Covers, "Game.png") == tmp + "/root/covers", "a file in the default folder");
	Dir(tmp + "/mycovers");
	File(tmp + "/mycovers/Other.png");
	Expect(add(Covers, tmp + "/mycovers", why), "a covers folder");
	Dir(tmp + "/mycovers2");
	Expect(add(Covers, tmp + "/mycovers2", why) && settings().kept[Covers].size() == 1, "covers have one folder: a new choice replaces the old");
	Expect(add(Covers, tmp + "/mycovers", why) && settings().kept[Covers][0] == tmp + "/mycovers", "back to the first");
	Expect(findIn(Covers, "Other.png") == tmp + "/mycovers" && findIn(Covers, "Game.png") == tmp + "/root/covers", "the chosen folder, then the default");
	Expect(writeFolder(Covers) == tmp + "/mycovers", "covers are written in the chosen folder");
	Expect(findIn(Covers, "None.png").empty(), "a file that is nowhere");
	reset(Covers);
	Expect(writeFolder(Covers) == tmp + "/root/covers", "the default again");

	std::string cleanup = "rm -rf " + tmp;
	if (std::system(cleanup.c_str()) != 0)
		std::fprintf(stderr, "(the temporary folder was left: %s)\n", tmp.c_str());
	if (g_failed)
	{
		std::fprintf(stderr, "folders: %d of %d checks FAILED\n", g_failed, g_checks);
		return 1;
	}
	std::printf("folders: %d checks passed\n", g_checks);
	return 0;
}
