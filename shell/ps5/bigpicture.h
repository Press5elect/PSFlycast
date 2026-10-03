/*
	PSFlyCast - a Big Picture style, controller-only interface.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Replaces Flycast's desktop screens on the console: the game library, the
	loading screen, the in-game quick menu, settings and disc selection. Everything is drawn for a
	TV at any resolution (one unit is 1/1080 of the screen height) and driven
	by the DualSense directly, not through the emulated controller.
*/
#pragma once
#include <string>

struct ImGuiIO;
class GameScanner;
class Boxart;

namespace bigpicture
{

void library(bool selectDisk);	// GuiState::Main, GuiState::SelectDisk
// The library shows the start-up animation once, the first time it is on;
// this has it shown again.
void showSplash();
void quickMenu();				// GuiState::Commands
void settings();				// GuiState::Settings
// GuiState::Loading: the game, what the loader is doing (`label`, or for a
// network game what its share is doing) and how far it is, 0..1. True when the
// user asks to cancel; `cancelling` is true from then until the loader stops.
bool loading(const char *label, float progress, bool cancelling);
// The load was cancelled by the user; it ended (the game runs, or it failed,
// or the cancel is done); it failed: the message to show for `why`.
void loadCancelled();
void loadEnded();
std::string loadFailed(const char *why);
// Dark, rounded ImGui style for Flycast's own dialogs (errors, loading).
void applyTheme();
// Gives ImGui's keyboard-less navigation the DualSense, for those dialogs.
void feedNav(ImGuiIO& io);
// Called when a game starts, to keep the recently played list.
void gameStarted(const std::string& path);

// Provided by core/ui/gui.cpp.
GameScanner& scanner();
Boxart& boxart();
void saveState();

}
