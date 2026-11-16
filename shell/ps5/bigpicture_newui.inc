/*
	PSFlyCast - the new interface (shell/ps5/ui) on the console: a part of bigpicture.cpp, which
	includes it (its helpers are that file's).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The new Settings draw the table of shell/ps5/tools/settings-table.py; each of its settings is
	bound here to the row of the Settings before (buildCategories) that it replaces, by the label
	that row has (the table's "was"). The row does the work as it did: its value, its change, its
	action and the dialogs they open. A row the table does not name (a network folder, a server
	found, a credit) is shown too, after the table's rows of the section its neighbours are in.
	Nothing of the Settings before is therefore out of reach.

	"Changed from default" compares a row with the value it had in a new install: Flycast's
	defaults with PSFlyCast's own over them, read once at start (captureSettingDefaults).
*/

namespace
{

struct NewSettings
{
	pui::Host host;
	bool open = false;
	std::map<std::string, Row *> rows;			// setting id -> its row in st.all
	std::deque<std::string> names;				// what the extras' ids point into
	const void *boundTo = nullptr;
	size_t boundSize = 0;
	bool boundForGame = false;
	double allAsked = -10;						// a game's Settings: Square once; again, all back
	std::vector<std::string> savedPins;
	bool pinsRead = false;
} ns;

std::map<std::string, int> settingDefaults;		// a row's label -> its value in a new install

std::string pinsFile()
{
	return ps5::rootDir + "data/settings-pins.txt";
}

void readPins()
{
	if (ns.pinsRead)
		return;
	ns.pinsRead = true;
	if (FILE *f = fopen(pinsFile().c_str(), "r"))
	{
		char line[256];
		while (fgets(line, sizeof(line), f))
		{
			std::string id = trimmed(line);
			if (!id.empty())
				ns.host.pins.push_back(id);
		}
		fclose(f);
	}
	ns.savedPins = ns.host.pins;
}

void savePins()
{
	if (ns.host.pins == ns.savedPins)
		return;
	ns.savedPins = ns.host.pins;
	if (FILE *f = fopen(pinsFile().c_str(), "w"))
	{
		for (const std::string& id : ns.host.pins)
			fprintf(f, "%s\n", id.c_str());
		fclose(f);
	}
}

Row *rowOf(const pui::Setting& s)
{
	const auto it = ns.rows.find(s.id);
	return it == ns.rows.end() ? nullptr : it->second;
}

pui::Kind kindOfRow(const Row& r)
{
	switch (r.kind)
	{
	case Row::Toggle: return pui::Kind::Toggle;
	case Row::Choice: return pui::Kind::Choice;
	case Row::Slider: return pui::Kind::Range;
	case Row::Action: return pui::Kind::Action;
	default: return pui::Kind::Info;
	}
}

// The area and section of the table a game's row goes to when no setting of the table names it
// and no neighbour of its category does: by what its option is.
std::pair<int, const char *> placeOfGameRow(const Row& r)
{
	int area = 0;
	if (r.game != nullptr)
	{
		if (isControlOption(*r.game) && r.game->patch == nullptr)
			area = 2;
		else if (r.game->category == GameOption::Audio)
			area = 1;
		else if (r.game->category == GameOption::System)
			area = 4;
	}
	for (const pui::Setting& s : pui::settings())
		if (s.area == area && (area != 0 || std::string(s.section) == "Game Patches"))
			return { area, s.section };
	return { area, "" };
}

// Each setting of the table to its row; the rows no setting names become extras. For a game's
// Settings (st.cats from buildGameCategories), its rows; else every row of the Settings (st.all).
void bindRows(bool forGame)
{
	std::vector<Category>& cats = forGame ? st.cats : st.all;
	ns.rows.clear();
	ns.host.extras.clear();
	ns.names.clear();
	std::map<std::string, std::vector<Row *>> byLabel;
	for (Category& c : cats)
		for (Row& r : c.rows)
			if (r.kind != Row::Heading)
				byLabel[r.label].push_back(&r);
	std::map<const Row *, const pui::Setting *> settingOfRow;
	for (const pui::Setting& s : pui::settings())
	{
		if (s.has('G') && !forGame)
			continue;
		auto it = byLabel.find(s.was);
		if (it == byLabel.end() || it->second.empty())
			continue;
		Row *r = it->second.front();
		it->second.erase(it->second.begin());
		ns.rows[s.id] = r;
		settingOfRow[r] = &s;
	}
	for (Category& c : cats)
	{
		const pui::Setting *near = nullptr;
		for (Row& r : c.rows)
			if (settingOfRow.count(&r))
			{
				near = settingOfRow[&r];
				break;
			}
		for (Row& r : c.rows)
		{
			if (r.kind == Row::Heading)
				continue;
			if (settingOfRow.count(&r))
			{
				near = settingOfRow[&r];
				continue;
			}
			int area = 0;
			const char *section = nullptr;
			pui::Level level = pui::Level::Everything;
			if (near != nullptr)
			{
				area = near->area;
				section = near->section;
				level = near->level;
			}
			else if (forGame)
			{
				const auto place = placeOfGameRow(r);
				area = place.first;
				section = place.second;
			}
			else
				continue;
			ns.names.push_back("extra:" + std::string(c.name) + "/" + r.label);
			// A game's row: one game can have it; its flags say so in the detail.
			pui::Setting e{ ns.names.back().c_str(), area, section, kindOfRow(r), level, r.label.c_str(),
					r.desc.c_str(), 'P', forGame ? "G" : "", {}, 0, 0, 0, 1, "", r.label.c_str() };
			ns.host.extras.push_back(e);
			ns.rows[e.id] = &r;
		}
	}
	ns.boundTo = cats.data();
	ns.boundSize = cats.size();
	ns.boundForGame = forGame;
}

void setUpHost()
{
	pui::Host& h = ns.host;
	h.regular = regular();
	h.bold = bold();
	h.running = -1;
	h.kind = [](const pui::Setting& s) {
		const Row *r = rowOf(s);
		return r ? kindOfRow(*r) : pui::Kind::Info;
	};
	h.get = [](const pui::Setting& s) {
		const Row *r = rowOf(s);
		return r && r->get ? r->get() : 0;
	};
	h.value = [](const pui::Setting& s) -> std::string {
		const Row *r = rowOf(s);
		if (r == nullptr)
			return "";
		if (r->kind == Row::Action || r->kind == Row::Info)
			return r->info ? r->info() : "";
		if (r->kind == Row::Toggle)
			return r->get() ? "On" : "Off";
		return valueText(*r);
	};
	h.step = [](const pui::Setting& s, int dir) {
		if (Row *r = rowOf(s))
			change(*r, dir);
	};
	h.run = [](const pui::Setting& s) {
		if (Row *r = rowOf(s); r && r->action)
			r->action();
	};
	h.choices = [](const pui::Setting& s) -> std::string {
		const Row *r = rowOf(s);
		if (r == nullptr)
			return "";
		switch (r->kind)
		{
		case Row::Toggle: return "On or Off";
		case Row::Slider: return std::to_string(r->minValue) + r->unit + " to " + std::to_string(r->maxValue) + r->unit;
		case Row::Choice:
		{
			std::string all;
			for (const std::string& c : r->choices)
				all += (all.empty() ? "" : ", ") + c;
			if (all.size() > 34 && r->choices.size() > 1)
				all = std::to_string(r->choices.size()) + ", " + r->choices.front() + " to " + r->choices.back();
			return all;
		}
		case Row::Action: return "Opens a screen";
		default: return "Shown only";
		}
	};
	h.changed = [](const pui::Setting& s) {
		const Row *r = rowOf(s);
		if (r == nullptr || !r->get || (r->kind != Row::Toggle && r->kind != Row::Choice && r->kind != Row::Slider))
			return false;
		// A game's Settings: a value of its own.
		if (r->game != nullptr)
			return ownValue(*r);
		const auto it = settingDefaults.find(r->label);
		return it != settingDefaults.end() && r->get() != it->second;
	};
	h.available = [](const pui::Setting& s) { return rowOf(s) != nullptr; };
	h.resetAll = [] {
		// Every value of the game as in the Settings for every game: asked twice, as all of its
		// own values go.
		if (timeNow < ns.allAsked + 4)
		{
			for (Category& c : st.cats)
				for (Row& one : c.rows)
					if (ownValue(one))
						one.set(settingsValue(*one.game));
			st.dirty = true;
			ns.allAsked = -10;
			os_notify("Every option of this game is as in Settings", 2500);
		}
		else
		{
			ns.allAsked = timeNow;
			os_notify("Press Square again", 2500, "Every value of this game's own goes back to the Settings for every game");
		}
	};
	h.reset = [](const pui::Setting& s) {
		Row *r = rowOf(s);
		if (r == nullptr || !r->set)
			return;
		if (r->game != nullptr)
		{
			// Back to the value of the Settings for every game.
			if (ownValue(*r))
			{
				r->set(settingsValue(*r->game));
				st.dirty = true;
			}
			return;
		}
		const auto it = settingDefaults.find(r->label);
		if (it == settingDefaults.end() || r->get() == it->second)
			return;
		r->set(it->second);
		if (r->restart && std::find(st.restartFor.begin(), st.restartFor.end(), r->label) == st.restartFor.end())
			st.restartFor.push_back(r->label);
		st.dirty = true;
	};
	h.typeQuery = [] {
		openKeyboard(KeyboardSettings, "Search the settings", "A setting's name, or what it does", ns.host.query, 40);
	};
	h.close = [] {
		ns.open = false;
		leaveSettings();
		// A game's Settings are gone with st.cats: no setting points into them any more.
		ns.rows.clear();
		ns.host.extras.clear();
		ns.names.clear();
		ns.boundTo = nullptr;
		ns.boundSize = 0;
	};
}

// The skin's colours, as the new interface takes them.
pui::Palette paletteOfSkin()
{
	pui::Palette p;
	p.bg = col::bgTop;
	p.bg2 = col::bgBottom;
	p.text = col::text;
	p.sub = mix(col::dim, col::text, 0.35f);
	p.faint = mix(col::faint, col::dim, 0.45f);
	p.card = col::panel;
	p.cardHi = col::panelHi;
	p.line = col::line;
	p.warm = col::warm;
	p.accent = col::accent;
	// Dark text on a light accent.
	const ImVec4 a = ImGui::ColorConvertU32ToFloat4(col::accent);
	p.onAccent = 0.2126f * a.x + 0.7152f * a.y + 0.0722f * a.z > 0.62f ? col::rgba(12, 14, 18) : col::rgba(255, 255, 255);
	return p;
}

// The host as every screen drawn with the kit needs it this frame: the skin, the fonts, the clock.
void refreshHost()
{
	pui::Host& h = ns.host;
	h.palette = paletteOfSkin();
	h.circleConfirms = ps5::options().circleConfirms;
	h.uiScale = ps5::options().uiScale;
	h.ground = [](ImDrawList *dl) { drawBackdrop(dl); };
	h.regular = regular();
	h.bold = bold();
	char clock[16] = "";
	const time_t t = time(nullptr);
	struct tm local;
	if (localtime_r(&t, &local) != nullptr)
		strftime(clock, sizeof(clock), "%H:%M", &local);
	h.clock = clock;
	h.pads = ps5::pad::connectedCount();
}

// Before a screen of bigpicture.cpp draws with the kit (pui::kit).
void kitBegin(ImDrawList *dl)
{
	refreshHost();
	pui::kit::begin(ns.host, dl);
}

// A game of the library as the new interface draws it.
pui::Game puiGameOf(Game& g)
{
	pui::Game out;
	out.title = g.title;
	out.platform = g.media.arcade ? "Arcade" : "Dreamcast";
	out.place = g.source == Usb ? "USB" : g.source == Network ? "Share" : g.source == Disc ? "Disc" : "Console";
	out.line = metaText(g);
	out.file = g.media.path;
	out.cover = coverTexture(g);
	out.favourite = g.favourite;
	// The colour its placeholder has (placeholder()).
	unsigned hash = 2166136261u;
	for (char c : g.title)
		hash = (hash ^ (unsigned char)c) * 16777619u;
	float cr, cg, cb;
	ImGui::ColorConvertHSVtoRGB((hash % 360) / 360.f, 0.55f, 0.42f, cr, cg, cb);
	out.colour = ImGui::ColorConvertFloat4ToU32(ImVec4(cr, cg, cb, 1.f));
	return out;
}

pui::Host& kitHost()
{
	return ns.host;
}

// The title of the game that runs.
std::string runningTitle()
{
	if (!::settings.content.title.empty())
		return ::settings.content.title;
	return get_file_basename(::settings.content.fileName);
}

// The Settings, in the new interface: for every game, or for the running game (st.forGame, the
// quick menu's "Game options"). Called by settings() in place of its own drawing, after the
// dialogs and the keyboard have had the pad.
void newSettings(ImDrawList *dl)
{
	// Opened again (it was not drawn the frame before), or for the other kind: from its home.
	static int lastFrame = -10;
	if (ImGui::GetFrameCount() > lastFrame + 2 || ns.boundForGame != st.forGame)
		ns.open = false;
	lastFrame = ImGui::GetFrameCount();
	std::vector<Category>& cats = st.forGame ? st.cats : st.all;
	if (!ns.open)
	{
		ns.open = true;
		readPins();
		setUpHost();
		ns.boundTo = nullptr;
		pui::show(pui::Screen::SettingsHome);
	}
	if (ns.boundTo != cats.data() || ns.boundSize != cats.size() || ns.boundForGame != st.forGame)
		bindRows(st.forGame);
	ns.host.forGame = st.forGame ? runningTitle() : "";
	ns.host.running = game_started ? 0 : -1;
	refreshHost();
	pui::Input pin;
	pin.up = in.up; pin.down = in.down; pin.left = in.left; pin.right = in.right;
	pin.accept = in.accept; pin.back = in.back; pin.triangle = in.triangle; pin.square = in.square;
	pin.l1 = in.l1; pin.r1 = in.r1; pin.l2 = in.l2; pin.r2 = in.r2; pin.r3 = in.r3; pin.options = in.options;
	pui::frame(ns.host, pin, dl);
	savePins();
	// Settings that need PSFlyCast started again: said at the bottom left, with Square.
	if (!st.restartFor.empty() && !st.forGame && gui_state == GuiState::Settings)
	{
		std::string names;
		for (const std::string& n : st.restartFor)
			names += (names.empty() ? "" : ", ") + n;
		// A pill at the bottom left, beside the hints (the kit is still on this screen's list).
		const std::string say = pui::kit::fit(true, 19, "Start PSFlyCast again to apply: " + names, 640);
		const float w = pui::kit::measure(true, 19, say).x + 64 + 150;
		pui::kit::fill(84, 1012, w, 48, pui::kit::alpha(col::warm, 0.18f), 24);
		pui::kit::text(false, 18, 106, 1024, col::warm, ICON_FA_ROTATE_LEFT);
		pui::kit::text(true, 19, 138, 1023, col::text, say);
		pui::kit::text(false, 18, 138 + pui::kit::measure(true, 19, say).x + 18, 1024, col::warm, "L2 restarts now");
	}
	endScreen();
}

} // namespace
