/*
	PSFlyCast - the controller profiles' screen: a part of bigpicture.cpp,
	which includes it (its helpers are that file's).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A dialog over the Settings or the quick menu, in two panes. On the left the
	profile being looked at: whether the right stick is the mouse, then every
	input that can be given something to do (the buttons, the triggers, the
	D-pad, the four directions of the left stick), each with what it does. On
	the right, once Cross picks an entry, what it can do: stay the controller's,
	nothing, a mouse button, the wheel, or any key. L1 and R1 go from profile
	to profile. The four built-in profiles cannot change: Square copies the one
	shown into one of the player's own, which can. Triangle makes the shown
	profile the one in use. The profiles are ps5::pad::bind's.
*/

namespace
{

struct BindsUi
{
	bool open = false;
	bool picking = false;		// the pad is in the right pane
	int shown = 0;				// the profile looked at
	int row = 0;				// 0 the right stick, 1.. the inputs
	int choice = 0;				// in the right pane
	std::vector<int> choices;	// the actions the right pane lists
	std::string note;
	bool noteBad = false;
} bindsUi;

void bindsMakeChoices()
{
	namespace B = ps5::pad::bind;
	bindsUi.choices = { B::Controller, B::Blocked, B::MouseLeft, B::MouseRight, B::MouseMiddle, B::WheelUp, B::WheelDown };
	for (const B::KeyName& k : B::keys())
		bindsUi.choices.push_back(B::Key + k.code);
}

void openBinds()
{
	bindsUi = BindsUi{};
	bindsUi.open = true;
	bindsUi.shown = ps5::pad::bind::chosen();
	bindsMakeChoices();
}

// The first row of a window of `shown` rows that keeps `focus` in view.
int bindsWindow(int focus, int count, int shown)
{
	return std::clamp(focus - shown / 2, 0, std::max(0, count - shown));
}

void bindsNote(const std::string& text, bool bad = false)
{
	bindsUi.note = text;
	bindsUi.noteBad = bad;
	if (bad)
		ps5::sound::cue(ps5::sound::Cue::Refuse);
}

void bindsDialog(ImDrawList *dl, const Input& given)
{
	namespace B = ps5::pad::bind;
	BindsUi& ui = bindsUi;
	const int step = given.up ? -1 : given.down ? 1 : 0;
	const int rows = B::Inputs + 1;
	B::Profile p = B::profile(ui.shown);
	const bool own = B::profileIsOwn(ui.shown);

	// ---- the pad
	if (!ui.picking)
	{
		if (step != 0)
			ui.row = std::clamp(ui.row + step, 0, rows - 1);
		if (given.l2)
			ui.row = std::max(0, ui.row - 8);
		if (given.r2)
			ui.row = std::min(rows - 1, ui.row + 8);
		if (given.l1 || given.r1)
		{
			ui.shown = (ui.shown + (given.l1 ? B::Profiles - 1 : 1)) % B::Profiles;
			ui.note.clear();
		}
		if (given.back)
		{
			ui.open = false;
			SaveSettings();
			return;
		}
		if (given.accept)
		{
			if (!own)
				bindsNote("A built-in profile cannot change: Square copies it into one of your own", true);
			else if (ui.row == 0)
			{
				p.mouse = !p.mouse;
				B::setProfile(ui.shown, p);
				bindsNote(p.mouse ? "The right stick moves the mouse" : "The right stick is the controller's");
			}
			else
			{
				ui.picking = true;
				ui.choice = 0;
				for (size_t i = 0; i < ui.choices.size(); i++)
					if (ui.choices[i] == p.action[ui.row - 1])
						ui.choice = (int)i;
				ui.note.clear();
			}
		}
		if (given.square)
		{
			// Into the first of the player's own that holds nothing yet, or the last of them.
			int target = B::Profiles - 1;
			for (int i = B::BuiltIn; i < B::Profiles; i++)
			{
				const B::Profile q = B::profile(i);
				bool empty = !q.mouse;
				for (int a : q.action)
					empty = empty && a == B::Controller;
				if (empty)
				{
					target = i;
					break;
				}
			}
			B::setProfile(target, p);
			bindsNote(std::string(B::profileName(ui.shown)) + " copied into " + B::profileName(target));
			ui.shown = target;
		}
		if (given.triangle)
		{
			ps5::pad::PadProfile.set(ui.shown);
			bindsNote(std::string(B::profileName(ui.shown)) + " is the profile in use. The mouse and keyboard take their ports when a game starts");
		}
	}
	else
	{
		const int count = (int)ui.choices.size();
		if (step != 0)
			ui.choice = std::clamp(ui.choice + step, 0, count - 1);
		if (given.l2 || given.l1)
			ui.choice = std::max(0, ui.choice - 10);
		if (given.r2 || given.r1)
			ui.choice = std::min(count - 1, ui.choice + 10);
		if (given.back)
			ui.picking = false;
		else if (given.accept)
		{
			p.action[ui.row - 1] = ui.choices[ui.choice];
			B::setProfile(ui.shown, p);
			bindsNote(std::string(B::inputName(ui.row - 1)) + ": " + B::actionName(ui.choices[ui.choice]));
			ui.picking = false;
		}
	}

	// ---- the dialog
	p = B::profile(ui.shown);
	const bool inUse = ui.shown == B::chosen();
	const float w = 1560, h = 900, pad = 52, inner = w - 2 * pad;
	const ImVec2 at = dialogPanel(dl, w, h);
	const float x0 = at.x + pad;
	text(dl, bold(), 38, x0, at.y + pad - 6, col::text, "Controller profiles");
	{
		// The profile's name, with L1 and R1 either side of it, and whether it is the one in use.
		const std::string title = std::string(B::profileName(ui.shown)) + (inUse ? "   (in use)" : "") + (own ? "" : "   (built in)");
		float x = x0;
		x += glyph(dl, Glyph::L1, x, at.y + pad + 62) + 16;
		text(dl, bold(), 26, x, at.y + pad + 48, inUse ? col::accent : col::text, title.c_str());
		x += textSize(bold(), 26, title.c_str()).x + 16;
		glyph(dl, Glyph::R1, x, at.y + pad + 62);
	}
	const float top = at.y + pad + 104;
	const float buttonsY = at.y + h - pad - 60 + 8;
	const float noteY = buttonsY - 20 - 34;
	const float paneH = noteY - 12 - top;
	const float leftW = 720, gap = 28;
	const float rx = x0 + leftW + gap, rw = inner - leftW - gap;
	const float rowH = 48;
	const int shown = std::max(4, (int)((paneH - 16) / rowH));

	// the profile
	rect(dl, x0, top, leftW, paneH, col::rgba(0, 0, 0, 48), 16);
	if (!ui.picking)
		outline(dl, x0, top, leftW, paneH, col::accent, 16, 2.5f);
	{
		const int first = bindsWindow(ui.row, rows, shown);
		for (int n = 0; n < shown && first + n < rows; n++)
		{
			const int i = first + n;
			const float y = top + 8 + n * rowH;
			const bool on = i == ui.row;
			if (on)
				rect(dl, x0 + 8, y, leftW - 16, rowH - 4, ui.picking ? col::panelHi : col::accent, 12);
			const char *label = i == 0 ? "Right stick" : B::inputName(i - 1);
			const std::string value = i == 0 ? (p.mouse ? "Mouse" : "Controller") : B::actionName(p.action[i - 1]);
			// What stays the controller's is dim: the eye finds what the profile gives away.
			const bool given_ = i == 0 ? p.mouse : p.action[i - 1] != B::Controller;
			text(dl, bold(), 23, x0 + 24, y + 11, on ? col::text : col::dim, label);
			text(dl, regular(), 23, x0 + 250, y + 11, on ? col::text : given_ ? col::text : col::faint,
					fit(regular(), 23, value, leftW - 250 - 24).c_str());
		}
	}

	// what it can do
	rect(dl, rx, top, rw, paneH, col::rgba(0, 0, 0, 48), 16);
	if (ui.picking)
		outline(dl, rx, top, rw, paneH, col::accent, 16, 2.5f);
	{
		const int count = (int)ui.choices.size();
		const int first = bindsWindow(ui.picking ? ui.choice : 0, count, shown);
		if (!ui.picking)
			text(dl, regular(), 22, rx + 28, top + 28, col::dim,
					own ? "Press Cross to change the entry on the left: keep it on the controller, disable it, "
							"or make it a mouse button, the mouse wheel or a key.\n\n"
							"An input the profile reassigns is no longer sent to the Dreamcast controller, so nothing is pressed twice. "
							"Entries left as \"Controller\" work as before.\n\n"
							"Press Triangle to use this profile."
						: "Built-in profiles cannot be changed. Press Square to copy this one into a profile of your own "
							"(Custom 1 to 4).\n\n"
							"Controller: the controller alone. Controller and mouse: the right stick aims; every button stays on the controller. "
							"Mouse and keyboard: for games played with them, with PC-style FPS keys.\n\n"
							"Press Triangle to use this profile.", rw - 56);
		else
			for (int n = 0; n < shown && first + n < count; n++)
			{
				const int i = first + n;
				const float y = top + 8 + n * rowH;
				const bool on = i == ui.choice;
				if (on)
					rect(dl, rx + 8, y, rw - 16, rowH - 4, col::accent, 12);
				text(dl, regular(), 23, rx + 24, y + 11, on ? col::text : col::dim,
						fit(regular(), 23, B::actionName(ui.choices[i]), rw - 48).c_str());
			}
	}

	if (!ui.note.empty())
		text(dl, regular(), 22, x0, noteY, ui.noteBad ? col::warm : col::text, ui.note.c_str(), inner);
	else
		text(dl, regular(), 22, x0, noteY, col::faint,
				"The profile in use is a setting, and each game can have its own (quick menu > Controls). The mouse and keyboard take their ports when a game starts.", inner);
	float bx = x0;
	if (!ui.picking)
	{
		bx += dialogButton(dl, bx, buttonsY, Glyph::Cross, own ? "Change" : "Look", true) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::Triangle, inUse ? "In use" : "Use it", false) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::Square, "Copy to own", false) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::L1R1, "Profile", false) + 20;
		dialogButton(dl, bx, buttonsY, Glyph::Circle, "Close", false);
	}
	else
	{
		bx += dialogButton(dl, bx, buttonsY, Glyph::Cross, "Give it", true) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::L1R1, "Page", false) + 20;
		dialogButton(dl, bx, buttonsY, Glyph::Circle, "Back", false);
	}
}

}
