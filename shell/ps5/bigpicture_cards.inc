/*
	PSFlyCast - the memory card manager's screen: a part of bigpicture.cpp,
	which includes it (its helpers are that file's).

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A dialog over the Settings, in two panes. On the left the cards: the
	shared ones that have a file, then each game's own. On the right the saves
	of the card the left pane is on. The pad is in one place at a time:

	  the cards        Cross goes into the card's saves (or formats a card that
	                   holds nothing yet), Square lists the save files in
	                   <root>/vmu to add one, Circle closes the dialog;
	  the saves        Cross opens the three things that can be done with one;
	  what to do       copy it to another card, write it to <root>/vmu, delete it;
	  where to copy    the left pane again, to choose the other card;
	  delete?          asked before a save is deleted;
	  the save files   Cross adds the one chosen to the card.

	Circle goes back one place. Everything is done by ps5_vmu.cpp, which reads a
	card from its file each time: the lists here are what it found when they
	were last made, and they are made again after everything that was tried.
	What came of it is a line in the dialog.

	Nothing here changes a card that could not be read. A card that holds
	nothing at all (an empty file, or zeroes) can be formatted and nothing
	else; one that holds anything is never formatted here.
*/

namespace
{

namespace vmu = ps5::vmu;

// A save's picture as a texture, by the name it was made under.
struct CardIcon
{
	ImTextureID id = ImTextureID();
	bool failed = false;		// it could not be made: not tried every frame
};

// A file in <root>/vmu that might be a save.
struct CardFile
{
	std::string path;			// what importSave() is given
	std::string shown;			// what the list calls it: "NAME.vmi and .vms"
	vmu::Save save;				// what is in it, when ok
	std::string icon;			// its picture's name
	bool ok = false;
	std::string why;			// why not
};

struct CardsUi
{
	bool open = false;
	// Where the pad is.
	enum Level { Cards, Saves, Actions, Destination, Confirm, Import };
	int level = Cards;

	std::vector<vmu::Card> cards;
	std::vector<std::string> names;		// what each is called: its game's title or ID, or its port and slot
	std::vector<vmu::Save> saves;		// the focused card's
	std::vector<std::string> saveIcons;	// their pictures' names
	std::vector<CardFile> files;		// Import: the saves in <root>/vmu
	int filesLeftOut = 0;				// and how many more files there are than are listed
	int card = 0;						// the focused card, save, thing to do, card to copy to, file
	int save = 0;
	int action = 0;
	int target = 0;
	int file = 0;
	float cardScroll = 0, saveScroll = 0, fileScroll = 0;

