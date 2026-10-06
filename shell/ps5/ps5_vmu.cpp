/*
	PSFlyCast - the memory card manager's back end.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	THE FILES. Flycast keeps each memory card (VMU) as the card's flash memory
	and nothing else: 131072 bytes, no header, not compressed. It reads the
	whole file when a game starts and writes the blocks a game changes at
	their offsets (core/hw/maple/maple_devs.cpp, maple_sega_vmu). The name is
	the port and the slot, vmu_save_A1.bin to vmu_save_D2.bin, in the data
	folder; with "Per Game VMU A1", which is on unless emu.cfg says otherwise,
	port A's first card is <game>_vmu_save_A1.bin instead, where <game> is the
	disc's product number (core/oslib/oslib.cpp, hostfs::getVmuPath). A file
	that is missing, empty or all zeroes becomes a blank card when a game
	starts: 276 bytes of zlib that Flycast carries, which blankImage() here
	reproduces byte for byte.

	THE CARD. 256 blocks of 512 bytes. Numbers are little-endian.

	  Block 255, the root: sixteen bytes of 0x55 say it is formatted; the
	  card's colour at 0x10; the date it was formatted at 0x30; at 0x46 where
	  the block table is (254) and its size (1), at 0x4A where the directory
	  is (253) and its size (13), at 0x50 how many blocks saves may use (200).
	  Block 254, the block table (FAT): a 16-bit word for each block. 0xFFFC
	  is a free block, 0xFFFA the last block of a file, anything else the
	  block that follows this one in its file.
	  Blocks 253 down to 241, the directory: 208 entries of 32 bytes.
	    0x00  type: 0x00 no file, 0x33 a save, 0xCC a game for the VMU
	    0x01  0x00 may be copied, 0xFF copy-protected
	    0x02  first block
	    0x04  name, 12 bytes
	    0x10  date, BCD: century, year, month, day, hour, minute, second,
	          day of the week (0 is Monday)
	    0x18  size in blocks
	    0x1A  which of its blocks has the header (0 for a save, 1 for a game)
	  Blocks 0 to 199, the files. The Dreamcast gives a save the highest free
	  blocks, and a game the blocks from 0 up without a gap, because the VMU
	  runs it from there; so does this.

	A file's header, at the start of the block the directory names:
	    0x00  description for the VMU's file list, 16 bytes
	    0x10  description for the Dreamcast's file manager, 32 bytes
	    0x30  the program that made it, 16 bytes
	    0x40  number of icons, 0x42 animation speed, 0x44 eyecatch type,
	          0x46 CRC, 0x48 length of the data after header and pictures
	    0x60  palette: 16 colours, ARGB4444
	    0x80  the icons: 32x32, four bits a pixel, the left pixel in the high
	          half of each byte
	Text is Shift-JIS. ICONDATA_VMS, the picture a card shows in the
	Dreamcast's menu, has its own layout (see describe()).

	WHAT IS REFUSED. A card is only touched when all of it is understood: the
	root block's signature and layout, every directory entry's type, and each
	file's chain of blocks, which must stay within the card's file area, have
	exactly the length the directory gives and share no block with another
	file. Walking a chain marks each block as it goes, so a chain that loops
	meets its own mark and the walk ends. A card that fails any of this is
	shown with the reason and left alone.

	WRITING. A changed card is written whole to <card>.tmp beside it, synced,
	and renamed over the card: at every moment the card's name is either the
	old image or the new one. Before the first change of a session the image
	as it was goes to <card>.bak the same way.

	EXPORTED SAVES are a .vms and a .vmi, the pair the Dreamcast's own web
	browsers downloaded and the form saves are mostly published in. The .vms
	is the file's blocks exactly as they are on the card. The .vmi is 108
	bytes that say what the file is on a card, written here as:
	    0x00  checksum: the first four bytes of the resource name ANDed
	          with "SEGA"
	    0x04  description, 32 bytes: the save's long description
	    0x24  copyright, 32 bytes: "PSFlyCast"
	    0x44  the directory entry's date: year (16 bits), month, day, hour,
	          minute, second, day of the week (0 is Sunday here)
	    0x4C  version, 0
	    0x4E  file number, 1
	    0x50  resource name, 8 bytes: the .vms file's name without ".vms".
	          The field cannot hold a 12-character name, so it has the first
	          eight; the two files are found by their shared name instead.
	    0x58  the file's name on a card, 12 bytes as the card has them
	    0x64  mode: bit 0 copy-protected, bit 1 a game
	    0x66  0
	    0x68  size of the .vms in bytes
	A .dci, which importSave() also reads, is the 32-byte directory entry
	followed by the blocks with every four bytes reversed.
*/
#include "ps5_vmu.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace ps5::vmu
{
namespace
{

constexpr int RootBlock = 255;
constexpr int FatBlock = 254;
constexpr int DirBlock = 253;
constexpr int DirBlocks = 13;
constexpr int EntrySize = 32;
constexpr int DirEntries = DirBlocks * (int)(BlockSize / EntrySize);	// 208
constexpr int SystemStart = DirBlock - DirBlocks + 1;	// 241: from here up the card is its own
constexpr uint16_t FatFree = 0xFFFC;
constexpr uint16_t FatLast = 0xFFFA;
constexpr uint8_t TypeNone = 0x00;
constexpr uint8_t TypeData = 0x33;
constexpr uint8_t TypeGame = 0xCC;
constexpr size_t HeaderSize = 0x80;		// a file's header, up to its icons
constexpr size_t VmiSize = 108;

uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

std::string say(const char *format, ...)
{
	char buffer[768];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

Result fail(Error error, const std::string& message)
{
	Result result;
	result.error = error;
	result.message = message;
	return result;
}

Result done(const std::string& message)
{
	Result result;
	result.ok = true;
	result.message = message;
	return result;
}

// ---- Paths and files

std::string lower(std::string text)
{
	for (char& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

bool endsWith(const std::string& text, const char *end)
{
	const size_t n = strlen(end);
	return text.size() >= n && text.compare(text.size() - n, n, end) == 0;
}

std::string folderOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

std::string nameOf(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string join(const std::string& folder, const std::string& name)
{
	if (folder.empty() || folder.back() == '/')
		return folder + name;
	return folder + "/" + name;
}

// A file's name without its extension, and the extension in lower case.
std::string stemOf(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string extensionOf(const std::string& name)
{
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos || dot == 0 ? "" : lower(name.substr(dot));
}

std::vector<std::string> namesIn(const std::string& folder)
{
	std::vector<std::string> names;
	DIR *dir = opendir(folder.empty() ? "." : folder.c_str());
	if (dir == nullptr)
		return names;
	while (const dirent *entry = readdir(dir))
		names.push_back(entry->d_name);
	closedir(dir);
	std::sort(names.begin(), names.end());
	return names;
}

enum class Read { Ok, Missing, TooLarge, Failed };

// A whole file, when it is no larger than limit. why is for the user when it
// is not Ok.
Read readWhole(const std::string& path, size_t limit, std::vector<uint8_t>& data, std::string& why)
{
	data.clear();
	FILE *file = fopen(path.c_str(), "rb");
	if (file == nullptr)
	{
		const bool missing = errno == ENOENT;
		why = missing ? say("There is no file %s.", path.c_str())
				: say("%s could not be opened (%s).", path.c_str(), strerror(errno));
		return missing ? Read::Missing : Read::Failed;
	}
	// One byte more than the limit is asked for, to see that there is more.
	data.resize(limit + 1);
	const size_t got = fread(data.data(), 1, data.size(), file);
	const bool bad = ferror(file) != 0;
	const int error = errno;
	fclose(file);
	if (bad)
	{
		data.clear();
		why = say("%s could not be read (%s).", path.c_str(), strerror(error));
		return Read::Failed;
	}
	if (got > limit)
	{
		data.clear();
		why = say("%s is larger than %zu bytes.", path.c_str(), limit);
		return Read::TooLarge;
	}
	data.resize(got);
	return Read::Ok;
}

// The Error for a file that could not be read to import it.
Error errorOf(Read read)
{
	return read == Read::Missing ? Error::NotFound : read == Read::TooLarge ? Error::BadFile : Error::Io;
}

// Writes a file so that its name is never a half-written one: the bytes go to
// a temporary file in the same folder, are synced to the disc, and the
// temporary file is renamed over the name, which the file system does in one
// step. Whatever fails, the file of that name is as it was.
bool writeWhole(const std::string& path, const uint8_t *data, size_t size, std::string& why)
{
	const std::string temp = path + ".tmp";
	FILE *file = fopen(temp.c_str(), "wb");
	if (file == nullptr)
	{
		why = say("%s could not be created (%s).", temp.c_str(), strerror(errno));
		return false;
	}
	int error = 0;
	if (fwrite(data, 1, size, file) != size || fflush(file) != 0)
		error = errno != 0 ? errno : EIO;
	// A file system that has no sync (EINVAL and its kin) is not an error:
	// the rename is still all or nothing as far as this process can make it.
	if (error == 0 && fsync(fileno(file)) != 0 && errno != EINVAL && errno != ENOSYS && errno != ENOTSUP)
		error = errno;
	if (fclose(file) != 0 && error == 0)
		error = errno != 0 ? errno : EIO;
	if (error == 0 && rename(temp.c_str(), path.c_str()) != 0)
		error = errno;
	if (error != 0)
	{
		unlink(temp.c_str());
		why = say("%s could not be written (%s).", path.c_str(), strerror(error));
		return false;
	}
	// The new name itself is in the folder: sync that too, where it can be.
	const std::string folder = folderOf(path);
	const int fd = ::open(folder.empty() ? "." : folder.c_str(), O_RDONLY);
	if (fd >= 0)
	{
		fsync(fd);
		::close(fd);
	}
	return true;
}

// ---- Text

void putUtf8(std::string& out, uint32_t c)
{
	if (c < 0x80)
		out += (char)c;
	else if (c < 0x800)
	{
		out += (char)(0xC0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3F));
	}
	else
	{
		out += (char)(0xE0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
}

// Shift-JIS 0x8140 to 0x81FC, the signs (0 where there is none), as Windows'
// code page 932 has them.
const uint16_t Signs[189] = {
	0x3000, 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x30FB, 0xFF1A, 0xFF1B, 0xFF1F, 0xFF01, 0x309B, 0x309C,
	0x00B4, 0xFF40, 0x00A8, 0xFF3E, 0xFFE3, 0xFF3F, 0x30FD, 0x30FE, 0x309D, 0x309E, 0x3003, 0x4EDD,
	0x3005, 0x3006, 0x3007, 0x30FC, 0x2015, 0x2010, 0xFF0F, 0xFF3C, 0xFF5E, 0x2225, 0xFF5C, 0x2026,
	0x2025, 0x2018, 0x2019, 0x201C, 0x201D, 0xFF08, 0xFF09, 0x3014, 0x3015, 0xFF3B, 0xFF3D, 0xFF5B,
	0xFF5D, 0x3008, 0x3009, 0x300A, 0x300B, 0x300C, 0x300D, 0x300E, 0x300F, 0x3010, 0x3011, 0xFF0B,
	0xFF0D, 0x00B1, 0x00D7, 0x0000, 0x00F7, 0xFF1D, 0x2260, 0xFF1C, 0xFF1E, 0x2266, 0x2267, 0x221E,
	0x2234, 0x2642, 0x2640, 0x00B0, 0x2032, 0x2033, 0x2103, 0xFFE5, 0xFF04, 0xFFE0, 0xFFE1, 0xFF05,
	0xFF03, 0xFF06, 0xFF0A, 0xFF20, 0x00A7, 0x2606, 0x2605, 0x25CB, 0x25CF, 0x25CE, 0x25C7, 0x25C6,
	0x25A1, 0x25A0, 0x25B3, 0x25B2, 0x25BD, 0x25BC, 0x203B, 0x3012, 0x2192, 0x2190, 0x2191, 0x2193,
	0x3013, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
	0x2208, 0x220B, 0x2286, 0x2287, 0x2282, 0x2283, 0x222A, 0x2229, 0x0000, 0x0000, 0x0000, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x2227, 0x2228, 0xFFE2, 0x21D2, 0x21D4, 0x2200, 0x2203, 0x0000,
	0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x2220, 0x22A5,
	0x2312, 0x2202, 0x2207, 0x2261, 0x2252, 0x226A, 0x226B, 0x221A, 0x223D, 0x221D, 0x2235, 0x222B,
	0x222C, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x212B, 0x2030, 0x266F, 0x266D,
	0x266A, 0x2020, 0x2021, 0x00B6, 0x0000, 0x0000, 0x0000, 0x0000, 0x25EF,
};

// A two-byte Shift-JIS character as Unicode, or 0 for one this does not know.
// The rows before the kanji are runs that Unicode has in the same order, so
// they need arithmetic and no table; the kanji have no such order.
uint32_t twoBytes(uint8_t lead, uint8_t trail)
{
	switch (lead)
	{
	case 0x81:
		return Signs[trail - 0x40];
	case 0x82:
		if (trail >= 0x4F && trail <= 0x58)		// full-width digits
			return 0xFF10 + (trail - 0x4F);
		if (trail >= 0x60 && trail <= 0x79)		// and capitals
			return 0xFF21 + (trail - 0x60);
		if (trail >= 0x81 && trail <= 0x9A)		// and small letters
			return 0xFF41 + (trail - 0x81);
		if (trail >= 0x9F && trail <= 0xF1)		// hiragana
			return 0x3041 + (trail - 0x9F);
		return 0;
	case 0x83:
		if (trail >= 0x40 && trail <= 0x7E)		// katakana, around the trail byte 0x7F that is never used
			return 0x30A1 + (trail - 0x40);
		if (trail >= 0x80 && trail <= 0x96)
			return 0x30E0 + (trail - 0x80);
		if (trail >= 0x9F && trail <= 0xB6)		// Greek capitals; Unicode keeps a gap after rho
			return 0x0391 + (trail - 0x9F) + (trail - 0x9F >= 17 ? 1 : 0);
		if (trail >= 0xBF && trail <= 0xD6)		// and small letters
			return 0x03B1 + (trail - 0xBF) + (trail - 0xBF >= 17 ? 1 : 0);
		return 0;
	case 0x84:
	{
		// Cyrillic, where Shift-JIS has io after ie and Unicode has it apart.
		int i;
		uint32_t base, io;
		if (trail >= 0x40 && trail <= 0x60)
		{
			i = trail - 0x40;
			base = 0x0410;
			io = 0x0401;
		}
		else if (trail >= 0x70 && trail <= 0x91 && trail != 0x7F)
		{
			i = trail <= 0x7E ? trail - 0x70 : trail - 0x71;
			base = 0x0430;
			io = 0x0451;
		}
		else
			return 0;
		return i < 6 ? base + i : i == 6 ? io : base + i - 1;
	}
	case 0x87:
		if (trail >= 0x40 && trail <= 0x53)		// circled 1 to 20
			return 0x2460 + (trail - 0x40);
		if (trail >= 0x54 && trail <= 0x5D)		// Roman I to X
			return 0x2160 + (trail - 0x54);
		return 0;
	default:
		return 0;
	}
}

} // namespace

std::string toUtf8(const uint8_t *text, size_t size)
{
	size_t length = 0;
	while (length < size && text[length] != 0)
		length++;
	std::string out;
	for (size_t i = 0; i < length;)
	{
		const uint8_t c = text[i];
		const bool lead = (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
		if (c >= 0x20 && c < 0x7F)
		{
			out += (char)c;
			i++;
		}
		else if (c >= 0xA1 && c <= 0xDF)
		{
			// Half-width katakana: one byte each, and Unicode has them in the
			// same order.
			putUtf8(out, 0xFF61 + (c - 0xA1));
			i++;
		}
		else if (lead && i + 1 < length && text[i + 1] >= 0x40 && text[i + 1] <= 0xFC && text[i + 1] != 0x7F)
		{
			// One '?' for the pair, so that an unknown character does not also
			// turn its second byte into a stray letter.
			const uint32_t unicode = twoBytes(c, text[i + 1]);
			if (unicode != 0)
				putUtf8(out, unicode);
			else
				out += '?';
			i += 2;
		}
		else
		{
			out += '?';
			i++;
		}
	}
	// Fields are filled up with spaces, of either width.
	for (;;)
	{
		if (!out.empty() && out.back() == ' ')
			out.pop_back();
		else if (endsWith(out, "\xE3\x80\x80"))
			out.erase(out.size() - 3);
		else
			break;
	}
	return out;
}

namespace
{

// ---- A card in memory

struct Volume
{
	std::vector<uint8_t> image;
	int userBlocks = 0;
	int freeBlocks = 0;
	struct File
	{
		int entry;						// which directory entry
		std::vector<uint16_t> chain;	// its blocks, in the file's order
	};
	std::vector<File> files;

	uint8_t *block(int n) { return image.data() + (size_t)n * BlockSize; }
	const uint8_t *block(int n) const { return image.data() + (size_t)n * BlockSize; }
	uint16_t fat(int n) const { return le16(block(FatBlock) + n * 2); }
	void setFat(int n, uint16_t v) { put16(block(FatBlock) + n * 2, v); }
	// The directory starts in its highest block and goes down.
	uint8_t *entry(int index) { return block(DirBlock - index / 16) + (index % 16) * EntrySize; }
	const uint8_t *entry(int index) const { return block(DirBlock - index / 16) + (index % 16) * EntrySize; }
};

// A file's name as the key it is on a card: its bytes up to the first zero.
std::string keyOf(const uint8_t *entry)
{
	size_t length = 0;
	while (length < 12 && entry[4 + length] != 0)
		length++;
	return std::string((const char *)entry + 4, length);
}

// And for a message.
std::string shownName(const uint8_t *entry)
{
	const std::string name = toUtf8(entry + 4, 12);
	return name.empty() ? "(a file with no name)" : name;
}

enum class Parsed { Ok, Unformatted, Bad };

// Checks all of a card (the top of this file says what) and finds its files.
// Nothing here trusts a number from the card before it is checked, and no
// loop runs longer than the card is large, whatever the card says.
Parsed parse(Volume& vol, std::string& why)
{
	vol.files.clear();
	vol.userBlocks = 0;
	vol.freeBlocks = 0;
	if (vol.image.empty())
	{
		why = "The file is empty: there is no card in it yet. Flycast formats it when a game next starts.";
		return Parsed::Unformatted;
	}
	if (vol.image.size() != ImageSize)
	{
		why = say("The file is %zu bytes, and a memory card image is %zu: it is cut short or not a card.",
				vol.image.size(), ImageSize);
		return Parsed::Bad;
	}
	if (std::all_of(vol.image.begin(), vol.image.end(), [](uint8_t b) { return b == 0; }))
	{
		why = "The card is all zeroes: it has not been formatted yet. Flycast formats it when a game next starts.";
		return Parsed::Unformatted;
	}
	const uint8_t *root = vol.block(RootBlock);
	if (!std::all_of(root, root + 16, [](uint8_t b) { return b == 0x55; }))
	{
		why = "The card is not formatted, or its root block is damaged (the signature of a formatted card is missing).";
		return Parsed::Bad;
	}
	const int fatBlock = le16(root + 0x46), fatBlocks = le16(root + 0x48);
	const int dirBlock = le16(root + 0x4A), dirBlocks = le16(root + 0x4C);
	const int userBlocks = le16(root + 0x50);
	if (fatBlock != FatBlock || fatBlocks != 1 || dirBlock != DirBlock || dirBlocks != DirBlocks
			|| userBlocks < 1 || userBlocks > SystemStart)
	{
		why = say("The card's root block is damaged or describes a card of another kind (block table at %d, directory at %d "
				"with %d blocks, %d blocks for saves).", fatBlock, dirBlock, dirBlocks, userBlocks);
		return Parsed::Bad;
	}
	vol.userBlocks = userBlocks;

	// Which directory entry has each block, to see a block claimed twice.
	std::array<int, SystemStart> owner;
	owner.fill(-1);
	for (int index = 0; index < DirEntries; index++)
	{
		const uint8_t *entry = vol.entry(index);
		if (entry[0] == TypeNone)
			continue;
		if (entry[0] != TypeData && entry[0] != TypeGame)
		{
			why = say("The card's directory is damaged: entry %d has the unknown type 0x%02X.", index, entry[0]);
			return Parsed::Bad;
		}
		const std::string name = shownName(entry);
		const int size = le16(entry + 0x18);
		if (size < 1 || size > SystemStart)
		{
			why = say("The card's directory is damaged: %s claims a size of %d blocks.", name.c_str(), size);
			return Parsed::Bad;
		}
		Volume::File file;
		file.entry = index;
		int block = le16(entry + 2);
		for (int n = 0; n < size; n++)
		{
			if (block == FatLast || block == FatFree)
			{
				why = say("The card's block table is damaged: %s is %d blocks long, and its chain of blocks ends after %d.",
						name.c_str(), size, n);
				return Parsed::Bad;
			}
			if (block >= SystemStart)
			{
				why = say("The card is damaged: %s points to block %d, outside the part of the card that holds saves.",
						name.c_str(), block);
				return Parsed::Bad;
			}
			if (owner[block] >= 0)
			{
				// Also where a chain that loops ends: it comes back to a
				// block this very file has.
				if (owner[block] == index)
					why = say("The card's block table is damaged: the chain of blocks of %s loops back on itself at block %d.",
							name.c_str(), block);
				else
					why = say("The card's block table is damaged: block %d belongs to both %s and %s.", block,
							shownName(vol.entry(owner[block])).c_str(), name.c_str());
				return Parsed::Bad;
			}
			owner[block] = index;
			file.chain.push_back((uint16_t)block);
			block = vol.fat(block);
		}
		if (block != FatLast)
		{
			why = say("The card's block table is damaged: %s is %d blocks long, and its chain of blocks goes on after that.",
					name.c_str(), size);
			return Parsed::Bad;
		}
		vol.files.push_back(std::move(file));
	}
	for (int block = 0; block < vol.userBlocks; block++)
		if (vol.fat(block) == FatFree)
			vol.freeBlocks++;
	return Parsed::Ok;
}

std::vector<uint8_t> contentOf(const Volume& vol, const Volume::File& file)
{
	std::vector<uint8_t> content(file.chain.size() * BlockSize);
	for (size_t i = 0; i < file.chain.size(); i++)
		memcpy(content.data() + i * BlockSize, vol.block(file.chain[i]), BlockSize);
	return content;
}

// ---- Describing a save

int fromBcd(uint8_t byte)
{
	return (byte >> 4) > 9 || (byte & 15) > 9 ? -1 : (byte >> 4) * 10 + (byte & 15);
}

uint8_t toBcd(int value)
{
	return (uint8_t)(((value / 10) << 4) | (value % 10));
}

Date dateOf(const uint8_t *bcd)
{
	Date date;
	const int century = fromBcd(bcd[0]), year = fromBcd(bcd[1]);
	date.month = fromBcd(bcd[2]);
	date.day = fromBcd(bcd[3]);
	date.hour = fromBcd(bcd[4]);
	date.minute = fromBcd(bcd[5]);
	date.second = fromBcd(bcd[6]);
	date.year = century * 100 + year;
	date.valid = century >= 0 && year >= 0 && date.month >= 1 && date.month <= 12 && date.day >= 1 && date.day <= 31
			&& date.hour >= 0 && date.hour <= 23 && date.minute >= 0 && date.minute <= 59 && date.second >= 0
			&& date.second <= 59;
	if (!date.valid)
		date = Date{};
	return date;
}

// 0 for Sunday (Sakamoto's method).
int dayOfWeek(int year, int month, int day)
{
	static const int offsets[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
	if (month < 3)
		year--;
	return ((year + year / 4 - year / 100 + year / 400 + offsets[month - 1] + day) % 7 + 7) % 7;
}

void decodeIcon(Save& save, const uint8_t *palette, const uint8_t *pixels)
{
	uint8_t colours[16][4];
	for (int i = 0; i < 16; i++)
	{
		// ARGB4444: 0xF becomes 0xFF by repeating the four bits.
		const unsigned v = le16(palette + i * 2);
		colours[i][0] = (uint8_t)(((v >> 8) & 15) * 17);
		colours[i][1] = (uint8_t)(((v >> 4) & 15) * 17);
		colours[i][2] = (uint8_t)((v & 15) * 17);
		colours[i][3] = (uint8_t)((v >> 12) * 17);
	}
	uint8_t *out = save.icon.data();
	for (int i = 0; i < IconSide * IconSide / 2; i++)
	{
		memcpy(out, colours[pixels[i] >> 4], 4);
		memcpy(out + 4, colours[pixels[i] & 15], 4);
		out += 8;
	}
	save.hasIcon = true;
}

// A Save from a directory entry and the file's blocks.
void describe(Save& save, const uint8_t *entry, const std::vector<uint8_t>& content)
{
	save = Save{};
	save.type = entry[0] == TypeGame ? Type::Game : Type::Data;
	save.copyProtected = entry[1] != 0;
	save.firstBlock = le16(entry + 2);
	memcpy(save.rawName.data(), entry + 4, 12);
	save.name = toUtf8(entry + 4, 12);
	save.date = dateOf(entry + 0x10);
	save.blocks = (int)(content.size() / BlockSize);

	if (save.type == Type::Data && keyOf(entry) == "ICONDATA_VMS")
	{
		// The card's own picture for the Dreamcast's menu has no header like
		// the others: a 16-byte description, then where in the file its
		// one-bit picture is and, when it has one, its 16-colour picture (a
		// palette of 32 bytes and the pixels after it).
		if (content.size() < 0x18)
			return;
		save.shortDesc = toUtf8(content.data(), 16);
		save.longDesc = "The card's own icon";
		const size_t mono = le32(content.data() + 0x10), colour = le32(content.data() + 0x14);
		if (colour != 0 && colour <= content.size() && content.size() - colour >= 32 + 512)
		{
			decodeIcon(save, content.data() + colour, content.data() + colour + 32);
			save.iconCount = 1;
		}
		else if (mono != 0 && mono <= content.size() && content.size() - mono >= 128)
		{
			// As the VMU's screen has it: dark dots on a light ground.
			const uint8_t *bits = content.data() + mono;
			for (int i = 0; i < IconSide * IconSide; i++)
			{
				const bool dot = (bits[i / 8] & (0x80 >> (i % 8))) != 0;
				uint8_t *out = save.icon.data() + i * 4;
				out[0] = out[1] = out[2] = dot ? 0x10 : 0xE0;
				out[3] = 0xFF;
			}
			save.hasIcon = true;
			save.iconCount = 1;
		}
		return;
	}

	// The header, in the block the directory names. A file whose directory
	// entry names a block it does not have keeps its name and no more.
	const size_t at = (size_t)le16(entry + 0x1A) * BlockSize;
	if (at > content.size() || content.size() - at < HeaderSize)
		return;
	const uint8_t *header = content.data() + at;
	save.shortDesc = toUtf8(header, 16);
	save.longDesc = toUtf8(header + 0x10, 32);
	save.application = toUtf8(header + 0x30, 16);
	save.iconCount = le16(header + 0x40);
	save.animationSpeed = le16(header + 0x42);
	// A count the file has no room for is not a count: a file with no real
	// header (some have none) would otherwise show noise as its icon.
	const size_t room = content.size() - at - HeaderSize;
	if (save.iconCount >= 1 && (size_t)save.iconCount <= room / 512)
		decodeIcon(save, header + 0x60, header + HeaderSize);
	else
		save.iconCount = 0;
}

// ---- Naming a card

void nameCard(Card& card)
{
	const std::string& name = card.fileName;
	card.label = name;
	static const char PerGame[] = "_vmu_save_A1.bin";
	if (name.size() == 15 && name.compare(0, 9, "vmu_save_") == 0 && name.compare(11, 4, ".bin") == 0
			&& name[9] >= 'A' && name[9] <= 'D' && name[10] >= '1' && name[10] <= '2')
	{
		card.port = name[9] - 'A';
		card.slot = name[10] - '1';
	}
	else if (endsWith(name, PerGame) && name.size() > strlen(PerGame))
	{
		card.port = 0;
		card.slot = 0;
		card.perGame = true;
		card.game = name.substr(0, name.size() - strlen(PerGame));
	}
	else
		return;
	card.label = say("Port %c, slot %d", 'A' + card.port, card.slot + 1);
}

// ---- Changing a card

std::mutex changing;				// one change at a time, whichever thread asks
std::set<std::string> backedUp;		// the cards this session has a backup of

// The same file under one spelling ("data//x" and "data/x").
std::string plainPath(const std::string& path)
{
	std::string plain;
	for (char c : path)
		if (c != '/' || plain.empty() || plain.back() != '/')
			plain += c;
	return plain;
}

// Reads a card to change it or to take a save from it. raw is the file as it
// is, for the backup.
bool openCard(const std::string& path, std::vector<uint8_t>& raw, Volume& vol, Result& result)
{
	std::string why;
	const Read read = readWhole(path, ImageSize, raw, why);
	if (read != Read::Ok)
	{
		if (read == Read::Missing)
			result = fail(Error::NotFound, "There is no memory card image at " + path + ".");
		else if (read == Read::TooLarge)
			result = fail(Error::Unreadable, say("%s is larger than a memory card image (%zu bytes): it is not a card.",
					path.c_str(), ImageSize));
		else
			result = fail(Error::Io, why);
		return false;
	}
	vol.image = raw;
	if (parse(vol, why) != Parsed::Ok)
	{
		result = fail(Error::Unreadable, why);
		return false;
	}
	return true;
}

// The save a description is of, when the card still has it as described.
const Volume::File *findSave(const Volume& vol, const Save& save)
{
	for (const Volume::File& file : vol.files)
	{
		if (file.entry != save.entry)
			continue;
		const uint8_t *entry = vol.entry(file.entry);
		if (memcmp(entry + 4, save.rawName.data(), 12) == 0 && le16(entry + 2) == save.firstBlock
				&& (int)file.chain.size() == save.blocks && (entry[0] == TypeGame) == (save.type == Type::Game))
			return &file;
		break;
	}
	return nullptr;
}

Result changed(const Save& save)
{
	return fail(Error::Changed, "The card no longer has " + (save.name.empty() ? std::string("that save") : save.name)
			+ " where it was when the card was read, so nothing was done. Read the card again.");
}

// Keeps the image as it was, once a session.
bool backUp(const std::string& path, const std::vector<uint8_t>& before, std::string& why)
{
	const std::string key = plainPath(path);
	if (backedUp.count(key) != 0)
		return true;
	if (!writeWhole(path + ".bak", before.data(), before.size(), why))
		return false;
	backedUp.insert(key);
	return true;
}

// Writes a changed card. Before anything is written the new image goes
// through the same checks as a card read from disc: a mistake here must end
// as a message and not as a damaged card.
Result commit(const std::string& path, const std::vector<uint8_t>& before, const Volume& vol, const std::string& message)
{
	std::string why;
	Volume check;
	check.image = vol.image;
	if (parse(check, why) != Parsed::Ok)
		return fail(Error::Unreadable, "The card was not changed: the change would have damaged it, which is a fault in "
				"the memory card manager (" + why + ").");
	if (!backUp(path, before, why))
		return fail(Error::Io, "The card was not changed, because its backup could not be made: " + why);
	if (!writeWhole(path, vol.image.data(), vol.image.size(), why))
		return fail(Error::Io, "The card was not changed: " + why);
	return done(message);
}

// Puts a file on a card in memory, as the Dreamcast would: entry is its
// directory entry as it was elsewhere (its first block is set here) and
// content its blocks. Nothing is changed unless all of it can be done.
Result add(Volume& vol, const uint8_t *entry, const std::vector<uint8_t>& content)
{
	const int blocks = (int)(content.size() / BlockSize);
	const std::string key = keyOf(entry);
	for (const Volume::File& file : vol.files)
		if (keyOf(vol.entry(file.entry)) == key)
			return fail(Error::NameExists, "The card already has a save called " + shownName(entry)
					+ ". Delete that one first if this one is to take its place.");
	if (blocks > vol.freeBlocks)
		return fail(Error::NoSpace, say("Not enough free blocks: needs %d, the card has %d.", blocks, vol.freeBlocks));
	int slot = -1;
	for (int index = 0; index < DirEntries && slot < 0; index++)
		if (vol.entry(index)[0] == TypeNone)
			slot = index;
	if (slot < 0)
		return fail(Error::DirectoryFull, say("The card's directory is full: a card holds %d files at most.", DirEntries));

	std::vector<uint16_t> chain;
	if (entry[0] == TypeGame)
	{
		// The VMU runs a game from the start of its memory, so a game is the
		// blocks from 0 up, and a card has one.
		for (const Volume::File& file : vol.files)
			if (vol.entry(file.entry)[0] == TypeGame)
				return fail(Error::GameSlot, "The card already holds a VMU game (" + shownName(vol.entry(file.entry))
						+ "), and a card has room for one.");
		for (int block = 0; block < blocks; block++)
		{
			if (vol.fat(block) != FatFree)
				return fail(Error::GameSlot, say("A VMU game has to be the first blocks of a card, and of the %d this "
						"one needs some hold saves. It fits on a card with those blocks free.", blocks));
			chain.push_back((uint16_t)block);
		}
	}
	else
	{
		for (int block = vol.userBlocks - 1; block >= 0 && (int)chain.size() < blocks; block--)
			if (vol.fat(block) == FatFree)
				chain.push_back((uint16_t)block);
	}

	for (int i = 0; i < blocks; i++)
	{
		memcpy(vol.block(chain[i]), content.data() + (size_t)i * BlockSize, BlockSize);
		vol.setFat(chain[i], i + 1 < blocks ? chain[i + 1] : FatLast);
	}
	uint8_t *out = vol.entry(slot);
	memcpy(out, entry, EntrySize);
	put16(out + 2, chain[0]);
	put16(out + 0x18, (unsigned)blocks);
	vol.freeBlocks -= blocks;
	Volume::File file;
	file.entry = slot;
	file.chain = std::move(chain);
	vol.files.push_back(std::move(file));
	return done("");
}

const char *const ProtectedNote = " The game marked this save copy-protected: the Dreamcast's own file manager would have "
		"refused, and the game may not accept a copy.";

// ---- Saves in files

struct Incoming
{
	std::array<uint8_t, EntrySize> entry{};
	std::vector<uint8_t> content;		// whole blocks
};

// The file in a folder whose name is this one whatever its capitals (a .VMS
// beside a .vmi, as they come from a FAT drive).
std::string beside(const std::string& folder, const std::string& name)
{
	const std::string wanted = lower(name);
	for (const std::string& found : namesIn(folder))
		if (lower(found) == wanted)
			return join(folder, found);
	return "";
}

std::string trimmed(const uint8_t *text, size_t size)
{
	size_t length = 0;
	while (length < size && text[length] != 0)
		length++;
	while (length > 0 && text[length - 1] == ' ')
		length--;
	return std::string((const char *)text, length);
}

Result loadDci(const std::string& file, Incoming& in)
{
	std::vector<uint8_t> data;
	std::string why;
	const Read read = readWhole(file, EntrySize + SystemStart * BlockSize, data, why);
	if (read != Read::Ok)
		return fail(errorOf(read), read == Read::TooLarge ? nameOf(file) + " is larger than a memory card: it is not a save." : why);
	if (data.size() < EntrySize + BlockSize || (data.size() - EntrySize) % BlockSize != 0)
		return fail(Error::BadFile, say("%s is not a .dci save: its %zu bytes are not a directory entry and whole blocks.",
				nameOf(file).c_str(), data.size()));
	const int blocks = (int)((data.size() - EntrySize) / BlockSize);
	if (data[0] != TypeData && data[0] != TypeGame)
		return fail(Error::BadFile, say("%s is not a .dci save: it starts with the file type 0x%02X.", nameOf(file).c_str(),
				data[0]));
	if (le16(data.data() + 0x18) != blocks)
		return fail(Error::BadFile, say("%s is damaged: it says the save is %d blocks and holds %d.", nameOf(file).c_str(),
				le16(data.data() + 0x18), blocks));
	if (data[4] == 0)
		return fail(Error::BadFile, nameOf(file) + " is damaged: the save in it has no name.");
	memcpy(in.entry.data(), data.data(), EntrySize);
	in.content.resize(data.size() - EntrySize);
	// A .dci has each four bytes of the blocks in reverse.
	for (size_t i = 0; i < in.content.size(); i += 4)
		for (size_t b = 0; b < 4; b++)
			in.content[i + b] = data[EntrySize + i + 3 - b];
	return done("");
}

Result loadVmi(const std::string& vmiFile, const std::string& vmsFile, Incoming& in)
{
	std::vector<uint8_t> vmi;
	std::string why;
	Read read = readWhole(vmiFile, 4096, vmi, why);
	if (read != Read::Ok)
		return fail(errorOf(read), read == Read::TooLarge ? nameOf(vmiFile) + " is too large to be a .vmi file." : why);
	if (vmi.size() < VmiSize)
		return fail(Error::BadFile, say("%s is not a .vmi file: it is %zu bytes, and one is %zu.", nameOf(vmiFile).c_str(),
				vmi.size(), VmiSize));
	if (vmi[0x58] == 0)
		return fail(Error::BadFile, nameOf(vmiFile) + " does not say what the save is called on a card.");
	read = readWhole(vmsFile, SystemStart * BlockSize, in.content, why);
	if (read != Read::Ok)
		return fail(errorOf(read), read == Read::TooLarge ? nameOf(vmsFile) + " is larger than a memory card: it is not a save." : why);
	if (in.content.empty())
		return fail(Error::BadFile, nameOf(vmsFile) + " is empty.");
	// A save is whole blocks on a card, whatever length the file was cut to.
	in.content.resize((in.content.size() + BlockSize - 1) / BlockSize * BlockSize);

	const unsigned mode = le16(vmi.data() + 0x64);
	const bool game = (mode & 2) != 0;
	uint8_t *entry = in.entry.data();
	entry[0] = game ? TypeGame : TypeData;
	entry[1] = (mode & 1) != 0 ? 0xFF : 0x00;
	memcpy(entry + 4, vmi.data() + 0x58, 12);
	const int year = le16(vmi.data() + 0x44), month = vmi[0x46], day = vmi[0x47];
	const int hour = vmi[0x48], minute = vmi[0x49], second = vmi[0x4A];
	if (year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59 && second <= 59)
	{
		entry[0x10] = toBcd(year / 100);
		entry[0x11] = toBcd(year % 100);
		entry[0x12] = toBcd(month);
		entry[0x13] = toBcd(day);
		entry[0x14] = toBcd(hour);
		entry[0x15] = toBcd(minute);
		entry[0x16] = toBcd(second);
		// A .vmi's week starts on Sunday and a card's on Monday.
		const int weekday = vmi[0x4B] <= 6 ? vmi[0x4B] : dayOfWeek(year, month, day);
		entry[0x17] = (uint8_t)((weekday + 6) % 7);
	}
	put16(entry + 0x18, (unsigned)(in.content.size() / BlockSize));
	put16(entry + 0x1A, game ? 1 : 0);
	return done("");
}

// The save in a file of any of the kinds importSave() takes.
Result loadIncoming(const std::string& file, Incoming& in)
{
	const std::string folder = folderOf(file), name = nameOf(file);
	const std::string extension = extensionOf(name), stem = stemOf(name);
	struct stat info;
	if (stat(file.c_str(), &info) != 0)
		return fail(Error::NotFound, "There is no file " + file + ".");
	if (extension == ".dci")
		return loadDci(file, in);
	if (extension == ".vmi")
	{
		// The .vms of the same name, else the one the .vmi itself names.
		std::string vms = beside(folder, stem + ".vms");
		if (vms.empty())
		{
			std::vector<uint8_t> vmi;
			std::string why;
			if (readWhole(file, 4096, vmi, why) == Read::Ok && vmi.size() >= VmiSize)
			{
				const std::string resource = trimmed(vmi.data() + 0x50, 8);
				if (!resource.empty())
					vms = beside(folder, resource + ".vms");
			}
		}
		if (vms.empty())
			return fail(Error::NotFound, name + " describes a save, and the save itself, " + stem + ".vms, is not beside it.");
		return loadVmi(file, vms, in);
	}
	if (extension == ".vms")
	{
		// The .vmi of the same name, else one beside it that names this file.
		std::string vmi = beside(folder, stem + ".vmi");
		if (vmi.empty())
			for (const std::string& other : namesIn(folder))
			{
				if (extensionOf(other) != ".vmi")
					continue;
				std::vector<uint8_t> data;
				std::string why;
				if (readWhole(join(folder, other), 4096, data, why) == Read::Ok && data.size() >= VmiSize
						&& lower(trimmed(data.data() + 0x50, 8)) == lower(stem))
				{
					vmi = join(folder, other);
					break;
				}
			}
		if (vmi.empty())
			return fail(Error::NotFound, name + " has no .vmi file beside it. The .vmi is what says what the save is called "
					"on a card, and a game only finds a save by that name.");
		return loadVmi(vmi, file, in);
	}
	return fail(Error::BadFile, name + " is not a save this can import: it takes a .vmi with its .vms, or a .dci.");
}

} // namespace

// ---- The interface

std::string Date::text() const
{
	return valid ? say("%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute) : std::string();
}

Card card(const std::string& path)
{
	Card result;
	result.path = path;
	result.fileName = nameOf(path);
	nameCard(result);
	std::vector<uint8_t> raw;
	std::string why;
	const Read read = readWhole(path, ImageSize, raw, why);
	if (read != Read::Ok)
	{
		result.exists = read != Read::Missing;
		if (read == Read::Missing)
			result.error = "There is no card here yet.";
		else if (read == Read::TooLarge)
			result.error = say("The file is larger than a memory card image (%zu bytes): it is not a card.", ImageSize);
		else
			result.error = why;
		return result;
	}
	result.exists = true;
	Volume vol;
	vol.image = std::move(raw);
	const Parsed parsed = parse(vol, why);
	result.unformatted = parsed == Parsed::Unformatted;
	result.readable = parsed == Parsed::Ok;
	if (!result.readable)
	{
		result.error = why;
		return result;
	}
	result.freeBlocks = vol.freeBlocks;
	result.userBlocks = vol.userBlocks;
	result.saves = (int)vol.files.size();
	return result;
}

std::string sharedCardPath(const std::string& folder, int port, int slot)
{
	return join(folder, say("vmu_save_%c%d.bin", 'A' + port, slot + 1));
}

std::vector<Card> cards(const std::string& folder)
{
	std::vector<Card> found;
	for (const std::string& name : namesIn(folder))
	{
		Card probe;
		probe.fileName = name;
		nameCard(probe);
		// Only what Flycast itself would open: not a backup, a temporary
		// file or anything else that ends in .bin.
		if (probe.port < 0)
			continue;
		found.push_back(card(join(folder, name)));
	}
	std::stable_sort(found.begin(), found.end(), [](const Card& a, const Card& b) {
		if (a.perGame != b.perGame)
			return !a.perGame;
		if (a.perGame)
			return lower(a.game) < lower(b.game);
		return a.port * 2 + a.slot < b.port * 2 + b.slot;
	});
	return found;
}

std::vector<Save> list(const Card& from)
{
	std::vector<Save> saves;
	std::vector<uint8_t> raw;
	Volume vol;
	Result result;
	if (!openCard(from.path, raw, vol, result))
		return saves;
	saves.resize(vol.files.size());
	for (size_t i = 0; i < vol.files.size(); i++)
	{
		describe(saves[i], vol.entry(vol.files[i].entry), contentOf(vol, vol.files[i]));
		saves[i].entry = vol.files[i].entry;
	}
	return saves;
}

Result remove(const Card& from, const Save& save)
{
	std::lock_guard<std::mutex> lock(changing);
	std::vector<uint8_t> raw;
	Volume vol;
	Result result;
	if (!openCard(from.path, raw, vol, result))
		return result;
	const Volume::File *file = findSave(vol, save);
	if (file == nullptr)
		return changed(save);
	// As the Dreamcast deletes: the blocks are free and the entry is empty.
	// What the blocks held stays until something else is written there.
	const std::string name = shownName(vol.entry(file->entry));
	for (uint16_t block : file->chain)
		vol.setFat(block, FatFree);
	memset(vol.entry(file->entry), 0, EntrySize);
	return commit(from.path, raw, vol, say("%s was deleted: %d %s free again.", name.c_str(), save.blocks,
			save.blocks == 1 ? "block is" : "blocks are"));
}

Result copy(const Card& from, const Save& save, const Card& to)
{
	std::lock_guard<std::mutex> lock(changing);
	std::vector<uint8_t> raw, rawTo;
	Volume source, target;
	Result result;
	if (!openCard(from.path, raw, source, result))
		return result;
	const Volume::File *file = findSave(source, save);
	if (file == nullptr)
		return changed(save);
	const uint8_t *entry = source.entry(file->entry);
	const bool copyProtected = entry[1] != 0;
	if (!openCard(to.path, rawTo, target, result))
	{
		result.message = "The card to copy to cannot be used. " + result.message;
		result.copyProtected = copyProtected;
		return result;
	}
	result = add(target, entry, contentOf(source, *file));
	if (result.ok)
		result = commit(to.path, rawTo, target, shownName(entry) + " was copied to " + (to.label.empty() ? to.path : to.label)
				+ (to.perGame ? " of " + to.game : "") + "." + (copyProtected ? ProtectedNote : ""));
	result.copyProtected = copyProtected;
	return result;
}

Result exportSave(const Card& from, const Save& save, const std::string& folder)
{
	// The card is only read, but two exports of one name would share a
	// temporary file.
	std::lock_guard<std::mutex> lock(changing);
	std::vector<uint8_t> raw;
	Volume vol;
	Result result;
	if (!openCard(from.path, raw, vol, result))
		return result;
	const Volume::File *file = findSave(vol, save);
	if (file == nullptr)
		return changed(save);
	const uint8_t *entry = vol.entry(file->entry);
	const std::vector<uint8_t> content = contentOf(vol, *file);

	// A name any file system takes, FAT on a USB drive too.
	std::string base = keyOf(entry);
	for (char& c : base)
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			c = '_';
	while (!base.empty() && base.back() == '.')
		base.back() = '_';
	if (base.empty() || base[0] == '.')
		base = "_" + base;

	std::array<uint8_t, VmiSize> vmi{};
	for (size_t i = 0; i < 8 && i < base.size(); i++)
		vmi[0x50 + i] = (uint8_t)base[i];
	for (int i = 0; i < 4; i++)
		vmi[i] = vmi[0x50 + i] & (uint8_t)"SEGA"[i];
	const size_t header = (size_t)le16(entry + 0x1A) * BlockSize;
	if (keyOf(entry) != "ICONDATA_VMS" && header <= content.size() && content.size() - header >= HeaderSize)
	{
		const std::string description = trimmed(content.data() + header + 0x10, 32);
		memcpy(vmi.data() + 0x04, description.data(), description.size());
	}
	memcpy(vmi.data() + 0x24, "PSFlyCast", 9);
	const Date date = dateOf(entry + 0x10);
	if (date.valid)
	{
		put16(vmi.data() + 0x44, (unsigned)date.year);
		vmi[0x46] = (uint8_t)date.month;
		vmi[0x47] = (uint8_t)date.day;
		vmi[0x48] = (uint8_t)date.hour;
		vmi[0x49] = (uint8_t)date.minute;
		vmi[0x4A] = (uint8_t)date.second;
		// A card's week starts on Monday and a .vmi's on Sunday. The card's
		// own day is kept when it is one, so that importing gives it back.
		vmi[0x4B] = entry[0x17] <= 6 ? (uint8_t)((entry[0x17] + 1) % 7) : (uint8_t)dayOfWeek(date.year, date.month, date.day);
	}
	put16(vmi.data() + 0x4E, 1);
	memcpy(vmi.data() + 0x58, entry + 4, 12);
	put16(vmi.data() + 0x64, (entry[1] != 0 ? 1u : 0u) | (entry[0] == TypeGame ? 2u : 0u));
	put32(vmi.data() + 0x68, (uint32_t)content.size());

	mkdir(folder.c_str(), 0755);
	const std::string vmsPath = join(folder, base + ".vms"), vmiPath = join(folder, base + ".vmi");
	std::string why;
	if (!writeWhole(vmsPath, content.data(), content.size(), why))
		result = fail(Error::Io, "The save was not exported: " + why);
	else if (!writeWhole(vmiPath, vmi.data(), vmi.size(), why))
	{
		// Half a pair is of no use to anyone.
		unlink(vmsPath.c_str());
		result = fail(Error::Io, "The save was not exported: " + why);
	}
	else
	{
		result = done(shownName(entry) + " was exported as " + base + ".vms and " + base + ".vmi in " + folder + "."
				+ (entry[1] != 0 ? ProtectedNote : ""));
		result.path = vmiPath;
	}
	result.copyProtected = entry[1] != 0;
	return result;
}

Result importSave(const Card& to, const std::string& file)
{
	std::lock_guard<std::mutex> lock(changing);
	Incoming in;
	Result result = loadIncoming(file, in);
	if (!result.ok)
		return result;
	std::vector<uint8_t> raw;
	Volume vol;
	if (!openCard(to.path, raw, vol, result))
		return result;
	result = add(vol, in.entry.data(), in.content);
	if (!result.ok)
		return result;
	return commit(to.path, raw, vol, say("%s was added to the card: %d %s.", shownName(in.entry.data()).c_str(),
			(int)(in.content.size() / BlockSize), in.content.size() == BlockSize ? "block" : "blocks"));
}

Result inspect(const std::string& file, Save& save)
{
	Incoming in;
	const Result result = loadIncoming(file, in);
	save = Save{};
	if (result.ok)
		describe(save, in.entry.data(), in.content);
	return result;
}

std::vector<uint8_t> blankImage()
{
	// What Flycast unpacks for a new card (vmu_default in maple_devs.cpp),
	// built here from what it means.
	std::vector<uint8_t> image(ImageSize, 0);
	uint8_t *root = image.data() + RootBlock * BlockSize;
	memset(root, 0x55, 16);
	// The card's colour in the Dreamcast's menu: "custom", then blue, green,
	// red and alpha - white, a little transparent.
	static const uint8_t colour[] = { 0x01, 0xFF, 0xFF, 0xFF, 0x64 };
	memcpy(root + 0x10, colour, sizeof(colour));
	// Formatted on Friday 27 November 1998 at 00:00:59, the day the
	// Dreamcast went on sale.
	static const uint8_t formatted[] = { 0x19, 0x98, 0x11, 0x27, 0x00, 0x00, 0x59, 0x04 };
	memcpy(root + 0x30, formatted, sizeof(formatted));
	put16(root + 0x40, 255);			// the last block
	put16(root + 0x44, RootBlock);
	put16(root + 0x46, FatBlock);
	put16(root + 0x48, 1);
	put16(root + 0x4A, DirBlock);
	put16(root + 0x4C, DirBlocks);
	put16(root + 0x4E, 5);				// which of the Dreamcast's built-in icons the card shows
	put16(root + 0x50, 200);			// blocks for saves
	put16(root + 0x52, 31);
	put16(root + 0x56, 0x80);
	// The block table: everything free but the card's own blocks, where the
	// directory is a chain from its first block (253) down to its last.
	uint8_t *fat = image.data() + FatBlock * BlockSize;
	for (int block = 0; block < 256; block++)
		put16(fat + block * 2, FatFree);
	put16(fat + SystemStart * 2, FatLast);
	for (int block = SystemStart + 1; block <= DirBlock; block++)
		put16(fat + block * 2, (unsigned)(block - 1));
	put16(fat + FatBlock * 2, FatLast);
	put16(fat + RootBlock * 2, FatLast);
	// Flycast's image is a card someone once formatted, and two of its free
	// blocks (0 and 11) still hold a few bytes of whatever was there. They
	// mean nothing; they are here so that this is that image to the byte.
	static const uint8_t stray0[] = { 0xC4, 0x00, 0xA4, 0x00, 0x00, 0x4F, 0x97, 0x00 };
	static const uint8_t stray11[] = {
		0x04, 0x00, 0x00, 0x00, 0x1D, 0x8C, 0x06, 0x00, 0x90, 0x00, 0xA4, 0x00, 0x90, 0x00, 0xA4, 0x00,
		0x38, 0x00, 0x93, 0x00, 0x38, 0x00, 0x93, 0x00, 0x00, 0x60, 0x98, 0x00, 0x00, 0xA0, 0x0A, 0x00,
	};
	memcpy(image.data(), stray0, sizeof(stray0));
	memcpy(image.data() + 0x1788, stray11, sizeof(stray11));
	return image;
}

Result format(const std::string& path, bool erase)
{
	std::lock_guard<std::mutex> lock(changing);
	std::vector<uint8_t> raw;
	std::string why;
	const std::vector<uint8_t> blank = blankImage();
	// Whatever is in the way is kept as the backup, a file that is no card
	// too; one far larger than a card is somebody's other file and stays.
	const Read read = readWhole(path, 16 * ImageSize, raw, why);
	if (read == Read::Failed)
		return fail(Error::Io, why);
	if (read == Read::TooLarge)
		return fail(Error::Unreadable, nameOf(path) + " is far larger than a memory card and was left as it is.");
	const bool holdsSomething = std::any_of(raw.begin(), raw.end(), [](uint8_t b) { return b != 0; });
	if (holdsSomething)
	{
		if (!erase)
			return fail(Error::NotEmpty, nameOf(path) + " already holds a card or some other data. It was left as it is.");
		if (!backUp(path, raw, why))
			return fail(Error::Io, "The card was not formatted, because its backup could not be made: " + why);
	}
	if (!writeWhole(path, blank.data(), blank.size(), why))
		return fail(Error::Io, "The card was not formatted: " + why);
	return done(holdsSomething ? "The card was formatted. What it held is in " + nameOf(path) + ".bak."
			: "A blank card was made.");
}

void beginSession()
{
	std::lock_guard<std::mutex> lock(changing);
	backedUp.clear();
}

}
