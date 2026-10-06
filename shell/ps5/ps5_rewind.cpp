/*
	PSFlyCast - rewind: going back a few seconds, or minutes, in the game.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Flycast has save states and, for netplay, a state in memory at every
	frame; it has no rewind. This is one made of the first: every few seconds
	of play the whole machine is serialised as "Save state" serialises it
	(dc_serialize, with its memories: the netplay kind leaves them out and
	tracks their pages instead, which costs a page fault per page written and
	is not worth it for one state in five seconds), compressed with zstd, and
	kept in memory, the oldest dropped when they are too old or too many.
	Going back loads one as "Load state" does (Emulator::loadstate).

	Who does what:

	  The emulator's thread takes the snapshot. A state is only whole
	  between two time slices of the SH4, when no device is half-way through
	  its turn. Flycast has no call there in a normal game (the thread stays
	  inside the SH4's run loop until the emulator is stopped), and its
	  vertical blank event is no such place: it is raised from inside the
	  video scheduler's own callback, before that callback has asked for its
	  next turn, with whatever comes after it in the scheduler's list still
	  to be called. A state taken there has a video timer that never fires
	  again. So the vertical blank event (frame()) only keeps the time and,
	  when a snapshot is due, asks the SH4 scheduler for a turn of this
	  file's own (tick()), which comes at the end of that scheduler pass or
	  the next, 448 SH4 cycles later. There every callback before this one
	  in the list has finished and asked for its next turn. Whether one after
	  it is still waiting for this same pass is asked of the scheduler
	  (tickIsClean()); if so the snapshot is put off by a pass. What tick()
	  serialises is then what Emulator::stop() would have left, except that
	  the interrupts just raised are not yet taken: they are in the state,
	  and taken a time slice after it is loaded.
	  It costs this thread one copy of the machine (16 + 8 + 2 MiB of
	  memories and a megabyte or two of the rest for a Dreamcast; 32 + 16 + 8
	  for a NAOMI, and 32 more for a NAOMI 2's), into a buffer kept from one
	  snapshot to the next. That is the hitch there is: a few milliseconds
	  every interval. The boot log has the first ones' real cost.

	  A thread of this file's own compresses it, and puts it in the ring.
	  The emulator's thread never waits for it: while it still has the
	  buffer, no snapshot is taken (the next frame tries again).

	  The interface's thread lists the points and goes back to one, with the
	  emulator stopped. It decompresses on its own.

	  One mutex guards the ring and who has the buffer. It is never held
	  across a serialisation, a compression, a decompression or the freeing
	  of a snapshot, and the emulator's thread only tries it before a
	  snapshot (taken by someone else: the next frame tries again).

	The render thread reads the emulated video memory while the SH4 runs, and
	writes it when a game renders to a texture that is copied back, or with
	the full framebuffer emulation: a snapshot can hold such a texture half
	written, as the running game can see it half written. The game draws it
	again.

	Off during netplay and while the game is online (a state from before
	would break the connection), for multi-board and linked arcade games,
	and with hardcore achievements, as "Load state" is. Every line of
	flycast-boot.log about it starts with "rewind:".
*/
#include "ps5_rewind.h"

#include <zstd.h>
#include <zstd_errors.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "emulator.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "ps5_diag.h"

#ifndef PS5_REWIND_TEST
#include "cfg/option.h"
#include "hw/pvr/Renderer_if.h"
#include "network/ggpo.h"
#include "network/net_handshake.h"
#include "oslib/oslib.h"
#include "serialize.h"
#include "ui/gui.h"
#endif

namespace ps5::rewind
{
namespace
{
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point from)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

// ---- the history: the ring, the buffer and the compressor's thread. Nothing
// of the emulator is in it: times are numbers that only grow, a state is
// bytes.

class History
{
public:
	using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

	struct Shot
	{
		uint64_t id = 0;
		uint64_t time = 0;
		size_t rawBytes = 0;
		Bytes packed;
		Bytes picture;
		int pictureWidth = 0;
		int pictureHeight = 0;

		size_t bytes() const {
			return (packed != nullptr ? packed->size() : 0) + (picture != nullptr ? picture->size() : 0);
		}
	};

	// What became of a snapshot, told on the compressor's thread.
	struct Report
	{
		uint64_t id = 0;
		bool ok = false;		// compressed
		bool kept = false;		// and put in the ring (not if it was cleared meanwhile)
		size_t rawBytes = 0;
		size_t packedBytes = 0;
		double serialiseMs = 0;
		double packMs = 0;
		size_t points = 0;		// in the ring afterwards
		size_t held = 0;		// bytes
	};

	enum class Begin { Ready, Busy, NoMemory };
	enum class Unpack { Ok, Gone, NoMemory, Corrupt };

