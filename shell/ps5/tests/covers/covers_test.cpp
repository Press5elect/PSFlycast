/*
	PSFlyCast - the cover matching (ps5_covers_match.cpp) on a PC. The names are made up, in the
	forms the libretro collection's names take (Redump's: regions, languages, discs, revisions).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "ps5_covers_match.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace ps5::covers::match;

static int checks = 0, failures = 0;

static void expect(bool ok, const char *what, const std::string& got = "")
{
	checks++;
	if (!ok)
	{
		failures++;
		std::printf("FAIL: %s%s%s\n", what, got.empty() ? "" : ": got ", got.c_str());
	}
}

int main()
{
	// The collection, as its index lists it.
	const std::vector<std::string> index = {
		"Harbor Drift (Europe) (En,Ja,Fr,De,Es)", "Harbor Drift (Japan)", "Harbor Drift (USA) (En,Ja,Fr,De,Es) (Rev A)",
		"Harbor Drift (USA)", "Harbor Drift - Limited Edition (USA)", "Harbor Drift 2 (USA) (En,Ja,Fr,De,Es)",
		"Neon Courier (Europe) (En,Fr,De,Es) (Disc 1)", "Neon Courier (Europe) (En,Fr,De,Es) (Disc 2)",
		"Neon Courier (USA) (Disc 1)", "Neon Courier (USA) (Disc 2)", "Neon Courier (USA)",
		"Orbit vs. Arena 2 - Return of the Ring (Japan)", "Orbit vs. Arena 2 - Return of the Ring (USA)",
		"Legend of Kiln - Soul Furnace (Europe)", "Legend of Kiln - Soul Furnace (USA)",
		"Polar & Run (Japan) (Taikenban)", "Polar & Run (Japan)",
		"Tower, The (Europe)",
	};

	// The exact names asked for first, as before.
	expect(cleaned("[GDI] Harbor Drift (US)[10S 51035]") == "Harbor Drift (USA)", "cleaned", cleaned("[GDI] Harbor Drift (US)[10S 51035]"));
	expect(thumbnailName("Polar & Run: Two") == "Polar _ Run_ Two", "thumbnailName");
	{
		const std::vector<std::string> n = namesFor("Neon Courier (Disc 1)", "NEON COURIER");
		expect(n.size() == 6 && n[0] == "Neon Courier (Disc 1)" && n[1] == "Neon Courier" && n[2] == "NEON COURIER"
				&& n[3] == "NEON COURIER (USA)", "namesFor: the file, without its disc, the title with each region",
				n.size() > 2 ? n[2] : "");
	}

	// The key: no tags, no case, no punctuation, no "the".
	expect(key("Orbit vs. Arena 2 - Return of the Ring (USA)") == "orbitvsarena2returnofring", "key", key("Orbit vs. Arena 2 - Return of the Ring (USA)"));
	expect(key("ORBIT VS ARENA 2: RETURN OF THE RING") == key("Orbit vs. Arena 2 - Return of the Ring (Japan)"), "key: a disc header's title");
	expect(key("Polar and Run") == key("Polar & Run (Japan)"), "key: & is and");
	expect(key("The Tower") == key("Tower, The (Europe)"), "key: the article either side");
	expect(key("Harbor Drift 2") != key("Harbor Drift"), "key: a sequel is another game");

	// Loose: the region asked for, else USA, then Europe, then Japan; fewest other tags.
	expect(loose(index, { "harbor drift (usa)" }) == "Harbor Drift (USA)", "loose: case", loose(index, { "harbor drift (usa)" }));
	expect(loose(index, { "HARBOR DRIFT" }) == "Harbor Drift (USA)", "loose: no region: USA, fewest tags", loose(index, { "HARBOR DRIFT" }));
	expect(loose(index, { "Harbor-Drift (Japan)" }) == "Harbor Drift (Japan)", "loose: the region asked for", loose(index, { "Harbor-Drift (Japan)" }));
	expect(loose(index, { "Harbor Drift (Europe)" }) == "Harbor Drift (Europe) (En,Ja,Fr,De,Es)", "loose: Europe with its languages",
			loose(index, { "Harbor Drift (Europe)" }));
	expect(loose(index, { "Neon Courier (USA) (Disc 2)" }) == "Neon Courier (USA) (Disc 2)", "loose: the disc", loose(index, { "Neon Courier (USA) (Disc 2)" }));
	expect(loose(index, { "neon courier" }) == "Neon Courier (USA)", "loose: no disc asked: the set's own picture", loose(index, { "neon courier" }));
	expect(loose(index, { "Neon Courier (Europe) (Disc 2)" }) == "Neon Courier (Europe) (En,Fr,De,Es) (Disc 2)", "loose: region and disc",
			loose(index, { "Neon Courier (Europe) (Disc 2)" }));
	expect(loose(index, { "ORBIT VS ARENA 2: RETURN OF THE RING" }) == "Orbit vs. Arena 2 - Return of the Ring (USA)", "loose: a header's title",
			loose(index, { "ORBIT VS ARENA 2: RETURN OF THE RING" }));
	expect(loose(index, { "Polar and Run (JP)" }) == "Polar & Run (Japan)", "loose: & and a short region, no demo",
			loose(index, { "Polar and Run (JP)" }));
	expect(loose(index, { "Legend of Kiln Soul Furnace" }) == "Legend of Kiln - Soul Furnace (USA)", "loose: a missing dash",
			loose(index, { "Legend of Kiln Soul Furnace" }));
	expect(loose(index, { "Harbor Drifts" }).empty(), "loose: another word is not a match");
	expect(loose(index, { "Unknown Game", "Tower (Europe)" }) == "Tower, The (Europe)", "loose: the next name wanted",
			loose(index, { "Unknown Game", "Tower (Europe)" }));
	expect(loose(index, { "T" }).empty(), "loose: too short to match");

	// The server's index.
	{
		const std::string html = "<tr><td><a href=\"/Sega%20-%20Dreamcast/\">Parent Directory</a></td></tr>"
				"<tr><td><a href=\"?C=N;O=D\">Name</a></td></tr>"
				"<tr><td><a href=\"Polar%20&amp;%20Run%20(Japan).png\">Polar &amp; Run (Japan).png</a></td></tr>"
				"<tr><td><a href=\"Harbor%20Drift%20(USA).png\">Harbor Drift (USA).png</a></td></tr>"
				"<tr><td><a href=\"notes.txt\">notes.txt</a></td></tr>";
		const std::vector<std::string> names = parseIndex(html);
		expect(names.size() == 2 && names[0] == "Polar & Run (Japan)" && names[1] == "Harbor Drift (USA)", "parseIndex",
				names.empty() ? "" : names[0]);
	}

	// Not found, and when.
	{
		const auto nf = parseNotFound("Old Game\nNew Game\t1000000\r\nNew Game\t2000000\n\n");
		expect(nf.size() == 2 && nf.at("Old Game") == 0 && nf.at("New Game") == 2000000, "parseNotFound");
		expect(lapsed(nf.at("Old Game"), 2000000), "a line of the builds before lapses at once");
		expect(!lapsed(2000000, 2000000 + NotFoundLapse - 1), "not lapsed before 30 days");
		expect(lapsed(2000000, 2000000 + NotFoundLapse), "lapsed after 30 days");
	}

	std::printf("covers: %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