	std::string note;					// what came of the last thing tried
	bool noteBad = false;
	std::map<std::string, CardIcon> icons;	// the textures made here, to be deleted here
} cardsUi;

enum { CardCopy, CardExport, CardDelete, CardActions };
constexpr int CardFilesListed = 256;
constexpr float CardRowH = 76;

// Said when a copy-protected save was copied or exported all the same.
const char *const cardProtected = "It is marked copy-protected: a Dreamcast would have refused, and its game may not accept the copy";

// The part of a card's file name that stands for a game's ID, as Flycast makes
// it (hostfs::getVmuPath in core/oslib/oslib.cpp): the characters a file name
// cannot have are '_'.
std::string cardGameName(std::string id)
{
	for (char& c : id)
		if (c != 0 && strchr(" /\\:*?|<>\"", c) != nullptr)
			c = '_';
	return id;
}

// The title of each game that may have a card of its own, by the name its
// card's file would start with. That is the ID Flycast read from the game
// when it ran; an older Flycast used the game's file name; and a card brought
// from elsewhere may be of a game that has not run here, whose ID is then the
// disc's product number as the scraper read it. In that order: the first
// title under a name stays.
std::map<std::string, std::string> cardTitles()
{
	std::map<std::string, std::string> titles;
	// A disc of a set by its own title: each disc has its own ID, and so its own card.
	auto titleOf = [](const Game& g) { return !g.set.empty() && !g.fullTitle.empty() ? g.fullTitle : g.title; };
	for (const Game& g : games)
	{
		const std::string id = ps5::games::knownId(g.media.path);
		if (!id.empty())
			titles.emplace(cardGameName(id), titleOf(g));
	}
	for (const Game& g : games)
	{
		if (!g.media.fileName.empty())
			titles.emplace(g.media.fileName, titleOf(g));
		if (g.disc && !g.media.arcade && g.source != Network && !g.art.uniqueId.empty())
			titles.emplace(cardGameName(g.art.uniqueId), titleOf(g));
	}
	return titles;
}

// The name a save's picture is kept under as a texture: where the save is
// and what the picture is, so that another picture is another texture.
std::string cardIconName(const std::string& file, const vmu::Save& save)
{
	uint32_t hash = 2166136261u;	// FNV-1a
	for (uint8_t byte : save.icon)
		hash = (hash ^ byte) * 16777619u;
	char tail[40];
	snprintf(tail, sizeof(tail), ":%d:%08x", save.entry, (unsigned)hash);
	return "vmu:" + file + tail;
}

// The texture of a save's picture, made the first time it is drawn. None for
// a save without a picture, and while the renderer has none to give.
ImTextureID cardIcon(const std::string& name, const vmu::Save& save)
{
	if (!save.hasIcon || name.empty() || imguiDriver == nullptr)
		return ImTextureID();
	CardIcon& icon = cardsUi.icons[name];
	if (icon.id == ImTextureID() && !icon.failed)
	{
		try {
			icon.id = imguiDriver->updateTexture(name, save.icon.data(), vmu::IconSide, vmu::IconSide, true);
		} catch (...) {
			icon.failed = true;
		}
	}
	return icon.id;
}

// Deletes the textures of the pictures no list has any more: all of them when
// the lists are empty.
void forgetCardIcons()
{
	CardsUi& ui = cardsUi;
	std::map<std::string, CardIcon> kept;
	auto keep = [&](const std::string& name) {
		const auto it = ui.icons.find(name);
		if (it != ui.icons.end())
			kept.insert(*it);
	};
	for (const std::string& name : ui.saveIcons)
		keep(name);
	for (const CardFile& file : ui.files)
		keep(file.icon);
	for (const auto& made : ui.icons)
		if (kept.count(made.first) == 0 && imguiDriver != nullptr)
			imguiDriver->deleteTexture(made.first);
	ui.icons = std::move(kept);
}

// The focused card's saves, read from its file. With keepFocus the focus
// stays on the save it was on, when the card still has it there.
void readCardSaves(bool keepFocus)
{
	CardsUi& ui = cardsUi;
	const int entry = keepFocus && ui.save >= 0 && ui.save < (int)ui.saves.size() ? ui.saves[ui.save].entry : -1;
	ui.saves.clear();
	ui.saveIcons.clear();
	if (ui.card >= 0 && ui.card < (int)ui.cards.size() && ui.cards[ui.card].readable)
		ui.saves = vmu::list(ui.cards[ui.card]);
	if (!keepFocus)
	{
		ui.save = 0;
		ui.saveScroll = 0;
	}
	for (size_t i = 0; i < ui.saves.size(); i++)
	{
		ui.saveIcons.push_back(ui.saves[i].hasIcon ? cardIconName(ui.cards[ui.card].path, ui.saves[i]) : "");
		if (ui.saves[i].entry == entry)
			ui.save = (int)i;
	}
	ui.save = std::clamp(ui.save, 0, std::max(0, (int)ui.saves.size() - 1));
	forgetCardIcons();
}

// The cards, read from the data folder, and the focused one's saves. The
// focus, and the card being copied to, stay on the cards they were on.
void readCards()
{
	CardsUi& ui = cardsUi;
	auto pathOf = [&](int index) { return index >= 0 && index < (int)ui.cards.size() ? ui.cards[index].path : std::string(); };
	const std::string focused = pathOf(ui.card), target = pathOf(ui.target);
	ui.cards = vmu::cards(get_writable_data_path(""));
	const std::map<std::string, std::string> titles = cardTitles();
	ui.names.clear();
	for (size_t i = 0; i < ui.cards.size(); i++)
	{
		const vmu::Card& card = ui.cards[i];
		const auto title = card.perGame ? titles.find(card.game) : titles.end();
		ui.names.push_back(title != titles.end() ? title->second : card.perGame ? card.game : card.label);
		if (card.path == focused)
			ui.card = (int)i;
		if (card.path == target)
			ui.target = (int)i;
	}
	ui.card = std::clamp(ui.card, 0, std::max(0, (int)ui.cards.size() - 1));
	ui.target = std::clamp(ui.target, 0, std::max(0, (int)ui.cards.size() - 1));
	readCardSaves(!focused.empty() && pathOf(ui.card) == focused);
}

// The save files in <root>/vmu, each looked into. A .vmi and the .vms of its
// name are one save; a .dci is one; a .vms with no .vmi of its name is listed
// by itself, and says why when nothing beside it names it.
void readCardFiles()
{
	CardsUi& ui = cardsUi;
	ui.files.clear();
	ui.filesLeftOut = 0;
	ui.file = 0;
	ui.fileScroll = 0;
	const std::string folder = ps5::rootDir + "vmu/";
	std::vector<std::string> names;
	if (DIR *dir = opendir(folder.c_str()))
	{
		while (const dirent *entry = readdir(dir))
			names.push_back(entry->d_name);
		closedir(dir);
	}
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
		const std::string la = lowered(a), lb = lowered(b);
		return la != lb ? la < lb : a < b;
	});
	std::vector<std::string> lower;
	for (const std::string& name : names)
		lower.push_back(lowered(name));
	std::sort(lower.begin(), lower.end());
	auto has = [&](const std::string& name) { return std::binary_search(lower.begin(), lower.end(), name); };

	for (const std::string& name : names)
	{
		const size_t dot = name.find_last_of('.');
		if (dot == std::string::npos || dot == 0)
			continue;
		const std::string extension = lowered(name.substr(dot)), stem = name.substr(0, dot);
		if (extension != ".vmi" && extension != ".vms" && extension != ".dci")
			continue;
		// The .vms of a pair is its .vmi's row.
		if (extension == ".vms" && has(lowered(stem) + ".vmi"))
			continue;
		if ((int)ui.files.size() >= CardFilesListed)
		{
			ui.filesLeftOut++;
			continue;
		}
		CardFile file;
		file.path = folder + name;
		file.shown = name;
		const bool paired = extension == ".vmi" && has(lowered(stem) + ".vms");
		if (paired)
			file.shown += " and .vms";
		const vmu::Result result = vmu::inspect(file.path, file.save);
		file.ok = result.ok;
		file.why = result.message;
		// A .vmi that names a .vms of another name: that .vms has a row, which is this save's.
		if (extension == ".vmi" && !paired && file.ok)
			continue;
		if (file.ok && file.save.hasIcon)
			file.icon = cardIconName(file.path, file.save);
		ui.files.push_back(std::move(file));
	}
	forgetCardIcons();
}