	History(std::function<void(const Report&)> report, std::function<void()> threadStarts = nullptr)
		: report(std::move(report)), threadStarts(std::move(threadStarts))
	{
		worker = std::thread([this] { run(); });
	}

	~History()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			quit = true;
		}
		wake.notify_all();
		worker.join();
		if (cctx != nullptr)
			ZSTD_freeCCtx(cctx);
	}

	// -- the thread that takes the snapshots

	// Whether the buffer is with the compressor (or being filled). Costs
	// nothing: asked at every frame.
	bool busy() const {
		return inUse.load(std::memory_order_relaxed);
	}

	// A buffer of at least capacity bytes to serialise into, or the reason
	// there is none. Never waits: not for the compressor, and not for the
	// mutex either.
	Begin beginCapture(size_t capacity, uint8_t *& to)
	{
		{
			std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
			if (!lock.owns_lock() || stage != Stage::Free)
				return Begin::Busy;
			stage = Stage::Filling;
			inUse = true;
			job.epoch = epoch;
		}
		// Filling: the buffer is this thread's alone. It only grows: how much
		// a state can take changes from one frame to the next (the display
		// lists being built are counted at their largest).
		if (capacity > rawCapacity)
		{
			constexpr size_t Step = 1024 * 1024;
			const size_t rounded = (capacity + Step - 1) / Step * Step;
			raw.reset();
			rawCapacity = 0;
			raw.reset(new (std::nothrow) uint8_t[rounded]);
			if (raw == nullptr)
			{
				abortCapture();
				return Begin::NoMemory;
			}
			rawCapacity = rounded;
		}
		to = raw.get();
		return Begin::Ready;
	}

	// The buffer holds used bytes of a state taken at time: the compressor
	// has it from here. Returns the snapshot's id, 0 if the history was
	// cleared while it was being taken.
	uint64_t commitCapture(size_t used, uint64_t time, double serialiseMs)
	{
		std::unique_ptr<uint8_t[]> doomed;
		std::lock_guard<std::mutex> lock(mutex);
		if (job.epoch != epoch)
		{
			freeBuffer(doomed);
			return 0;
		}
		job.id = ++lastId;
		job.used = used;
		job.time = time;
		job.serialiseMs = serialiseMs;
		stage = Stage::Queued;
		wake.notify_all();
		return job.id;
	}

	void abortCapture()
	{
		std::unique_ptr<uint8_t[]> doomed;
		std::lock_guard<std::mutex> lock(mutex);
		freeBuffer(doomed);
	}

	// -- anyone

	// The ring as it is at time now, the newest first, without what has
	// become too old. listed() answers for this list.
	void list(uint64_t now, std::vector<Shot>& out)
	{
		// What the caller's list held is let go of after the mutex, as all
		// that may free a snapshot is.
		std::vector<Shot> fresh;
		std::deque<Shot> doomed;
		{
			std::lock_guard<std::mutex> lock(mutex);
			evict(now, doomed);
			fresh.assign(ring.rbegin(), ring.rend());
			listing.clear();
			for (const Shot& shot : fresh)
				listing.push_back(shot.id);
		}
		out.swap(fresh);
	}

	// The id of what was at index in the last list.
	bool listed(size_t index, uint64_t& id)
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (index >= listing.size())
			return false;
		id = listing[index];
		return true;
	}

	// The state of a snapshot, as it was serialised.
	Unpack unpack(uint64_t id, std::unique_ptr<uint8_t[]>& state, size_t& size, uint64_t& time)
	{
		Bytes packed;
		{
			std::lock_guard<std::mutex> lock(mutex);
			for (const Shot& shot : ring)
				if (shot.id == id)
				{
					packed = shot.packed;
					size = shot.rawBytes;
					time = shot.time;
				}
		}
		if (packed == nullptr)
			return Unpack::Gone;
		// The snapshot is not the ring's alone any more: it stays whole
		// here whatever the ring drops meanwhile.
		state.reset(new (std::nothrow) uint8_t[size]);
		if (state == nullptr)
			return Unpack::NoMemory;
		const size_t got = ZSTD_decompress(state.get(), size, packed->data(), packed->size());
		if (ZSTD_isError(got))
			return ZSTD_getErrorCode(got) == ZSTD_error_memory_allocation ? Unpack::NoMemory : Unpack::Corrupt;
		return got == size ? Unpack::Ok : Unpack::Corrupt;
	}

	// The machine went back to this snapshot: what came after it never
	// happened, the one still with the compressor included.
	void dropNewerThan(uint64_t id)
	{
		std::deque<Shot> doomed;
		std::lock_guard<std::mutex> lock(mutex);
		epoch++;
		pendingPicture = Shot();
		while (!ring.empty() && ring.back().id > id)
			drop(false, doomed);
	}

	// Forgets everything; the snapshot being taken or compressed is thrown
	// away when it is done. With buffers, the memory worked in is given back
	// too (a game ended).
	void clear(bool buffers)
	{
		std::deque<Shot> doomed;
		std::unique_ptr<uint8_t[]> doomedBuffer;
		std::lock_guard<std::mutex> lock(mutex);
		epoch++;
		pendingPicture = Shot();
		doomed.swap(ring);
		held = 0;
		listing.clear();
		if (buffers)
		{
			dropBuffer = true;
			dropScratch = true;
			if (stage == Stage::Free)
				freeBuffer(doomedBuffer);
			wake.notify_all();
		}
	}

	// A picture for a snapshot, which may still be with the compressor.
	void attachPicture(uint64_t id, Bytes picture, int width, int height)
	{
		std::deque<Shot> doomed;
		std::lock_guard<std::mutex> lock(mutex);
		for (Shot& shot : ring)
			if (shot.id == id)
			{
				held -= shot.bytes();
				shot.picture = std::move(picture);
				shot.pictureWidth = width;
				shot.pictureHeight = height;
				held += shot.bytes();
				evict(ring.back().time, doomed);
				return;
			}
		if ((stage == Stage::Queued || stage == Stage::Packing) && job.id == id && job.epoch == epoch)
		{
			pendingPicture.id = id;
			pendingPicture.picture = std::move(picture);
			pendingPicture.pictureWidth = width;
			pendingPicture.pictureHeight = height;
		}
	}

	void setBudget(size_t bytes)
	{
		std::deque<Shot> doomed;
		std::lock_guard<std::mutex> lock(mutex);
		budget = bytes;
		if (!ring.empty())
			evict(ring.back().time, doomed);
	}

	void setMaxAge(uint64_t age)
	{
		std::lock_guard<std::mutex> lock(mutex);
		maxAge = age;
	}

	size_t count()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return ring.size();
	}

	size_t heldBytes()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return held;
	}

