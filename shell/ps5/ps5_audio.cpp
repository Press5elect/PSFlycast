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

	The start-up sound (ps5::sound, at the end) is a WAV file played through
	a port of its own, which is closed when the sound ends and before the
	emulator's port is opened.
*/
#include "audio/audiostream.h"
#include "cfg/option.h"
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "log/Log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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

void audioLibrary()
{
	static std::once_flag once;
	std::call_once(once, [] {
		const int rc = sceAudioOutInit();
		INFO_LOG(AUDIO, "PS5: sceAudioOutInit -> %x", rc);
	});
}

class PS5AudioBackend : public AudioBackend
{
public:
	PS5AudioBackend() : AudioBackend("ps5", "PS5 AudioOut") {}

	bool init() override
	{
		// The start-up sound, if it still plays, ends first: one port at a time.
		ps5::sound::stop(true);
		audioLibrary();
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

// ------------------------------------------------------ the start-up sound

namespace
{

u32 le32(const u8 *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24); }
u32 le16(const u8 *p) { return p[0] | (p[1] << 8); }

// Reads a WAV file of 16-bit PCM, one or two channels at any rate, as 48 kHz
// stereo. At most a minute of it.
bool readWav(const std::string& path, std::vector<s16>& out, std::string& why)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (f == nullptr)
	{
		why = "no file";
		return false;
	}
	fseek(f, 0, SEEK_END);
	const long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	std::vector<u8> file((size_t)std::clamp(size, 0L, 32L * 1024 * 1024));
	file.resize(fread(file.data(), 1, file.size(), f));
	fclose(f);
	if (file.size() < 44 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0)
	{
		why = "not a WAV file";
		return false;
	}
	u32 channels = 0, rate = 0, bits = 0, format = 0;
	const u8 *data = nullptr;
	size_t dataSize = 0;
	for (size_t at = 12; at + 8 <= file.size(); )
	{
		const u8 *chunk = file.data() + at;
		const size_t size = std::min<size_t>(le32(chunk + 4), file.size() - at - 8);
		if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16)
		{
			format = le16(chunk + 8);
			channels = le16(chunk + 10);
			rate = le32(chunk + 12);
			bits = le16(chunk + 22);
		}
		else if (memcmp(chunk, "data", 4) == 0)
		{
			data = chunk + 8;
			dataSize = size;
			break;
		}
		at += 8 + size + (size & 1);
	}
	// 1 is PCM; 0xfffe says the same in a longer header.
	if (data == nullptr || (format != 1 && format != 0xfffe) || bits != 16 || channels < 1 || channels > 2
			|| rate < 8000 || rate > 192000)
	{
		why = "not 16-bit PCM, mono or stereo";
		return false;
	}
	const size_t frames = dataSize / (2 * channels);
	auto sample = [&](size_t frame, u32 channel) {
		return (float)(s16)le16(data + (frame * channels + std::min(channel, channels - 1)) * 2);
	};
	const size_t outFrames = std::min<size_t>((size_t)((double)frames * OutRate / rate), (size_t)OutRate * 60);
	out.resize(outFrames * 2);
	for (size_t i = 0; i < outFrames; i++)
	{
		// Linear interpolation; a 48 kHz file is copied as it is.
		const double position = (double)i * rate / OutRate;
		const size_t before = (size_t)position, after = std::min(before + 1, frames - 1);
		const float part = (float)(position - before);
		for (u32 c = 0; c < 2; c++)
			out[i * 2 + c] = (s16)(sample(before, c) + (sample(after, c) - sample(before, c)) * part);
	}
	return outFrames > 0;
}

std::thread startupThread;
std::atomic<bool> startupStop;

void playStartupFile(float fromSeconds, float gain)
{
	const auto began = std::chrono::steady_clock::now();
	std::vector<s16> samples;
	std::string why;
	const std::string path = ps5::rootDir + "sounds/startup.wav";
	if (!readWav(path, samples, why))
	{
		ps5::diag::mark("sound: no start-up sound, %s: %s", path.c_str(), why.c_str());
		return;
	}
	audioLibrary();
	const int port = sceAudioOutOpen(255, 0, 0, Grain, OutRate, FormatS16Stereo);
	if (port < 0)
	{
		ps5::diag::mark("sound: no port for the start-up sound (%x)", port);
		return;
	}
	// The animation went on while the file was read: the sound starts where
	// the animation is.
	const double late = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
	const size_t frames = samples.size() / 2;
	size_t at = (size_t)((std::max(fromSeconds, 0.f) + late) * OutRate);
	ps5::diag::mark("sound: the start-up sound from %.2f s of %.2f s, loudness %.2f (the file was read in %.0f ms)",
			(double)at / OutRate, (double)frames / OutRate, gain, late * 1000);
	alignas(64) s16 grain[Grain * 2];
	// It comes in over a hundredth of a second, and when it is asked to stop
	// goes out over two.
	float level = 0;
	while (at < frames)
	{
		for (u32 i = 0; i < Grain; i++, at++)
		{
			if (startupStop)
				level = std::max(0.f, level - 1.f / (OutRate * 0.02f));
			else
				level = std::min(1.f, level + 1.f / (OutRate * 0.01f));
			for (u32 c = 0; c < 2; c++)
				grain[i * 2 + c] = at < frames ? (s16)std::clamp(samples[at * 2 + c] * gain * level, -32767.f, 32767.f) : 0;
		}
		sceAudioOutOutput(port, grain);
		if (startupStop && level <= 0)
			break;
	}
	sceAudioOutOutput(port, nullptr);	// what is left of it is played
	sceAudioOutClose(port);
	ps5::diag::mark("sound: the start-up sound %s", startupStop ? "was stopped" : "ended");
}

}

void ps5::sound::playStartup(float fromSeconds, float gain)
{
	stop(true);
	startupStop = false;
	startupThread = std::thread(playStartupFile, fromSeconds, gain);
}

void ps5::sound::stop(bool wait)
{
	startupStop = true;
	if (wait && startupThread.joinable())
		startupThread.join();
}
