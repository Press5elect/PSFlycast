/*
	PSFlyCast - the bindings screen for the pad as a mouse and a keyboard: a
	part of bigpicture.cpp, which includes it (its helpers are that file's).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A dialog over the Settings, in two panes. On the left every input that can
	be given something to do: the buttons, the triggers, the D-pad and the four
	directions of the left stick, each with what it does now. Cross picks one;
	the right pane then lists what it can do (nothing, a mouse button, the wheel,
	or any key) and Cross gives it. The table is ps5::pad::bind's; a change makes
	it the player's own, starting from the preset that was in use.
*/

namespace
{

struct BindsUi
{
	bool open = false;
	bool picking = false;		// the pad is in the right pane
	int input = 0;
	int choice = 0;				// in the right pane
	std::vector<int> choices;	// the actions the right pane lists
	std::string note;
} bindsUi;

void bindsMakeChoices()
{
	namespace B = ps5::pad::bind;
	bindsUi.choices = { B::None, B::MouseLeft, B::MouseRight, B::MouseMiddle, B::WheelUp, B::WheelDown };
	for (const B::KeyName& k : B::keys())
		bindsUi.choices.push_back(B::Key + k.code);
}

void openBinds()
{
	bindsUi = BindsUi{};
	bindsUi.open = true;
	bindsMakeChoices();
}

// The first row of a window of `shown` rows that keeps `focus` in view.
int bindsWindow(int focus, int count, int shown)
{
	return std::clamp(focus - shown / 2, 0, std::max(0, count - shown));
}

void bindsDialog(ImDrawList *dl, const Input& given)
{
	namespace B = ps5::pad::bind;
	BindsUi& ui = bindsUi;
	const int step = given.up ? -1 : given.down ? 1 : 0;
	const B::Table table = B::current();

	// ---- the pad
	if (!ui.picking)
	{
		if (step != 0)
			ui.input = std::clamp(ui.input + step, 0, (int)B::Inputs - 1);
		if (given.back)
		{
			ui.open = false;
			SaveSettings();
			return;
		}
		if (given.accept)
		{
			ui.picking = true;
			ui.choice = 0;
			for (size_t i = 0; i < ui.choices.size(); i++)
				if (ui.choices[i] == table.action[ui.input])
					ui.choice = (int)i;
			ui.note.clear();
		}
		if (given.triangle)
		{
			B::set(ui.input, B::None);
			ui.note = std::string(B::inputName(ui.input)) + " does nothing now";
		}
		if (given.square)
		{
			B::use(ps5::pad::PadKeysWasd);
			ui.note = "The WASD layout";
		}
		if (given.options)
		{
			B::use(ps5::pad::PadKeysArrows);
			ui.note = "The arrow keys layout";
		}
	}
	else
	{
		const int count = (int)ui.choices.size();
		if (step != 0)
			ui.choice = std::clamp(ui.choice + step, 0, count - 1);
		if (given.l1)
			ui.choice = std::max(0, ui.choice - 10);
		if (given.r1)
			ui.choice = std::min(count - 1, ui.choice + 10);
		if (given.back)
			ui.picking = false;
		else if (given.accept)
		{
			B::set(ui.input, ui.choices[ui.choice]);
			ui.note = std::string(B::inputName(ui.input)) + ": " + B::actionName(ui.choices[ui.choice]);
			ui.picking = false;
		}
	}

	// ---- the dialog
	const B::Table now = B::current();
	const float w = 1500, h = 880, pad = 52, inner = w - 2 * pad;
	const ImVec2 at = dialogPanel(dl, w, h);
	const float x0 = at.x + pad;
	text(dl, bold(), 38, x0, at.y + pad - 6, col::text, "Mouse and keyboard bindings");
	text(dl, regular(), 23, x0, at.y + pad + 50, col::dim,
			"What the controller's buttons do when it is the Dreamcast's mouse and keyboard. The right stick is the mouse", inner);
	const float top = at.y + pad + 104;
	const float buttonsY = at.y + h - pad - 60 + 8;
	const float noteY = buttonsY - 20 - 34;
	const float paneH = noteY - 12 - top;
	const float leftW = 700, gap = 28;
	const float rx = x0 + leftW + gap, rw = inner - leftW - gap;
	const float rowH = 50;
	const int shown = std::max(4, (int)((paneH - 16) / rowH));

	// the inputs
	rect(dl, x0, top, leftW, paneH, col::rgba(0, 0, 0, 48), 16);
	if (!ui.picking)
		outline(dl, x0, top, leftW, paneH, col::accent, 16, 2.5f);
	{
		const int first = bindsWindow(ui.input, B::Inputs, shown);
		for (int n = 0; n < shown && first + n < B::Inputs; n++)
		{
			const int i = first + n;
			const float y = top + 8 + n * rowH;
			const bool on = i == ui.input;
			if (on)
				rect(dl, x0 + 8, y, leftW - 16, rowH - 4, ui.picking ? col::panelHi : col::accent, 12);
			text(dl, bold(), 24, x0 + 24, y + 11, on ? col::text : col::dim, B::inputName(i));
			text(dl, regular(), 23, x0 + 250, y + 12, on ? col::text : col::dim,
					fit(regular(), 23, B::actionName(now.action[i]), leftW - 250 - 24).c_str());
		}
	}

	// what it can do
	rect(dl, rx, top, rw, paneH, col::rgba(0, 0, 0, 48), 16);
	if (ui.picking)
		outline(dl, rx, top, rw, paneH, col::accent, 16, 2.5f);
	{
		const int count = (int)ui.choices.size();
		const int focus = ui.picking ? ui.choice : 0;
		const int first = bindsWindow(focus, count, shown);
		if (!ui.picking)
			text(dl, regular(), 23, rx + 28, top + 28, col::dim,
					"Cross picks what the input on the left does.\n\nTriangle: nothing\nSquare: the WASD layout\nOPTIONS: the arrow keys layout", rw - 56);
		else
			for (int n = 0; n < shown && first + n < count; n++)
			{
				const int i = first + n;
				const float y = top + 8 + n * rowH;
				const bool on = i == ui.choice;
				if (on)
					rect(dl, rx + 8, y, rw - 16, rowH - 4, col::accent, 12);
				text(dl, regular(), 24, rx + 24, y + 11, on ? col::text : col::dim,
						fit(regular(), 24, B::actionName(ui.choices[i]), rw - 48).c_str());
			}
	}

	if (!ui.note.empty())
		text(dl, regular(), 22, x0, noteY, col::text, ui.note.c_str(), inner);
	else
		text(dl, regular(), 22, x0, noteY, col::faint,
				"What has something here leaves the controller. The mouse and keyboard take their ports when a game starts", inner);
	float bx = x0;
	if (!ui.picking)
	{
		bx += dialogButton(dl, bx, buttonsY, Glyph::Cross, "Change", true) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::Triangle, "Nothing", false) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::Square, "WASD", false) + 20;
		bx += dialogButton(dl, bx, buttonsY, Glyph::Options, "Arrows", false) + 20;
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