#ifdef PS5_REWIND_TEST
	// For the test on the build machine: a compressor as slow as asked, and
	// waiting until it has nothing left to do.
	std::atomic<int> packDelayMs{0};
	void waitIdle()
	{
		std::unique_lock<std::mutex> lock(mutex);
		idle.wait(lock, [this] { return stage == Stage::Free; });
	}
	size_t bufferBytes()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return rawCapacity;
	}
#endif

private:
	enum class Stage
	{
		Free,		// nobody has the buffer
		Filling,	// the snapshots' thread serialises into it
		Queued,		// full, waiting for the compressor
		Packing,	// the compressor reads it
	};

	struct Job
	{
		uint64_t id = 0;
		uint64_t epoch = 0;
		uint64_t time = 0;
		size_t used = 0;
		double serialiseMs = 0;
	};

	// The following are called with the mutex held. What they take out of
	// the ring goes to doomed, which the caller frees after the mutex: a
	// snapshot is megabytes, and the emulator's thread may be at the door.

	void drop(bool oldest, std::deque<Shot>& doomed)
	{
		Shot& shot = oldest ? ring.front() : ring.back();
		held -= shot.bytes();
		doomed.push_back(std::move(shot));
		if (oldest)
			ring.pop_front();
		else
			ring.pop_back();
	}

	void evict(uint64_t now, std::deque<Shot>& doomed)
	{
		while (!ring.empty() && now > ring.front().time && now - ring.front().time > maxAge)
			drop(true, doomed);
		while (!ring.empty() && held > budget)
			drop(true, doomed);
	}

	void freeBuffer(std::unique_ptr<uint8_t[]>& doomed)
	{
		stage = Stage::Free;
		inUse = false;
		if (dropBuffer)
		{
			doomed = std::move(raw);
			rawCapacity = 0;
			dropBuffer = false;
		}
		idle.notify_all();
	}

	// zstd, in one go when what it gives fits in scratch, which is kept from
	// one snapshot to the next and grows to what this game's states take.
	bool pack(const uint8_t *from, size_t size, size_t& packedSize)
	{
		if (cctx == nullptr)
		{
			cctx = ZSTD_createCCtx();
			if (cctx == nullptr)
				return false;
			// The fastest of the ordinary levels: a state is mostly memory
			// the game has not touched, and the levels above gain little on
			// it for twice the time. With a checksum, so that a state that
			// is not what it was is refused rather than loaded.
			ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1);
			ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1);
		}
		ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only);
		ZSTD_CCtx_setPledgedSrcSize(cctx, size);
		if (scratch.size() < size / 4 + 65536)
			scratch.resize(size / 4 + 65536);
		ZSTD_inBuffer in{ from, size, 0 };
		ZSTD_outBuffer out{ scratch.data(), scratch.size(), 0 };
		for (;;)
		{
			const size_t left = ZSTD_compressStream2(cctx, &out, &in, ZSTD_e_end);
			if (ZSTD_isError(left))
				return false;
			if (left == 0)
				break;
			scratch.resize(scratch.size() + std::max(scratch.size() / 2, left));
			out.dst = scratch.data();
			out.size = scratch.size();
		}
		packedSize = out.pos;
		return true;
	}

	void run()
	{
		if (threadStarts)
			threadStarts();
		std::unique_lock<std::mutex> lock(mutex);
		for (;;)
		{
			wake.wait(lock, [this] { return quit || stage == Stage::Queued || dropScratch; });
			if (quit)
				break;
			if (dropScratch)
			{
				dropScratch = false;
				std::vector<uint8_t> doomed;
				doomed.swap(scratch);
				lock.unlock();
				doomed = std::vector<uint8_t>();
				lock.lock();
				continue;
			}
			stage = Stage::Packing;
			const Job taken = job;
			lock.unlock();

#ifdef PS5_REWIND_TEST
			if (packDelayMs > 0)
				std::this_thread::sleep_for(std::chrono::milliseconds(packDelayMs));
#endif
			Report done;
			done.id = taken.id;
			done.rawBytes = taken.used;
			done.serialiseMs = taken.serialiseMs;
			Shot shot;
			shot.id = taken.id;
			shot.time = taken.time;
			shot.rawBytes = taken.used;
			const Clock::time_point began = Clock::now();
			try {
				size_t packedSize = 0;
				if (pack(raw.get(), taken.used, packedSize))
				{
					shot.packed = std::make_shared<const std::vector<uint8_t>>(scratch.begin(), scratch.begin() + packedSize);
					done.packedBytes = packedSize;
					done.ok = true;
				}
			} catch (const std::bad_alloc&) {
			}
			done.packMs = msSince(began);

			std::deque<Shot> doomed;
			std::unique_ptr<uint8_t[]> doomedBuffer;
			lock.lock();
			if (done.ok && taken.epoch == epoch)
			{
				if (pendingPicture.id == shot.id)
				{
					shot.picture = std::move(pendingPicture.picture);
					shot.pictureWidth = pendingPicture.pictureWidth;
					shot.pictureHeight = pendingPicture.pictureHeight;
				}
				held += shot.bytes();
				ring.push_back(std::move(shot));
				evict(taken.time, doomed);
				done.kept = !ring.empty() && ring.back().id == taken.id;
			}
			pendingPicture = Shot();
			done.points = ring.size();
			done.held = held;
			freeBuffer(doomedBuffer);
			lock.unlock();

			doomed.clear();
			doomedBuffer.reset();
			shot = Shot();
			if (report)
				report(done);
			lock.lock();
		}
	}

	std::function<void(const Report&)> report;
	std::function<void()> threadStarts;

	std::mutex mutex;
	std::condition_variable wake;	// for the compressor: a full buffer, or the end
	std::condition_variable idle;	// the buffer is free again
	std::deque<Shot> ring;			// the oldest first
	size_t held = 0;				// the bytes of what is in the ring
	size_t budget = 384u * 1024 * 1024;
	uint64_t maxAge = UINT64_MAX;
	std::vector<uint64_t> listing;	// the ids of the last list()
	Shot pendingPicture;			// a picture that came before its snapshot was compressed
	uint64_t lastId = 0;
	// Grows whenever what is being taken or compressed must not reach the
	// ring any more.
	uint64_t epoch = 0;
	Stage stage = Stage::Free;
	std::atomic<bool> inUse{false};	// stage is not Free
	Job job;
	bool dropBuffer = false;		// free the buffer the next time nobody has it
	bool dropScratch = false;		// and the compressor's
	bool quit = false;

	// Whoever the stage says has it.
	std::unique_ptr<uint8_t[]> raw;
	size_t rawCapacity = 0;

	// The compressor's thread alone.
	ZSTD_CCtx *cctx = nullptr;
	std::vector<uint8_t> scratch;

	std::thread worker;
};

