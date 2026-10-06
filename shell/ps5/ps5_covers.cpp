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

	"Change cover", in a game's details, offers the collection's other pictures
	of the game: beside its box art (Named_Boxarts) it keeps a title screen
	(Named_Titles) and a moment of play (Named_Snaps) under the same name. The
	three are downloaded by the same worker, before the covers that wait, into
	covers/.choices, and the one chosen is copied to covers/<file name>.png.
	Which pictures in covers/ are PSFlyCast's own is kept with the library's
	notes (ps5_library.cpp): any other is the user's, and is never deleted. It
	is put aside in covers/.yours while another choice has its place.

	The same client is Flycast's HTTP client on the console (http::get, at the
	end of this file; core/oslib/http_client.cpp has none for the PS5), which
	is what Flycast's own scraper needs to ask TheGamesDB for a game's
	description, release date and box art. One request at a time: the worker
	here and the scraper's thread take turns.
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "ps5_build.h"
#include "version.h"
#include "types.h"
#include "oslib/http_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
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
int sceHttp2AddRequestHeader(int request, const char *name, const char *value, uint32_t mode);
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

namespace ps5::net
{
// Until when (seconds of the steady clock; 0: not).
static std::atomic<int64_t> endingGameUntil;

static int64_t steadySeconds()
{
	return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void gameEnding(bool ending)
{
	// It ends by itself, for a game that is started again and does not load.
	endingGameUntil = ending ? steadySeconds() + 30 : 0;
}

static bool endingGame()
{
	const int64_t until = endingGameUntil;
	return until != 0 && steadySeconds() < until;
}

// Said honestly, so that a service sees what asks it: this port and its
// version, then the emulator and its.
std::string userAgent()
{
	const std::string flycast = GIT_VERSION;
	return "PSFlyCast/" PS5_VERSION " (PlayStation 5) Flycast/" + (flycast.rfind('v', 0) == 0 ? flycast.substr(1) : flycast);
}
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
	{ "GitHub (HTTPS)", "https://raw.githubusercontent.com/libretro-thumbnails/Sega_-_Dreamcast/master/", true },
	{ "thumbnails.libretro.com (HTTP)", "http://thumbnails.libretro.com/Sega%20-%20Dreamcast/", false },
};
constexpr int SourceCount = 2;
// The collection's three sets of pictures, in the order of ps5_frontend.h:
// box art, title screen, a moment of play.
constexpr const char *Folders[PictureCount] = { "Named_Boxarts/", "Named_Titles/", "Named_Snaps/" };

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
// "Change cover": the games whose three pictures are asked for, and how each
// game's stand, by file name.
std::deque<std::string> choiceQueue;
std::map<std::string, Alternatives> choices;
// Why the worker's last request could not be made, for the screen.
std::string failure;

struct Http
{
	bool tried = false, ok = false;
	bool noNetwork = false;		// the console was not connected when last asked
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
			if (!noNetwork)
				diag::mark("covers: the console is not connected (netctl %#x, state %d)", (unsigned)nc, state[0]);
			// Asked again with the next request: it may be connected by then.
			noNetwork = true;
			tried = false;
			return false;
		}
		noNetwork = false;
		const int net = sceNetInit();	// an error only means it was up already
		const int pool = sceNetPoolCreate("flycast-covers", 64 * 1024, 0);
		const int ssl = pool >= 0 ? sceSslInit(256 * 1024) : -1;
		const int context = ssl >= 0 ? sceHttp2Init(pool, ssl, 256 * 1024, 1) : -1;
		templateId = context >= 0 ? sceHttp2CreateTemplate(context, net::userAgent().c_str(), 3, 1) : -1;
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
		return request("GET", url, {}, nullptr, 0, out, seconds, false);
	}

	// Any request: its method, the headers to send with it, and for a POST
	// what is posted. The answer's body is read for a 2xx status, and with
	// anyStatus for every status (a service that says what was wrong there).
	int request(const char *method, const std::string& url,
			const std::vector<std::pair<std::string, std::string>>& headers, const void *body, size_t bodySize,
			std::vector<uint8_t>& out, unsigned seconds, bool anyStatus)
	{
		out.clear();
		lastError = 0;
		if (!init())
			return -1;
		const int request = sceHttp2CreateRequestWithURL(templateId, method, url.c_str(), bodySize);
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
		for (const auto& [name, value] : headers)
		{
			// 0: in place of the template's header of that name.
			const int added = sceHttp2AddRequestHeader(request, name.c_str(), value.c_str(), 0);
			if (added < 0 && reported++ < 6)
				diag::mark("covers: the header %s was not taken: %#x", name.c_str(), (unsigned)added);
		}
		int status = -1;
		const int sent = sceHttp2SendRequest(request, bodySize != 0 ? body : nullptr, bodySize);
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
		else if ((status >= 200 && status < 300) || anyStatus)
		{
			std::vector<uint8_t> chunk(64 * 1024);
			for (;;)
			{
				const int n = sceHttp2ReadData(request, chunk.data(), chunk.size());
				if (n < 0)
				{
					if (reported++ < 4)
						diag::mark("covers: reading the answer failed: %#x", (unsigned)n);
					if (status >= 200 && status < 300)
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

bool exists(const std::string& file)
{
	struct stat st;
	return stat(file.c_str(), &st) == 0 && st.st_size > 0;
}

// A picture written whole or not at all, and open to the console's FTP server.
bool saveFile(const std::string& file, const std::vector<uint8_t>& data)
{
	const std::string temporary = file + ".part";
	FILE *f = fopen(temporary.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool written = fwrite(data.data(), 1, data.size(), f) == data.size();
	if (fclose(f) != 0 || !written || rename(temporary.c_str(), file.c_str()) != 0)
	{
		unlink(temporary.c_str());
		return false;
	}
	chmod(file.c_str(), 0666);
	return true;
}

bool readFile(const std::string& file, std::vector<uint8_t>& data)
{
	data.clear();
	FILE *f = fopen(file.c_str(), "rb");
	if (f == nullptr)
		return false;
	uint8_t chunk[64 * 1024];
	size_t n;
	while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0 && data.size() <= (8u << 20))
		data.insert(data.end(), chunk, chunk + n);
	fclose(f);
	return !data.empty() && data.size() <= (8u << 20);
}

// A game's picture from one of the collection's sets, into data: 1 it is
// there, 0 not in the collection, -1 could not ask (offline, time-out;
// failure then says which).
int download(const Source& source, const char *folder, const std::string& base, std::vector<uint8_t>& data)
{
	for (const std::string& name : namesFor(base))
	{
		std::string url = std::string(source.baseUrl) + folder + urlEncode(thumbnailName(name)) + ".png";
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
				url = std::string(source.baseUrl) + folder + urlEncode(target);
				status = get(url, data);
			}
		}
		if (status == 200 && isImage(data))
			return 1;
		if (status != 404 && status != 200)
		{
			if (status > 0)
			{
				diag::mark("covers: the server answered %d", status);
				failure = "the server answered " + std::to_string(status);
			}
			else if (client.noNetwork)
				failure = "the console is not connected to a network";
			else if (!client.ok)
				failure = "the console's HTTP client did not start";
			else
			{
				char code[24];
				snprintf(code, sizeof(code), "%#x", (unsigned)client.lastError);
				failure = std::string("no answer from the server (") + code + ")";
			}
			return -1;
		}
	}
	return 0;
}

// The game's cover: 1 saved, 0 not in the collection, -1 could not ask.
int fetch(const Source& source, const std::string& base)
{
	std::vector<uint8_t> data;
	const int found = download(source, Folders[Boxart], base, data);
	if (found != 1)
		return found;
	// A picture the user put there meanwhile stays.
	for (const char *extension : { ".png", ".jpg", ".jpeg" })
		if (exists(coversDir() + base + extension))
			return 1;
	if (!saveFile(coversDir() + base + ".png", data))
		return -1;
	// It is PSFlyCast's own: "Change cover" may take it away again.
	library::coverPut(base, "auto");
	static bool first = true;
	if (first)
	{
		first = false;
		diag::mark("covers: downloads work, from %s (%s)", source.name, base.c_str());
	}
	return 1;
}

// Asks the source in use. When it does not answer and there is another: says
// how far requests get, for the boot log, and asks that one, from then on.
template<typename Ask>
int askSource(int& source, Ask ask)
{
	int result = ask(Sources[source]);
	if (result < 0 && client.ok && source + 1 < SourceCount)
	{
		diag::mark("covers: %s does not answer", Sources[source].name);
		diagnose();
		source++;
		diag::mark("covers: trying %s", Sources[source].name);
		result = ask(Sources[source]);
	}
	return result;
}

std::string choiceFile(const std::string& base, int picture)
{
	static const char *const tags[PictureCount] = { ".boxart", ".title", ".snap" };
	return coversDir() + ".choices/" + base + tags[picture] + ".png";
}

// What the library's notes call each picture (ps5_library.cpp).
const char *const choiceNames[PictureCount] = { "boxart", "title", "snap" };

// "Change cover" asked for a game's three pictures: each that is still waited
// for is downloaded into covers/.choices.
void fetchChoices(const std::string& base, int& source)
{
	const std::string dir = coversDir() + ".choices";
	mkdir(dir.c_str(), 0777);
	chmod(dir.c_str(), 0777);
	bool failed = false;
	for (int picture = 0; picture < PictureCount; picture++)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (choices[base].state[picture] != Alternatives::Waiting)
				continue;
		}
		// One that could not be asked for is enough: the others are not waited for.
		Alternatives::State state = Alternatives::Failed;
		if (!failed)
		{
			std::vector<uint8_t> data;
			const int found = askSource(source, [&](const Source& from) {
				return download(from, Folders[picture], base, data);
			});
			if (found == 1 && saveFile(choiceFile(base, picture), data))
				state = Alternatives::Found;
			else if (found == 1)
				failure = "the picture could not be written to the covers folder";
			else if (found == 0)
				state = Alternatives::Missing;
			failed = state == Alternatives::Failed;
			// An answer: the covers that wait may be asked for again.
			if (found >= 0)
				offline = false;
			// A cover an earlier build downloaded is in no note. It is the box
			// art, byte for byte: PSFlyCast's own, then, and not a picture of
			// the user's. Known before the dialog hears that the box art is here.
			const std::string cover = coversDir() + base + ".png";
			if (picture == Boxart && state == Alternatives::Found && exists(cover) && library::coverOurs(base).empty())
			{
				std::vector<uint8_t> there;
				if (readFile(cover, there) && there == data)
				{
					library::coverPut(base, "auto");
					currentGeneration++;
				}
			}
		}
		std::lock_guard<std::mutex> lock(mutex);
		Alternatives& now = choices[base];
		now.state[picture] = state;
		if (state == Alternatives::Failed)
			now.error = "Could not download: " + failure;
	}
}

