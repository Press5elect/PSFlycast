/*
	PSFlyCast - RetroAchievements in the interface: a part of bigpicture.cpp,
	which includes it (its helpers are that file's).

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The achievements themselves are Flycast's (core/achievements, on the
	rcheevos library): it signs in, recognises the game by its hash, watches
	the game's memory and tells the site. Here are the account's dialog, in
	Settings > Achievements, and what the Settings say about it. The quick
	menu's page with a game's achievements is with the quick menu.

	Only the mode the site calls softcore is offered: save states, cheats,
	rewind and fast forward stay as they are. ps5_main.cpp turns the other
	off at every start.
*/

namespace
{

struct AccountUi
{
	bool open = false;
	int field = 0;				// 0 the user name, 1 the password, 2 "Sign in"
	std::string user;
	std::string password;		// until it is sent; never saved
	std::future<void> login;	// the sign-in under way
	std::string before;			// the user name the settings had until it began
	std::string note;			// what came of it
	bool noteBad = false;
	double askedAt = -1;		// when the saved sign-in was first seen to be tried
} accountUi;

// Nothing of a password stays in memory that was freed.
void wipe(std::string& secret)
{
	std::fill(secret.begin(), secret.end(), '\0');
	secret.clear();
}

bool signingIn()
{
	return accountUi.login.valid() && accountUi.login.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

// A sign-in that has ended: what came of it.
void signInEnded()
{
	if (!accountUi.login.valid() || signingIn())
		return;
	try {
		accountUi.login.get();
		accountUi.note.clear();
	} catch (const std::exception& e) {
		accountUi.note = std::string("Not signed in: ") + e.what();
		accountUi.noteBad = true;
		accountUi.field = 1;
		// The name goes with the key the site gave: the one that was saved stays.
		config::AchievementsUserName.set(accountUi.before);
	} catch (...) {
		accountUi.note = "Not signed in";
		accountUi.noteBad = true;
		config::AchievementsUserName.set(accountUi.before);
	}
}

}

// What the Settings show for the account.
std::string accountText()
{
	if (!config::EnableAchievements)
		return "Off";
	// The client is made here, the first time: it signs in by itself with the
	// key the site gave the last time.
	achievements::init();
	signInEnded();
	if (achievements::isLoggedOn())
		return "Signed in as " + config::AchievementsUserName.get();
	if (signingIn())
		return "Signing in...";
	if (!config::AchievementsToken.get().empty())
	{
		// The saved sign-in is tried once, when the client is made. Nothing
		// says when it has failed (the console is offline, the key is no
		// longer good): after a while it is taken to have.
		if (accountUi.askedAt < 0)
			accountUi.askedAt = timeNow;
		if (timeNow - accountUi.askedAt < 20)
			return "Signing in as " + config::AchievementsUserName.get() + "...";
		return "Not signed in: the saved sign-in did not work";
	}
	return "Not signed in";
}

void openAccount()
{
	wipe(accountUi.password);
	accountUi.open = true;
	accountUi.user = config::AchievementsUserName.get();
	accountUi.field = accountUi.user.empty() ? 0 : 1;
	// What a sign-in that failed after the dialog was closed came to stays to be read.
	if (!signingIn() && !accountUi.noteBad)
		accountUi.note.clear();
}

// What the console's keyboard gave for one of the dialog's two fields.
void accountTyped(int purpose, const std::string& typed)
{
	if (purpose == KeyboardRaUser)
	{
		accountUi.user = trimmed(typed);
		if (!accountUi.user.empty())
			accountUi.field = 1;
	}
	else
	{
		wipe(accountUi.password);
		accountUi.password = typed;
		ps5::ime::forget();
		if (!accountUi.password.empty())
			accountUi.field = 2;
	}
	accountUi.note.clear();
}

void accountDialog(ImDrawList *dl, const Input& given)
{
	AccountUi& ui = accountUi;
	signInEnded();
	const bool signedIn = achievements::isLoggedOn();
	const bool busy = signingIn();

	if (given.back)
	{
		// A sign-in under way goes on: the Settings say how it ends.
		wipe(ui.password);
		ui.open = false;
	}
	else if (signedIn)
	{
		if (given.triangle)
		{
			achievements::logout();
			ui.note = "Signed out";
			ui.noteBad = false;
			ui.field = 1;
			ui.askedAt = -1;
		}
	}
	else if (busy)
	{
		if (given.accept || given.triangle)
			ps5::sound::cue(ps5::sound::Cue::Refuse);
	}
	else
	{
		if (given.up && ui.field > 0)
			ui.field--;
		if (given.down && ui.field < 2)
			ui.field++;
		if (given.accept && ui.field == 0)
			openKeyboard(KeyboardRaUser, "RetroAchievements user name", "Your user name on retroachievements.org", ui.user, 60,
					ps5::ime::Kind::Name);
		else if (given.accept && ui.field == 1)
		{
			if (openKeyboard(KeyboardRaPassword, "RetroAchievements password", "It is sent to the site and not kept", "", 120,
					ps5::ime::Kind::Password) && ps5::ime::plain())
				// The console would not open its keyboard for a password: the plain one is up.
				os_notify("The keyboard shows the password as you type it", 6000, "This console did not open its password keyboard");
		}
		else if (given.accept && (ui.user.empty() || ui.password.empty()))
		{
			ps5::sound::cue(ps5::sound::Cue::Refuse);
			ui.note = ui.user.empty() ? "Enter your user name first" : "Enter your password first";
			ui.noteBad = true;
			ui.field = ui.user.empty() ? 0 : 1;
		}
		else if (given.accept)
		{
			// Flycast says who signed in by the name in the settings: set
			// now, and put back if the site refuses.
			ui.before = config::AchievementsUserName.get();
			config::AchievementsUserName.set(ui.user);
			ui.login = achievements::login(ui.user.c_str(), ui.password.c_str());
			wipe(ui.password);
			ui.note.clear();
			ui.noteBad = false;
		}
	}

	const float w = 1000, h = signedIn ? 380 : 590, pad = 52;
	const ImVec2 at = dialogPanel(dl, w, h);
	text(dl, bold(), 38, at.x + pad, at.y + pad - 6, col::text, "RetroAchievements account");
	text(dl, regular(), 23, at.x + pad, at.y + pad + 50, col::dim,
			signedIn ? "Achievements are earned in the games the site has them for"
			: "Your account on retroachievements.org. The password is sent to the site and not kept");

	float y = at.y + 150;
	if (signedIn)
	{
		const std::string who = ICON_FA_CIRCLE_CHECK "   Signed in as " + config::AchievementsUserName.get();
		text(dl, bold(), 30, at.x + pad, y + 8, col::good, fit(bold(), 30, who, w - 2 * pad).c_str());
		y += 70;
	}
	else
	{
		// The two fields and the button, the one focused lit.
		const char *labels[3] = { "User name", "Password", "Sign in" };
		for (int i = 0; i < 3; i++)
		{
			const bool on = i == ui.field && !busy;
			const float rowH = 84;
			rect(dl, at.x + pad, y, w - 2 * pad, rowH - 12, on ? alpha(col::accent, 0.22f) : col::panelHi, 14);
			if (on)
				outline(dl, at.x + pad, y, w - 2 * pad, rowH - 12, col::accent, 14, 3);
			const float cy = y + (rowH - 12) / 2;
			if (i == 2)
			{
				const std::string label = busy ? "Signing in..." : std::string(ICON_FA_RIGHT_TO_BRACKET "   ") + labels[i];
				const ImVec2 ls = textSize(bold(), 26, label.c_str());
				text(dl, bold(), 26, at.x + (w - ls.x) / 2, cy - ls.y / 2, on || busy ? col::text : col::dim, label.c_str());
			}
			else
			{
				const ImVec2 ls = textSize(bold(), 26, labels[i]);
				text(dl, bold(), 26, at.x + pad + 28, cy - ls.y / 2, on ? col::text : col::dim, labels[i]);
				const float right = at.x + w - pad - 28;
				if (i == 1 && !ui.password.empty())
				{
					// A dot for each character, as far as they fit.
					const int dots = (int)std::min<size_t>(ui.password.size(), 24);
					for (int n = 0; n < dots; n++)
						dl->AddCircleFilled(V(right - 8 - n * 22, cy), 6 * S, col::text);
				}
				else
				{
					const bool empty = i == 1 || ui.user.empty();
					const std::string value = fit(regular(), 26, empty ? "Not entered" : ui.user, w - 2 * pad - 320);
					const ImVec2 vs = textSize(regular(), 26, value.c_str());
					text(dl, regular(), 26, right - vs.x, cy - vs.y / 2, empty ? col::faint : col::text, value.c_str());
				}
			}
			y += rowH;
		}
	}
	std::string note = ui.note;
	bool bad = ui.noteBad;
	if (note.empty() && ps5::elevated && !signedIn)
	{
		// Outside its sandbox the console can refuse the title a secure connection.
		note = "With USB drives on, the console may refuse the connection to the site";
		bad = false;
	}
	if (!note.empty())
		text(dl, regular(), 22, at.x + pad, y + 6, bad ? col::warm : note == ui.note ? col::text : col::faint,
				fit(regular(), 22, note, w - 2 * pad).c_str());

	float bx = at.x + pad;
	const float buttonsY = at.y + h - pad - 60 + 8;
	if (signedIn)
	{
		bx += dialogButton(dl, bx, buttonsY, Glyph::Circle, "Close", true) + 20;
		dialogButton(dl, bx, buttonsY, Glyph::Triangle, "Sign out", false);
	}
	else if (busy)
		dialogButton(dl, bx, buttonsY, Glyph::Circle, "Close", false);
	else
	{
		bx += dialogButton(dl, bx, buttonsY, Glyph::Cross, ui.field == 2 ? "Sign in" : "Type it", true) + 20;
		dialogButton(dl, bx, buttonsY, Glyph::Circle, "Cancel", false);
	}
}