// What came of something, for the dialog's line. A refusal has its sound.
void cardsNote(std::string words, bool bad)
{
	// A line of the dialog has no full stop at its end.
	while (!words.empty() && (words.back() == '.' || words.back() == ' '))
		words.pop_back();
	cardsUi.note = words;
	cardsUi.noteBad = bad;
	if (bad)
		ps5::sound::cue(ps5::sound::Cue::Refuse);
}

void closeCards()
{
	CardsUi& ui = cardsUi;
	ui.saves.clear();
	ui.saveIcons.clear();
	ui.files.clear();
	forgetCardIcons();
	ui.open = false;
}

// What a save is called in a list: as the Dreamcast's file manager would,
// else as the VMU would, else by its file's name.
std::string saveTitle(const vmu::Save& save)
{
	return !save.longDesc.empty() ? save.longDesc : !save.shortDesc.empty() ? save.shortDesc
			: !save.name.empty() ? save.name : "A file with no name";
}

std::string blocksText(int blocks)
{
	return std::to_string(blocks) + (blocks == 1 ? " block" : " blocks");
}

// "NAME  ·  12 blocks  ·  2001-03-14 21:05"
std::string saveDetail(const std::string& name, const vmu::Save& save)
{
	std::string detail = name + "   \xc2\xb7   " + blocksText(save.blocks);
	const std::string date = save.date.text();
	if (!date.empty())
		detail += "   \xc2\xb7   " + date;
	return detail;
}

// A card's line under its name.
std::string cardStatus(const vmu::Card& card)
{
	if (card.readable)
		return std::to_string(card.freeBlocks) + " of " + blocksText(card.userBlocks) + " free   \xc2\xb7   "
				+ std::to_string(card.saves) + (card.saves == 1 ? " save" : " saves");
	return card.unformatted ? "Not formatted yet" : "Cannot be read";
}

// A small label at the right of a row; returns where the next one ends.
float cardMark(ImDrawList *dl, float right, float cy, const char *words, bool on)
{
	const ImVec2 ws = textSize(regular(), 18, words);
	const float w = ws.x + 28, h = 32;
	rect(dl, right - w, cy - h / 2, w, h, on ? col::rgba(0, 0, 0, 70) : col::rgba(255, 255, 255, 18), h / 2);
	text(dl, regular(), 18, right - w + 14, cy - ws.y / 2, on ? col::text : col::dim, words);
	return right - w - 10;
}

