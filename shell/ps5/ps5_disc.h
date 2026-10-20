/*
	PSFlyCast - a game disc in a USB CD or DVD drive.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	Settings > Library > "USB disc drive" leaves the sandbox at the start (as
	"USB drives" does) and watches for drives (ps5_drive_cam.cpp; ps5_drive_usb.cpp
	for one the console does not attach itself). A disc in the drive is listed under
	the library's Disc Drive tab, named from the disc's own header, and plays
	from the drive:

	  - as the game asks for sectors, the drive is read in blocks, and every block
	    read stays in memory;
	  - behind the game, the rest of the disc is read into memory in order, from where
	    the game last read, so that once it is all there the drive rests and
	    nothing the game does waits for a seek;
	  - a block the drive will not read twice in a row slows the drive a step.

	The disc's sessions are read as the drive reports them, and its data must
	start with a Dreamcast header. A disc that is not a Dreamcast disc is
	listed, with why it is not played.

	The disc is given to the emulator as the path "ps5disc:/dev/cd1#MK-51000" (the
	drive, and the game's product number) and read by the Disc that
	ps5disc_parse() makes, which core/imgread/common.cpp asks first.
*/
#pragma once
#include <string>
#include <vector>

namespace ps5::disc
{
struct Info
{
	std::string device;		// /dev/cd1
	std::string path;		// ps5disc:/dev/cd1#MK-51000: what the library starts
	std::string title;		// from the disc's header; empty when it has none
	std::string id;			// the disc's product number
	std::string drive;		// the drive's maker and model, and its bus
	std::string state;		// what the entry says: "Ready", "No disc", "Not a Dreamcast disc"
	std::string problem;	// and why, when it is not ready
	bool present = false;	// a disc is in the drive and its table of contents was read
	bool playable = false;	// a Dreamcast disc: it can be started
	bool onUsb = true;
};

// The watcher: with the option on and the sandbox left. Safe to call again.
void start();
void stop();

// The drives and discs there are now, and a number that changes when they do.
std::vector<Info> list();
unsigned generation();

// Whether a path is a disc in a drive, the name Flycast keeps its files for it
// under (a state, the game's own settings), and whether the disc is there.
bool isPath(const std::string& path);
std::string fileName(const std::string& path);
bool exists(const std::string& path);

// The share of the disc that is in memory while one is being played (0 to 1),
// and -1 when none is played.
float cached();

// Opens the tray (the drive's button works again; some consoles lock it).
bool eject(const std::string& device, std::string& why);

// Copying the disc in a drive to an image in the games folder, in the background.
// CDI keeps the disc's sessions as they are; CHD is not compressed (chdman can do that). A GDI is
// not offered: Flycast reads one only as a GD-ROM's layout (data, audio, data at sector 45000).
enum class CopyFormat { Cdi, Chd };
struct CopyStatus
{
	bool running = false;
	bool finished = false;		// ended: ok says how
	bool ok = false;
	float progress = 0.f;		// 0 to 1
	std::string text;			// what it is doing, or how it ended
	std::string result;			// the finished file's path
};
bool copyStart(const std::string& device, CopyFormat format, const std::string& title, const std::string& id, std::string& why);
CopyStatus copyStatus();
void copyCancel();
void copyAcknowledge();		// the ending was seen

// The reason the watcher found no drive at all, for the entry that says so; empty when one was found.
std::string noDriveReason();
}
