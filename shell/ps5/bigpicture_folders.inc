/*
	PSFlyCast - Settings > Games and saves > Folders: a part of bigpicture.cpp, which includes it (its
	helpers are that file's).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The folders themselves are ps5_folders.h's (the engine, with PC tests). Here are the screens: a
	list to choose from, the browser that walks drives, shares and folders, "Look For My Files", which
	looks through a place and lists what it recognises there, and the rows of the Folders page.
	The browser and the writable test follow Crux2's (Crux2 by Press5elect). Nothing is moved or
	copied by any of it.
*/

namespace
{

// ---- a list to choose from. The items are made on a thread of their own (a share may be slow to
// list), and "Looking..." shows meanwhile. Circle leaves it.

struct PickUi
{
	bool open = false;
	std::string title, note;
	std::vector<std::string> items;
	std::future<std::vector<std::string>> loading;
	bool waiting = false;
	std::function<void(int, const std::vector<std::string>&)> chosen;
	int focus = 0;
	float scroll = 0;
} pickUi;

// A listing that was given up on goes on in the background; its future is kept until it is done,
// as a future of std::async waits for its task when it is let go.
std::vector<std::future<std::vector<std::string>>> abandonedLists;

void closePick()
{
	if (pickUi.waiting && pickUi.loading.valid())
		abandonedLists.push_back(std::move(pickUi.loading));
	pickUi.waiting = false;
	pickUi.open = false;
}

void openPick(const std::string& title, const std::string& note, std::function<std::vector<std::string>()> loader,
		std::function<void(int, const std::vector<std::string>&)> chosen)
{
	closePick();
	pickUi = PickUi();
	pickUi.open = true;
	pickUi.title = title;
	pickUi.note = note;
	pickUi.chosen = std::move(chosen);
	pickUi.waiting = true;
	pickUi.loading = std::async(std::launch::async, std::move(loader));
}

// A short list that is known already.
void openPick(const std::string& title, const std::string& note, std::vector<std::string> items,
		std::function<void(int, const std::vector<std::string>&)> chosen)
{
	openPick(title, note, [items = std::move(items)] { return items; }, std::move(chosen));
}

void pickDialog(ImDrawList *dl, const Input& given)
{
	abandonedLists.erase(std::remove_if(abandonedLists.begin(), abandonedLists.end(), [](std::future<std::vector<std::string>>& f) {
		return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
	}), abandonedLists.end());
	if (pickUi.waiting && pickUi.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		pickUi.items = pickUi.loading.get();
		pickUi.waiting = false;
		pickUi.focus = 0;
		pickUi.scroll = 0;
	}
	const int n = (int)pickUi.items.size();
	if (!pickUi.waiting && n > 0)
	{
		if (given.up) pickUi.focus = std::max(0, pickUi.focus - 1);
		if (given.down) pickUi.focus = std::min(n - 1, pickUi.focus + 1);
		if (given.l2) pickUi.focus = std::max(0, pickUi.focus - 8);
		if (given.r2) pickUi.focus = std::min(n - 1, pickUi.focus + 8);
	}
	if (given.back)
	{
		closePick();
		return;
	}
	if (given.accept && !pickUi.waiting && n > 0)
	{
		const auto chosen = std::move(pickUi.chosen);
		const std::vector<std::string> items = pickUi.items;
		const int at = pickUi.focus;
		closePick();
		if (chosen)
			chosen(at, items);
		return;
	}
	const float w = 1240, h = 800, pad = 52, rowH = 60;
	const ImVec2 at = dialogPanel(dl, w, h);
	text(dl, bold(), 38, at.x + pad, at.y + pad - 8, col::text, fit(bold(), 38, pickUi.title, w - 2 * pad).c_str());
	float top = at.y + pad + 50;
	if (!pickUi.note.empty())
		top += text(dl, regular(), 22, at.x + pad, top, col::faint, pickUi.note.c_str(), w - 2 * pad).y + 14;
	const float bottom = at.y + h - pad - 60 - 20;
	const int visible = std::max(1, (int)((bottom - top) / rowH));
	if (pickUi.waiting)
	{
		const int dots = (int)(timeNow * 3) % 4;
		text(dl, regular(), 28, at.x + pad, top + 20, col::dim, (std::string("Looking") + std::string(dots, '.')).c_str());
	}
	else if (n == 0)
		text(dl, regular(), 26, at.x + pad, top + 20, col::dim, "Nothing here.");
	else
	{
		const float target = (float)std::clamp(pickUi.focus - visible / 2, 0, std::max(0, n - visible));
		pickUi.scroll = motion() == MotionOff ? target : approach(pickUi.scroll, target, 16);
		dl->PushClipRect(V(at.x + pad - 12, top - 4), V(at.x + w - pad + 12, bottom + 4), true);
		for (int i = std::max(0, (int)pickUi.scroll - 1); i < n; i++)
		{
			const float y = top + (i - pickUi.scroll) * rowH;
			if (y > bottom + rowH)
				break;
			const bool on = i == pickUi.focus;
			if (on)
				rect(dl, at.x + pad - 12, y, w - 2 * pad + 24, rowH - 6, alpha(col::accent, 0.22f), 12);
			text(dl, on ? bold() : regular(), 26, at.x + pad + 8, y + (rowH - 6) / 2 - 17, on ? col::text : col::dim,
					fit(on ? bold() : regular(), 26, pickUi.items[i], w - 2 * pad - 24).c_str());
		}
		dl->PopClipRect();
		if (n > visible)
		{
			const float tw = 6, th = bottom - top, bh = std::max(40.f, th * visible / n);
			rect(dl, at.x + w - pad + 22, top, tw, th, col::rgba(255, 255, 255, 24), 3);
			rect(dl, at.x + w - pad + 22, top + (th - bh) * (pickUi.scroll / std::max(1, n - visible)), tw, bh, col::dim, 3);
		}
	}
	float x = at.x + pad;
	const float y = at.y + h - pad - 60 + 8;
	if (!pickUi.waiting && n > 0)
		x += dialogButton(dl, x, y, Glyph::Cross, "Choose", true) + 20;
	dialogButton(dl, x, y, Glyph::Circle, "Back", false);
}

// ---- the places, and a folder browser made of lists: the places first, then each folder's folders,
// with "Choose this folder" and "Up one folder" on top. `chosen` gets the folder picked.

ps5::folders::PlaceSources folderSources()
{
	ps5::folders::PlaceSources sources;
	if (ps5::elevated)
		for (int drive = 0; drive < 8; drive++)
		{
			const std::string root = "/mnt/usb" + std::to_string(drive);
			struct stat st;
			if (stat(root.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				sources.usb.push_back(root);
		}
	for (const std::string& folder : ps5::smb::gameFolders())
	{
		// smb://server/share is the place; the folder inside it is where the games are.
		const size_t serverEnd = folder.find('/', 6);
		const size_t shareEnd = serverEnd == std::string::npos ? std::string::npos : folder.find('/', serverEnd + 1);
		const std::string share = shareEnd == std::string::npos ? folder : folder.substr(0, shareEnd);
		if (std::find(sources.shares.begin(), sources.shares.end(), share) == sources.shares.end())
			sources.shares.push_back(share);
	}
	struct stat st;
	if (stat("/data", &st) == 0 && S_ISDIR(st.st_mode))
		sources.console = "/data";
	return sources;
}

void browseFolder(const std::string& title, const std::string& path, std::function<void(const std::string&)> chosen)
{
	if (path.empty())
	{
		std::vector<ps5::folders::Place> places = ps5::folders::places(folderSources());
		if (places.empty())
		{
			ps5::sound::cue(ps5::sound::Cue::Refuse);
			os_notify("No place to choose a folder from", 4000, "A USB drive (Settings > Library > USB drives), a network share or the console's storage");
			return;
		}
		std::vector<std::string> names;
		for (const auto& place : places)
			names.push_back(place.label);
		openPick(title, "Choose a drive, a share or the console's storage.", names,
				[title, places, chosen](int i, const std::vector<std::string>&) { browseFolder(title, places[(size_t)i].path, chosen); });
		return;
	}
	openPick(title, path,
			[path] {
				std::vector<std::string> items{ "Choose this folder", "Up one folder" };
				for (const std::string& name : ps5::folders::subfolders(path))
					items.push_back(name + "/");
				return items;
			},
			[title, path, chosen](int i, const std::vector<std::string>& items) {
				if (i == 0)
				{
					chosen(path);
					return;
				}
				if (i == 1)
				{
					const std::string up = ps5::folders::parentOf(path, ps5::folders::places(folderSources()));
					browseFolder(title, up, chosen);
					return;
				}
				std::string name = items[(size_t)i];
				name.pop_back();
				browseFolder(title, ps5::folders::join(path, name), chosen);
			});
}

// What changed in the folders is handed on: Flycast gets them, the covers are looked up again and
// the library is looked through again.
void foldersChanged(bool shareGames = false)
{
	ps5::applyFolders();
	ps5::covers::folderChanged();
	scanner().refresh();
	scannedCount = (size_t)-1;
	rebuildCategories = true;
	if (shareGames)
		scanNetwork();
}

void refuse(const std::string& why)
{
	ps5::sound::cue(ps5::sound::Cue::Refuse);
	os_notify("Not added", 6000, why.c_str());
}

// A folder on a share is "server/share/folder" in network.cfg.
std::string shareTyped(const std::string& folder)
{
	return folder.rfind("smb://", 0) == 0 ? folder.substr(6) : folder;
}

// ---- Look For My Files

struct FoundRow
{
	ps5::folders::Kind kind;
	std::string folder;
	int count = 0;
	bool on = false;
	bool blocked = false;
	std::string why;
};

struct FoundUi
{
	bool open = false;
	bool scanning = false;
	std::string place;
	std::future<ps5::folders::Scan> scan;
	std::shared_ptr<std::atomic<bool>> stop;
	std::vector<FoundRow> rows;
	bool partial = false;
	int focus = 0;
} foundUi;
std::vector<std::future<ps5::folders::Scan>> abandonedScans;

const char *kindName(ps5::folders::Kind kind)
{
	static const char *const names[] = { "Games", "BIOS", "Save states", "Covers", "Texture packs", "Cheats", "Game videos" };
	return names[kind];
}

std::string countText(const FoundRow& r)
{
	const int n = r.count;
	const char *one[] = { "game", "file", "state", "picture", "pack", "file", "video" };
	return std::to_string(n) + " " + one[r.kind] + (n == 1 ? "" : "s");
}

void closeFound()
{
	if (foundUi.scanning && foundUi.stop)
	{
		foundUi.stop->store(true);
		if (foundUi.scan.valid())
			abandonedScans.push_back(std::move(foundUi.scan));
	}
	foundUi.scanning = false;
	foundUi.open = false;
}

void openFound(const std::string& place)
{
	closeFound();
	foundUi = FoundUi();
	foundUi.open = true;
	foundUi.scanning = true;
	foundUi.place = place;
	foundUi.stop = std::make_shared<std::atomic<bool>>(false);
	const auto stop = foundUi.stop;
	foundUi.scan = std::async(std::launch::async, [place, stop] {
		return ps5::folders::lookFor(place, 4, 800, [stop] { return stop->load(); });
	});
}

bool alreadyUsed(ps5::folders::Kind kind, const std::string& folder)
{
	using namespace ps5::folders;
	if (kind == ps5::folders::Games)
	{
		if (std::find(ps5::folders::settings().games.begin(), ps5::folders::settings().games.end(), folder) != ps5::folders::settings().games.end())
			return true;
		if (isShare(folder))
			for (const std::string& net : ps5::smb::gameFolders())
				if (net == folder || net == folder + "/")
					return true;
		return false;
	}
	const auto& chosen = ps5::folders::settings().kept[kind];
	return std::find(chosen.begin(), chosen.end(), folder) != chosen.end();
}

void applyFound()
{
	using namespace ps5::folders;
	int used = 0;
	bool shareGames = false;
	std::string problems;
	for (const FoundRow& r : foundUi.rows)
	{
		if (!r.on || r.blocked)
			continue;
		std::string why;
		bool ok;
		if (r.kind == ps5::folders::Games && isShare(r.folder))
		{
			ok = ps5::smb::addFolder(shareTyped(r.folder), why);
			shareGames = shareGames || ok;
		}
		else
			ok = add(r.kind, r.folder, why);
		if (ok)
			used++;
		else
			problems += (problems.empty() ? "" : "  ") + std::string(kindName(r.kind)) + ": " + why;
	}
	closeFound();
	if (used > 0)
		foldersChanged(shareGames);
	if (!problems.empty())
		refuse(problems);
	else if (used > 0)
		os_notify(used == 1 ? "Using 1 folder" : ("Using " + std::to_string(used) + " folders").c_str(), 3500,
				"Takes effect for the next game started; the games are being looked for");
}

void foundDialog(ImDrawList *dl, const Input& given)
{
	using namespace ps5::folders;
	abandonedScans.erase(std::remove_if(abandonedScans.begin(), abandonedScans.end(), [](std::future<Scan>& f) {
		return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
	}), abandonedScans.end());
	if (foundUi.scanning && foundUi.scan.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		const Scan scan = foundUi.scan.get();
		foundUi.scanning = false;
		foundUi.partial = scan.partial;
		bool first[KindCount] = {};
		int gameRows = 0;
		for (const Found& f : scan.found)
		{
			// Three folders of games at most, and the best of every other kind: all fit the panel.
			if (foundUi.rows.size() >= 9 || (f.kind == ps5::folders::Games && gameRows >= 3) || (f.kind != ps5::folders::Games && first[f.kind]))
				continue;
			gameRows += f.kind == ps5::folders::Games;
			FoundRow r{ f.kind, f.folder, f.count };
			if (alreadyUsed(f.kind, f.folder))
			{
				r.blocked = true;
				r.why = "Already used";
			}
			else if (info(f.kind).written && isShare(f.folder))
			{
				r.blocked = true;
				r.why = "A share is read only here";
			}
			// The best of each kind is switched on; every folder of games is.
			r.on = !r.blocked && (f.kind == ps5::folders::Games || !first[f.kind]);
			first[f.kind] = true;
			foundUi.rows.push_back(r);
		}
	}
	const int n = (int)foundUi.rows.size();
	if (!foundUi.scanning)
	{
		if (given.up) foundUi.focus = std::max(0, foundUi.focus - 1);
		if (given.down) foundUi.focus = std::min(std::max(0, n - 1), foundUi.focus + 1);
		if (given.accept && n > 0 && !foundUi.rows[(size_t)foundUi.focus].blocked)
			foundUi.rows[(size_t)foundUi.focus].on = !foundUi.rows[(size_t)foundUi.focus].on;
		if (given.triangle)
		{
			bool any = false;
			for (const FoundRow& r : foundUi.rows)
				any = any || (r.on && !r.blocked);
			if (any)
			{
				applyFound();
				return;
			}
		}
	}
	if (given.back)
	{
		closeFound();
		return;
	}
	const float w = 1360, h = 840, pad = 52, rowH = 60;
	const ImVec2 at = dialogPanel(dl, w, h);
	text(dl, bold(), 38, at.x + pad, at.y + pad - 8, col::text, foundUi.scanning ? "Looking through it..." : "Found here");
	text(dl, regular(), 22, at.x + pad, at.y + pad + 46, col::faint, fit(regular(), 22, foundUi.place, w - 2 * pad).c_str());
	text(dl, regular(), 20, at.x + pad, at.y + pad + 72, col::dim,
			"Nothing is moved or copied. PSFlyCast reads, and writes what it keeps, where the files are.", w - 2 * pad);
	const float top = at.y + pad + 100;
	if (foundUi.scanning)
	{
		const int dots = (int)(timeNow * 3) % 4;
		text(dl, regular(), 28, at.x + pad, top + 10, col::dim, (std::string("Looking") + std::string(dots, '.')).c_str());
	}
	else if (n == 0)
		text(dl, regular(), 26, at.x + pad, top + 10, col::dim,
				"Nothing PSFlyCast knows was found. Games, BIOS files, save states, covers, texture packs, cheats and videos are looked for.", w - 2 * pad);
	for (int i = 0; i < n; i++)
	{
		const FoundRow& r = foundUi.rows[(size_t)i];
		const float y = top + i * rowH;
		const bool on = i == foundUi.focus;
		rect(dl, at.x + pad - 12, y, w - 2 * pad + 24, rowH - 8, on ? alpha(col::accent, 0.22f) : col::card, 12);
		text(dl, bold(), 24, at.x + pad + 8, y + 5, r.blocked ? col::faint : col::text, kindName(r.kind));
		const std::string detail = r.blocked ? r.why : countText(r);
		text(dl, regular(), 20, at.x + pad + 8, y + 32, col::dim, fit(regular(), 20, detail + "   " + r.folder, 880).c_str());
		const float sw = 84, sh = 44, sx = at.x + w - pad - sw, cy = y + (rowH - 8) / 2;
		if (!r.blocked)
		{
			rect(dl, sx, cy - sh / 2, sw, sh, r.on ? col::accent : col::rgba(255, 255, 255, 40), sh / 2);
			dl->AddCircleFilled(V(sx + (r.on ? sw - sh / 2 : sh / 2), cy), (sh / 2 - 5) * S, col::text);
		}
	}
	if (!foundUi.scanning && foundUi.partial)
		text(dl, regular(), 20, at.x + pad, at.y + h - pad - 92, col::warm, "It stopped at its limits: there may be more further down.");
	float x = at.x + pad;
	const float y = at.y + h - pad - 60 + 8;
	if (!foundUi.scanning && n > 0)
	{
		x += dialogButton(dl, x, y, Glyph::Triangle, "Use These Folders", true) + 20;
		x += dialogButton(dl, x, y, Glyph::Cross, "Switch", false) + 20;
	}
	dialogButton(dl, x, y, Glyph::Circle, foundUi.scanning ? "Stop" : "Cancel", false);
}

// ---- the rows of the Folders page

std::string keptText(ps5::folders::Kind kind)
{
	const auto& chosen = ps5::folders::settings().kept[kind];
	if (chosen.empty())
		return std::string("Default (") + ps5::folders::info(kind).fallback + "/)";
	return chosen.size() > 1 ? chosen[0] + "  + " + std::to_string(chosen.size() - 1) + " more" : chosen[0];
}

void editKept(ps5::folders::Kind kind)
{
	using namespace ps5::folders;
	const KindInfo& ki = info(kind);
	std::vector<std::string> items{ kind == Covers ? "Choose a folder" : "Add a folder", std::string("Use the default (") + ki.fallback + "/)" };
	for (const std::string& path : ps5::folders::settings().kept[kind])
		items.push_back("Take off: " + path);
	std::string note = std::string(ki.desc) + " Takes effect for the next game started; nothing is moved.";
	if (kind == Covers)
		note += " Covers already downloaded stay where they are, and are downloaded again if the new folder has none.";
	openPick(ki.label, note, items, [kind](int i, const std::vector<std::string>& shown) {
		using namespace ps5::folders;
		if (i == 0)
			browseFolder(info(kind).label, "", [kind](const std::string& path) {
				std::string why;
				if (add(kind, path, why))
					foldersChanged();
				else
					refuse(why);
			});
		else if (i == 1)
		{
			reset(kind);
			foldersChanged();
		}
		else
		{
			remove(kind, shown[(size_t)i].substr(std::strlen("Take off: ")));
			foldersChanged();
		}
	});
}

Category foldersCategory()
{
	using namespace ps5::folders;
	Category c{ ICON_FA_FOLDER_TREE, "Folders" };
	c.rows.push_back(heading("Look for my files"));
	{
		Row r{ Row::Action, "Look For My Files",
			"Choose a drive, a share or a folder. PSFlyCast lists the games, BIOS files, save states, covers, texture packs, cheats and "
			"videos it finds there, and you switch on the ones to use. Nothing is moved or copied." };
		r.action = [] { browseFolder("Look For My Files", "", [](const std::string& place) { openFound(place); }); };
		c.rows.push_back(r);
	}
	c.rows.push_back(heading("Game folders"));
	{
		Row r{ Row::Action, "Add Game Folder",
			"Pick any folder on a USB drive, a network share or the console. Its games, and those in the folders inside it, join the "
			"library where they are: nothing is moved or copied." };
		r.action = [] {
			browseFolder("Add Game Folder", "", [](const std::string& path) {
				std::string why;
				if (isShare(path))
				{
					if (ps5::smb::addFolder(shareTyped(path), why))
						foldersChanged(true);
					else
						refuse(why);
				}
				else if (add(ps5::folders::Games, path, why))
					foldersChanged();
				else
					refuse(why);
			});
		};
		c.rows.push_back(r);
	}
	for (const std::string& path : ps5::folders::settings().games)
	{
		Row r{ Row::Action, path, "A game folder you added. Press to take it off the list: its games leave the library, the files stay where they are." };
		r.info = [] { return std::string("Remove"); };
		r.action = [path] {
			openPick("Take this folder off the list?", path + "\nThe games in it are not deleted.", { "Take it off", "Keep it" },
					[path](int i, const std::vector<std::string>&) {
						if (i == 0)
						{
							remove(ps5::folders::Games, path);
							foldersChanged();
						}
					});
		};
		c.rows.push_back(r);
	}
	{
		Row r{ Row::Info, "Folders on shares", "Added and taken off in Library > Network." };
		r.info = [] { return std::to_string(ps5::smb::gameFolders().size()); };
		c.rows.push_back(r);
	}
	c.rows.push_back(heading("Where PSFlyCast keeps its files"));
	for (const Kind kind : { Bios, States, Covers, Textures, Cheats, Videos })
	{
		Row r{ Row::Action, info(kind).label, info(kind).desc };
		r.info = [kind] { return keptText(kind); };
		r.action = [kind] { editKept(kind); };
		c.rows.push_back(r);
	}
	return c;
}

} // namespace
