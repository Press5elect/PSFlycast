/*
	PSFlyCast - updating the title from its releases on GitHub.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A release is a ZIP of the title's folder, published on the project's
	GitHub page with its SHA-256 beside it. Updating is what the release notes
	tell the user to do by hand - copy the ZIP's PPSA99247 folder over the one
	on the console - done by the title itself:

	  1. Ask GitHub for the project's releases (one HTTPS request, the
	     console's own client, ps5_covers.cpp) and take the one with the
	     highest version: its tag, its notes, the ZIP's address, size and
	     SHA-256. Drafts are left out; pre-releases are not.
	  2. Download the ZIP to <root>/update/, and check its size and SHA-256.
	     A ZIP that is not the one the release names is deleted.
	  3. Unpack it to <root>/update/new/. libzip checks each file against the
	     ZIP's own checksum as it is read.
	  4. Put the files in place: each file of the title that the ZIP has is
	     moved to <root>/update/old/, and the new one to where it was. Both
	     are renames within one folder tree, so a file is whole or not there.
	     The running eboot.bin is renamed like the rest: the console goes on
	     running the file it opened, and starts the new one next time. If
	     any rename is refused, every file already moved is moved back, and
	     the title is as it was.
	  5. The next start, which is the new version's, deletes <root>/update/.
	     (A version from before build 40 does not know to: after going back
	     to one, the folder stays until a newer version starts.)

	Only what a release ZIP has is replaced. Whatever is the user's is never
	touched, whatever a ZIP says: games, bios, covers, data, logs, patches,
	the settings (*.cfg) and logo.png.

	Every step is a line of flycast-boot.log ("update: ..."), with errno where
	a file operation failed.
*/
#include "ps5_frontend.h"
#include "ps5_build.h"
#include "ps5_diag.h"

