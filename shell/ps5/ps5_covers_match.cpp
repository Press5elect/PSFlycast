/*
	PSFlyCast - matching a game's file to a picture of the libretro thumbnails collection
	(ps5_covers_match.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ps5_covers_match.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace ps5::covers::match
{
std::string regionName(std::string token)
{
	for (char& c : token)
		c = (char)toupper((unsigned char)c);
	if (token == "US" || token == "U" || token == "USA" || token == "NTSC-U" || token == "NTSC")
		return "USA";
	if (token == "EU" || token == "EUR" || token == "E" || token == "EUROPE" || token == "PAL")
		return "Europe";
	if (token == "JP" || token == "JPN" || token == "J" || token == "JAPAN" || token == "NTSC-J")
		return "Japan";
	return "";
}

std::string cleaned(const std::string& name)
{
	std::string out;
	int depth = 0;
	for (char c : name)
	{
		if (c == '[')
			depth++;
		else if (c == ']' && depth > 0)
			depth--;
		else if (depth == 0)
			out += c;
	}
	// (US) and its kind.
	std::string result;
	for (size_t i = 0; i < out.size(); )
	{
		if (out[i] == '(')
		{
			const size_t close = out.find(')', i);
			if (close != std::string::npos)
			{
				const std::string region = regionName(out.substr(i + 1, close - i - 1));
				result += region.empty() ? out.substr(i, close - i + 1) : "(" + region + ")";
				i = close + 1;
				continue;
			}
		}
		result += out[i++];
	}
	// One space between words, none at the ends, and one before a bracket.
	std::string tidy;
	for (char c : result)
	{
		if (c == '_')
			c = ' ';
		if (c == '(' && !tidy.empty() && tidy.back() != ' ')
			tidy += ' ';
		if (c == ' ' && (tidy.empty() || tidy.back() == ' '))
			continue;
		tidy += c;
	}
	while (!tidy.empty() && tidy.back() == ' ')
		tidy.pop_back();
	return tidy;
}

std::string thumbnailName(const std::string& name)
{
	std::string out = name;
	for (char& c : out)
		if (strchr("&*/:`<>?\\|\"", c) != nullptr)
			c = '_';
	return out;
}

std::vector<std::string> namesFor(const std::string& base, const std::string& title)
{
	std::vector<std::string> names;
	auto add = [&](const std::string& name) {
		if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
			names.push_back(name);
	};
	auto withVariants = [&](const std::string& name) {
		if (name.empty())
			return;
		add(name);
		if (name.find('(') == std::string::npos)
			for (const char *region : { " (USA)", " (Europe)", " (Japan)" })
				add(name + region);
		const size_t disc = name.find(" (Disc ");
		if (disc != std::string::npos)
		{
			const size_t end = name.find(')', disc);
			if (end != std::string::npos)
				add(name.substr(0, disc) + name.substr(end + 1));
		}
	};
	withVariants(base);
	withVariants(cleaned(base));
	withVariants(cleaned(title));
	return names;
}

namespace
{
std::string lowered(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

// The words in brackets of a name, lower case, regions spelled out: "(USA) (Disc 1) (Rev A)" is
// usa, disc 1, rev a.
std::vector<std::string> tags(const std::string& name)
{
	std::vector<std::string> out;
	for (size_t i = 0; i < name.size(); i++)
	{
		const char open = name[i];
		if (open != '(' && open != '[')
			continue;
		const size_t close = name.find(open == '(' ? ')' : ']', i);
		if (close == std::string::npos)
			break;
		std::string inside = name.substr(i + 1, close - i - 1);
		const std::string region = regionName(inside);
		out.push_back(lowered(region.empty() ? inside : region));
		i = close;
	}
	return out;
}

bool isRegion(const std::string& tag)
{
	return tag == "usa" || tag == "europe" || tag == "japan";
}

bool isDisc(const std::string& tag)
{
	return tag.rfind("disc ", 0) == 0;
}

bool has(const std::vector<std::string>& list, const std::string& one)
{
	return std::find(list.begin(), list.end(), one) != list.end();
}

// How well a picture's tags fit those of the name looked for: the region first, the disc, then
// as few tags the name does not have as can be (a revision, a demo, languages).
int fitOf(const std::vector<std::string>& want, const std::vector<std::string>& have)
{
	int score = 0;
	std::string wantRegion, wantDisc;
	for (const std::string& t : want)
	{
		if (isRegion(t) && wantRegion.empty())
			wantRegion = t;
		if (isDisc(t))
			wantDisc = t;
	}
	if (!wantRegion.empty())
		score += has(have, wantRegion) ? 100 : 0;
	else
		score += has(have, "usa") ? 30 : has(have, "europe") ? 20 : has(have, "japan") ? 10 : 0;
	for (const std::string& t : have)
	{
		if (isRegion(t))
			continue;
		if (isDisc(t))
			score += t == wantDisc ? 50 : wantDisc.empty() ? -5 : -50;
		else if (!has(want, t))
			score -= 3;
	}
	return score;
}
}

std::string key(const std::string& name)
{
	std::string words;
	int depth = 0;
	for (size_t i = 0; i < name.size(); i++)
	{
		const char c = name[i];
		if (c == '(' || c == '[')
			depth++;
		else if ((c == ')' || c == ']') && depth > 0)
			depth--;
		else if (depth == 0)
		{
			if (c == '&')
				words += " and ";
			else if (isalnum((unsigned char)c))
				words += (char)tolower((unsigned char)c);
			else if (c == '\'')
				continue;		// "Don't" is "dont"
			else
				words += ' ';
		}
	}
	std::string out;
	size_t at = 0;
	while (at < words.size())
	{
		while (at < words.size() && words[at] == ' ')
			at++;
		size_t end = at;
		while (end < words.size() && words[end] != ' ')
			end++;
		const std::string word = words.substr(at, end - at);
		if (!word.empty() && word != "the")
			out += word;
		at = end;
	}
	return out;
}

std::string loose(const std::vector<std::string>& index, const std::vector<std::string>& wanted)
{
	for (const std::string& want : wanted)
	{
		const std::string k = key(want);
		if (k.size() < 2)
			continue;
		const std::vector<std::string> wantTags = tags(want);
		std::string best;
		int bestScore = 0;
		for (const std::string& name : index)
		{
			if (key(name) != k)
				continue;
			const int score = fitOf(wantTags, tags(name));
			if (best.empty() || score > bestScore)
			{
				best = name;
				bestScore = score;
			}
		}
		if (!best.empty())
			return best;
	}
	return "";
}

std::vector<std::string> parseIndex(const std::string& html)
{
	std::vector<std::string> out;
	const std::string open = "<a href=\"";
	for (size_t at = html.find(open); at != std::string::npos; at = html.find(open, at + 1))
	{
		const size_t from = at + open.size(), end = html.find('"', from);
		if (end == std::string::npos)
			break;
		const std::string href = html.substr(from, end - from);
		if (href.size() <= 4 || href.compare(href.size() - 4, 4, ".png") != 0 || href[0] == '/' || href[0] == '?'
				|| href.find('/') != std::string::npos)
			continue;
		std::string name;
		for (size_t i = 0; i < href.size() - 4; i++)
		{
			if (href[i] == '%' && i + 2 < href.size() - 4 + 2 && isxdigit((unsigned char)href[i + 1]) && isxdigit((unsigned char)href[i + 2]))
			{
				name += (char)strtol(href.substr(i + 1, 2).c_str(), nullptr, 16);
				i += 2;
			}
			else if (href.compare(i, 5, "&amp;") == 0)
			{
				name += '&';
				i += 4;
			}
			else
				name += href[i];
		}
		out.push_back(name);
	}
	return out;
}

std::map<std::string, int64_t> parseNotFound(const std::string& text)
{
	std::map<std::string, int64_t> out;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find('\n', at);
		if (end == std::string::npos)
			end = text.size();
		std::string line = text.substr(at, end - at);
		at = end + 1;
		while (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;
		const size_t tab = line.rfind('\t');
		const std::string base = tab == std::string::npos ? line : line.substr(0, tab);
		const int64_t when = tab == std::string::npos ? 0 : strtoll(line.c_str() + tab + 1, nullptr, 10);
		// The latest line for a game is the one that counts.
		int64_t& kept = out[base];
		kept = std::max(kept, when);
	}
	return out;
}

bool lapsed(int64_t when, int64_t now)
{
	return when <= 0 || now - when >= NotFoundLapse;
}
}