// A save's row, in the card's list and in the list of files: its picture, what
// it is, where it is and how large, and its marks. on is the row the pad is
// on, lit the row something is being done with. bad has detail in the warm
// colour (a file that is no save).
void saveRow(ImDrawList *dl, const vmu::Save& save, const std::string& icon, const std::string& title, const std::string& detail,
		bool bad, float x, float y, float w, bool on, bool lit)
{
	if (on || lit)
		rect(dl, x + 8, y + 2, w - 16, CardRowH - 4, on ? col::accent : col::panelHi, 12);
	// The picture: 32 dots a side, each a whole number of the screen's pixels.
	const float tile = 64, tx = x + 22, ty = y + (CardRowH - tile) / 2;
	rect(dl, tx, ty, tile, tile, bad ? col::rgba(0, 0, 0, 40) : col::card, 4);
	const ImTextureID id = bad ? ImTextureID() : cardIcon(icon, save);
	if (id != ImTextureID())
	{
		const int times = std::max(1, (int)(tile * S / vmu::IconSide + 0.01f));
		const float side = (float)(times * vmu::IconSide);
		const float px = std::floor(tx * S + (tile * S - side) / 2 + 0.5f), py = std::floor(ty * S + (tile * S - side) / 2 + 0.5f);
		dl->AddImage(id, ImVec2(px, py), ImVec2(px + side, py + side));
	}
	else
	{
		const char *sign = bad ? ICON_FA_TRIANGLE_EXCLAMATION : save.type == vmu::Type::Game ? ICON_FA_GAMEPAD : ICON_FA_FLOPPY_DISK;
		const ImVec2 ss = textSize(regular(), 28, sign);
		text(dl, regular(), 28, tx + (tile - ss.x) / 2, ty + (tile - ss.y) / 2, bad ? alpha(col::warm, 0.8f) : col::faint, sign);
	}
	float right = x + w - 28;
	const float cy = y + CardRowH / 2;
	if (!bad && save.copyProtected)
		right = cardMark(dl, right, cy, ICON_FA_LOCK "  Copy-protected", on);
	if (!bad && save.type == vmu::Type::Game)
		right = cardMark(dl, right, cy, ICON_FA_GAMEPAD "  VMU game", on);
	const float textX = tx + tile + 20, textW = right - 8 - textX;
	text(dl, bold(), 26, textX, y + 11, on || lit ? col::text : bad ? col::faint : col::dim, fit(bold(), 26, title, textW).c_str());
	text(dl, regular(), 20, textX, y + 45, on || lit ? col::text : bad ? alpha(col::warm, 0.9f) : col::faint,
			fit(regular(), 20, detail, textW).c_str());
}

// Words in a pane where a list would be: a heading and what it means.
void paneWords(ImDrawList *dl, float x, float y, float w, const char *sign, bool bad, const std::string& heading, const std::string& more)
{
	const ImVec2 ss = textSize(regular(), 40, sign);
	text(dl, regular(), 40, x + 28, y + 30, bad ? alpha(col::warm, 0.85f) : col::faint, sign);
	const float tx = x + 28 + ss.x + 24, tw = w - (tx - x) - 36;
	text(dl, bold(), 28, tx, y + 28, col::text, fit(bold(), 28, heading, tw).c_str());
	text(dl, regular(), 22, tx, y + 74, col::dim, more.c_str(), tw);
}

}

void openCards()
{
	// The textures of a visit that was not closed from here.
	cardsUi.saves.clear();
	cardsUi.saveIcons.clear();
	cardsUi.files.clear();
	forgetCardIcons();
	cardsUi = CardsUi{};
	cardsUi.open = true;
	// From here on, a card's first change keeps the card as it is now beside it.
	ps5::vmu::beginSession();
	readCards();
}