// ---- the machine: all that is asked of Flycast, but for its events and its
// scheduler. (The test on the build machine has one of its own.)

#ifndef PS5_REWIND_TEST
namespace machine
{
constexpr uint64_t TicksPerSecond = SH4_MAIN_CLOCK;

// The time of play, in SH4 cycles: it stands still in the menus, and goes
// back with a state.
uint64_t now()
{
	return sh4_sched_now64();
}

// Why this game cannot be rewound now, or null. What "Load state" refuses,
// and netplay: a state from before breaks what the other side believes.
const char *refusal()
{
	if (settings.content.path.empty())
		return "no game is loaded";
	if (config::GGPOEnable || ggpo::active())
		return "netplay";
	if (settings.network.online)
		return "the game is online";
	if (settings.naomi.multiboard || settings.naomi.slave)
		return "the game runs on more than one arcade board";
	if (naomiNetworkSupported())
		return "the game links arcade cabinets";
	if (settings.raHardcoreMode)
		return "hardcore achievements";
	if (!dc_savestateAllowed())
		return "states cannot be loaded now";
	return nullptr;
}

// The whole machine into to; or, with no buffer, the most that can take.
// (Flycast counts each display list being built at its largest then: more
// than is written.) On the emulator's thread.
size_t serialise(uint8_t *to, size_t capacity)
{
	if (to == nullptr)
	{
		Serializer measure;
		dc_serialize(measure);
		return measure.size();
	}
	Serializer ser(to, capacity);
	dc_serialize(ser);
	return ser.size();
}

bool stopped()
{
	return !emu.running();
}

// As dc_loadstate() loads the state of a file: the recompilers' code and the
// texture cache go, the controllers are made again. With the emulator
// stopped.
// Returns how much of the state was read.
size_t load(const uint8_t *from, size_t size)
{
	// The state was taken with the SH4 running, and says so. Flycast's own
	// are taken with it stopped, and resetting the machine (leaving the
	// game from the menu, without resuming first) insists that it is.
	// Whatever becomes of the load.
	struct Stopped
	{
		~Stopped() { Sh4cntx.CpuRunning = false; }
	} stopped;
	Deserializer deser(from, size);
	emu.loadstate(deser);
	return deser.size();
}

bool threaded()
{
	return config::ThreadedRendering;
}

// The last frame shown, width pixels wide, as RGB. On the render thread.
bool lastFrame(std::vector<uint8_t>& rgb, int& width, int& height)
{
	return renderer != nullptr && renderer->GetLastFrame(rgb, width, height);
}

// How a screenshot gets to the render thread: with the next frame's overlay.
void onRenderThread(std::function<void()> task)
{
	gui_runOnUiThread(std::move(task));
}

void nameThread()
{
	os_SetThreadName("Rewind");
}

std::string game()
{
	return settings.content.gameId.empty() ? settings.content.fileName : settings.content.gameId;
}
}
#endif

