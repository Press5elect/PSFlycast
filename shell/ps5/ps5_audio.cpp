/*
	PSFlyCast - audio through libSceAudioOut.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The port is opened as both console-proven PS5 projects open it: the system
	user (255), the main port, 256-frame grains at 48 kHz, 16-bit stereo, with
	a thread of its own feeding sceAudioOutOutput, which blocks until the
	previous grain has been taken. Flycast produces 44.1 kHz, so push()
	resamples linearly into a ring the thread drains.

	The emulator runs at the display's pace (VSync), the port at its own
	48 kHz clock, and the two drift apart: a ring that filled up blocked the
	emulator for a moment, one that ran dry played a gap, the occasional
	micro-stutter. The resampling ratio is therefore nudged, by at most
	0.5 %, inaudibly, to keep the ring half full (dynamic rate control, as
	RetroArch does); push() blocks only if the ring is full regardless.
	That is the "Sync to display" frame pacing; with "VSync" the ratio is the
	exact one.
*/
#include "audio/audiostream.h"
#include "cfg/option.h"
#include "ps5_frontend.h"
#include "log/Log.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

extern "C"
{
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t userId, int32_t type, int32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioOutOutput(int32_t handle, const void *p);
int sceAudioOutClose(int32_t handle);
}

namespace ps5
{
config::Option<bool> SyncToDisplay("SyncToDisplay", true, "ps5");
}

namespace
{

constexpr u32 OutRate = 48000;
constexpr u32 InRate = 44100;
constexpr u32 Grain = 256;
constexpr u32 FormatS16Stereo = 1;
constexpr float MaxRateDelta = 0.005f;

class PS5AudioBackend : public AudioBackend
{
public:
	PS5AudioBackend() : AudioBackend("ps5", "PS5 AudioOut") {}

	bool init() override
	{
		static bool libraryReady;
		if (!libraryReady)
		{
			const int rc = sceAudioOutInit();
			INFO_LOG(AUDIO, "PS5: sceAudioOutInit -> %x", rc);
			libraryReady = true;
		}
		port = sceAudioOutOpen(255, 0, 0, Grain, OutRate, FormatS16Stereo);
		INFO_LOG(AUDIO, "PS5: sceAudioOutOpen -> %x", port);
		if (port < 0)
			return false;
		// About 100 ms of buffering; config::AudioBufferSize is in 44.1 kHz frames.
		const u32 frames = std::max<u32>(Grain * 8, (u32)config::AudioBufferSize * OutRate / InRate * 2);
		ring.assign(frames * 2, 0);
		readPos = writePos = 0;
		phase = 0.f;
		lastL = lastR = 0;
		quit = false;
		thread = std::thread([this] { run(); });
		return true;
	}

	void term() override
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			quit = true;
		}
		space.notify_all();
		if (thread.joinable())
			thread.join();
		if (port >= 0)
		{
			sceAudioOutOutput(port, nullptr);	// drain
			sceAudioOutClose(port);
			port = -1;
		}
	}

	u32 push(const void *data, u32 frames, bool wait) override
	{
		const s16 *in = static_cast<const s16 *>(data);
		std::unique_lock<std::mutex> lock(mutex);
		// Resample 44.1 -> 48 kHz with linear interpolation between the
		// previous input frame and the current one, at a ratio that steers
		// the ring back towards half full.
		const float target = capacity() / 2.f;
		float deviation = ((float)fill() - target) / target;
		deviation = std::clamp(deviation, -1.f, 1.f);
		if (!ps5::SyncToDisplay)
			deviation = 0.f;		// "VSync": the sound at its own rate
		const float step = (float)InRate / OutRate * (1.f + MaxRateDelta * deviation);
		for (u32 i = 0; i < frames; i++)
		{
			const s16 l = in[i * 2], r = in[i * 2 + 1];
			while (phase < 1.f)
			{
				if (fill() >= capacity() - 1)
				{
					if (!wait)
						goto done;	// running fast: drop the rest
					space.wait(lock, [this] { return quit || fill() < capacity() - 1; });
					if (quit)
						return 1;
				}
				const float a = phase;
				ring[writePos * 2] = (s16)(lastL + (l - lastL) * a);
				ring[writePos * 2 + 1] = (s16)(lastR + (r - lastR) * a);
				writePos = (writePos + 1) % capacity();
				phase += step;
			}
			phase -= 1.f;
			lastL = l;
			lastR = r;
		}
done:
		return 1;
	}

private:
	u32 capacity() const { return (u32)(ring.size() / 2); }
	u32 fill() const { return (writePos + capacity() - readPos) % capacity(); }

	void run()
	{
		alignas(64) s16 grain[Grain * 2];
		while (true)
		{
			{
				std::lock_guard<std::mutex> lock(mutex);
				if (quit)
					break;
				const u32 available = std::min(fill(), Grain);
				for (u32 i = 0; i < available; i++)
				{
					grain[i * 2] = ring[readPos * 2];
					grain[i * 2 + 1] = ring[readPos * 2 + 1];
					readPos = (readPos + 1) % capacity();
				}
				// An underrun plays silence rather than stalling the port.
				std::memset(grain + available * 2, 0, (Grain - available) * 4);
			}
			space.notify_one();
			sceAudioOutOutput(port, grain);
		}
	}

	int port = -1;
	std::vector<s16> ring;
	u32 readPos = 0, writePos = 0;
	float phase = 0.f;
	s16 lastL = 0, lastR = 0;
	bool quit = false;
	std::mutex mutex;
	std::condition_variable space;
	std::thread thread;
};

PS5AudioBackend ps5AudioBackend;

}
