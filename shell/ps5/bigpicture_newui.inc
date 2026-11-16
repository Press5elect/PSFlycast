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

// Each setting of the table to its row; the rows no setting names become extras.
void bindRows()
{
	ns.rows.clear();
	ns.host.extras.clear();
	ns.names.clear();
	std::map<std::string, std::vector<Row *>> byLabel;
	for (Category& c : st.all)
		for (Row& r : c.rows)
			if (r.kind != Row::Heading)
				byLabel[r.label].push_back(&r);
	std::map<const Row *, const pui::Setting *> settingOfRow;
	for (const pui::Setting& s : pui::settings())
	{
		if (s.has('G'))
			continue;
		auto it = byLabel.find(s.was);
		if (it == byLabel.end() || it->second.empty())
			continue;
		Row *r = it->second.front();
		it->second.erase(it->second.begin());
		ns.rows[s.id] = r;
		settingOfRow[r] = &s;
	}
	for (Category& c : st.all)
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
			if (near == nullptr)
				continue;
			ns.names.push_back("extra:" + std::string(c.name) + "/" + r.label);
			pui::Setting e{ ns.names.back().c_str(), near->area, near->section, kindOfRow(r), near->level, r.label.c_str(),
					r.desc.c_str(), 'P', "", {}, 0, 0, 0, 1, "", r.label.c_str() };
			ns.host.extras.push_back(e);
			ns.rows[e.id] = &r;
		}
	}
	ns.boundTo = st.all.data();
	ns.boundSize = st.all.size();
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
		const auto it = settingDefaults.find(r->label);
		return it != settingDefaults.end() && r->get() != it->second;
	};
	h.reset = [](const pui::Setting& s) {
		Row *r = rowOf(s);
		if (r == nullptr || !r->set)
			return;
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
	};
}

// The Settings for every game, in the new interface: called by settings() in place of its own
// drawing, after the dialogs and the keyboard have had the pad.
void newSettings(ImDrawList *dl)
{
	// Opened again (it was not drawn the frame before): from its home.
	static int lastFrame = -10;
	if (ImGui::GetFrameCount() > lastFrame + 2)
		ns.open = false;
	lastFrame = ImGui::GetFrameCount();
	if (!ns.open)
	{
		ns.open = true;
		readPins();
		setUpHost();
		pui::show(pui::Screen::SettingsHome);
	}
	if (ns.boundTo != st.all.data() || ns.boundSize != st.all.size())
		bindRows();
	ns.host.regular = regular();
	ns.host.bold = bold();
	char clock[16] = "";
	const time_t t = time(nullptr);
	struct tm local;
	if (localtime_r(&t, &local) != nullptr)
		strftime(clock, sizeof(clock), "%H:%M", &local);
	ns.host.clock = clock;
	ns.host.pads = ps5::pad::connectedCount();
	pui::Input pin;
	pin.up = in.up; pin.down = in.down; pin.left = in.left; pin.right = in.right;
	pin.accept = in.accept; pin.back = in.back; pin.triangle = in.triangle; pin.square = in.square;
	pin.l1 = in.l1; pin.r1 = in.r1; pin.l2 = in.l2; pin.r2 = in.r2; pin.r3 = in.r3; pin.options = in.options;
	pui::frame(ns.host, pin, dl);
	savePins();
	// Settings that need PSFlyCast started again: said at the bottom left, with Square.
	if (!st.restartFor.empty() && gui_state == GuiState::Settings)
	{
		std::string names;
		for (const std::string& n : st.restartFor)
			names += (names.empty() ? "" : ", ") + n;
		text(dl, bold(), 22, 84, H - 72 - 40, col::warm,
				fit(bold(), 22, "Start PSFlyCast again to apply: " + names + "   (L2 restarts now)", W * 0.55f).c_str());
	}
	endScreen();
}

} // namespace
