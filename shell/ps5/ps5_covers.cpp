/*
	PSFlyCast - cover downloads.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	A game with no cover gets one from the libretro thumbnails collection
	(github.com/libretro-thumbnails/Sega_-_Dreamcast, the box art RetroArch
	shows), looked up by the game's file name, which is how that collection is
	named (Redump names, "Re-Volt (USA)"). The picture is saved as
	<root>/covers/<file name>.png, where the library already looks for the
	user's own covers; a name the collection does not have is remembered in
	covers/not-found.txt and not asked for again.

	HTTPS is the console's own, libSceHttp2 on one worker thread, with the
	calls, pool sizes and time-outs of PS5SX2's cover fetcher (Swordpdf,
	ps5/frontend/fe_ps5.cpp), which runs in a title's sandbox.

	Two sources hold the same pictures under the same names. The first is the
	collection on GitHub, over HTTPS. On one console Flycast's requests to it
	ended in libSceHttp2's time-out after 20 seconds each (0x817b1068, builds
	14 and 15, in which the SMB code's sockets were failing or stuck) and then
	worked in build 16; why is not established. So when the first source does
	not answer, a few checks say in the boot log how far a request gets (a
	plain connection, HTTP to an address, HTTP to a name), and the downloads
	go on from libretro's own server over plain HTTP (thumbnails.libretro.com,
	RetroArch's source), which needs no TLS.

	The same client is Flycast's HTTP client on the console (http::get, at the
	end of this file; core/oslib/http_client.cpp has none for the PS5), which
	is what Flycast's own scraper needs to ask TheGamesDB for a game's
	description, release date and box art. One request at a time: the worker
	here and the scraper's thread take turns.
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "types.h"
#include "oslib/http_client.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern "C"
{
int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceSslInit(size_t poolSize);
int sceHttp2Init(int netPool, int sslContext, size_t poolSize, int maxRequests);
int sceHttp2CreateTemplate(int context, const char *userAgent, int httpVersion, int autoProxy);
int sceHttp2CreateRequestWithURL(int templateId, const char *method, const char *url, uint64_t contentLength);
int sceHttp2DeleteRequest(int request);
int sceHttp2SendRequest(int request, const void *data, size_t size);
int sceHttp2GetStatusCode(int request, int *status);
int sceHttp2ReadData(int request, void *data, size_t size);
int sceHttp2SetResolveTimeOut(int id, uint32_t usec);
int sceHttp2SetConnectTimeOut(int id, uint32_t usec);
int sceHttp2SetSendTimeOut(int id, uint32_t usec);
int sceHttp2SetRecvTimeOut(int id, uint32_t usec);
int sceHttp2SetTimeOut(int id, uint32_t usec);
int sceHttp2SetAutoRedirect(int id, int enable);
int sceNetCtlInit(void);
int sceNetCtlGetState(int *state);
int sceNetCtlGetInfo(int code, void *info);
}

namespace ps5::covers
{
namespace
{
struct Source
{
	const char *name;
	const char *baseUrl;
	bool links;		// a regional duplicate is a text file naming the picture (a git link)
};
constexpr Source Sources[] = {
	{ "GitHub (HTTPS)", "https://raw.githubusercontent.com/libretro-thumbnails/Sega_-_Dreamcast/master/Named_Boxarts/", true },
	{ "thumbnails.libretro.com (HTTP)", "http://thumbnails.libretro.com/Sega%20-%20Dreamcast/Named_Boxarts/", false },
};
constexpr int SourceCount = 2;

std::mutex mutex;
std::condition_variable wake;
std::deque<std::string> queue;
std::set<std::string> asked;
std::set<std::string> notFound;
bool notFoundLoaded;
bool started;
std::atomic<unsigned> currentGeneration{1};
std::atomic<int> pending{0};
std::atomic<bool> offline{false};

struct Http
{
	bool tried = false, ok = false;
	int templateId = -1;
	int reported = 0;

	bool init()
	{
		if (tried)
			return ok;
		tried = true;
		// Is there a network at all? An offline console must not wait on the
		// resolver. State 3 is "address obtained".
		const int nc = sceNetCtlInit();
		int state[4] = { -1, 0, 0, 0 };	// room to spare
		const int gs = sceNetCtlGetState(state);
		if (gs == 0 && state[0] >= 0 && state[0] < 3)
		{
			diag::mark("covers: the console is not connected (netctl %#x, state %d)", (unsigned)nc, state[0]);
			return false;
		}
		const int net = sceNetInit();	// an error only means it was up already
		const int pool = sceNetPoolCreate("flycast-covers", 64 * 1024, 0);
		const int ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
		const int context = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
		templateId = context >= 0 ? sceHttp2CreateTemplate(context, "PSFlyCast/1.0", 3, 1) : -1;
		diag::mark("covers: https net %#x pool %#x ssl %#x http2 %#x template %#x", (unsigned)net, (unsigned)pool,
				(unsigned)ssl, (unsigned)context, (unsigned)templateId);
		ok = templateId >= 0;
		return ok;
	}

	// The HTTP status, or -1 when the request could not be made: lastError
	// then has libSceHttp2's code. `seconds` is the whole request's limit.
	int lastError = 0;

	int get(const std::string& url, std::vector<uint8_t>& out, unsigned seconds = 20)
	{
		out.clear();
		lastError = 0;
		if (!init())
			return -1;
		const int request = sceHttp2CreateRequestWithURL(templateId, "GET", url.c_str(), 0);
		if (request < 0)
		{
			lastError = request;
			return -1;
		}
		const unsigned phase = std::min(seconds, 10u) * 1000 * 1000;
		sceHttp2SetResolveTimeOut(request, phase);
		sceHttp2SetConnectTimeOut(request, phase);
		sceHttp2SetSendTimeOut(request, phase);
		sceHttp2SetRecvTimeOut(request, phase);
		sceHttp2SetTimeOut(request, seconds * 1000 * 1000);
		sceHttp2SetAutoRedirect(request, 1);
		int status = -1;
		const int sent = sceHttp2SendRequest(request, nullptr, 0);
		const int got = sent == 0 ? sceHttp2GetStatusCode(request, &status) : -1;
		if (sent != 0 || got != 0)
		{
			// The first few say why in the log: the codes are libSceHttp2's.
			lastError = sent != 0 ? sent : got;
			if (reported++ < 6)
				diag::mark("covers: request failed: send %#x, status %#x (%s)", (unsigned)sent, (unsigned)got,
						url.substr(0, 48).c_str());
			status = -1;
		}
		else if (status >= 200 && status < 300)
		{
			std::vector<uint8_t> chunk(64 * 1024);
			for (;;)
			{
				const int n = sceHttp2ReadData(request, chunk.data(), chunk.size());
				if (n < 0)
				{
					if (reported++ < 4)
						diag::mark("covers: reading the answer failed: %#x", (unsigned)n);
					status = -1;
					break;
				}
				if (n == 0)
					break;
				out.insert(out.end(), chunk.begin(), chunk.begin() + n);
				if (out.size() > (8u << 20))
				{
					status = -1;
					break;
				}
			}
		}
		sceHttp2DeleteRequest(request);
		return status;
	}

	// The same for something large: the answer goes to a file as it arrives.
	// progress is told how much has arrived, and stops the download by
	// returning false. The HTTP status, -1 as above (also when the file could
	// not be written), or -2 when progress stopped it.
	int download(const std::string& url, const std::string& file, const std::function<bool(uint64_t)>& progress)
	{
		lastError = 0;
		if (!init())
			return -1;
		const int request = sceHttp2CreateRequestWithURL(templateId, "GET", url.c_str(), 0);
		if (request < 0)
		{
			lastError = request;
			return -1;
		}
		// Each step has half a minute; the whole of it half an hour.
		sceHttp2SetResolveTimeOut(request, 30 * 1000 * 1000);
		sceHttp2SetConnectTimeOut(request, 30 * 1000 * 1000);
		sceHttp2SetSendTimeOut(request, 30 * 1000 * 1000);
		sceHttp2SetRecvTimeOut(request, 30 * 1000 * 1000);
		sceHttp2SetTimeOut(request, 1800u * 1000 * 1000);
		sceHttp2SetAutoRedirect(request, 1);
		int status = -1;
		const int sent = sceHttp2SendRequest(request, nullptr, 0);
		const int got = sent == 0 ? sceHttp2GetStatusCode(request, &status) : -1;
		if (sent != 0 || got != 0)
		{
			lastError = sent != 0 ? sent : got;
			diag::mark("download: request failed: send %#x, status %#x (%s)", (unsigned)sent, (unsigned)got,
					url.substr(0, 80).c_str());
			status = -1;
		}
		else if (status >= 200 && status < 300)
		{
			FILE *out = fopen(file.c_str(), "wb");
			if (out == nullptr)
			{
				diag::mark("download: %s cannot be written: %s", file.c_str(), strerror(errno));
				status = -1;
			}
			else
			{
				std::vector<uint8_t> chunk(256 * 1024);
				uint64_t done = 0;
				for (;;)
				{
					const int n = sceHttp2ReadData(request, chunk.data(), chunk.size());
					if (n < 0)
					{
						lastError = n;
						diag::mark("download: reading failed after %llu bytes: %#x", (unsigned long long)done, (unsigned)n);
						status = -1;
						break;
					}
					if (n == 0)
						break;
					if (fwrite(chunk.data(), 1, (size_t)n, out) != (size_t)n)
					{
						diag::mark("download: writing %s failed: %s", file.c_str(), strerror(errno));
						status = -1;
						break;
					}
					done += (uint64_t)n;
					if (progress && !progress(done))
					{
						status = -2;
						break;
					}
				}
				if (fclose(out) != 0 && status >= 0)
					status = -1;
				if (status < 0)
					unlink(file.c_str());
			}
		}
		sceHttp2DeleteRequest(request);
		return status;
	}
};

// The collection's file name for a game name: its reserved characters are '_'.
std::string thumbnailName(const std::string& name)
{
	std::string out = name;
	for (char& c : out)
		if (strchr("&*/:`<>?\\|\"", c) != nullptr)
			c = '_';
	return out;
}

std::string urlEncode(const std::string& s)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s)
	{
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += (char)c;
		else
		{
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

bool isImage(const std::vector<uint8_t>& data)
{
	static const uint8_t png[] = { 0x89, 'P', 'N', 'G' };
	return data.size() > 64 && (memcmp(data.data(), png, 4) == 0 || (data[0] == 0xff && data[1] == 0xd8));
}

std::string coversDir()
{
	return rootDir + "covers/";
}

void loadNotFound()
{
	if (notFoundLoaded)
		return;
	notFoundLoaded = true;
	if (FILE *f = fopen((coversDir() + "not-found.txt").c_str(), "r"))
	{
		char line[512];
		while (fgets(line, sizeof(line), f) != nullptr)
		{
			std::string s(line);
			while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
				s.pop_back();
			if (!s.empty())
				notFound.insert(s);
		}
		fclose(f);
	}
}

void rememberNotFound(const std::string& base)
{
	std::lock_guard<std::mutex> lock(mutex);
	notFound.insert(base);
	if (FILE *f = fopen((coversDir() + "not-found.txt").c_str(), "a"))
	{
		fprintf(f, "%s\n", base.c_str());
		fclose(f);
	}
}

// The names to try for a file name: itself, then with each region when it
// names none, then without a "(Disc N)" tag.
std::vector<std::string> namesFor(const std::string& base)
{
	std::vector<std::string> names{ base };
	if (base.find('(') == std::string::npos)
		for (const char *region : { " (USA)", " (Europe)", " (Japan)" })
			names.push_back(base + region);
	const size_t disc = base.find(" (Disc ");
	if (disc != std::string::npos)
	{
		const size_t end = base.find(')', disc);
		if (end != std::string::npos)
			names.push_back(base.substr(0, disc) + base.substr(end + 1));
	}
	return names;
}

// The one client, and whose turn it is.
Http client;
std::mutex httpMutex;

int get(const std::string& url, std::vector<uint8_t>& out, unsigned seconds = 20)
{
	std::lock_guard<std::mutex> lock(httpMutex);
	return client.get(url, out, seconds);
}

// When the first source does not answer: how far a request gets from here,
// for the boot log. Each check is short.
void diagnose()
{
	const int tcp = smb::tcpAnswers("1.1.1.1", 80, 5000);
	if (tcp == 0)
		diag::mark("covers: check 1, a plain connection to 1.1.1.1 port 80: made");
	else if (tcp < 0)
		diag::mark("covers: check 1, a plain connection to 1.1.1.1 port 80: no answer in 5 s");
	else
		diag::mark("covers: check 1, a plain connection to 1.1.1.1 port 80: %s (errno %d)", strerror(tcp), tcp);
	std::vector<uint8_t> data;
	int status = get("http://1.1.1.1/", data, 8);
	diag::mark("covers: check 2, the system's HTTP to an address (no name, no TLS): %s %#x",
			status >= 0 ? "HTTP status" : "error", status >= 0 ? (unsigned)status : (unsigned)client.lastError);
	status = get("http://thumbnails.libretro.com/", data, 8);
	diag::mark("covers: check 3, the system's HTTP to a name (no TLS): %s %#x, %d bytes",
			status >= 0 ? "HTTP status" : "error", status >= 0 ? (unsigned)status : (unsigned)client.lastError,
			(int)data.size());
}

// 1 saved, 0 not in the collection, -1 could not ask (offline, time-out).
int fetch(const Source& source, const std::string& base)
{
	std::vector<uint8_t> data;
	for (const std::string& name : namesFor(base))
	{
		std::string url = std::string(source.baseUrl) + urlEncode(thumbnailName(name)) + ".png";
		int status = get(url, data);
		// A regional duplicate is a link in the collection: its body is the
		// name of the picture it stands for.
		if (source.links && status == 200 && !isImage(data) && data.size() < 512)
		{
			std::string target(data.begin(), data.end());
			while (!target.empty() && (target.back() == '\n' || target.back() == '\r'))
				target.pop_back();
			if (target.size() > 4 && target.find('/') == std::string::npos)
			{
				url = std::string(source.baseUrl) + urlEncode(target);
				status = get(url, data);
			}
		}
		if (status == 200 && isImage(data))
		{
			const std::string file = coversDir() + base + ".png";
			const std::string temporary = file + ".part";
			FILE *f = fopen(temporary.c_str(), "wb");
			if (f == nullptr)
				return -1;
			const bool written = fwrite(data.data(), 1, data.size(), f) == data.size();
			fclose(f);
			if (!written || rename(temporary.c_str(), file.c_str()) != 0)
			{
				unlink(temporary.c_str());
				return -1;
			}
			chmod(file.c_str(), 0666);
			static bool first = true;
			if (first)
			{
				first = false;
				diag::mark("covers: downloads work, from %s (%s)", source.name, base.c_str());
			}
			return 1;
		}
		if (status != 404 && status != 200)
		{
			if (status > 0)
				diag::mark("covers: the server answered %d", status);
			return -1;
		}
	}
	return 0;
}

void worker()
{
	int failures = 0;
	int source = 0;
	for (;;)
	{
		std::string base;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !queue.empty(); });
			base = queue.front();
			queue.pop_front();
		}
		int result = offline ? -1 : fetch(Sources[source], base);
		if (result < 0 && !offline && client.ok && source + 1 < SourceCount)
		{
			// The source does not answer: say how far requests get, and go on
			// with the next one, this cover first.
			diag::mark("covers: %s does not answer", Sources[source].name);
			diagnose();
			source++;
			diag::mark("covers: trying %s", Sources[source].name);
			result = fetch(Sources[source], base);
		}
		if (result == 1)
		{
			currentGeneration++;
			failures = 0;
		}
		else if (result == 0)
		{
			rememberNotFound(base);
			failures = 0;
		}
		else if (++failures >= 3 && !offline)
		{
			// No network, or the collection cannot be reached: stop for this run.
			offline = true;
			diag::mark("covers: downloads stopped for this run (no answer from any source)");
		}
		pending--;
	}
}

} // namespace

void request(const std::string& base)
{
	if (!options().covers || base.empty() || offline)
		return;
	std::lock_guard<std::mutex> lock(mutex);
	loadNotFound();
	if (!asked.insert(base).second || notFound.count(base) != 0)
		return;
	queue.push_back(base);
	pending++;
	if (!started)
	{
		started = true;
		std::thread(worker).detach();
	}
	wake.notify_one();
}

unsigned generation()
{
	return currentGeneration.load(std::memory_order_relaxed);
}

std::string status()
{
	const int n = pending.load();
	if (n <= 0 || offline)
		return "";
	return "Downloading covers (" + std::to_string(n) + " left)";
}

} // namespace ps5::covers

// The same client for the rest of the title (ps5_frontend.h).
namespace ps5::net
{

int get(const std::string& url, std::vector<uint8_t>& out, unsigned seconds)
{
	return covers::get(url, out, seconds);
}

int download(const std::string& url, const std::string& file, const std::function<bool(uint64_t)>& progress)
{
	std::lock_guard<std::mutex> lock(covers::httpMutex);
	return covers::client.download(url, file, progress);
}

int lastError()
{
	return covers::client.lastError;
}

std::string localAddress()
{
	// Asked at most every few seconds: a settings row shows it every frame.
	static std::mutex mutex;
	static std::string address;
	static time_t askedAt;
	std::lock_guard<std::mutex> lock(mutex);
	const time_t now = time(nullptr);
	if (askedAt != 0 && now - askedAt < 5)
		return address;
	askedAt = now;
	address.clear();
	sceNetCtlInit();
	// The answer is a union of everything that can be asked: the address is
	// text at its start (code 14, as on the PS4).
	alignas(8) char info[512] = {};
	const int rc = sceNetCtlGetInfo(14, info);
	info[15] = '\0';
	if (rc == 0 && info[0] >= '0' && info[0] <= '9')
		address = info;
	return address;
}

}

// Flycast's HTTP client, for its scraper (core/ui/boxart/gamesdb.cpp).
namespace http
{

int get(const std::string& url, std::vector<u8>& content, const Headers *reqHeaders, Headers *respHeaders)
{
	(void)reqHeaders;
	const int status = ps5::covers::get(url, content);
	if (status < 0)
	{
		content.clear();
		return 503;
	}
	if (respHeaders != nullptr)
	{
		// The one header the scraper reads, from what the body is.
		const char *type = "application/octet-stream";
		if (content.size() > 8)
		{
			static const u8 png[] = { 0x89, 'P', 'N', 'G' };
			if (memcmp(content.data(), png, 4) == 0)
				type = "image/png";
			else if (content[0] == 0xff && content[1] == 0xd8)
				type = "image/jpeg";
			else if (memcmp(content.data(), "GIF8", 4) == 0)
				type = "image/gif";
			else if (memcmp(content.data(), "RIFF", 4) == 0 && memcmp(content.data() + 8, "WEBP", 4) == 0)
				type = "image/webp";
			else if (content[0] == '{' || content[0] == '[')
				type = "application/json";
		}
		respHeaders->emplace_back("content-type", type);
	}
	return status;
}

} // namespace http
