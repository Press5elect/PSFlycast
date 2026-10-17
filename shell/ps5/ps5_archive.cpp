/*
	PSFlyCast - disc images inside zip, 7z and rar archives (see ps5_archive.h).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
*/
#include "types.h"
#include "archive/archive.h"
#include "emulator.h"
#include "imgread/common.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include "ps5_archive.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <mutex>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace ps5::archive
{
namespace
{
std::string lower(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

std::string extensionOf(const std::string& name)
{
	const size_t slash = name.find_last_of('/');
	const size_t dot = name.find_last_of('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
		return std::string();
	return lower(name.substr(dot + 1));
}

std::string directoryOf(const std::string& name)
{
	const size_t slash = name.find_last_of('/');
	return slash == std::string::npos ? std::string() : name.substr(0, slash + 1);
}

// The disc image to play: a .gdi, else .cue, .cdi, .chd; the shortest path, then the first by name.
std::string imageIn(const std::vector<ArchiveEntry>& entries)
{
	static const char *order[] = { "gdi", "cue", "cdi", "chd" };
	for (const char *ext : order)
	{
		const ArchiveEntry *best = nullptr;
		for (const ArchiveEntry& e : entries)
		{
			if (extensionOf(e.name) != ext || e.name.rfind("__MACOSX/", 0) == 0)
				continue;
			const size_t slash = e.name.find_last_of('/');
			if (slash != std::string::npos && e.name.compare(slash + 1, 2, "._") == 0)
				continue;
			if (best == nullptr || e.name.size() < best->name.size() || (e.name.size() == best->name.size() && e.name < best->name))
				best = &e;
		}
		if (best != nullptr)
			return best->name;
	}
	return std::string();
}

// The files a .gdi or .cue names, in the order given.
std::vector<std::string> tracksOf(const std::string& text, bool gdi)
{
	std::vector<std::string> files;
	size_t at = 0;
	while (at < text.size())
	{
		size_t end = text.find_first_of("\r\n", at);
		if (end == std::string::npos)
			end = text.size();
		const std::string line = text.substr(at, end - at);
		at = end + 1;
		std::string name;
		const size_t open = line.find('"');
		if (open != std::string::npos)
		{
			const size_t close = line.find('"', open + 1);
			if (close != std::string::npos)
				name = line.substr(open + 1, close - open - 1);
		}
		else if (gdi)
		{
			// track lba type size file offset: the fifth word.
			size_t p = 0;
			for (int word = 0; word < 4 && p != std::string::npos; word++)
			{
				p = line.find_first_not_of(" \t", p);
				p = p == std::string::npos ? p : line.find_first_of(" \t", p);
			}
			if (p != std::string::npos)
			{
				const size_t b = line.find_first_not_of(" \t", p);
				if (b != std::string::npos)
					name = line.substr(b, line.find_first_of(" \t", b) - b);
			}
		}
		if (name.empty())
			continue;
		if (!gdi && line.find("FILE") == std::string::npos && line.find("file") == std::string::npos)
			continue;
		if (std::find(files.begin(), files.end(), name) == files.end())
			files.push_back(name);
	}
	return files;
}

std::string cacheRoot()
{
	return ps5::rootDir + "cache/archive/";
}

void makeDirectories(const std::string& dir)
{
	for (size_t i = 1; i <= dir.size(); i++)
		if (i == dir.size() || dir[i] == '/')
			::mkdir(dir.substr(0, i).c_str(), 0777);
}

void removeTree(const std::string& path)
{
	if (DIR *d = opendir(path.c_str()))
	{
		while (dirent *e = readdir(d))
		{
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			const std::string child = path + "/" + e->d_name;
			struct stat st{};
			if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				removeTree(child);
			else
				unlink(child.c_str());
		}
		closedir(d);
	}
	rmdir(path.c_str());
}

bool safeName(const std::string& name)
{
	if (name.empty() || name[0] == '/')
		return false;
	size_t at = 0;
	while (at <= name.size())
	{
		size_t end = name.find('/', at);
		if (end == std::string::npos)
			end = name.size();
		if (name.compare(at, end - at, "..") == 0)
			return false;
		at = end + 1;
	}
	return true;
}

std::string keyOf(const std::string& path, const hostfs::FileInfo& info)
{
	// FNV-1a over the path, size and date.
	uint64_t h = 1469598103934665603ull;
	const std::string s = path + "|" + std::to_string(info.size) + "|" + std::to_string((long long)info.updateTime);
	for (unsigned char c : s)
		h = (h ^ c) * 1099511628211ull;
	char buffer[20];
	snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)h);
	return buffer;
}

std::unique_ptr<Archive> open(const std::string& path)
{
	return std::unique_ptr<Archive>(OpenArchive(path));
}

std::mutex memoMutex;
std::map<std::string, bool> memo;		// path|size|date -> holds a disc image
std::mutex resolveMutex;
}

bool isArchiveName(const std::string& path)
{
	const std::string ext = extensionOf(path);
	return ext == "zip" || ext == "7z" || ext == "rar";
}

bool holdsDisc(const std::string& path)
{
	if (!isArchiveName(path))
		return false;
	hostfs::FileInfo info;
	try {
		info = hostfs::storage().getFileInfo(path);
	} catch (...) {
		return false;
	}
	const std::string key = keyOf(path, info);
	{
		std::lock_guard<std::mutex> lock(memoMutex);
		const auto it = memo.find(key);
		if (it != memo.end())
			return it->second;
	}
	bool found = false;
	if (auto archive = open(path))
	{
		std::vector<ArchiveEntry> entries;
		if (archive->list(entries))
			found = !imageIn(entries).empty();
	}
	std::lock_guard<std::mutex> lock(memoMutex);
	memo[key] = found;
	return found;
}

std::string cacheFolder()
{
	return cacheRoot();
}

unsigned long long folderBytes(const std::string& folder)
{
	unsigned long long total = 0;
	if (DIR *d = opendir(folder.c_str()))
	{
		while (dirent *e = readdir(d))
		{
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			const std::string child = folder + "/" + e->d_name;
			struct stat st{};
			if (lstat(child.c_str(), &st) != 0)
				continue;
			total += S_ISDIR(st.st_mode) ? folderBytes(child) : (unsigned long long)st.st_size;
		}
		closedir(d);
	}
	return total;
}

void emptyFolder(const std::string& folder)
{
	if (DIR *d = opendir(folder.c_str()))
	{
		std::vector<std::string> names;
		while (dirent *e = readdir(d))
			if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
				names.push_back(e->d_name);
		closedir(d);
		for (const std::string& name : names)
		{
			const std::string child = folder + "/" + name;
			struct stat st{};
			if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				removeTree(child);
			else
				unlink(child.c_str());
		}
	}
}

bool resolve(const std::string& path, std::string& image, LoadProgress *progress)
{
	if (!isArchiveName(path))
		return false;
	hostfs::FileInfo info;
	try {
		info = hostfs::storage().getFileInfo(path);
	} catch (...) {
		return false;
	}
	std::lock_guard<std::mutex> once(resolveMutex);
	const std::string key = keyOf(path, info);
	const std::string folder = cacheRoot() + key + "/";
	const std::string marker = folder + ".unpacked";
	// Unpacked before and still there.
	if (FILE *f = fopen(marker.c_str(), "rb"))
	{
		char name[1024] = {};
		const size_t n = fread(name, 1, sizeof(name) - 1, f);
		fclose(f);
		name[n] = 0;
		struct stat st{};
		if (n > 0 && stat((folder + name).c_str(), &st) == 0)
		{
			image = folder + name;
			return true;
		}
	}

	auto archive = open(path);
	if (!archive)
		return false;
	std::vector<ArchiveEntry> entries;
	if (!archive->list(entries))
		return false;
	const std::string main = imageIn(entries);
	if (main.empty())
		return false;

	if (progress != nullptr)
		progress->label = "Unpacking the archive";
	// What the image needs: its tracks, if it names them.
	std::vector<std::string> wanted{ main };
	const std::string ext = extensionOf(main);
	if (ext == "gdi" || ext == "cue")
	{
		std::string text;
		if (!archive->extract(main, [&](const void *data, size_t size) {
				if (text.size() + size > (1u << 20))
					return false;
				text.append((const char *)data, size);
				return true;
			}))
			throw FlycastException(i18n::Ts("The disc image in the archive could not be read"));
		const std::string dir = directoryOf(main);
		for (const std::string& file : tracksOf(text, ext == "gdi"))
		{
			// By the name given, then without regard to case.
			const std::string wantedName = dir + file;
			std::string found;
			for (const ArchiveEntry& e : entries)
				if (e.name == wantedName)
				{
					found = e.name;
					break;
				}
			if (found.empty())
				for (const ArchiveEntry& e : entries)
					if (lower(e.name) == lower(wantedName))
					{
						found = e.name;
						break;
					}
			if (found.empty())
				throw FlycastException(strprintf(i18n::T("The archive has no file %s, which the disc image needs"), file.c_str()));
			wanted.push_back(found);
		}
	}
	uint64_t total = 0;
	for (const std::string& name : wanted)
		for (const ArchiveEntry& e : entries)
			if (e.name == name)
				total += e.size;

	// The room: the last archive is replaced, so only this one is counted.
	removeTree(cacheRoot());
	makeDirectories(folder);
	struct statvfs vfs{};
	if (statvfs(folder.c_str(), &vfs) == 0)
	{
		const uint64_t room = (uint64_t)vfs.f_bavail * vfs.f_frsize;
		if (room < total + (64u << 20))
			throw FlycastException(strprintf(i18n::T("Not enough free space to unpack the archive: it needs %llu MB, and %llu MB are free"),
					(unsigned long long)(total >> 20), (unsigned long long)(room >> 20)));
	}
	ps5::diag::mark("archive: unpacking %s: %zu file(s), %llu MB", path.c_str(), wanted.size(), (unsigned long long)(total >> 20));

	uint64_t done = 0;
	for (const std::string& name : wanted)
	{
		if (!safeName(name))
			throw FlycastException(i18n::Ts("The archive has a file name that is not allowed"));
		const std::string target = folder + name;
		makeDirectories(target.substr(0, target.find_last_of('/')));
		FILE *out = fopen(target.c_str(), "wb");
		if (out == nullptr)
			throw FlycastException(strprintf(i18n::T("Cannot write %s"), target.c_str()));
		bool cancelled = false;
		bool wrote = true;
		const bool ok = archive->extract(name, [&](const void *data, size_t size) {
			if (progress != nullptr && progress->cancelled)
			{
				cancelled = true;
				return false;
			}
			if (fwrite(data, 1, size, out) != size)
			{
				wrote = false;
				return false;
			}
			done += size;
			if (progress != nullptr && total > 0)
				progress->progress = (float)((double)done / (double)total);
			return true;
		});
		fclose(out);
		if (cancelled)
		{
			removeTree(cacheRoot());
			throw FlycastException(i18n::Ts("Cancelled"));
		}
		if (!ok || !wrote)
		{
			removeTree(cacheRoot());
			throw FlycastException(strprintf(wrote ? i18n::T("The archive could not be unpacked (%s)") : i18n::T("No room left to unpack the archive (%s)"),
					name.c_str()));
		}
	}
	if (FILE *f = fopen(marker.c_str(), "wb"))
	{
		fwrite(main.data(), 1, main.size(), f);
		fclose(f);
	}
	image = folder + main;
	return true;
}
}

// Flycast's disc drivers (core/imgread/common.cpp): an archive with a disc image in it.
Disc *OpenDisc(const std::string& path, std::vector<u8> *digest);

Disc *ps5archive_parse(const char *path, std::vector<u8> *digest)
{
	if (path == nullptr)
		return nullptr;
	std::string image;
	if (!ps5::archive::resolve(path, image, nullptr))
		return nullptr;
	return OpenDisc(image, digest);
}

bool ps5ResolveArchive(const char *path, std::string& image, LoadProgress *progress)
{
	return path != nullptr && ps5::archive::resolve(path, image, progress);
}

bool ps5ArchiveHoldsDisc(const std::string& path)
{
	return ps5::archive::holdsDisc(path);
}