// ---- taking the snapshots

constexpr int LoggedShots = 5;		// whose cost the boot log has, for each game
constexpr int LoggedPictures = 3;
constexpr int PictureWidth = 160;
// How many scheduler passes in a row a snapshot is put off for before this
// frame gives up on it (the next one asks again).
constexpr int MaxPutOff = 64;

History *history;	// made by init(), for good: its thread has no end either

std::atomic<bool> on{false};			// the option
std::atomic<bool> pictures{false};
std::atomic<bool> loaded{false};		// a game is
std::atomic<bool> offForGame{false};	// a snapshot of it failed
std::atomic<bool> checkWorks{false};	// tickIsClean() tells what it should, on this build
std::atomic<bool> restoring{false};		// the state being loaded is one of these
// The next snapshot is a whole interval from now: set by whatever moved the
// clock or emptied the ring, obeyed by the emulator's thread.
std::atomic<bool> resync{true};
std::atomic<uint64_t> interval{5 * machine::TicksPerSecond};
std::atomic<uint64_t> playTime{0};		// the clock at the last vertical blank

// The emulator's thread alone (and whoever loads a game, which is when it
// does not run).
int schedId = -1;
uint64_t nextDue;
int putOff;

// For the boot log.
std::atomic<unsigned> shots{0}, missed{0}, passesPutOff{0}, framesGivenUp{0}, picturesLogged{0};
std::mutex totalsMutex;
struct Totals
{
	double serialiseMs = 0, serialiseMax = 0, packMs = 0, packMax = 0;
	uint64_t rawBytes = 0, packedBytes = 0;
	unsigned count = 0;
} totals;

bool wanted()
{
	return on.load(std::memory_order_relaxed) && loaded.load(std::memory_order_relaxed)
			&& !offForGame.load(std::memory_order_relaxed) && checkWorks.load(std::memory_order_relaxed);
}

// Something went wrong with this game's snapshots: no more of them, and none
// offered, until another game is loaded.
void fail(const char *what)
{
	if (offForGame.exchange(true))
		return;
	history->clear(true);
	diag::mark("rewind: %s: off until another game is loaded", what);
}

