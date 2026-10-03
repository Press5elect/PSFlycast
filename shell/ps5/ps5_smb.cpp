/*
	PSFlyCast - games on a network share (SMB).

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Flycast reads every game file through hostfs (the scanner, the disc image
	readers, the arcade archives), and hostfs asks a platform's "custom
	storage" first: Android's is its document provider, this one is SMB 2/3
	through libsmb2 (github.com/sahlberg/libsmb2, LGPL-2.1, built into the
	title). A path of the form

		smb://server/share/folder/file

	is opened on that server's share. <root>/network.cfg names the folders to
	scan and the account to use:

		path = 192.168.1.10/Games/Dreamcast
		user = guest
		password =

	Sockets. On the console a title's socket did not behave as a non-blocking
	one after fcntl(F_SETFL, O_NONBLOCK): build 15's port check, made that way,
	failed at once, and libsmb2, which reads until the socket says "nothing
	more", waited for ever on the server's first answer, exactly as it does on
	a socket left blocking. With the socket left blocking on purpose, build 16
	connected, listed the share and read games at 69 MB/s on a console. So
	libsmb2 is built that way (shell/ps5/smb/ps5_smb_compat.c):
	its reads ask poll first whether there is anything to read, and a connect
	is made only after the server's port has answered a connection tried on a
	thread of its own, which the caller waits for five seconds.

	The connection. One per share, made the first time it is needed and shared
	by every thread, one request at a time (a libsmb2 context is not
	thread-safe). Before logging in, the server's SMB port is tried for five
	seconds: a server that is off or a wrong address fails there, quickly. A
	server that is on gets a minute for each request, because a NAS that let
	its disks sleep answers the first one only when they are spinning again.
	A share that could not be reached is tried again after 30 seconds, or at
	once when the user asks (a scan, a game). A request that fails on a
	connection that is gone (the NAS restarted) is made once more on a new one.

	A game's files, two ways (the "Load network games into memory" option):

	  in memory   while a game is being loaded (beginLoad .. endLoad), each
	              file it opens is read whole into memory, in 16 MiB blocks
	              from the title's heap, and the share is not touched again:
	              the NAS can sleep while the game runs. Every open of a file
	              shares its one image. A file that does not fit is streamed
	              instead.
	  streamed    the file is read from the share as the game asks, through a
	              256 KiB buffer, so the disc readers' small sequential reads
	              are one request each time the buffer runs out. A request
	              that fails is made once more on a new connection.

	status() says what the share is doing, for the loading screen and the
	library: waiting for an answer, or how much of the file is in memory.
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"

#include "oslib/storage.h"
#include "log/Log.h"
#include "stdclass.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

extern "C"
{
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
}

namespace ps5::smb
{
namespace
{

constexpr int ProbeSeconds = 5;			// for the server's port to accept a connection
constexpr int RequestSeconds = 60;		// for each request: a NAS waking its disks
constexpr int RetrySeconds = 30;		// before a share that failed is tried again
constexpr size_t StreamBuffer = 256 * 1024;
constexpr size_t RamBlock = 16u << 20;	// a file in memory is made of these
constexpr size_t RamPiece = 4u << 20;	// read at a time; the cancel flag is looked at between two
constexpr u64 RamBudget = (u64)3 << 30;	// for all files in memory together

struct Account
{
	std::string user = "guest";
	std::string password;
	std::string domain;
};
Account account;
std::vector<std::string> folders;	// smb://server/share/folder, from network.cfg

// ---------------------------------------------------- what the share is doing

std::atomic<bool> loading{false};		// a network game is being loaded
std::atomic<bool> cancelled{false};		// and the user gave up
std::atomic<u64> ramTotal{0};			// the file being read into memory; 0 when none
std::atomic<u64> ramDone{0};
std::atomic<u64> ramInUse{0};			// all files in memory
std::atomic<int> busyDepth{0};			// requests in flight
std::atomic<long long> busySince{0};	// when the oldest started, in ms; 0 when none
std::atomic<unsigned> failureCount{0};
std::mutex errorMutex;
std::string errorText;

long long nowMs()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Around every request to a server.
struct Busy
{
	Busy()
	{
		if (busyDepth.fetch_add(1) == 0)
			busySince = nowMs();
	}
	~Busy()
	{
		if (busyDepth.fetch_sub(1) == 1)
			busySince = 0;
	}
	// The server answered and the next request follows at once.
	void answered()
	{
		busySince = nowMs();
	}
};

// The share answers, but not with what was asked for (a folder that cannot be
// listed): said on the screen and in the log.
void note(const std::string& text)
{
	{
		std::lock_guard<std::mutex> lock(errorMutex);
		errorText = text;
	}
	WARN_LOG(COMMON, "SMB: %s", text.c_str());
	diag::mark("smb: %s", text.c_str());
}

// The share could not be reached, or stopped answering: counted, so that a
// scan knows its list is not whole.
void fail(const std::string& text)
{
	failureCount++;
	note(text);
}

// The load in progress was cancelled: nothing more is asked of the share.
bool stopping()
{
	return loading && cancelled;
}

std::string stripped(std::string text)
{
	while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
		text.pop_back();
	return text;
}

// Whether the server accepts a connection on the SMB port.
enum class Probe
{
	Open,
	Closed,		// refused, unreachable, or no answer in the time given
	Unknown,	// could not tell (no socket, an error that says nothing): libsmb2 finds out
	Cancelled,
	NoName,		// the server is given by name, which the console cannot look up
};

// A connection tried on a thread of its own. Non-blocking sockets did not
// work on the console (see the top of this file), so the connect blocks, for
// as long as the system lets it when nothing answers, and the caller waits
// for it only so long. The thread ends by itself and closes its socket.
struct ConnectAttempt
{
	std::mutex mutex;
	std::condition_variable done;
	bool finished = false;
	int error = 0;			// errno of socket() or connect(); 0 when connected
	bool noSocket = false;
	long long tookMs = 0;
	// For the log, once: what fcntl says about a socket here (F_GETFL, then
	// F_SETFL with O_NONBLOCK on a socket of its own, then F_GETFL again).
	int fcntlResult = 0, fcntlErrno = 0;
	int setResult = 0, setErrno = 0, flagsAfter = 0;
};

std::shared_ptr<ConnectAttempt> startConnect(const sockaddr_in& address)
{
	std::shared_ptr<ConnectAttempt> attempt = std::make_shared<ConnectAttempt>();
	std::thread([attempt, address] {
		const long long started = nowMs();
		int error = 0;
		bool noSocket = false;
		int flags = 0, flagsErrno = 0;
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0)
		{
			error = errno;
			noSocket = true;
		}
		else
		{
			errno = 0;
			flags = fcntl(fd, F_GETFL, 0);
			flagsErrno = errno;
			if (connect(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0)
				error = errno != 0 ? errno : EIO;
			close(fd);
		}
		static std::atomic<bool> tried{false};
		int setResult = 0, setErrno = 0, flagsAfter = 0;
		if (!tried.exchange(true))
		{
			const int other = socket(AF_INET, SOCK_STREAM, 0);
			if (other >= 0)
			{
				errno = 0;
				setResult = fcntl(other, F_SETFL, fcntl(other, F_GETFL, 0) | O_NONBLOCK);
				setErrno = errno;
				flagsAfter = fcntl(other, F_GETFL, 0);
				close(other);
			}
		}
		std::lock_guard<std::mutex> lock(attempt->mutex);
		attempt->setResult = setResult;
		attempt->setErrno = setErrno;
		attempt->flagsAfter = flagsAfter;
		attempt->error = error;
		attempt->noSocket = noSocket;
		attempt->fcntlResult = flags;
		attempt->fcntlErrno = flagsErrno;
		attempt->tookMs = nowMs() - started;
		attempt->finished = true;
		attempt->done.notify_all();
	}).detach();
	return attempt;
}

bool numericAddress(const std::string& server, int port, sockaddr_in& address)
{
	address = sockaddr_in{};
	if (inet_pton(AF_INET, server.c_str(), &address.sin_addr) != 1)
		return false;
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port);
#if defined(__PROSPERO__) || defined(__FreeBSD__)
	address.sin_len = sizeof(address);
#endif
	return true;
}

Probe probe(const std::string& server)
{
	sockaddr_in address;
	if (!numericAddress(server, 445, address))
		return Probe::NoName;
	const std::shared_ptr<ConnectAttempt> attempt = startConnect(address);
	const long long deadline = nowMs() + ProbeSeconds * 1000;
	std::unique_lock<std::mutex> lock(attempt->mutex);
	while (!attempt->finished && nowMs() < deadline)
	{
		if (stopping())
			return Probe::Cancelled;
		attempt->done.wait_for(lock, std::chrono::milliseconds(100));
	}
	if (!attempt->finished)
	{
		diag::mark("smb: %s port 445: no answer in %d s", server.c_str(), ProbeSeconds);
		return Probe::Closed;
	}
	// Once, for the log: how this console's sockets take fcntl.
	static bool said;
	if (!said && !attempt->noSocket)
	{
		said = true;
		diag::mark("smb: sockets here: fcntl(F_GETFL) gives %#x (errno %d); F_SETFL O_NONBLOCK (%#x) gives %d (errno %d), flags then %#x",
				attempt->fcntlResult, attempt->fcntlErrno, (int)O_NONBLOCK, attempt->setResult, attempt->setErrno,
				attempt->flagsAfter);
	}
	if (attempt->error == 0)
	{
		diag::mark("smb: %s port 445 answers (%lld ms)", server.c_str(), attempt->tookMs);
		return Probe::Open;
	}
	diag::mark("smb: %s port 445: %s failed: %s (errno %d)", server.c_str(), attempt->noSocket ? "socket" : "connect",
			strerror(attempt->error), attempt->error);
	if (attempt->noSocket)
		return Probe::Unknown;
	switch (attempt->error)
	{
	case ECONNREFUSED:
	case ENETUNREACH:
	case EHOSTUNREACH:
	case ETIMEDOUT:
	case EHOSTDOWN:
	case ENETDOWN:
		return Probe::Closed;
	default:
		return Probe::Unknown;
	}
}

// One share's connection. Every libsmb2 call on it holds the mutex.
struct Share
{
	std::recursive_mutex mutex;
	std::string server, share;
	smb2_context *context = nullptr;
	unsigned generation = 0;	// changes when the connection is made again
	long long failedAt = 0;		// nowMs() of the last failure to connect; 0 for none
	int preferred = 0;			// the login that worked last time

	bool connect()
	{
		if (context != nullptr)
			return true;
		if (stopping() || (failedAt != 0 && nowMs() - failedAt < RetrySeconds * 1000))
			return false;
		Busy busy;
		const Probe answer = probe(server);
		if (answer == Probe::Cancelled)
			return false;
		if (answer == Probe::Closed)
		{
			fail(server + " does not answer: is it switched on, and is the address in network.cfg right?");
			failedAt = nowMs();
			return false;
		}
		if (answer == Probe::NoName)
		{
			fail("\"" + server + "\" is a name: give the server's IP address in network.cfg (192.168.x.x)");
			failedAt = nowMs();
			return false;
		}
		// The ways to log in, most capable first. A guest (no password) logs
		// in with no password at all: libsmb2 signs an SMB 3.1.1 session it
		// opened with an empty one, and a server that mapped it to its guest
		// account refuses the signature. SMB 3.0.2 and 2.1 are the fallbacks
		// for servers that take neither.
		struct Attempt
		{
			bool password;
			int version;
			const char *name;
		};
		static const Attempt withPassword[] = { { true, 0, "SMB 3" }, { true, SMB2_VERSION_0302, "SMB 3.0.2" },
				{ true, SMB2_VERSION_0210, "SMB 2.1" } };
		static const Attempt asGuest[] = { { false, 0, "SMB 3, no password" }, { true, SMB2_VERSION_0302, "SMB 3.0.2" },
				{ true, SMB2_VERSION_0210, "SMB 2.1" } };
		const Attempt *attempts = account.password.empty() ? asGuest : withPassword;
		smb2_context *c = nullptr;
		std::string error;
		for (int i = 0; i < 3 && !stopping(); i++)
		{
			const Attempt& attempt = attempts[(i + preferred) % 3];
			c = smb2_init_context();
			if (c == nullptr)
				return false;
			smb2_set_timeout(c, RequestSeconds);
			if (attempt.version != 0)
				smb2_set_version(c, (smb2_negotiate_version)attempt.version);
			smb2_set_user(c, account.user.c_str());
			if (attempt.password)
				smb2_set_password(c, account.password.c_str());
			if (!account.domain.empty())
				smb2_set_domain(c, account.domain.c_str());
			const time_t started = time(nullptr);
			if (smb2_connect_share(c, server.c_str(), share.c_str(), account.user.c_str()) == 0)
			{
				preferred = (i + preferred) % 3;
				diag::mark("smb: connected to //%s/%s as %s (%s) in %d s", server.c_str(), share.c_str(),
						account.user.c_str(), attempt.name, (int)(time(nullptr) - started));
				break;
			}
			error = stripped(smb2_get_error(c));
			diag::mark("smb: //%s/%s as %s (%s): %s", server.c_str(), share.c_str(), account.user.c_str(), attempt.name,
					error.c_str());
			smb2_destroy_context(c);
			c = nullptr;
			// A server that let a request time out is not asked two more times.
			if (time(nullptr) - started >= RequestSeconds - 1)
				break;
		}
		if (c == nullptr)
		{
			if (stopping())
				return false;
			fail("//" + server + "/" + share + ": " + error
					+ " (check the share's name, the user and the password in network.cfg)");
			failedAt = nowMs();
			return false;
		}
		context = c;
		generation++;
		failedAt = 0;
		return true;
	}

	// After a request failed: the connection may be gone (the NAS restarted,
	// the console's network was reset). The next request makes it again.
	void drop()
	{
		if (context != nullptr)
		{
			smb2_destroy_context(context);
			context = nullptr;
		}
	}

	// After a request that began at `started` failed: whether the connection
	// is what failed - the request timed out, or the share's root cannot be
	// read either. It is dropped then, and the caller asks once more on a new
	// one; otherwise the share answers and what was asked for is not there.
	bool lost(long long started)
	{
		if (context == nullptr)
			return true;
		smb2_stat_64 st{};
		if (nowMs() - started >= (RequestSeconds - 1) * 1000 || smb2_stat(context, "", &st) != 0)
		{
			drop();
			return true;
		}
		return false;
	}
};

std::mutex sharesMutex;
std::map<std::string, std::unique_ptr<Share>> shares;

// smb://server/share/a/b -> the share's connection and "a/b".
Share *locate(const std::string& path, std::string& relative)
{
	if (path.rfind("smb://", 0) != 0)
		return nullptr;
	const size_t serverEnd = path.find('/', 6);
	if (serverEnd == std::string::npos)
		return nullptr;
	size_t shareEnd = path.find('/', serverEnd + 1);
	if (shareEnd == std::string::npos)
		shareEnd = path.size();
	const std::string server = path.substr(6, serverEnd - 6);
	const std::string share = path.substr(serverEnd + 1, shareEnd - serverEnd - 1);
	if (server.empty() || share.empty())
		return nullptr;
	relative = shareEnd < path.size() ? path.substr(shareEnd + 1) : "";
	while (!relative.empty() && relative.back() == '/')
		relative.pop_back();
	std::lock_guard<std::mutex> lock(sharesMutex);
	std::unique_ptr<Share>& entry = shares[server + "/" + share];
	if (!entry)
	{
		entry = std::make_unique<Share>();
		entry->server = server;
		entry->share = share;
	}
	return entry.get();
}

std::string lastComponent(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// ------------------------------------------------- a file read from the share

class SmbFile : public hostfs::File
{
public:
	SmbFile(Share *share, const std::string& relative, smb2fh *handle, u64 bytes)
		: share(share), relative(relative), handle(handle), generation(share->generation), bytes(bytes)
	{
		buffer.resize(StreamBuffer);
	}

	~SmbFile() override
	{
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		if (handle != nullptr && share->context != nullptr && generation == share->generation)
			smb2_close(share->context, handle);
	}

	size_t read(void *out, size_t size, size_t count) override
	{
		if (size == 0 || count == 0)
			return 0;
		u8 *to = static_cast<u8 *>(out);
		size_t want = size * count, done = 0;
		while (want > 0)
		{
			if (position >= bufferStart && position < bufferStart + bufferFill)
			{
				const size_t offset = (size_t)(position - bufferStart);
				const size_t n = std::min(want, bufferFill - offset);
				memcpy(to + done, buffer.data() + offset, n);
				done += n;
				want -= n;
				position += n;
				continue;
			}
			if (position >= bytes)
			{
				atEnd = true;
				break;
			}
			// A large read goes straight to the caller; a small one fills the buffer.
			if (want >= buffer.size())
			{
				const int n = fetch(to + done, position, want);
				if (n <= 0)
					break;
				done += n;
				want -= n;
				position += n;
			}
			else
			{
				const int n = fetch(buffer.data(), position, buffer.size());
				if (n <= 0)
					break;
				bufferStart = position;
				bufferFill = (size_t)n;
			}
		}
		return done / size;
	}

	size_t write(const void *, size_t, size_t) override
	{
		failed = true;
		return 0;
	}

	s64 tell() override
	{
		return (s64)position;
	}

	int seek(s64 offset, int whence) override
	{
		s64 target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (s64)position + offset : (s64)bytes + offset;
		if (target < 0)
			return -1;
		position = (u64)target;
		atEnd = false;
		return 0;
	}

	char *gets(char *out, int count) override
	{
		if (count <= 0)
			return nullptr;
		int n = 0;
		while (n < count - 1)
		{
			char c;
			if (read(&c, 1, 1) != 1)
				break;
			out[n++] = c;
			if (c == '\n')
				break;
		}
		if (n == 0)
			return nullptr;
		out[n] = '\0';
		return out;
	}

	s64 size() override
	{
		return (s64)bytes;
	}

	int eof() override
	{
		return atEnd ? 1 : 0;
	}

	int error() override
	{
		return failed ? 1 : 0;
	}

private:
	// Reads up to `want` bytes at `offset`, making the connection and opening
	// the file again once if the request fails.
	int fetch(u8 *to, u64 offset, size_t want)
	{
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!share->connect())
				break;
			if (generation != share->generation || handle == nullptr)
			{
				handle = smb2_open(share->context, relative.c_str(), O_RDONLY);
				generation = share->generation;
				if (handle == nullptr)
					break;
			}
			// At most 1 MiB a request: each answer shows the share is alive
			// (status() counts the time since the last one).
			const u32 most = std::clamp<u32>(smb2_get_max_read_size(share->context), 4096, 1u << 20);
			size_t total = 0;
			int n = 0;
			while (total < want)
			{
				const u32 chunk = (u32)std::min<size_t>(want - total, most);
				n = smb2_pread(share->context, handle, to + total, chunk, offset + total);
				if (n <= 0)
					break;
				total += (size_t)n;
				busy.answered();
			}
			if (total > 0 || n == 0)
				return (int)total;
			const std::string error = stripped(smb2_get_error(share->context));
			share->drop();
			handle = nullptr;
			if (attempt == 1 || stopping())
			{
				fail("reading " + lastComponent(relative) + ": " + error);
				break;
			}
			diag::mark("smb: reading %s: %s; connecting again", lastComponent(relative).c_str(), error.c_str());
		}
		failed = true;
		return -1;
	}

	Share *share;
	std::string relative;
	smb2fh *handle;
	unsigned generation;
	u64 bytes;
	u64 position = 0;
	std::vector<u8> buffer;
	u64 bufferStart = 0;
	size_t bufferFill = 0;
	bool atEnd = false;
	bool failed = false;
};

// -------------------------------------------------- a file read into memory

// A file's bytes, in blocks from the title's heap. Flycast opens a game's
// files more than once (a .cue's track file for every track, a .cdi for each
// of its tracks, an archive as 7z and then as zip): every open of a path gets
// the one image.
struct RamImage
{
	std::vector<u8 *> blocks;
	u64 bytes = 0;

	~RamImage()
	{
		for (u8 *block : blocks)
			free(block);
		ramInUse -= bytes;
	}
};

std::mutex imagesMutex;
std::map<std::string, std::weak_ptr<RamImage>> images;	// by path, while a file has it open
// The images of the game being loaded, kept between two opens of a file.
std::vector<std::shared_ptr<RamImage>> pinned;
std::mutex imageLoadMutex;	// one file is read into memory at a time

std::shared_ptr<RamImage> findImage(const std::string& path)
{
	std::lock_guard<std::mutex> lock(imagesMutex);
	const auto it = images.find(path);
	if (it == images.end())
		return nullptr;
	std::shared_ptr<RamImage> image = it->second.lock();
	if (image == nullptr)
		images.erase(it);
	return image;
}

// The whole of `from` in memory, or nullptr: `stopped` says whether the read
// failed or was cancelled (then the game cannot start); otherwise the file
// does not fit and is to be streamed.
std::shared_ptr<RamImage> loadImage(const std::string& path, SmbFile& from, bool& stopped)
{
	stopped = false;
	std::lock_guard<std::mutex> oneAtATime(imageLoadMutex);
	if (std::shared_ptr<RamImage> image = findImage(path))
		return image;
	const std::string name = lastComponent(path);
	const u64 bytes = (u64)from.size();
	if (ramInUse.load() + bytes > RamBudget)
	{
		diag::mark("smb: %s (%llu MB) is streamed: more than %llu MB would be in memory", name.c_str(),
				(unsigned long long)(bytes >> 20), (unsigned long long)(RamBudget >> 20));
		return nullptr;
	}
	std::shared_ptr<RamImage> image = std::make_shared<RamImage>();
	image->bytes = bytes;
	ramInUse += bytes;
	for (u64 at = 0; at < bytes; at += RamBlock)
	{
		u8 *block = static_cast<u8 *>(malloc((size_t)std::min<u64>(RamBlock, bytes - at)));
		if (block == nullptr)
		{
			diag::mark("smb: %s (%llu MB) is streamed: not enough memory", name.c_str(),
					(unsigned long long)(bytes >> 20));
			return nullptr;
		}
		image->blocks.push_back(block);
	}
	// Small files (a .gdi, a .cue) come and go without a progress bar.
	const bool shown = bytes >= (1u << 20);
	if (shown)
	{
		ramDone = 0;
		ramTotal = bytes;
	}
	const long long started = nowMs();
	u64 done = 0;
	while (done < bytes)
	{
		if (cancelled)
		{
			stopped = true;
			break;
		}
		const size_t inBlock = (size_t)(done % RamBlock);
		const size_t n = (size_t)std::min<u64>({ (u64)RamPiece, bytes - done, (u64)(RamBlock - inBlock) });
		if (from.read(image->blocks[(size_t)(done / RamBlock)] + inBlock, 1, n) != n)
		{
			stopped = true;
			break;
		}
		done += n;
		if (shown)
			ramDone = done;
	}
	if (shown)
		ramTotal = 0;
	if (stopped)
	{
		diag::mark("smb: %s: %s after %llu of %llu MB", name.c_str(), cancelled ? "cancelled" : "the read failed",
				(unsigned long long)(done >> 20), (unsigned long long)(bytes >> 20));
		return nullptr;
	}
	if (shown)
	{
		const double seconds = std::max(0.001, (double)(nowMs() - started) / 1000.0);
		diag::mark("smb: %s in memory: %llu MB in %.1f s (%.0f MB/s)", name.c_str(),
				(unsigned long long)(bytes >> 20), seconds, (double)bytes / (1 << 20) / seconds);
	}
	std::lock_guard<std::mutex> lock(imagesMutex);
	images[path] = image;
	pinned.push_back(image);
	return image;
}

class RamFile : public hostfs::File
{
public:
	explicit RamFile(std::shared_ptr<RamImage> image) : image(std::move(image)), bytes(this->image->bytes)
	{
	}

	size_t read(void *out, size_t size, size_t count) override
	{
		if (size == 0 || count == 0)
			return 0;
		u8 *to = static_cast<u8 *>(out);
		const u64 want = (u64)size * count;
		const u64 available = position < bytes ? bytes - position : 0;
		u64 left = std::min(want, available);
		if (left < want)
			atEnd = true;
		// Whole items only, as fread counts them.
		left -= left % size;
		const size_t items = (size_t)(left / size);
		while (left > 0)
		{
			const size_t inBlock = (size_t)(position % RamBlock);
			const size_t n = (size_t)std::min<u64>(left, RamBlock - inBlock);
			memcpy(to, image->blocks[(size_t)(position / RamBlock)] + inBlock, n);
			to += n;
			position += n;
			left -= n;
		}
		return items;
	}

	size_t write(const void *, size_t, size_t) override
	{
		return 0;
	}

	s64 tell() override
	{
		return (s64)position;
	}

	int seek(s64 offset, int whence) override
	{
		const s64 target = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (s64)position + offset : (s64)bytes + offset;
		if (target < 0)
			return -1;
		position = (u64)target;
		atEnd = false;
		return 0;
	}

	char *gets(char *out, int count) override
	{
		if (count <= 0)
			return nullptr;
		int n = 0;
		while (n < count - 1 && position < bytes)
		{
			const char c = (char)image->blocks[(size_t)(position / RamBlock)][(size_t)(position % RamBlock)];
			position++;
			out[n++] = c;
			if (c == '\n')
				break;
		}
		if (n == 0)
		{
			atEnd = true;
			return nullptr;
		}
		out[n] = '\0';
		return out;
	}

	s64 size() override
	{
		return (s64)bytes;
	}

	int eof() override
	{
		return atEnd ? 1 : 0;
	}

	int error() override
	{
		return 0;
	}

private:
	std::shared_ptr<RamImage> image;
	u64 bytes;
	u64 position = 0;
	bool atEnd = false;
};

// ------------------------------------------------------------ hostfs storage

class SmbStorage : public hostfs::CustomStorage
{
public:
	bool isKnownPath(const std::string& path) override
	{
		return isNetworkPath(path);
	}

	std::vector<hostfs::FileInfo> listContent(const std::string& path) override
	{
		std::vector<hostfs::FileInfo> list;
		std::string relative;
		Share *share = locate(path, relative);
		if (share == nullptr)
			return list;
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!share->connect())
				break;
			const long long started = nowMs();
			smb2dir *dir = smb2_opendir(share->context, relative.c_str());
			if (dir == nullptr)
			{
				const std::string error = stripped(smb2_get_error(share->context));
				if (!share->lost(started))
				{
					// The share answers: it is this folder that cannot be listed.
					note("cannot list " + path.substr(6) + ": " + error);
					break;
				}
				if (attempt == 0)
					continue;
				fail("//" + share->server + "/" + share->share + " stopped answering: " + error);
				break;
			}
			std::string base = path;
			while (!base.empty() && base.back() == '/')
				base.pop_back();
			while (smb2dirent *entry = smb2_readdir(share->context, dir))
			{
				const std::string name = entry->name;
				if (name == "." || name == "..")
					continue;
				list.emplace_back(name, base + "/" + name, entry->st.smb2_type == SMB2_TYPE_DIRECTORY,
						(size_t)entry->st.smb2_size, false, entry->st.smb2_mtime);
			}
			smb2_closedir(share->context, dir);
			break;
		}
		return list;
	}

	hostfs::File *openFile(const std::string& path, const std::string& mode) override
	{
		if (mode.find('w') != std::string::npos || mode.find('a') != std::string::npos || mode.find('+') != std::string::npos)
			return nullptr;		// the share is read from, never written
		std::string relative;
		Share *share = locate(path, relative);
		if (share == nullptr)
			return nullptr;
		// A file that is in memory already is read from there, whoever asks.
		if (std::shared_ptr<RamImage> image = findImage(path))
			return new RamFile(std::move(image));
		if (stopping())
			return nullptr;
		SmbFile *file = open(share, relative);
		if (file == nullptr || !loading || !options().ramCache || file->size() == 0)
			return file;
		// A game is being loaded into memory: this file, whole, now. The share's
		// lock is taken for each piece, not for the whole file.
		bool stopped = false;
		std::shared_ptr<RamImage> image = loadImage(path, *file, stopped);
		if (image != nullptr || stopped)
		{
			delete file;
			return image != nullptr ? new RamFile(std::move(image)) : nullptr;
		}
		file->seek(0, SEEK_SET);
		return file;
	}

	std::string getParentPath(const std::string& path) override
	{
		std::string p = path;
		while (!p.empty() && p.back() == '/')
			p.pop_back();
		const size_t slash = p.find_last_of('/');
		// Not above smb://server/share.
		const size_t serverEnd = p.find('/', 6);
		if (slash == std::string::npos || serverEnd == std::string::npos || slash <= serverEnd)
			return p;
		return p.substr(0, slash);
	}

	std::string getSubPath(const std::string& reference, const std::string& subpath) override
	{
		if (isNetworkPath(subpath))
			return subpath;
		std::string base = reference;
		while (!base.empty() && base.back() == '/')
			base.pop_back();
		std::string sub = subpath;
		std::replace(sub.begin(), sub.end(), '\\', '/');
		while (!sub.empty() && sub.front() == '/')
			sub.erase(sub.begin());
		return base + "/" + sub;
	}

	hostfs::FileInfo getFileInfo(const std::string& path) override
	{
		std::string relative;
		Share *share = locate(path, relative);
		if (share == nullptr)
			throw hostfs::StorageException("Invalid network path " + path);
		smb2_stat_64 st{};
		const int found = stat(share, relative, st);
		if (found < 0)
			throw hostfs::StorageException("Cannot reach " + path);
		if (found == 0)
			throw hostfs::StorageException("Cannot find " + path);
		return hostfs::FileInfo(lastComponent(path), path, st.smb2_type == SMB2_TYPE_DIRECTORY, (size_t)st.smb2_size,
				false, st.smb2_mtime);
	}

	bool exists(const std::string& path) override
	{
		std::string relative;
		Share *share = locate(path, relative);
		if (share == nullptr)
			return false;
		smb2_stat_64 st{};
		return stat(share, relative, st) == 1;
	}

	bool addStorage(bool, bool, const std::string&, void (*)(bool cancelled, std::string selectedPath),
			const std::string&) override
	{
		return false;
	}

private:
	// The file, open for streaming, or nullptr.
	static SmbFile *open(Share *share, const std::string& relative)
	{
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!share->connect())
				return nullptr;
			const long long started = nowMs();
			smb2fh *handle = smb2_open(share->context, relative.c_str(), O_RDONLY);
			if (handle != nullptr)
			{
				smb2_stat_64 st{};
				if (smb2_fstat(share->context, handle, &st) != 0)
				{
					smb2_close(share->context, handle);
					return nullptr;
				}
				return new SmbFile(share, relative, handle, st.smb2_size);
			}
			// Not there (Flycast tries names: a set as .7z, then as .zip), or the
			// connection is gone: then once more on a new one.
			const std::string error = stripped(smb2_get_error(share->context));
			if (!share->lost(started))
				return nullptr;
			if (attempt == 1)
				fail("//" + share->server + "/" + share->share + " stopped answering: " + error);
		}
		return nullptr;
	}

	// 1 the path is there (`st` says what it is), 0 it is not, -1 the share
	// cannot be reached. A connection that is gone is made again once.
	static int stat(Share *share, const std::string& relative, smb2_stat_64& st)
	{
		std::lock_guard<std::recursive_mutex> lock(share->mutex);
		Busy busy;
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!share->connect())
				return -1;
			const long long started = nowMs();
			if (smb2_stat(share->context, relative.c_str(), &st) == 0)
				return 1;
			const std::string error = stripped(smb2_get_error(share->context));
			if (!share->lost(started))
				return 0;
			if (attempt == 1)
				fail("//" + share->server + "/" + share->share + " stopped answering: " + error);
		}
		return -1;
	}
};

std::string trim(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a]))
		a++;
	while (b > a && isspace((unsigned char)s[b - 1]))
		b--;
	return s.substr(a, b - a);
}

// "192.168.1.10/Share/Folder", "//server/share", "\\server\share\folder" and
// "smb://server/share" all name the same kind of place.
std::string normalize(std::string path)
{
	std::replace(path.begin(), path.end(), '\\', '/');
	if (path.rfind("smb://", 0) == 0)
		path = path.substr(6);
	while (!path.empty() && path.front() == '/')
		path.erase(path.begin());
	while (!path.empty() && path.back() == '/')
		path.pop_back();
	if (path.find('/') == std::string::npos)
		return "";		// a server with no share
	return "smb://" + path;
}

} // namespace

void loadConfig()
{
	const std::string file = rootDir + "network.cfg";
	FILE *f = fopen(file.c_str(), "r");
	if (f == nullptr)
	{
		// A template to fill in.
		if ((f = fopen(file.c_str(), "w")) != nullptr)
		{
			fputs("# PSFlyCast - games on a network share (SMB / Windows sharing).\n"
					"#\n"
					"# One \"path\" line for each folder to scan: server/share/folder, the server by\n"
					"# its IP address. For example:\n"
					"#   path = 192.168.1.10/Games/Dreamcast\n"
					"# Remove the # in front of a path line to use it. The folder and the\n"
					"# folders inside it are scanned for games the first time the library's\n"
					"# Network tab is opened, and again with Square on that tab; the list is\n"
					"# kept, so the share is not asked again until a game is started.\n"
					"#\n"
					"# The account: leave it as guest with no password for an open share.\n"
					"\n"
					"# path = server/share/folder\n"
					"user = guest\n"
					"password =\n"
					"# domain = WORKGROUP\n", f);
			fclose(f);
		}
		return;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f) != nullptr)
	{
		const std::string text = trim(line);
		if (text.empty() || text[0] == '#' || text[0] == ';')
			continue;
		const size_t equals = text.find('=');
		if (equals == std::string::npos)
			continue;
		const std::string key = trim(text.substr(0, equals));
		const std::string value = trim(text.substr(equals + 1));
		if (key == "path")
		{
			const std::string folder = normalize(value);
			if (!folder.empty())
				folders.push_back(folder);
			else
				diag::mark("smb: network.cfg: \"%s\" is not server/share/folder", value.c_str());
		}
		else if (key == "user")
			account.user = value.empty() ? "guest" : value;
		else if (key == "password")
			account.password = value;
		else if (key == "domain")
			account.domain = value;
	}
	fclose(f);
	for (const std::string& folder : folders)
		diag::mark("smb: games folder %s (user %s)", folder.c_str(), account.user.c_str());
}

const std::vector<std::string>& gameFolders()
{
	return folders;
}

bool isNetworkPath(const std::string& path)
{
	return path.rfind("smb://", 0) == 0;
}

void retryNow()
{
	std::lock_guard<std::mutex> lock(sharesMutex);
	for (auto& [name, share] : shares)
	{
		// Only a share nobody is using: failedAt is the connection's own.
		std::unique_lock<std::recursive_mutex> own(share->mutex, std::try_to_lock);
		if (own.owns_lock())
			share->failedAt = 0;
	}
}

void beginLoad()
{
	retryNow();
	cancelled = false;
	ramTotal = 0;
	ramDone = 0;
	loading = true;
	diag::mark("smb: a network game is starting (%s)", options().ramCache ? "read into memory" : "streamed");
}

void endLoad()
{
	loading = false;
	cancelled = false;
	ramTotal = 0;
	// The images no file has open go (a .gdi, an archive tried as 7z); the
	// game's own stay as long as it has them open.
	std::vector<std::shared_ptr<RamImage>> done;
	{
		std::lock_guard<std::mutex> lock(imagesMutex);
		done.swap(pinned);
	}
}

void cancelLoad()
{
	cancelled = true;
}

Status status()
{
	Status now;
	const u64 total = ramTotal.load(), done = ramDone.load();
	if (total != 0)
		now.progress = std::clamp((float)((double)done / (double)total), 0.f, 1.f);
	const long long since = busySince.load();
	const long long waited = since != 0 ? nowMs() - since : 0;
	char text[128];
	if (waited >= 2500)
	{
		// No answer for a while: a NAS waking its disks, or a server that is gone.
		snprintf(text, sizeof(text), "Waiting for the network share (%d s)", (int)(waited / 1000));
		now.text = text;
	}
	else if (total != 0)
	{
		snprintf(text, sizeof(text), "Loading into memory   %u / %u MB", (unsigned)(done >> 20), (unsigned)(total >> 20));
		now.text = text;
	}
	return now;
}

unsigned failures()
{
	return failureCount.load();
}

int tcpAnswers(const std::string& ip, int port, int waitMs)
{
	sockaddr_in address;
	if (!numericAddress(ip, port, address))
		return EINVAL;
	const std::shared_ptr<ConnectAttempt> attempt = startConnect(address);
	const long long deadline = nowMs() + waitMs;
	std::unique_lock<std::mutex> lock(attempt->mutex);
	while (!attempt->finished && nowMs() < deadline)
		attempt->done.wait_for(lock, std::chrono::milliseconds(100));
	if (!attempt->finished)
		return -1;
	return attempt->error;
}

void clearError()
{
	std::lock_guard<std::mutex> lock(errorMutex);
	errorText.clear();
}

std::string lastError()
{
	std::lock_guard<std::mutex> lock(errorMutex);
	return errorText;
}

} // namespace ps5::smb

namespace hostfs
{
CustomStorage& customStorage()
{
	static ps5::smb::SmbStorage storage;
	return storage;
}
}
