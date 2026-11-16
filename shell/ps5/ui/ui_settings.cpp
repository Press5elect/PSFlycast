/*
	PSFlyCast - the settings of the new interface: the table (ui_settings_table.inc, made by
	shell/ps5/tools/settings-table.py) and what is asked of it.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "ui.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace pui
{
namespace
{
#include "ui_settings_table.inc"

std::string lower(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}
}

bool Setting::has(char flag) const
{
	return std::strchr(flags, flag) != nullptr;
}

std::string Setting::valueText(int value) const
{
	switch (kind)
	{
	case Kind::Toggle:
		return value ? "On" : "Off";
	case Kind::Choice:
		return value >= 0 && value < (int)choices.size() ? choices[(size_t)value] : std::to_string(value);
	case Kind::Range:
		return std::to_string(value) + unit;
	default:
		return "";
	}
}

const std::vector<Setting>& settings()
{
	static const std::vector<Setting> all(std::begin(table), std::end(table));
	return all;
}

const std::vector<Area>& areas()
{
	static const std::vector<Area> all(std::begin(pui::areas_), std::end(pui::areas_));
	return all;
}

const Setting *find(const std::string& id)
{
	for (const Setting& s : settings())
		if (id == s.id)
			return &s;
	return nullptr;
}

std::vector<std::string> sections(int area)
{
	std::vector<std::string> out;
	for (const Setting& s : settings())
		if (s.area == area && !s.has('G') && std::find(out.begin(), out.end(), s.section) == out.end())
			out.push_back(s.section);
	return out;
}

std::vector<const Setting *> search(const std::string& query)
{
	std::vector<std::string> words;
	std::string word;
	for (const char c : lower(query) + " ")
	{
		if (c == ' ')
		{
			if (!word.empty())
				words.push_back(word);
			word.clear();
		}
		else
			word += c;
	}
	std::vector<const Setting *> byLabel, byText;
	if (words.empty())
		return byLabel;
	for (const Setting& s : settings())
	{
		if (s.has('G'))
			continue;
		const std::string label = lower(s.label);
		const std::string all = label + " " + lower(s.desc) + " " + lower(areas()[(size_t)s.area].name) + " " + lower(s.section);
		bool inAll = true, inLabel = true;
		for (const std::string& w : words)
		{
			inAll = inAll && all.find(w) != std::string::npos;
			inLabel = inLabel && label.find(w) != std::string::npos;
		}
		if (inLabel)
			byLabel.push_back(&s);
		else if (inAll)
			byText.push_back(&s);
	}
	byLabel.insert(byLabel.end(), byText.begin(), byText.end());
	return byLabel;
}
}