// Whether every scheduler callback that is due has had its turn.
//
// The scheduler calls, in the order of its list, each callback due in the
// time slice that just ended, and this is asked from inside one of them. A
// callback further down the list whose time has also come is still waiting,
// with a time to be called at that is now in the past. A state taken here
// would say so too, and once loaded, the scheduler would find that time
// behind it and not come back to it until its 32-bit clock has gone round:
// 21 seconds without that device (the sound, a timer, the disc drive).
//
// The scheduler does not show its list. It does tell how long until the
// soonest callback, counted from its clock, which is the difference of two
// numbers of which one is in the SH4's context. Set back by a few time
// slices, the clock makes a callback that is waiting look like the soonest
// one, due in less than that; without any, the soonest is at least that far.
// The clock is then put back to the cycle. (Its own callback is not in the
// way: the scheduler takes a callback off before calling it.)
bool tickIsClean()
{
	constexpr int Window = 4 * SH4_TIMESLICE;
	Sh4cntx.sh4_sched_next += Window;
	sh4_sched_ffts();
	const bool clean = Sh4cntx.sh4_sched_next >= Window;
	Sh4cntx.sh4_sched_next -= Window;
	sh4_sched_ffts();
	return clean;
}

// tickIsClean() leans on how Flycast's scheduler keeps its time, which an
// update of Flycast may change. So it is tried, at each game's start, on a
// callback that is made late on purpose (this file's own, asked for with
// the clock set back): it must be seen, and nothing must be seen once it is
// called off, with the clock where it was. If not, there is no rewind, and
// the boot log says why. The emulator does not run.
bool tickCheckWorks()
{
	constexpr int Late = SH4_TIMESLICE;
	const uint64_t before = machine::now();
	sh4_sched_request(schedId, -1);
	const bool cleanBefore = tickIsClean();
	Sh4cntx.sh4_sched_next += Late;
	sh4_sched_request(schedId, 0);
	Sh4cntx.sh4_sched_next -= Late;
	sh4_sched_ffts();
	const bool seen = !tickIsClean();
	sh4_sched_request(schedId, -1);
	const bool cleanAfter = tickIsClean();
	return cleanBefore && seen && cleanAfter && machine::now() == before;
}

// A picture for the snapshot just taken: the frame on screen, asked of the
// render thread the way a screenshot is.
void askForPicture(uint64_t id)
{
	machine::onRenderThread([id] {
		try {
			const Clock::time_point began = Clock::now();
			std::vector<uint8_t> rgb;
			int width = PictureWidth, height = 0;
			if (!machine::lastFrame(rgb, width, height) || width <= 0 || height <= 0
					|| rgb.size() < (size_t)width * height * 3)
				return;
			auto rgba = std::make_shared<std::vector<uint8_t>>((size_t)width * height * 4);
			const uint8_t *from = rgb.data();
			uint8_t *to = rgba->data();
			for (size_t i = (size_t)width * height; i != 0; i--, from += 3, to += 4)
			{
				to[0] = from[0];
				to[1] = from[1];
				to[2] = from[2];
				to[3] = 255;
			}
			const double ms = msSince(began);
			history->attachPicture(id, std::move(rgba), width, height);
			if (picturesLogged.fetch_add(1) < LoggedPictures)
				diag::mark("rewind: a %dx%d picture took the render thread %.2f ms", width, height, ms);
		} catch (const std::exception& e) {
			diag::mark("rewind: no picture: %s", e.what());
		}
	});
}

// The snapshot. On the emulator's thread, between two time slices.
void take(uint64_t now)
{
	try {
		const Clock::time_point began = Clock::now();
		const size_t most = machine::serialise(nullptr, 0);
		uint8_t *to = nullptr;
		switch (history->beginCapture(most, to))
		{
		case History::Begin::Busy:
			return;
		case History::Begin::NoMemory:
			fail("no memory for a snapshot");
			return;
		case History::Begin::Ready:
			break;
		}
		size_t used;
		try {
			used = machine::serialise(to, most);
		} catch (...) {
			history->abortCapture();
			throw;
		}
		const uint64_t id = history->commitCapture(used, now, msSince(began));
		// An interval after this one was due, not after it was taken: a
		// frame late each time would add up.
		const uint64_t every = interval;
		nextDue = now - nextDue < every ? nextDue + every : now + every;
		if (id != 0 && pictures && machine::threaded())
			askForPicture(id);
	} catch (const std::exception& e) {
		fail((std::string("taking a snapshot failed (") + e.what() + ")").c_str());
	} catch (...) {
		fail("taking a snapshot failed");
	}
}