void worker()
{
	int failures = 0;
	int source = 0;
	for (;;)
	{
		std::string base;
		bool chosen = false;
		{
			std::unique_lock<std::mutex> lock(mutex);
			wake.wait(lock, [] { return !queue.empty() || !choiceQueue.empty(); });
			// The pictures someone is looking at a dialog for come first.
			chosen = !choiceQueue.empty();
			std::deque<std::string>& from = chosen ? choiceQueue : queue;
			base = from.front();
			from.pop_front();
		}
		if (chosen)
		{
			// Asked for by hand: tried even after the covers gave up for this run.
			fetchChoices(base, source);
			continue;
		}
		// A source that does not answer: the next one, this cover first.
		const int result = offline ? -1 : askSource(source, [&](const Source& from) { return fetch(from, base); });
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

void fetchAlternatives(const std::string& base)
{
	if (base.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex);
	Alternatives& now = choices[base];
	for (int picture = 0; picture < PictureCount; picture++)
		if (now.state[picture] == Alternatives::Waiting)
			return;		// on their way already
	bool ask = false;
	for (int picture = 0; picture < PictureCount; picture++)
	{
		now.file[picture] = choiceFile(base, picture);
		if (exists(now.file[picture]))
			now.state[picture] = Alternatives::Found;
		else if (now.state[picture] != Alternatives::Missing)
		{
			now.state[picture] = Alternatives::Waiting;
			ask = true;
		}
	}
	if (!ask)
		return;
	now.error.clear();
	choiceQueue.push_back(base);
	if (!started)
	{
		started = true;
		std::thread(worker).detach();
	}
	wake.notify_one();
}

Alternatives alternatives(const std::string& base)
{
	std::lock_guard<std::mutex> lock(mutex);
	const auto it = choices.find(base);
	return it != choices.end() ? it->second : Alternatives();
}

int current(const std::string& base)
{
	const std::string ours = library::coverOurs(base);
	for (int picture = 0; picture < PictureCount; picture++)
		if (ours == choiceNames[picture])
			return picture;
	if (!ours.empty())
		return Automatic;		// a download
	for (const char *extension : { ".png", ".jpg", ".jpeg" })
		if (exists(coversDir() + base + extension))
			return Own;
	return Automatic;
}

std::string ownFile(const std::string& base)
{
	// In its place: a picture that is not PSFlyCast's (which are all .png).
	if (library::coverOurs(base).empty() && exists(coversDir() + base + ".png"))
		return coversDir() + base + ".png";
	for (const char *extension : { ".jpg", ".jpeg" })
		if (exists(coversDir() + base + extension))
			return coversDir() + base + extension;
	// Put aside.
	for (const char *extension : { ".png", ".jpg", ".jpeg" })
		if (exists(coversDir() + ".yours/" + base + extension))
			return coversDir() + ".yours/" + base + extension;
	return "";
}

bool choose(const std::string& base, int choice, const std::string& from)
{
	if (base.empty() || choice < 0 || choice >= ChoiceCount)
		return false;
	const std::string dir = coversDir(), cover = dir + base + ".png", aside = dir + ".yours/";
	const bool ours = !library::coverOurs(base).empty();
	if (choice == Own)
	{
		if (ownFile(base).empty())
			return false;
		// PSFlyCast's picture leaves, and the user's is back where the library looks.
		if (ours)
		{
			unlink(cover.c_str());
			library::coverGone(base);
		}
		// Not over a picture put in its place since: that one is the newer.
		for (const char *extension : { ".png", ".jpg", ".jpeg" })
			if (exists(aside + base + extension) && !exists(dir + base + extension))
				rename((aside + base + extension).c_str(), (dir + base + extension).c_str());
		currentGeneration++;
		return true;
	}
	// The picture to put there. Automatic: the box art a download would bring,
	// when it is here already and downloads are on.
	std::string picture;
	if (choice < PictureCount)
	{
		picture = choiceFile(from, choice);
		if (!exists(picture))
			return false;
	}
	else if (options().covers && exists(choiceFile(from, Boxart)))
		picture = choiceFile(from, Boxart);
	std::vector<uint8_t> data;
	if (!picture.empty() && !readFile(picture, data))
		return false;
	// The user's own picture gives way: it is kept, aside.
	for (const char *extension : { ".png", ".jpg", ".jpeg" })
	{
		const std::string file = dir + base + extension;
		if (!exists(file) || (ours && file == cover))
			continue;
		mkdir(aside.c_str(), 0777);
		chmod(aside.c_str(), 0777);
		// One put aside earlier is not written over: it stays, as the older one.
		if (exists(aside + base + extension))
			rename((aside + base + extension).c_str(), (aside + base + ".older" + extension).c_str());
		if (rename(file.c_str(), (aside + base + extension).c_str()) != 0)
			return false;
	}
	if (ours)
	{
		unlink(cover.c_str());
		library::coverGone(base);
	}
	if (!picture.empty())
	{
		if (!saveFile(cover, data))
			return false;
		library::coverPut(base, choice == Automatic ? "auto" : choiceNames[choice]);
	}
	else
	{
		// Automatic, and nothing here to put: the library asks for the
		// download again when it finds the game without a cover.
		std::lock_guard<std::mutex> lock(mutex);
		asked.erase(base);
	}
	currentGeneration++;
	return true;
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
	// See ps5::net::gameEnding: an achievement's picture, while a game is unloaded.
	if (ps5::net::endingGame() && url.find("retroachievements.org") != std::string::npos)
	{
		content.clear();
		return 503;
	}
	int status;
	if (reqHeaders != nullptr && !reqHeaders->empty())
	{
		std::lock_guard<std::mutex> lock(ps5::covers::httpMutex);
		status = ps5::covers::client.request("GET", url, *reqHeaders, nullptr, 0, content, 20, false);
	}
	else
		status = ps5::covers::get(url, content);
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

// What is posted, and the answer whatever its status: a service's own error
// is in it (the achievements' server).
int post(const std::string& url, const char *payload, const char *contentType, std::vector<u8>& reply)
{
	const size_t size = payload != nullptr ? strlen(payload) : 0;
	Headers headers{ { "Content-Type", contentType != nullptr ? contentType : "application/x-www-form-urlencoded" },
			{ "User-Agent", ps5::net::userAgent() } };
	std::lock_guard<std::mutex> lock(ps5::covers::httpMutex);
	const int status = ps5::covers::client.request("POST", url, headers, payload, size, reply, 30, true);
	if (status < 0)
	{
		reply.clear();
		return 503;
	}
	return status;
}

// A form: its fields as name=value pairs. A field that is a file (a
// screenshot sent with a report) is not sent from the console.
int post(const std::string& url, const std::vector<PostField>& fields)
{
	std::string body;
	for (const PostField& field : fields)
	{
		if (!field.contentType.empty())
			return 501;
		body += (body.empty() ? "" : "&") + urlEncode(field.name) + "=" + urlEncode(field.value);
	}
	std::vector<u8> reply;
	return post(url, body.c_str(), "application/x-www-form-urlencoded", reply);
}

} // namespace http