#include "json.hpp"
#include <zip.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ps5::update
{
namespace
{

// The project's releases, newest first. (GitHub's "latest release" leaves out
// the ones marked pre-release, which this project's have been.)
constexpr const char *ReleasesUrl = "https://api.github.com/repos/Press5elect/PSFlycast/releases?per_page=20";
constexpr const char *TitleFolder = "PPSA99247/";

// ---- SHA-256 (FIPS 180-4)

struct Sha256
{
	uint32_t state[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	uint8_t block[64];
	size_t filled = 0;
	uint64_t length = 0;

	static uint32_t rotr(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

	void transform(const uint8_t *p)
	{
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
		};
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
		for (int i = 16; i < 64; i++)
		{
			const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
		for (int i = 0; i < 64; i++)
		{
			const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e; state[5] += f; state[6] += g; state[7] += h;
	}

	void add(const uint8_t *data, size_t size)
	{
		length += size;
		while (size > 0)
		{
			const size_t n = std::min(size, sizeof(block) - filled);
			memcpy(block + filled, data, n);
			filled += n;
			data += n;
			size -= n;
			if (filled == sizeof(block))
			{
				transform(block);
				filled = 0;
			}
		}
	}

	std::string hex()
	{
		const uint64_t bits = length * 8;
		const uint8_t one = 0x80, zero = 0;
		add(&one, 1);
		while (filled != 56)
			add(&zero, 1);
		uint8_t size[8];
		for (int i = 0; i < 8; i++)
			size[i] = (uint8_t)(bits >> (56 - 8 * i));
		add(size, 8);
		char text[65];
		for (int i = 0; i < 8; i++)
			snprintf(text + i * 8, 9, "%08x", state[i]);
		return text;
	}
};

// A file's SHA-256 in lower-case hex, or "" when it cannot be read.
std::string sha256Of(const std::string& path)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
		return "";
	Sha256 sum;
	std::vector<uint8_t> buffer(256 * 1024);
	size_t n;
	while ((n = fread(buffer.data(), 1, buffer.size(), f)) > 0)
		sum.add(buffer.data(), n);
	const bool failed = ferror(f) != 0;
	fclose(f);
	return failed ? "" : sum.hex();
}

// ---- versions

// "v1.0.2" or "1.0.2" as numbers; missing parts are 0.
std::vector<int> versionParts(const std::string& text)
{
	std::vector<int> parts;
	size_t at = !text.empty() && (text[0] == 'v' || text[0] == 'V') ? 1 : 0;
	while (at < text.size() && parts.size() < 4)
	{
		if (text[at] < '0' || text[at] > '9')
			break;
		int value = 0;
		while (at < text.size() && text[at] >= '0' && text[at] <= '9')
			value = std::min(value * 10 + (text[at++] - '0'), 1000000);
		parts.push_back(value);
		if (at < text.size() && text[at] == '.')
			at++;
		else
			break;
	}
	parts.resize(4, 0);
	return parts;
}

// Under 0, 0 or over 0 as a is older than, the same as or newer than b.
int compareVersions(const std::string& a, const std::string& b)
{
	const std::vector<int> pa = versionParts(a), pb = versionParts(b);
	for (size_t i = 0; i < pa.size(); i++)
		if (pa[i] != pb[i])
			return pa[i] < pb[i] ? -1 : 1;
	return 0;
}

// ---- a release, from GitHub's answer

struct Release
{
	std::string tag;			// "v1.0.2"
	std::string asset;			// "PSFlyCast-v1.0.2.zip"
	std::string url;
	uint64_t size = 0;
	std::string sha256;			// lower-case hex, or empty when the release names none
	std::vector<std::string> notes;
};

bool endsWith(const std::string& text, const char *end)
{
	const size_t n = strlen(end);
	return text.size() >= n && text.compare(text.size() - n, n, end) == 0;
}

std::string lower(std::string text)
{
	for (char& c : text)
		c = (char)tolower((unsigned char)c);
	return text;
}

// What changed, from the notes: the heading of each entry of their lists
// ("- **Heading.** More about it" gives "Heading"), the first few.
std::vector<std::string> notesOf(const std::string& body)
{
	std::vector<std::string> notes;
	size_t at = 0;
	while (at < body.size() && notes.size() < 4)
	{
		size_t end = body.find('\n', at);
		if (end == std::string::npos)
			end = body.size();
		std::string line = body.substr(at, end - at);
		at = end + 1;
		if (line.rfind("- **", 0) != 0)
			continue;
		const size_t close = line.find("**", 4);
		if (close == std::string::npos || close <= 4)
			continue;
		std::string heading = line.substr(4, close - 4);
		while (!heading.empty() && (heading.back() == '.' || heading.back() == ' '))
			heading.pop_back();
		if (!heading.empty())
			notes.push_back(heading);
	}
	return notes;
}

// The SHA-256 the notes give for a file: "SHA-256 of `name`: `hex`".
std::string sha256InNotes(const std::string& body, const std::string& name)
{
	const size_t named = body.find("`" + name + "`");
	if (named == std::string::npos)
		return "";
	const size_t open = body.find('`', named + name.size() + 2);
	if (open == std::string::npos || open > named + name.size() + 8)
		return "";
	const size_t close = body.find('`', open + 1);
	if (close == std::string::npos || close - open - 1 != 64)
		return "";
	const std::string hex = lower(body.substr(open + 1, 64));
	return hex.find_first_not_of("0123456789abcdef") == std::string::npos ? hex : "";
}

// One release of GitHub's list. False when it is not one to install: a
// draft, or one with no ZIP of the title.
bool readRelease(const nlohmann::json& entry, Release& release, std::string& why)
{
	if (!entry.is_object() || !entry.contains("tag_name") || !entry["tag_name"].is_string())
	{
		why = "GitHub's answer names no release";
		return false;
	}
	release = Release{};
	release.tag = entry["tag_name"].get<std::string>();
	if (entry.contains("draft") && entry["draft"].is_boolean() && entry["draft"].get<bool>())
	{
		why = release.tag + " is a draft";
		return false;
	}
	const std::string body = entry.contains("body") && entry["body"].is_string() ? entry["body"].get<std::string>() : "";
	release.notes = notesOf(body);
	// The title's ZIP: named for the tag, else the one ZIP that is not source.
	const std::string wanted = "PSFlyCast-" + release.tag + ".zip";
	const nlohmann::json none = nlohmann::json::array();
	const nlohmann::json& assets = entry.contains("assets") && entry["assets"].is_array() ? entry["assets"] : none;
	const nlohmann::json *chosen = nullptr;
	for (const auto& asset : assets)
	{
		if (!asset.is_object() || !asset.contains("name") || !asset["name"].is_string())
			continue;
		const std::string name = asset["name"].get<std::string>();
		if (name == wanted)
		{
			chosen = &asset;
			break;
		}
		if (chosen == nullptr && endsWith(lower(name), ".zip") && lower(name).find("source") == std::string::npos)
			chosen = &asset;
	}
	if (chosen == nullptr || !chosen->contains("browser_download_url") || !(*chosen)["browser_download_url"].is_string())
	{
		why = release.tag + " has no ZIP of the title";
		return false;
	}
	release.asset = (*chosen)["name"].get<std::string>();
	release.url = (*chosen)["browser_download_url"].get<std::string>();
	if (chosen->contains("size") && (*chosen)["size"].is_number_unsigned())
		release.size = (*chosen)["size"].get<uint64_t>();
	if (chosen->contains("digest") && (*chosen)["digest"].is_string())
	{
		const std::string digest = lower((*chosen)["digest"].get<std::string>());
		if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64)
			release.sha256 = digest.substr(7);
	}
	if (release.sha256.empty())
		release.sha256 = sha256InNotes(body, release.asset);
	// Only GitHub's own addresses are downloaded from.
	if (release.url.rfind("https://github.com/", 0) != 0)
	{
		why = "the ZIP's address is not on github.com";
		return false;
	}
	return true;
}

// The release with the highest version in GitHub's answer (a list of
// releases, or one).
bool parseRelease(const std::string& text, Release& release, std::string& why)
{
	try {
		const nlohmann::json answer = nlohmann::json::parse(text);
		if (!answer.is_array())
			return readRelease(answer, release, why);
		bool any = false;
		why = "The project has no release yet";
		for (const auto& entry : answer)
		{
			Release one;
			std::string reason;
			if (!readRelease(entry, one, reason))
			{
				if (!any)
					why = reason;
				continue;
			}
			if (!any || compareVersions(one.tag, release.tag) > 0)
				release = one;
			any = true;
		}
		if (any)
			why.clear();
		return any;
	} catch (const std::exception& e) {
		why = std::string("GitHub's answer could not be read: ") + e.what();
		return false;
	}
}

// ---- files

bool exists(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// Makes the folders of a file's path, below a root that exists.
void makeFoldersFor(const std::string& root, const std::string& relative)
{
	for (size_t slash = relative.find('/'); slash != std::string::npos; slash = relative.find('/', slash + 1))
	{
		const std::string folder = root + relative.substr(0, slash);
		if (mkdir(folder.c_str(), 0777) == 0)
			chmod(folder.c_str(), 0777);
	}
}

void removeTree(const std::string& path)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode))
	{
		unlink(path.c_str());
		return;
	}
	std::vector<std::string> names;
	if (DIR *list = opendir(path.c_str()))
	{
		while (const dirent *entry = readdir(list))
		{
			const std::string name = entry->d_name;
			if (name != "." && name != "..")
				names.push_back(name);
		}
		closedir(list);
	}
	for (const std::string& name : names)
		removeTree(path + "/" + name);
	rmdir(path.c_str());
}

// Whether a file of a ZIP may be put in the title's folder: a plain path
// inside it, and not one of the user's own files, whatever the ZIP says.
bool mayReplace(const std::string& relative)
{
	if (relative.empty() || relative[0] == '/' || relative.find("..") != std::string::npos
			|| relative.find('\\') != std::string::npos || relative.find(':') != std::string::npos)
		return false;
	const std::string top = relative.substr(0, relative.find('/'));
	for (const char *users : { "games", "bios", "covers", "data", "logs", "patches", "update", "radv-shader-cache" })
		if (top == users)
			return false;
	const std::string name = lower(relative.substr(relative.rfind('/') + 1));
	return !endsWith(name, ".cfg") && name != "logo.png";
}

// Unpacks a release ZIP's title folder to `staging` (with a trailing '/').
// `files` gets what was unpacked, as paths within the title's folder.
bool unpack(const std::string& zipPath, const std::string& staging, std::vector<std::string>& files, std::string& why,
		const std::atomic<bool>& stop)
{
	files.clear();
	if (mkdir(staging.c_str(), 0777) != 0 && errno != EEXIST)
	{
		why = staging + " cannot be made: " + strerror(errno);
		return false;
	}
	int code = 0;
	zip_t *zip = zip_open(zipPath.c_str(), ZIP_RDONLY, &code);
	if (zip == nullptr)
	{
		why = "the ZIP cannot be opened (libzip " + std::to_string(code) + ")";
		return false;
	}
	bool ok = true;
	std::vector<char> buffer(256 * 1024);
	const zip_int64_t count = zip_get_num_entries(zip, 0);
	for (zip_int64_t i = 0; ok && i < count; i++)
	{
		if (stop)
		{
			why = "stopped";
			ok = false;
			break;
		}
		zip_stat_t st;
		if (zip_stat_index(zip, (zip_uint64_t)i, 0, &st) != 0 || st.name == nullptr)
			continue;
		const std::string name = st.name;
		if (name.rfind(TitleFolder, 0) != 0 || name.back() == '/')
			continue;		// a folder, or something beside the title's folder
		const std::string relative = name.substr(strlen(TitleFolder));
		if (!mayReplace(relative))
		{
			diag::mark("update: %s is left out (not the title's to replace)", relative.c_str());
			continue;
		}
		zip_file_t *in = zip_fopen_index(zip, (zip_uint64_t)i, 0);
		if (in == nullptr)
		{
			why = relative + " cannot be read from the ZIP";
			ok = false;
			break;
		}
		makeFoldersFor(staging, relative);
		const std::string target = staging + relative;
		FILE *out = fopen(target.c_str(), "wb");
		if (out == nullptr)
		{
			why = relative + " cannot be written: " + strerror(errno);
			zip_fclose(in);
			ok = false;
			break;
		}
		zip_uint64_t written = 0;
		zip_int64_t n;
		while ((n = zip_fread(in, buffer.data(), buffer.size())) > 0)
		{
			if (fwrite(buffer.data(), 1, (size_t)n, out) != (size_t)n)
			{
				why = relative + " cannot be written: " + strerror(errno);
				ok = false;
				break;
			}
			written += (zip_uint64_t)n;
		}
		if (ok && n < 0)
		{
			why = relative + " is damaged in the ZIP";
			ok = false;
		}
		if (fclose(out) != 0 && ok)
		{
			why = relative + " cannot be written: " + strerror(errno);
			ok = false;
		}
		// libzip compares the file's checksum with the ZIP's when it is closed.
		if (zip_fclose(in) != 0 && ok)
		{
			why = relative + " does not match the ZIP's checksum";
			ok = false;
		}
		if (ok && (st.valid & ZIP_STAT_SIZE) != 0 && written != st.size)
		{
			why = relative + " is cut short in the ZIP";
			ok = false;
		}
		if (ok)
		{
			chmod(target.c_str(), 0777);
			files.push_back(relative);
		}
	}
	zip_close(zip);
	if (ok && (std::find(files.begin(), files.end(), "eboot.bin") == files.end()
			|| std::find(files.begin(), files.end(), "sce_sys/param.json") == files.end()))
	{
		why = "the ZIP is not a PSFlyCast release (no " + std::string(TitleFolder) + "eboot.bin in it)";
		ok = false;
	}
	return ok;
}

// Puts the unpacked files in place: for each, the title's file goes to
// `old` and the new one from `staging` to where it was. The program itself
// last. When a rename is refused, everything already moved is moved back.
// `restored` says whether the title is then as it was.
bool swapIn(const std::string& root, const std::string& staging, const std::string& old, std::vector<std::string> files,
		std::string& why, bool& restored)
{
	restored = true;
	std::stable_partition(files.begin(), files.end(), [](const std::string& file) { return file != "eboot.bin"; });
	struct Moved { std::string file; bool hadOld; };
	std::vector<Moved> moved;
	bool ok = true;
	for (const std::string& file : files)
	{
		// Each cheat file and licence text would be a line: the others are.
		const bool say = file.rfind("cheats/", 0) != 0 && file.rfind("licenses/", 0) != 0;
		const std::string target = root + file;
		const bool hadOld = exists(target);
		makeFoldersFor(root, file);
		if (hadOld)
		{
			makeFoldersFor(old, file);
			if (rename(target.c_str(), (old + file).c_str()) != 0)
			{
				why = "this version's " + file + " could not be moved aside: " + strerror(errno);
				diag::mark("update: %s: moving this version's aside failed: %s (errno %d)", file.c_str(), strerror(errno), errno);
				ok = false;
				break;
			}
		}
		if (rename((staging + file).c_str(), target.c_str()) != 0)
		{
			why = "the new " + file + " could not be put in place: " + strerror(errno);
			diag::mark("update: %s: putting the new one in place failed: %s (errno %d)", file.c_str(), strerror(errno), errno);
			if (hadOld && rename((old + file).c_str(), target.c_str()) != 0)
			{
				diag::mark("update: %s: putting this version's back failed: %s (errno %d)", file.c_str(), strerror(errno), errno);
				restored = false;
			}
			ok = false;
			break;
		}
		if (say)
			diag::mark("update: %s: %s", file.c_str(), hadOld ? "this version's moved aside, the new one in place" : "new, in place");
		moved.push_back({ file, hadOld });
	}
	if (ok)
	{
		diag::mark("update: %d files in place", (int)moved.size());
		return true;
	}
	// Back to what it was, the last file first.
	for (auto it = moved.rbegin(); it != moved.rend(); ++it)
	{
		const std::string target = root + it->file;
		if (rename(target.c_str(), (staging + it->file).c_str()) != 0)
		{
			diag::mark("update: %s: taking the new one out again failed: %s (errno %d)", it->file.c_str(), strerror(errno), errno);
			if (it->hadOld)
				restored = false;
			continue;
		}
		if (it->hadOld && rename((old + it->file).c_str(), target.c_str()) != 0)
		{
			diag::mark("update: %s: putting this version's back failed: %s (errno %d)", it->file.c_str(), strerror(errno), errno);
			restored = false;
		}
	}
	diag::mark("update: %d files moved back: %s", (int)moved.size(),
			restored ? "the title is as it was" : "NOT every file could be put back (see the lines above)");
	return false;
}

// ---- this build, and the state the interface shows

std::mutex mutex;
Status current;
Release found;
std::atomic<bool> busy;
std::atomic<bool> stop;
bool offered;				// the start-up's offer was made, or is not to be
std::string ownTag;			// "v1.0.1" for a release's build, empty for a test build
std::string buildLine;		// BUILD.txt's first line

void set(Phase phase)
{
	std::lock_guard<std::mutex> lock(mutex);
	current.phase = phase;
}

void fail(const std::string& why, bool restored = true)
{
	diag::mark("update: failed: %s", why.c_str());
	std::lock_guard<std::mutex> lock(mutex);
	current.phase = Phase::Failed;
	current.error = why;
	current.restored = restored;
}

std::string workDir()
{
	return rootDir + "update/";
}

std::string skipFile()
{
	return rootDir + "data/update-skip.txt";
}

std::string readLine(const std::string& path)
{
	char line[256] = "";
	if (FILE *f = fopen(path.c_str(), "r"))
	{
		if (fgets(line, sizeof(line), f) == nullptr)
			line[0] = '\0';
		fclose(f);
	}
	std::string text = line;
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		text.pop_back();
	return text;
}

// The release this build is compared with: its own, or for a test build the
// one it follows.
std::string baseVersion()
{
	return ownTag.empty() ? std::string("v") + PS5_VERSION : ownTag;
}

bool doCheck()
{
	set(Phase::Checking);
	diag::mark("update: asking GitHub for the newest release");
	std::vector<uint8_t> answer;
	const int status = net::get(ReleasesUrl, answer, 20);
	if (status != 200)
	{
		if (status < 0)
		{
			char code[16];
			snprintf(code, sizeof(code), "%#x", (unsigned)net::lastError());
			fail(net::lastError() == 0 ? "The console is not connected to the internet"
					: std::string("GitHub did not answer (") + code + ")");
		}
		else if (status == 403 || status == 429)
			fail("GitHub takes no more questions from this network for now. Try again in an hour");
		else if (status == 404)
			fail("The project has no release yet");
		else
			fail("GitHub answered " + std::to_string(status));
		return false;
	}
	Release release;
	std::string why;
	if (!parseRelease(std::string(answer.begin(), answer.end()), release, why))
	{
		fail(why);
		return false;
	}
	const bool newer = compareVersions(release.tag, baseVersion()) > 0;
	diag::mark("update: the newest release is %s (%s, %llu bytes, sha256 %s); this is %s: %s", release.tag.c_str(),
			release.asset.c_str(), (unsigned long long)release.size, release.sha256.empty() ? "not given" : release.sha256.c_str(),
			thisBuild().c_str(), newer ? "there is an update" : "nothing newer");
	std::lock_guard<std::mutex> lock(mutex);
	found = release;
	current.phase = newer ? Phase::Available : Phase::UpToDate;
	current.latest = release.tag;
	current.notes = release.notes;
	current.size = release.size;
	current.done = 0;
	current.error.clear();
	return true;
}

void doInstall()
{
	Release release;
	{
		std::lock_guard<std::mutex> lock(mutex);
		release = found;
	}
	if (release.url.empty())
	{
		if (!doCheck())
			return;
		std::lock_guard<std::mutex> lock(mutex);
		release = found;
	}
	if (release.sha256.empty())
	{
		fail(release.tag + " gives no SHA-256 to check the download against");
		return;
	}
	const std::string work = workDir();
	removeTree(work + "new");
	mkdir(work.c_str(), 0777);
	chmod(work.c_str(), 0777);
	const std::string zipPath = work + "download.zip";
	unlink(zipPath.c_str());

	// 1. The ZIP.
	{
		std::lock_guard<std::mutex> lock(mutex);
		current.phase = Phase::Downloading;
		current.latest = release.tag;
		current.size = release.size;
		current.done = 0;
	}
	diag::mark("update: downloading %s", release.url.c_str());
	const int status = net::download(release.url, zipPath, [](uint64_t done) {
		std::lock_guard<std::mutex> lock(mutex);
		current.done = done;
		return !stop.load();
	});
	if (status == -2)
	{
		diag::mark("update: the download was stopped");
		removeTree(work);
		set(Phase::Available);
		return;
	}
	if (status != 200)
	{
		char code[32];
		snprintf(code, sizeof(code), status < 0 ? "%#x" : "HTTP %d", status < 0 ? (unsigned)net::lastError() : (unsigned)status);
		removeTree(work);
		if (status < 0 && net::lastError() == 0)
			fail("The download could not start: the console is not connected, or the update folder cannot be written");
		else
			fail(std::string("The download did not finish (") + code + ")");
		return;
	}

	// 2. Is it the file the release names?
	set(Phase::Verifying);
	struct stat st{};
	stat(zipPath.c_str(), &st);
	const std::string sum = sha256Of(zipPath);
	diag::mark("update: downloaded %lld bytes, sha256 %s", (long long)st.st_size, sum.c_str());
	if ((release.size != 0 && (uint64_t)st.st_size != release.size) || sum != release.sha256)
	{
		removeTree(work);
		fail("The download is not the file the release names (its SHA-256 differs). Nothing was changed");
		return;
	}

	// 3. Unpacked beside the title's files.
	set(Phase::Unpacking);
	std::vector<std::string> files;
	std::string why;
	if (!unpack(zipPath, work + "new/", files, why, stop))
	{
		removeTree(work);
		if (stop)
		{
			diag::mark("update: unpacking was stopped");
			set(Phase::Available);
		}
		else
			fail("Unpacking failed: " + why + ". Nothing was changed");
		return;
	}
	diag::mark("update: %d files unpacked", (int)files.size());

	// 4. In place. Not stopped half way.
	set(Phase::Swapping);
	removeTree(work + "old");
	mkdir((work + "old").c_str(), 0777);
	bool restored = true;
	if (!swapIn(rootDir, work + "new/", work + "old/", files, why, restored))
	{
		if (restored)
			removeTree(work);
		fail("The console did not let the files be replaced: " + why + (restored ? ". The title is as it was"
				: ". Some files could not be put back: copy the release's ZIP over the title's folder"), restored);
		return;
	}
	if (FILE *f = fopen((work + "installed.txt").c_str(), "w"))
	{
		fprintf(f, "%s\n", release.tag.c_str());
		fclose(f);
	}
	unlink(zipPath.c_str());
	removeTree(work + "new");
	diag::mark("update: %s is in place; it starts the next time the title is opened", release.tag.c_str());
	set(Phase::Installed);
}

void run(void (*work)())
{
	if (busy.exchange(true))
		return;
	stop = false;
	std::thread([work] {
		work();
		busy = false;
	}).detach();
}

} // namespace

