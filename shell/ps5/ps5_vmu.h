/*
	PSFlyCast - the memory card manager's back end.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Reads and changes the memory card images Flycast keeps in its data folder
	(ps5_vmu.cpp says what they are). Nothing here knows the console or the
	emulator: plain C++ and POSIX files, so that it is tested on a PC.

	Every function reads the card from its file when it is called and keeps
	nothing in between, so a Card or a Save is a description of what was there
	at that moment. A function that changes a card checks that the save it was
	given is still where the description says, and changes nothing when it is
	not.

	NOT WHILE A GAME IS LOADED. Flycast reads a card into memory when the game
	starts and keeps its file open until the game is unloaded
	(maple_sega_vmu::OnSetup, Emulator::unloadGame). A card changed here in
	between is replaced by a new file, so the game goes on writing to the old
	one, which no longer has a name: what it saves from then on is lost, and it
	never sees the change. cards(), list() and inspect() are harmless then but
	may show a card the game has half written; remove(), copy(), importSave()
	and format() must wait until the game is unloaded.
*/
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ps5::vmu
{

constexpr size_t BlockSize = 512;
constexpr size_t ImageSize = 256 * BlockSize;	// a card is 128 KiB, and so is its file
constexpr int IconSide = 32;

// One memory card image.
struct Card
{
	std::string path;			// the image's file
	std::string fileName;		// its name alone: "vmu_save_A1.bin"
	std::string label;			// "Port A, slot 1" (the file's name when it says no port)
	int port = -1;				// 0 to 3 for A to D, -1 when the name says none
	int slot = -1;				// 0 or 1
	// Flycast's "Per Game VMU A1" (on unless emu.cfg turns it off): port A's
	// first card is one file for each game, named for the game. game is that
	// part of the file's name: the disc's product number with the characters
	// a file name cannot have turned to '_' ("T1234N", "MK-51000"), or for a
	// card an older Flycast made, the game's file name.
	bool perGame = false;
	std::string game;

	bool exists = false;		// there is a file
	// The file is there but holds no card yet (it is empty or all zeroes):
	// Flycast formats it when a game starts, and so does format().
	bool unformatted = false;
	// It was read and understood. Only then do the numbers below mean
	// anything, and only then will anything here change it.
	bool readable = false;
	std::string error;			// why not, for the user

	int freeBlocks = 0;			// of the blocks saves may use
	int userBlocks = 0;			// 200 on every ordinary card
	int saves = 0;				// how many files it holds
};

struct Date
{
	bool valid = false;			// the card's timestamp was a date
	int year = 0, month = 0, day = 0;
	int hour = 0, minute = 0, second = 0;
	std::string text() const;	// "2001-03-14 21:05", or empty when not valid
};

enum class Type
{
	Data,		// a save
	Game,		// a program for the VMU itself (a card holds one, at its start)
};

// One file of a card.
struct Save
{
	// Text is UTF-8. The card has Shift-JIS: ASCII, half-width katakana, kana,
	// full-width letters and signs, Greek and Cyrillic are converted, and each
	// kanji is a '?' (their table is 7000 entries).
	std::string name;			// the file's name on the card, at most 12 characters
	std::string shortDesc;		// what the VMU's own file list shows (16 bytes)
	std::string longDesc;		// what the Dreamcast's file manager shows (32 bytes)
	std::string application;	// the program that made it, as it names itself (16 bytes)
	Type type = Type::Data;
	// The game asked that the Dreamcast's file manager refuse to copy it.
	// Nothing here refuses: copy() and exportSave() repeat the flag in their
	// Result, for the screen to say so.
	bool copyProtected = false;
	int blocks = 0;				// its size
	Date date;

	// The first of its icons: 32x32, four bytes a pixel in the order R, G, B,
	// A, rows from the top, alpha as the save has it (not premultiplied).
	// All zeroes when it has none.
	bool hasIcon = false;
	int iconCount = 0;			// frames of the animation (only the first is decoded)
	int animationSpeed = 0;		// as the save has it
	std::array<uint8_t, IconSide * IconSide * 4> icon{};

	// Where it is on the card, to find it again: these are what remove(),
	// copy() and exportSave() go by. entry is -1 for a save inspect() read
	// from a file.
	int entry = -1;				// which of the directory's 208 entries
	int firstBlock = 0;
	std::array<uint8_t, 12> rawName{};	// the name's bytes as the card has them
};

enum class Error
{
	None,
	NotFound,		// no such card or file
	Unreadable,		// the card is damaged, unformatted or not a card
	Changed,		// the card is not as it was when the save was listed: list again
	NameExists,		// the destination has a file of that name
	NoSpace,		// too few free blocks
	DirectoryFull,	// 208 files already
	GameSlot,		// a VMU game needs the card's first blocks, and they are taken
	BadFile,		// the file to import is not a save this understands
	NotEmpty,		// format() without erase, and the file holds something
	Io,				// the file system refused: message has its reason
};

struct Result
{
	bool ok = false;
	Error error = Error::None;
	// Plain English, for the user: what went wrong ("Not enough free blocks:
	// needs 12, the card has 5."), or what was done.
	std::string message;
	// The save has the copy-protect flag (copy() and exportSave(), done or
	// not): the Dreamcast itself would have refused.
	bool copyProtected = false;
	// exportSave(): the .vmi it wrote, which is what importSave() takes.
	std::string path;

	explicit operator bool() const { return ok; }
};

// Every card image in a folder (Flycast's data folder, <root>/data/): the
// shared cards vmu_save_A1.bin to vmu_save_D2.bin in the order of their ports,
// then each game's own card (*_vmu_save_A1.bin) in the order of their names.
// A card that could not be read is in the list too, with the reason.
std::vector<Card> cards(const std::string& folder);

// One image, by its path: as an entry of cards(), also when there is no file
// (exists is false then), which is how a slot with no card yet is described.
Card card(const std::string& path);
// The path of a shared card: port 0 to 3, slot 0 or 1.
std::string sharedCardPath(const std::string& folder, int port, int slot);

// The card's files, in the order of its directory. Empty when the card cannot
// be read (card(path).error says why).
std::vector<Save> list(const Card& card);

// Deletes a save: its blocks are free again and its directory entry is empty.
Result remove(const Card& card, const Save& save);

// Copies a save to another card, byte for byte, with its date and its flags.
// Refused when the destination has a file of that name or too few free blocks.
Result copy(const Card& from, const Save& save, const Card& to);

// Writes a save to a folder (made when only its last part is missing) as <name>.vms, the
// save's blocks as they are, and <name>.vmi, the small file that says what it
// is called on a card. <name> is the save's name with anything but letters,
// digits, '.', '_' and '-' turned to '_'. Files of those names are replaced.
Result exportSave(const Card& card, const Save& save, const std::string& folder);

// Adds a save from a file: a .vmi (its .vms is looked for beside it, by its
// own name and then by the name the .vmi gives), a .vms (which needs its .vmi
// beside it: without one nothing says what the save is called on a card), or
// a .dci. Refused like copy().
Result importSave(const Card& to, const std::string& file);

// What importSave() would add, without a card: the save in a file. entry is -1.
Result inspect(const std::string& file, Save& save);

// A blank card exactly as Flycast makes one for a slot with no file.
std::vector<uint8_t> blankImage();
// Writes one. Where there is no file, or one that holds no card yet, it is
// simply made. A file with anything in it is only replaced when erase is
// true (and is kept as the backup like any other change).
Result format(const std::string& path, bool erase = false);

// Before a card is changed for the first time, the image as it was is kept
// beside it as <card>.bak, replacing an older one; later changes leave that
// backup alone. "The first time" is since the title started or since this was
// last called: call it when the manager's screen opens, and the backup is the
// card as it was before this visit.
void beginSession();

// Shift-JIS as the Dreamcast wrote it, to UTF-8 (as for Save's text): up to
// the first zero byte, without the spaces at the end.
std::string toUtf8(const uint8_t *text, size_t size);

}