// The scheduler's callback: frame() asked for it. (It can also come long
// after it was asked for, when a state was loaded in between and the clock
// went elsewhere: hence asking again whether one is due.)
int tick(int, int, int, void *)
{
	if (!wanted() || resync)
		return 0;
	const uint64_t now = machine::now();
	if (now < nextDue || history->busy())
		return 0;
	if (!tickIsClean())
	{
		// A callback after this one is still to come in this pass: the pass
		// after it, then.
		passesPutOff++;
		if (++putOff <= MaxPutOff)
			return 1;
		putOff = 0;
		if (framesGivenUp.fetch_add(1) == 0)
			diag::mark("rewind: %d scheduler passes in a row had a device waiting for its turn: no snapshot at this frame"
					" (said once; the count is given when the game ends)", MaxPutOff);
		return 0;
	}
	putOff = 0;
	take(now);
	return 0;
}

// Once per emulated frame, on the emulator's thread: Flycast's vertical
// blank event.
void frame(Event, void *)
{
	if (!wanted() || schedId < 0)
		return;
	const uint64_t now = machine::now();
	playTime = now;
	const uint64_t every = interval;
	if (resync.exchange(false) || nextDue > now + every)
		nextDue = now + every;
	if (now < nextDue)
		return;
	if (machine::refusal() != nullptr)
	{
		nextDue = now + every;
		return;
	}
	if (history->busy())
	{
		// The compressor still has the one before: no waiting here, the
		// next frame will do.
		missed++;
		return;
	}
	// At the end of this scheduler pass if this file's callback is after
	// the video's in the list, of the next one if it is before.
	sh4_sched_request(schedId, 0);
}

// ---- what the compressor's thread says of each snapshot

void packed(const History::Report& done)
{
	if (!done.ok)
	{
		fail("compressing a snapshot failed");
		return;
	}
	{
		std::lock_guard<std::mutex> lock(totalsMutex);
		totals.count++;
		totals.serialiseMs += done.serialiseMs;
		totals.serialiseMax = std::max(totals.serialiseMax, done.serialiseMs);
		totals.packMs += done.packMs;
		totals.packMax = std::max(totals.packMax, done.packMs);
		totals.rawBytes += done.rawBytes;
		totals.packedBytes += done.packedBytes;
	}
	const unsigned number = shots.fetch_add(1) + 1;
	if (number <= LoggedShots)
		diag::mark("rewind: snapshot %u: %zu bytes serialised in %.2f ms, compressed to %zu bytes in %.1f ms%s;"
				" %zu point(s), %.1f MiB held", number, done.rawBytes, done.serialiseMs, done.packedBytes, done.packMs,
				done.kept ? "" : " (not kept)", done.points, done.held / 1048576.0);
}

void summary()
{
	Totals all;
	{
		std::lock_guard<std::mutex> lock(totalsMutex);
		all = totals;
		totals = Totals();
	}
	if (all.count != 0)
		diag::mark("rewind: %u snapshot(s): serialising %.2f ms on average, %.2f ms at most (%.1f MiB each);"
				" compressing %.1f ms on average, %.1f ms at most (to %.1f MiB each);"
				" %u frame(s) waited for the compressor, %u scheduler pass(es) put off, %u frame(s) gave up",
				all.count, all.serialiseMs / all.count, all.serialiseMax, all.rawBytes / 1048576.0 / all.count,
				all.packMs / all.count, all.packMax, all.packedBytes / 1048576.0 / all.count,
				missed.load(), passesPutOff.load(), framesGivenUp.load());
	shots = 0;
	missed = 0;
	passesPutOff = 0;
	framesGivenUp = 0;
	picturesLogged = 0;
}

// One line for the game loaded: whether it is rewound, and how.
void describe()
{
	if (!loaded)
		return;
	const char *why = machine::refusal();
	if (!on)
		diag::mark("rewind: off (the option)");
	else if (!checkWorks)
		diag::mark("rewind: off: this build's scheduler does not answer as ps5_rewind.cpp expects (tickCheckWorks)");
	else if (why != nullptr)
		diag::mark("rewind: not for %s: %s", machine::game().c_str(), why);
	else
		diag::mark("rewind: on for %s: a snapshot every %d s of play%s", machine::game().c_str(),
				(int)(interval / machine::TicksPerSecond), pictures ? ", with a picture" : "");
}

// ---- the emulator's events

// A game was loaded (on the loader's thread; the emulator does not run, so
// the scheduler's list can be added to).
void started(Event, void *)
{
	if (schedId < 0)
		schedId = sh4_sched_register(0, tick);
	history->clear(false);
	summary();
	offForGame = false;
	putOff = 0;
	resync = true;
	checkWorks = tickCheckWorks();
	loaded = true;
	describe();
}

void ended(Event, void *)
{
	loaded = false;
	history->clear(true);
	summary();
}

