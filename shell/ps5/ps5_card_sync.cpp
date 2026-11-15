// Crux2 - the memory cards kept the same on both sides: the engine (ps5_card_sync.h). PSFlyCast:
// from Crux2 f0efa95, with Io::is_card and the records' heading (ps5_card_sync.h says so).
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.

#include "ps5_card_sync.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <set>
#include <sstream>

namespace PS5CardSync
{
namespace
{
std::string Join(const std::string& folder, const std::string& name)
{
	if (folder.empty())
		return name;
	return (folder.back() == '/') ? folder + name : folder + "/" + name;
}

// "SLUS-21059.ps2" put away at a moment: "SLUS-21059 2026-10-19_19-42-07.ps2".
std::string AwayName(const std::string& name, time_t now)
{
	struct tm local = {};
	localtime_r(&now, &local);
	char when[32];
	std::strftime(when, sizeof(when), "%Y-%m-%d_%H-%M-%S", &local);
	const size_t dot = name.rfind('.');
	const std::string stem = (dot == std::string::npos) ? name : name.substr(0, dot);
	const std::string ext = (dot == std::string::npos) ? std::string() : name.substr(dot);
	return stem + " " + when + ext;
}

bool Copy(const Io& io, const std::string& from, const std::string& to, std::string* data_out = nullptr)
{
	std::string data;
	if (!io.read(from, &data))
		return false;
	if (!io.write(to, data))
		return false;
	if (data_out)
		*data_out = std::move(data);
	return true;
}
} // namespace

bool IsCard(const std::string& name)
{
	if (name.size() < 5 || name[0] == '.')
		return false;
	std::string ext = name.substr(name.size() - 4);
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return ext == ".ps2";
}

std::string FormatRecords(const Records& records)
{
	std::string out = "# PSFlyCast: the memory cards as they were after the last sync (name, size and time here, size and time there)\n";
	for (const auto& [name, r] : records)
	{
		char line[96];
		std::snprintf(line, sizeof(line), "\t%" PRIu64 "\t%" PRId64 "\t%" PRIu64 "\t%" PRId64 "\n", r.here_size, r.here_mtime, r.there_size,
			r.there_mtime);
		out += name;
		out += line;
	}
	return out;
}

Records ParseRecords(const std::string& text)
{
	Records records;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line))
	{
		if (line.empty() || line[0] == '#')
			continue;
		std::vector<std::string> fields;
		size_t start = 0;
		for (;;)
		{
			const size_t tab = line.find('\t', start);
			fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
			if (tab == std::string::npos)
				break;
			start = tab + 1;
		}
		if (fields.size() != 5 || fields[0].empty())
			continue;
		Record r;
		char* end = nullptr;
		r.here_size = std::strtoull(fields[1].c_str(), &end, 10);
		r.here_mtime = std::strtoll(fields[2].c_str(), &end, 10);
		r.there_size = std::strtoull(fields[3].c_str(), &end, 10);
		r.there_mtime = std::strtoll(fields[4].c_str(), &end, 10);
		records[fields[0]] = r;
	}
	return records;
}

