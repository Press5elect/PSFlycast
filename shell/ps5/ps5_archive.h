/*
	PSFlyCast - disc images inside zip, 7z and rar archives.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	An archive that holds a disc image (.gdi with its tracks, .cue with its
	tracks, .cdi or .chd) is listed in the library like the image itself. When
	it is started, the image is unpacked into <root>/cache/archive/ (the last
	one stays there, so starting it again is quick; the next archive replaces
	it) and played from there.
*/
#pragma once
#include <string>

struct LoadProgress;

namespace ps5::archive
{
// A file name that is a zip, 7z or rar archive.
bool isArchiveName(const std::string& path);

// Whether the archive holds a disc image, found by listing it (not unpacking);
// remembered per file, size and date.
bool holdsDisc(const std::string& path);

// For an archive with a disc image: unpacks it and gives the path of the image
// to play. False when the path is not such an archive; throws FlycastException
// when unpacking fails.
bool resolve(const std::string& path, std::string& image, LoadProgress *progress);

// Housekeeping: where the unpacked images are, how many bytes a folder holds, and emptying one (the
// folder itself stays).
std::string cacheFolder();
unsigned long long folderBytes(const std::string& folder);
void emptyFolder(const std::string& folder);
}