// A state was loaded, the disc was changed, or the game went online or came
// back: what was kept is of another time, another disc or another session.
void moved(Event event, void *)
{
	if (event == Event::LoadState && restoring)
		return;
	history->clear(false);
	resync = true;
}
} // namespace

void init()
{
	if (history != nullptr)
		return;
	history = new History(packed, machine::nameThread);
	history->setMaxAge(180 * machine::TicksPerSecond);
	EventManager::listen(Event::Start, started);
	EventManager::listen(Event::Terminate, ended);
	EventManager::listen(Event::LoadState, moved);
	EventManager::listen(Event::DiskChange, moved);
	EventManager::listen(Event::Network, moved);
	// For good: the list of listeners is not to be changed while the
	// emulator's thread goes through it. Off, it is one test per frame.
	EventManager::listen(Event::VBlank, frame);
}

void setEnabled(bool enable)
{
	if (on.exchange(enable) == enable || history == nullptr)
		return;
	if (!enable)
		history->clear(true);
	resync = true;
	describe();
}

void setInterval(int seconds)
{
	interval = (uint64_t)std::clamp(seconds, 1, 60) * machine::TicksPerSecond;
	resync = true;
}

void setMaxAge(int seconds)
{
	if (history != nullptr)
		history->setMaxAge((uint64_t)std::max(seconds, 1) * machine::TicksPerSecond);
}

void setBudget(size_t bytes)
{
	if (history != nullptr)
		history->setBudget(bytes);
}

void setPictures(bool enable)
{
	pictures = enable;
}

std::vector<Point> points()
{
	std::vector<Point> list;
	if (history == nullptr || !wanted())
		return list;
	// Stopped, the clock is the emulator's own, to the cycle. Running, it is
	// not to be read from another thread: the last frame's will do.
	const uint64_t now = machine::stopped() ? machine::now() : playTime.load();
	std::vector<History::Shot> shots;
	history->list(now, shots);
	list.reserve(shots.size());
	for (const History::Shot& shot : shots)
	{
		Point point;
		point.id = shot.id;
		point.secondsAgo = now > shot.time ? (double)(now - shot.time) / machine::TicksPerSecond : 0.0;
		point.bytes = shot.bytes();
		point.rawBytes = shot.rawBytes;
		point.picture = shot.picture;
		point.pictureWidth = shot.pictureWidth;
		point.pictureHeight = shot.pictureHeight;
		list.push_back(std::move(point));
	}
	return list;
}

bool available()
{
	return history != nullptr && wanted() && machine::refusal() == nullptr && history->count() != 0;
}

Result restore(size_t index)
{
	if (history == nullptr || !wanted() || machine::refusal() != nullptr)
		return Result::Unavailable;
	if (!machine::stopped())
		return Result::EmulatorRunning;
	uint64_t id;
	if (!history->listed(index, id))
		return Result::NoSuchPoint;

	Clock::time_point began = Clock::now();
	std::unique_ptr<uint8_t[]> state;
	size_t size = 0;
	uint64_t time = 0;
	switch (history->unpack(id, state, size, time))
	{
	case History::Unpack::Gone:
		return Result::NoSuchPoint;
	case History::Unpack::NoMemory:
		diag::mark("rewind: no memory to go back");
		return Result::NoMemory;
	case History::Unpack::Corrupt:
		diag::mark("rewind: a snapshot is not what it was, and was not loaded");
		return Result::Corrupt;
	case History::Unpack::Ok:
		break;
	}
	const double unpackMs = msSince(began);

	const uint64_t from = machine::now();
	began = Clock::now();
	const char *error = nullptr;
	std::string what;
	// Flycast tells whoever listens that a state was loaded; here that is
	// no reason to forget the points.
	restoring = true;
	size_t read = size;
	try {
		read = machine::load(state.get(), size);
	} catch (const std::exception& e) {
		what = e.what();
		error = what.c_str();
	} catch (...) {
		error = "unknown error";
	}
	restoring = false;
	if (error != nullptr)
	{
		// The machine is neither what it was nor what the snapshot is, as
		// after a "Load state" that failed. Nothing kept can be trusted to
		// follow from it.
		history->clear(false);
		resync = true;
		diag::mark("rewind: going back failed: %s", error);
		return Result::LoadFailed;
	}
	history->dropNewerThan(id);
	resync = true;
	if (read != size)
		diag::mark("rewind: the state is %zu bytes, and %zu were read", size, read);
	diag::mark("rewind: back %.1f s of play: decompressed in %.1f ms, loaded in %.1f ms",
			from > time ? (double)(from - time) / machine::TicksPerSecond : 0.0, unpackMs, msSince(began));
	return Result::Ok;
}

void clear()
{
	if (history == nullptr)
		return;
	history->clear(false);
	resync = true;
}

} // namespace ps5::rewind
