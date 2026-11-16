/*
	PSFlyCast - how a game's file is matched to a picture of the libretro thumbnails collection
	(ps5_covers.cpp), apart so that a PC can test it (shell/ps5/tests/covers).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps5::covers::match
{
// A region as the collection spells it ("USA", "Europe", "Japan") for one in short ("US", "PAL");
// empty when the word is not a region.
std::string regionName(std::string token);
// A file name as the collection names its games: [tags] out, regions spelled out, spaces tidied.
// "[GDI] Crazy Taxi (US)[10S 51035]" is "Crazy Taxi (USA)".
std::string cleaned(const std::string& name);
// The collection's file name for a game name: its reserved characters are '_'.
std::string thumbnailName(const std::string& name);
// The names to ask for exactly, in order: the file's own, the file's tidied, the game's title;
// each with the three regions when it has none, and without its disc number when it has one.
std::vector<std::string> namesFor(const std::string& base, const std::string& title);

// What a name is without its tags, case and punctuation: "Capcom vs. SNK 2 (USA)" is
// "capcomvssnk2". "&" counts as "and"; the word "the" is left out ("Legend, The" is "legend").
std::string key(const std::string& name);
// The picture of the collection (a name of its index, without ".png") for the first of wanted
// that has the same key as one: the one whose region, disc and other tags fit best. Empty: none.
std::string loose(const std::vector<std::string>& index, const std::vector<std::string>& wanted);
// The names of a folder listing of the collection's server (thumbnails.libretro.com, an Apache
// index): every "<name>.png" it links to, decoded, without ".png".
std::vector<std::string> parseIndex(const std::string& html);

// The games no picture was found for, and when (covers/not-found-2.txt: "<file name>\t<time>").
// A line without a time is of the builds before loose matching and the lapse: it is lapsed.
std::map<std::string, int64_t> parseNotFound(const std::string& text);
// A game is asked for again once this long has passed since it was not found.
constexpr int64_t NotFoundLapse = 30 * 24 * 3600;
bool lapsed(int64_t when, int64_t now);
}