void cardsDialog(ImDrawList *dl, const Input& given)
{
	using ps5::sound::Cue;
	CardsUi& ui = cardsUi;
	const int cardCount = (int)ui.cards.size();
	auto cardAt = [&](int index) -> const vmu::Card * {
		return index >= 0 && index < (int)ui.cards.size() ? &ui.cards[index] : nullptr;
	};
	const std::string shownFolder = shownRoot() + "vmu/";

	// ---- the pad
	// readInput() has played the press's own sound: here only a refusal's. A
	// list made again (readCards) takes the place of what a pointer here was to:
	// nothing is used after it.
	const int step = given.up ? -1 : given.down ? 1 : 0;
	switch (ui.level)
	{
	case CardsUi::Cards:
	{
		if (step != 0 && ui.card + step >= 0 && ui.card + step < cardCount)
		{
			ui.card += step;
			ui.note.clear();
			readCardSaves(false);
		}
		const vmu::Card *card = cardAt(ui.card);
		if (given.back)
		{
			closeCards();
			return;
		}
		if (given.accept && card != nullptr && card->unformatted)
		{
			// The file holds nothing: a blank card is written, as Flycast would when a game starts.
			const vmu::Result result = vmu::format(card->path);
			cardsNote(result.message, !result.ok);
			readCards();
		}
		else if ((given.accept || given.right) && card != nullptr && card->readable && !ui.saves.empty())
		{
			ui.level = CardsUi::Saves;
			ui.note.clear();
		}
		else if (given.accept && card != nullptr)
			cardsNote(card->readable ? "This card has no saves. Square adds one from a file"
					: "This card cannot be read, so nothing here changes it", true);
		else if (given.accept)
			ps5::sound::cue(Cue::Refuse);
		else if (given.square && card != nullptr && card->readable)
		{
			ui.level = CardsUi::Import;
			ui.note.clear();
			readCardFiles();
		}
		else if (given.square && card != nullptr)
			cardsNote(card->unformatted ? "This card is not formatted yet. Cross formats it"
					: "This card cannot be read, so nothing is added to it", true);
		else if (given.square)
			ps5::sound::cue(Cue::Refuse);
		break;
	}

	case CardsUi::Saves:
		if (step != 0 && ui.save + step >= 0 && ui.save + step < (int)ui.saves.size())
			ui.save += step;
		if (given.back || given.left || ui.saves.empty())
			ui.level = CardsUi::Cards;
		else if (given.accept)
		{
			ui.level = CardsUi::Actions;
			ui.action = CardCopy;
			ui.note.clear();
		}
		break;

	case CardsUi::Actions:
	{
		ui.action = std::clamp(ui.action + step, 0, (int)CardActions - 1);
		const vmu::Card *card = cardAt(ui.card);
		if (given.back || card == nullptr || !card->readable || ui.save >= (int)ui.saves.size())
			ui.level = CardsUi::Saves;
		else if (given.accept && ui.action == CardCopy)
		{
			// The first card it could go to, when there is one.
			int first = -1;
			for (int i = 0; i < cardCount && first < 0; i++)
				if (i != ui.card && ui.cards[i].readable)
					first = i;
			if (first < 0)
				cardsNote(cardCount > 1 ? "No other card can be read: there is none to copy to" : "There is no other card to copy to", true);
			else
			{
				ui.target = first;
				ui.level = CardsUi::Destination;
				ui.note.clear();
			}
		}
		else if (given.accept && ui.action == CardExport)
		{
			const vmu::Save save = ui.saves[ui.save];
			const vmu::Result result = vmu::exportSave(*card, save, ps5::rootDir + "vmu/");
			if (result.ok)
			{
				// Where it went, as the user reaches it.
				std::string file = result.path.substr(result.path.find_last_of('/') + 1);
				if (file.size() > 4)
					file.resize(file.size() - 4);
				cardsNote(save.name + " was exported to " + shownFolder + file + ".vmi and .vms"
						+ (result.copyProtected ? std::string(". ") + cardProtected : ""), false);
			}
			else
				cardsNote(result.message, true);
			ui.level = CardsUi::Saves;
			readCards();
		}
		else if (given.accept)
		{
			ui.level = CardsUi::Confirm;
			ui.note.clear();
		}
		break;
	}

	case CardsUi::Destination:
	{
		// Every card but the one the save is on.
		int next = ui.target + step;
		if (next == ui.card)
			next += step;
		if (step != 0 && next >= 0 && next < cardCount)
		{
			ui.target = next;
			ui.note.clear();
		}
		const vmu::Card *from = cardAt(ui.card), *to = cardAt(ui.target);
		if (given.back || from == nullptr || to == nullptr || ui.save >= (int)ui.saves.size())
			ui.level = CardsUi::Actions;
		else if (given.accept && (!from->readable || ui.target == ui.card))
			ps5::sound::cue(Cue::Refuse);
		else if (given.accept && !to->readable)
			cardsNote(to->unformatted ? "That card is not formatted yet: nothing is copied to it"
					: "That card cannot be read, so nothing is copied to it", true);
		else if (given.accept)
		{
			const vmu::Save save = ui.saves[ui.save];
			const std::string where = ui.names[ui.target] + (to->perGame ? "'s card" : "");
			const vmu::Result result = vmu::copy(*from, save, *to);
			if (result.ok)
			{
				cardsNote(save.name + " was copied to " + where + (result.copyProtected ? std::string(". ") + cardProtected : ""), false);
				ui.level = CardsUi::Saves;
			}
			else
				cardsNote(result.message, true);		// another card can be chosen
			readCards();
		}
		break;
	}

	case CardsUi::Confirm:
	{
		const vmu::Card *card = cardAt(ui.card);
		if (given.back || card == nullptr || !card->readable || ui.save >= (int)ui.saves.size())
			ui.level = CardsUi::Actions;
		else if (given.accept)
		{
			const vmu::Result result = vmu::remove(*card, ui.saves[ui.save]);
			cardsNote(result.message, !result.ok);
			readCards();
			ui.level = ui.saves.empty() ? CardsUi::Cards : CardsUi::Saves;
		}
		break;
	}

	default:
	{
		if (step != 0 && ui.file + step >= 0 && ui.file + step < (int)ui.files.size())
		{
			ui.file += step;
			ui.note.clear();
		}
		const vmu::Card *card = cardAt(ui.card);
		const CardFile *file = ui.file < (int)ui.files.size() ? &ui.files[ui.file] : nullptr;
		if (given.back || card == nullptr || !card->readable)
		{
			ui.level = CardsUi::Cards;
			ui.files.clear();
			forgetCardIcons();
		}
		else if (given.accept && file == nullptr)
			ps5::sound::cue(Cue::Refuse);
		else if (given.accept && !file->ok)
			cardsNote(file->why, true);
		else if (given.accept)
		{
			const std::array<uint8_t, 12> added = file->save.rawName;
			const vmu::Result result = vmu::importSave(*card, file->path);
			cardsNote(result.message, !result.ok);
			if (result.ok)
				ui.files.clear();
			readCards();
			if (result.ok)
			{
				// Back to the card, which now lists it: the list is where the new save is.
				ui.level = CardsUi::Cards;
				for (size_t i = 0; i < ui.saves.size(); i++)
					if (ui.saves[i].rawName == added)
						ui.save = (int)i;
			}
		}
		break;
	}
	}

	// ---- the dialog
	const float w = 1640, h = 880, pad = 52, inner = w - 2 * pad;
	const ImVec2 at = dialogPanel(dl, w, h);
	const float x0 = at.x + pad;
	text(dl, bold(), 38, x0, at.y + pad - 6, col::text, "Memory cards");
	text(dl, regular(), 23, x0, at.y + pad + 50, col::dim,
			"A card changed here is kept as it was before this visit, beside it as a .bak file");

	const float top = at.y + pad + 104;
	const float buttonsY = at.y + h - pad - 60 + 8;
	const float noteY = buttonsY - 20 - 54;
	const float paneH = noteY - 18 - top;
	const float leftW = 540, gap = 28;
	const float rx = x0 + leftW + gap, rw = inner - leftW - gap;
	const vmu::Card *card = cardAt(ui.card);
	const bool choosing = ui.level == CardsUi::Destination;

	// ---- the cards
	rect(dl, x0, top, leftW, paneH, col::rgba(0, 0, 0, 48), 16);
	if (choosing)
		outline(dl, x0, top, leftW, paneH, col::accent, 16, 2.5f);
	if (cardCount == 0)
		text(dl, regular(), 22, x0 + 28, top + 28, col::dim,
				"No memory cards yet.\n\nA game makes its card the first time it starts, in the data folder.", leftW - 56);
	else
	{
		// The rows, with a heading over the shared cards and one over the games' own.
		const float headH = 50;
		std::vector<float> rowY(cardCount);
		float total = 8;
		int sharedHead = -1, gameHead = -1;
		for (int i = 0; i < cardCount; i++)
		{
			int& head = ui.cards[i].perGame ? gameHead : sharedHead;
			if (head < 0)
			{
				head = i;
				total += headH;
			}
			rowY[i] = total;
			total += CardRowH;
		}
		total += 8;
		const int lit = choosing ? ui.target : ui.card;
		const float target = std::clamp(rowY[lit] + CardRowH / 2 - paneH / 2, 0.f, std::max(0.f, total - paneH));
		ui.cardScroll = motion() == MotionOff ? target : approach(ui.cardScroll, target, 18);
		dl->PushClipRect(V(x0, top + 4), V(x0 + leftW, top + paneH - 4), true);
		for (int i = 0; i < cardCount; i++)
		{
			const vmu::Card& c = ui.cards[i];
			const float y = top + rowY[i] - ui.cardScroll;
			if (y > top + paneH || y + CardRowH < top - headH)
				continue;
			if (i == sharedHead || i == gameHead)
				text(dl, bold(), 20, x0 + 28, y - headH + 18, col::faint,
						i == gameHead ? "Each game's own card, in port A, slot 1" : "Shared cards");
			const bool on = (ui.level == CardsUi::Cards && i == ui.card) || (choosing && i == ui.target);
			const bool source = i == ui.card && !on;
			if (on || source)
				rect(dl, x0 + 8, y + 2, leftW - 16, CardRowH - 4, on ? col::accent : col::panelHi, 12);
			const bool bad = !c.readable && !c.unformatted;
			const ImU32 ink = on || source ? col::text : c.readable ? col::dim : col::faint;
			const char *sign = bad ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_SD_CARD;
			const ImVec2 ss = textSize(regular(), 28, sign);
			text(dl, regular(), 28, x0 + 46 - ss.x / 2, y + CardRowH / 2 - ss.y / 2, bad && !on ? alpha(col::warm, 0.85f) : ink, sign);
			float right = x0 + leftW - 28;
			if (choosing && source)
				right = cardMark(dl, right, y + CardRowH / 2, "The save is here", false);
			const float tx = x0 + 84, tw = right - 4 - tx;
			text(dl, bold(), 26, tx, y + 11, ink, fit(bold(), 26, ui.names[i], tw).c_str());
			text(dl, regular(), 20, tx, y + 45, on || source ? col::text : bad ? alpha(col::warm, 0.9f) : col::faint,
					fit(regular(), 20, cardStatus(c), tw).c_str());
		}
		dl->PopClipRect();
	}

	// ---- the focused card: its saves, or the files one can be added from
	rect(dl, rx, top, rw, paneH, col::rgba(0, 0, 0, 48), 16);
	const float headerH = 84, listTop = top + headerH, listH = paneH - headerH - 4;
	const bool importing = ui.level == CardsUi::Import;
	float actionsY = -1;	// where the focused save's row is, for the things to do with it
	if (card != nullptr)
	{
		// Which card, and how full; under it its file, or where the files come from.
		const std::string free = card->readable ? std::to_string(card->freeBlocks) + " of " + blocksText(card->userBlocks) + " free" : "";
		const ImVec2 fs = textSize(regular(), 22, free.c_str());
		text(dl, regular(), 22, rx + rw - 28 - fs.x, top + 20, col::dim, free.c_str());
		const std::string heading = importing ? "Add a save to " + ui.names[ui.card] + (card->perGame ? "'s card" : "") : ui.names[ui.card];
		text(dl, bold(), 28, rx + 28, top + 14, col::text, fit(bold(), 28, heading, rw - 56 - fs.x - 24).c_str());
		const std::string under = importing ? "From " + shownFolder
				: (card->perGame ? "This game's card in port A, slot 1   \xc2\xb7   " : "") + ("data/" + card->fileName);
		text(dl, regular(), 20, rx + 28, top + 50, col::faint, fit(regular(), 20, under, rw - 56).c_str());
		rect(dl, rx + 20, listTop - 1, rw - 40, 1, col::line);
	}

	const float visible = listH / CardRowH;
	if (card == nullptr)
		paneWords(dl, rx, top, rw, ICON_FA_SD_CARD, false, "Nothing to show", "Saves are listed here once there is a card.");
	else if (importing)
	{
		const int count = (int)ui.files.size();
		if (count == 0)
			paneWords(dl, rx, listTop, rw, ICON_FA_FILE_IMPORT, false, "No save files there",
					"Put a save's .vmi file and its .vms, or a .dci file, in " + shownFolder + " and open this list again.");
		const float target = std::clamp((float)ui.file - (visible - 1) / 2, 0.f, std::max(0.f, (float)count - visible));
		ui.fileScroll = motion() == MotionOff ? target : approach(ui.fileScroll, target, 18);
		dl->PushClipRect(V(rx, listTop), V(rx + rw, listTop + listH), true);
		for (int i = 0; i < count; i++)
		{
			const CardFile& file = ui.files[i];
			const float y = listTop + (i - ui.fileScroll) * CardRowH;
			if (y > listTop + listH || y + CardRowH < listTop)
				continue;
			if (file.ok)
				saveRow(dl, file.save, file.icon, saveTitle(file.save), saveDetail(file.shown, file.save), false, rx, y, rw,
						i == ui.file, false);
			else
				saveRow(dl, file.save, "", file.shown, "Not a save that can be added", true, rx, y, rw, i == ui.file, false);
		}
		dl->PopClipRect();
	}
	else if (!card->readable)
	{
		if (card->unformatted)
			paneWords(dl, rx, listTop, rw, ICON_FA_SD_CARD, false, "This card is not formatted yet",
					"Its file holds nothing. A game formats it when it next starts with this card in, and Cross formats it now.");
		else
			paneWords(dl, rx, listTop, rw, ICON_FA_TRIANGLE_EXCLAMATION, true, "This card cannot be read",
					card->error + "\n\nNothing here changes it. To start again with a blank card, take its file out of the data "
					"folder: a game then makes a new one.");
	}
	else if (ui.saves.empty())
		paneWords(dl, rx, listTop, rw, ICON_FA_FLOPPY_DISK, false, "No saves on this card",
				"Square adds one from a file in " + shownFolder);
	else
	{
		const int count = (int)ui.saves.size();
		const float target = std::clamp((float)ui.save - (visible - 1) / 2, 0.f, std::max(0.f, (float)count - visible));
		ui.saveScroll = motion() == MotionOff ? target : approach(ui.saveScroll, target, 18);
		dl->PushClipRect(V(rx, listTop), V(rx + rw, listTop + listH), true);
		for (int i = 0; i < count; i++)
		{
			const float y = listTop + (i - ui.saveScroll) * CardRowH;
			if (i == ui.save)
				actionsY = y;
			if (y > listTop + listH || y + CardRowH < listTop)
				continue;
			const bool here = i == ui.save && ui.level != CardsUi::Cards;
			saveRow(dl, ui.saves[i], ui.saveIcons[i], saveTitle(ui.saves[i]), saveDetail(ui.saves[i].name, ui.saves[i]), false,
					rx, y, rw, here && ui.level == CardsUi::Saves, here);
		}
		dl->PopClipRect();
	}

	// ---- one line: what came of the last thing, else what the focus is about
	std::string line = ui.note;
	const vmu::Save *save = card != nullptr && ui.save < (int)ui.saves.size() ? &ui.saves[ui.save] : nullptr;
	if (line.empty())
	{
		if (ui.level == CardsUi::Cards && card != nullptr && !card->perGame && card->port == 0 && card->slot == 0 && config::PerGameVmu)
			line = "Games do not use this card: each has its own in port A, slot 1 (emu.cfg's PerGameVmu, which is on)";
		else if (ui.level == CardsUi::Actions && save != nullptr)
			line = ui.action == CardCopy ? "The save stays on this card too"
					: ui.action == CardExport ? "Writes it to " + shownFolder + " as a .vms and a .vmi file, in place of files of the same name"
					: "Its " + blocksText(save->blocks) + (save->blocks == 1 ? " is free again" : " are free again");
		else if (choosing && save != nullptr)
			line = "Choose the card " + save->name + " is copied to";
		else if (importing && ui.file < (int)ui.files.size() && !ui.files[ui.file].ok)
			line = ui.files[ui.file].why;
		else if (importing && ui.filesLeftOut > 0)
			line = "The first " + std::to_string(CardFilesListed) + " files are listed: " + std::to_string(ui.filesLeftOut) + " more are in the folder";
		while (!line.empty() && line.back() == '.')
			line.pop_back();
	}
	dl->PushClipRect(V(x0, noteY - 2), V(x0 + inner, noteY + 56), true);
	text(dl, regular(), 22, x0, noteY, ui.note.empty() ? col::faint : ui.noteBad ? col::warm : col::text, line.c_str(), inner);
	dl->PopClipRect();

	// ---- the buttons
	struct Button { Glyph glyph; std::string label; bool primary; };
	std::vector<Button> buttons;
	switch (ui.level)
	{
	case CardsUi::Cards:
		if (card != nullptr && card->unformatted)
			buttons.push_back({ Glyph::Cross, "Format", true });
		else if (card != nullptr && card->readable && !ui.saves.empty())
			buttons.push_back({ Glyph::Cross, "Saves", true });
		if (card != nullptr && card->readable)
			buttons.push_back({ Glyph::Square, "Add a save from a file", ui.saves.empty() });
		buttons.push_back({ Glyph::Circle, "Close", false });
		break;
	case CardsUi::Saves:
		buttons = { { Glyph::Cross, "Copy, export or delete", true }, { Glyph::Circle, "Cards", false } };
		break;
	case CardsUi::Actions:
	case CardsUi::Confirm:
		buttons = { { Glyph::Cross, "Choose", true }, { Glyph::Circle, "Back", false } };
		break;
	case CardsUi::Destination:
		buttons = { { Glyph::Cross, "Copy here", true }, { Glyph::Circle, "Back", false } };
		break;
	default:
		if (!ui.files.empty())
			buttons.push_back({ Glyph::Cross, "Add to the card", ui.files[std::min(ui.file, (int)ui.files.size() - 1)].ok });
		buttons.push_back({ Glyph::Circle, "Back", false });
		break;
	}
	float bx = x0;
	for (const Button& button : buttons)
		bx += dialogButton(dl, bx, buttonsY, button.glyph, button.label.c_str(), button.primary) + 20;

	// ---- what can be done with the focused save, under its row (over it at the list's end)
	if ((ui.level == CardsUi::Actions || ui.level == CardsUi::Confirm) && save != nullptr)
	{
		const char *const signs[CardActions] = { ICON_FA_COPY, ICON_FA_FILE_EXPORT, ICON_FA_TRASH_CAN };
		const char *const names[CardActions] = { "Copy to another card", "Export to a file", "Delete" };
		const float rowH = 52, mw = 400, mh = CardActions * rowH + 20;
		const float mx = rx + rw - 36 - mw;
		float my = actionsY + CardRowH - 6;
		if (my + mh > top + paneH - 8)
			my = std::max(listTop + 6, actionsY + 6 - mh);
		glow(dl, mx, my, mw, mh, 16, col::rgba(0, 0, 0, 150), 24);
		rect(dl, mx, my, mw, mh, col::panelHi, 16);
		outline(dl, mx, my, mw, mh, col::line, 16, 1.5f);
		for (int i = 0; i < CardActions; i++)
		{
			const float y = my + 10 + i * rowH;
			const bool on = i == ui.action;
			if (on)
				rect(dl, mx + 10, y + 2, mw - 20, rowH - 4, col::accent, 12);
			// Its sign in a column of its own, so that the words line up.
			const ImVec2 ss = textSize(regular(), 24, signs[i]);
			text(dl, regular(), 24, mx + 46 - ss.x / 2, y + 12, on ? col::text : col::dim, signs[i]);
			text(dl, bold(), 24, mx + 80, y + 12, on ? col::text : col::dim, names[i]);
		}
	}

	// ---- asked before a save is deleted: a panel of its own, over this one
	if (ui.level == CardsUi::Confirm && save != nullptr)
	{
		const float cw = 820, ch = 352;
		const ImVec2 ca = dialogPanel(dl, cw, ch);
		text(dl, bold(), 38, ca.x + pad, ca.y + pad - 6, col::text, "Delete this save?");
		text(dl, regular(), 23, ca.x + pad, ca.y + pad + 50, col::dim, fit(regular(), 23, saveTitle(*save), cw - 2 * pad).c_str());
		text(dl, regular(), 23, ca.x + pad, ca.y + pad + 84, col::dim,
				fit(regular(), 23, saveDetail(save->name, *save), cw - 2 * pad).c_str());
		text(dl, regular(), 22, ca.x + pad, ca.y + pad + 130, col::faint,
				"The card as it was before this visit stays beside it, as a .bak file");
		float cx = ca.x + pad;
		const float cy = ca.y + ch - pad - 60 + 8;
		cx += dialogButton(dl, cx, cy, Glyph::Cross, "Delete", true) + 20;
		dialogButton(dl, cx, cy, Glyph::Circle, "Keep it", false);
	}
}
