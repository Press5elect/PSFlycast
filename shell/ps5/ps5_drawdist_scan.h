/*
	PSFlyCast - finding a game's object tables in its memory.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Sonic Adventure and Sonic Adventure 2 decide by distance which of a
	stage's objects exist: each kind of object has an entry in a table, with
	the distance (squared) inside which it is made and beyond which it is
	removed. That is why rings, enemies and item boxes appear a short way in
	front of the player, on a Dreamcast as here. The PC version's "Higher Draw
	Distance" mod (github.com/kellsnc/sa2b-limit-break, by Kell and
	SonicFreak94) multiplies those distances; its source and the mod loader's
	headers give the table's layout:

	  Sonic Adventure 2, 16 bytes an entry
	    u8 load flags, u8 list, s16 object flags, float distance,
	    pointer to the object's function, pointer to its name
	  Sonic Adventure, 20 bytes an entry
	    u8 flags, u8 list, s16 "use distance", float distance, u32,
	    pointer to the object's function, pointer to its name

	The Dreamcast programs are not the PC ones and their addresses are not
	known here, so the tables are looked for by what they are: a run of
	entries whose two pointers point into the program, the second at a short
	printable name. A distance that has been multiplied is marked in its four
	lowest bits (a change of a millionth), so it is not multiplied again:
	not by the next pass, and not after a save state that was made with it
	multiplied is loaded.

	Nothing here knows about the emulator: ps5_drawdist.cpp gives it the
	console's memory, and a test on the build machine a made-up one.
*/
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ps5::drawdist
{

struct Table
{
	uint32_t address;		// in the console's address space
	int stride;				// 16 or 20
	int entries;
	int scaled;				// distances multiplied by this pass
	std::string names;		// its first few objects, for the log
};

class Scanner
{
public:
	// ram: the console's main memory, of size bytes, which the program sees
	// from base (8C000000). factor: what a squared distance is multiplied by.
	Scanner(uint8_t *ram, uint32_t size, uint32_t base, float factor)
		: ram(ram), size(size), base(base), factor(factor) {}

	// Looks for tables that begin in [from, from + length) and multiplies
	// their distances. Adds the tables in which it changed something to out.
	void scan(uint32_t from, uint32_t length, std::vector<Table>& out)
	{
		uint32_t end = from + length < size ? from + length : size;
		for (uint32_t at = from & ~3u; at < end; )
		{
			// Nearly all of memory is passed here: neither word where an
			// entry's name would be points into the program.
			if (at + 20 > size || (!nameStart(read32(at + 12)) && !nameStart(read32(at + 16))))
			{
				at += 4;
				continue;
			}
			const int run16 = run(at, 16), run20 = run(at, 20);
			const int stride = run16 >= run20 ? 16 : 20;
			const int entries = stride == 16 ? run16 : run20;
			if (entries < MinEntries)
			{
				at += 4;
				continue;
			}
			Table table{ base + at, stride, entries, 0, "" };
			for (int i = 0; i < entries; i++)
			{
				const uint32_t entry = at + i * stride;
				if (i < 4)
					table.names += (i ? ", " : "") + name(read32(entry + stride - 4));
				// Sonic Adventure: an entry that does not use its distance has the game's own.
				if (stride == 20 && read16(entry + 2) == 0)
					continue;
				uint32_t bits = read32(entry + 4);
				float distance;
				memcpy(&distance, &bits, 4);
				if (distance <= 0 || (bits & MarkMask) == Mark)
					continue;
				distance *= factor;
				memcpy(&bits, &distance, 4);
				bits = (bits & ~MarkMask) | Mark;
				memcpy(ram + entry + 4, &bits, 4);
				table.scaled++;
			}
			if (table.scaled > 0)
				out.push_back(table);
			at += entries * stride;
		}
	}

	static constexpr int MinEntries = 6;
	static constexpr uint32_t MarkMask = 0xf, Mark = 0xb;

private:
	uint32_t read32(uint32_t at) const { uint32_t v; memcpy(&v, ram + at, 4); return v; }
	uint16_t read16(uint32_t at) const { uint16_t v; memcpy(&v, ram + at, 2); return v; }

	// A pointer into the program as it is loaded: 8C010000 and up, in memory.
	bool pointer(uint32_t p) const { return p >= base + 0x10000 && p < base + size; }

	// What a pointer to a name is, at the least: into the program, at a printable character.
	bool nameStart(uint32_t p) const
	{
		return pointer(p) && ram[p - base] >= 0x20 && ram[p - base] <= 0x7e;
	}

	// A name: one to thirty-one printable characters and a zero.
	bool named(uint32_t p) const
	{
		if (!pointer(p))
			return false;
		const uint32_t at = p - base;
		for (uint32_t i = 0; i < 32 && at + i < size; i++)
		{
			const uint8_t c = ram[at + i];
			if (c == 0)
				return i > 0;
			if (c < 0x20 || c > 0x7e)
				return false;
		}
		return false;
	}

	std::string name(uint32_t p) const
	{
		return named(p) ? std::string((const char *)ram + (p - base)) : std::string("?");
	}

	// Whether an entry of this size is at `at`; function says whether it has one.
	bool entry(uint32_t at, int stride, bool& function) const
	{
		if (at + stride > size)
			return false;
		if (!named(read32(at + stride - 4)))		// the cheapest test that most of memory fails
			return false;
		const uint32_t code = read32(at + stride - 8);
		if (code != 0 && (!pointer(code) || (code & 1)))
			return false;
		if (ram[at + 1] > 8)						// the list an object is put in
			return false;
		if (stride == 20 && read16(at + 2) > 5)		// how the distance is used
			return false;
		uint32_t bits = read32(at + 4);
		float distance;
		memcpy(&distance, &bits, 4);
		if (bits != 0 && !(std::isfinite(distance) && distance >= 1.f && distance <= 1e12f))
			return false;
		function = code != 0;
		return true;
	}

	// How many entries of this size follow one another from `at`: none unless
	// most of them have a function and they are not all one name.
	int run(uint32_t at, int stride) const
	{
		int count = 0, functions = 0;
		bool function = false, varied = false;
		uint32_t first = 0;
		while (count < 1024 && entry(at + count * stride, stride, function))
		{
			const uint32_t namePointer = read32(at + count * stride + stride - 4);
			if (count == 0)
				first = namePointer;
			else if (namePointer != first)
				varied = true;
			functions += function;
			count++;
		}
		return count >= MinEntries && varied && functions * 2 >= count ? count : 0;
	}

	uint8_t *ram;
	uint32_t size, base;
	float factor;
};

} // namespace ps5::drawdist