void init()
{
	buildLine = readLine(rootDir + "BUILD.txt");
	// "PSFlyCast v1.0.1, build 39, built ..." is a release's; a test build's has no tag.
	ownTag.clear();
	if (buildLine.rfind("PSFlyCast v", 0) == 0)
		ownTag = buildLine.substr(10, buildLine.find_first_of(", ", 10) - 10);
	diag::mark("update: this is %s", thisBuild().c_str());

	// What an update left behind.
	const std::string work = workDir();
	const std::string installed = readLine(work + "installed.txt");
	if (!installed.empty())
	{
		// The title started with the new files in place: the ones before
		// them are not needed again. (What started is named, in case it is
		// not the version that was installed.)
		diag::mark("update: %s was installed, and %s has started; the previous version's files (update/old) are deleted",
				installed.c_str(), thisBuild().c_str());
		removeTree(work);
	}
	else if (exists(work))
	{
		diag::mark("update: an unfinished download is deleted");
		removeTree(work);
	}
	// A release's build looks for a newer one as it starts, unless that is off.
	if (isRelease() && options().updateCheck)
		check();
	else
		offered = true;
}

std::string thisVersion()
{
	return baseVersion();
}

bool isRelease()
{
	return !ownTag.empty();
}

std::string thisBuild()
{
	if (isRelease())
		return ownTag + ", build " + std::to_string(PS5_BUILD_NUMBER);
	return "Test build " + std::to_string(PS5_BUILD_NUMBER) + ", after v" PS5_VERSION;
}