Result Sync(const Io& io, const Folders& folders, Records* records, time_t now)
{
	Result result;
	std::vector<std::string> here_names, there_names;
	io.make_folder(folders.there);
	if (!io.list(folders.there, &there_names))
		return result;
	result.listed = true;
	io.list(folders.here, &here_names);

	const auto card = [&](const std::string& n) { return io.is_card ? io.is_card(n) : IsCard(n); };
	std::set<std::string> names;
	for (const std::string& n : here_names)
		if (card(n))
			names.insert(n);
	for (const std::string& n : there_names)
		if (card(n))
			names.insert(n);

	const auto remember = [&](const std::string& name) {
		const FileInfo h = io.stat(Join(folders.here, name));
		const FileInfo t = io.stat(Join(folders.there, name));
		(*records)[name] = Record{h.size, h.mtime, t.size, t.mtime};
	};
	const auto stopped = [&] {
		if (io.stop && io.stop())
		{
			result.stopped = true;
			return true;
		}
		return false;
	};

	for (const std::string& name : names)
	{
		const std::string here = Join(folders.here, name), there = Join(folders.there, name);
		const FileInfo h = io.stat(here), t = io.stat(there);
		const auto found = records->find(name);
		const Record* rec = (found == records->end()) ? nullptr : &found->second;

		if (h.exists && !t.exists)
		{
			if (Copy(io, here, there))
			{
				result.sent++;
				result.lines.push_back(name + ": sent (not there)");
				remember(name);
			}
			else
			{
				result.failed++;
				result.lines.push_back(name + ": could not be sent");
			}
			continue;
		}
		if (!h.exists && t.exists)
		{
			if (stopped())
				break;
			if (Copy(io, there, here))
			{
				result.brought++;
				result.lines.push_back(name + ": brought here (not here)");
				remember(name);
			}
			else
			{
				result.failed++;
				result.lines.push_back(name + ": could not be brought here");
			}
			continue;
		}
		if (!h.exists && !t.exists)
			continue;

		bool here_changed = !rec || h.size != rec->here_size || h.mtime != rec->here_mtime;
		bool there_changed = !rec || t.size != rec->there_size || t.mtime != rec->there_mtime;
		if (!rec)
		{
			// Never synced: the same bytes need nothing but remembering.
			std::string a, b;
			if (io.read(here, &a) && io.read(there, &b) && a == b)
			{
				result.unchanged++;
				remember(name);
				continue;
			}
		}
		if (!here_changed && !there_changed)
		{
			result.unchanged++;
			continue;
		}
		if (here_changed && !there_changed)
		{
			if (Copy(io, here, there))
			{
				result.sent++;
				result.lines.push_back(name + ": sent (changed here)");
				remember(name);
			}
			else
			{
				result.failed++;
				result.lines.push_back(name + ": could not be sent");
			}
			continue;
		}
		if (!here_changed && there_changed)
		{
			if (stopped())
				break;
			if (Copy(io, there, here))
			{
				result.brought++;
				result.lines.push_back(name + ": brought here (changed there)");
				remember(name);
			}
			else
			{
				result.failed++;
				result.lines.push_back(name + ": could not be brought here");
			}
			continue;
		}

		// Changed on both sides: the newer is kept on both, the other put away on its side first.
		const bool here_wins = (h.mtime >= t.mtime);
		const std::string& loser = here_wins ? there : here;
		const std::string& winner = here_wins ? here : there;
		const std::string& older = here_wins ? folders.older_there : folders.older_here;
		if (!here_wins && stopped())
			break;
		io.make_folder(older);
		if (!Copy(io, loser, Join(older, AwayName(name, now))))
		{
			result.failed++;
			result.lines.push_back(name + ": changed on both sides, and the older could not be put away: left as it is");
			continue;
		}
		if (Copy(io, winner, loser))
		{
			result.both_changed++;
			here_wins ? result.sent++ : result.brought++;
			result.lines.push_back(name + (here_wins ? ": changed on both sides; the console's is newer and was sent, the other put away" :
												  ": changed on both sides; the other side's is newer and was brought here, the console's put away"));
			remember(name);
		}
		else
		{
			result.failed++;
			result.lines.push_back(name + ": changed on both sides, and the newer could not be copied");
		}
	}
	return result;
}

std::string Summary(const Result& result, const std::string& place_name)
{
	if (!result.listed)
		return "The memory cards were not synced: " + place_name + " did not answer.";
	std::string text = "Memory cards synced with " + place_name;
	if (result.sent == 0 && result.brought == 0 && result.failed == 0)
		return text + ": nothing had changed.";
	text += ":";
	std::string parts;
	const auto add = [&](int n, const char* what) {
		if (n == 0)
			return;
		parts += (parts.empty() ? " " : ", ") + std::to_string(n) + " " + what;
	};
	add(result.sent, "sent");
	add(result.brought, "brought here");
	add(result.both_changed, "changed on both sides (the older one kept aside)");
	add(result.failed, "could not be copied");
	text += parts;
	if (result.stopped)
		text += " (stopped: a game started)";
	return text + ".";
}
} // namespace PS5CardSync