Status status()
{
	std::lock_guard<std::mutex> lock(mutex);
	return current;
}

void check()
{
	run([] { doCheck(); });
}

void install()
{
	run(doInstall);
}

void cancel()
{
	stop = true;
}

void skip()
{
	const std::string tag = status().latest;
	if (tag.empty())
		return;
	if (FILE *f = fopen(skipFile().c_str(), "w"))
	{
		fprintf(f, "%s\n", tag.c_str());
		fclose(f);
	}
	diag::mark("update: %s will not be offered at start-up", tag.c_str());
}

bool offerAtStart()
{
	if (offered)
		return false;
	const Status now = status();
	if (now.phase == Phase::Checking || now.phase == Phase::Idle)
		return false;		// not known yet
	offered = true;
	return now.phase == Phase::Available && now.latest != readLine(skipFile());
}

} // namespace ps5::update

#ifdef PS5_UPDATE_TEST
// The parts that need no console, for a test on the build machine.
namespace ps5::update::test
{
std::string sha256(const std::string& path) { return sha256Of(path); }
int compare(const std::string& a, const std::string& b) { return compareVersions(a, b); }
bool release(const std::string& json, std::string& tag, std::string& asset, std::string& url, uint64_t& size, std::string& sum,
		std::vector<std::string>& notes, std::string& why)
{
	Release r;
	const bool ok = parseRelease(json, r, why);
	tag = r.tag; asset = r.asset; url = r.url; size = r.size; sum = r.sha256; notes = r.notes;
	return ok;
}
bool unpackTo(const std::string& zip, const std::string& staging, std::vector<std::string>& files, std::string& why)
{
	std::atomic<bool> never{false};
	return unpack(zip, staging, files, why, never);
}
bool swap(const std::string& root, const std::string& staging, const std::string& old, const std::vector<std::string>& files,
		std::string& why, bool& restored)
{
	return swapIn(root, staging, old, files, why, restored);
}
}
#endif
