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

	The interface's sounds (ps5::sound, at the end): the start-up sound, a
	WAV file, and the menu's sounds, a few notes each made in code. They are
	mixed into a port of their own, which is closed when they have been
	silent for a while and before the emulator's port is opened.
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
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <memory>
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
		ps5::sound::close();
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
		// "VSync", or a display that shows each frame when it comes
		// (variable refresh): the sound at its own rate.
		if (!ps5::SyncToDisplay || ps5::variableRefresh)
			deviation = 0.f;
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

// ---- one port for the interface's sounds
//
// The start-up sound and the menu's sounds are voices of one small mixer,
// which opens a port when the first of them plays and closes it after twenty
// seconds of silence, or at once when a game is about to open its own
// (close(), from the backend above): one port at a time.

struct Voice
{
	std::shared_ptr<const std::vector<s16>> pcm;	// 48 kHz stereo
	size_t at;			// the next frame
	float gain;
	bool startup;		// the start-up sound, which can be ended by itself
	float level;		// 1, on its way to 0 once it was ended
	bool ending;
};

std::mutex mixMutex;
std::mutex playMutex;			// one caller at a time starts or ends the mixer's thread
std::vector<Voice> voices;
std::thread mixThread, loadThread;
bool mixRunning;				// under mixMutex
std::atomic<bool> mixStop;
// When the console last gave no port: not asked again for a while, so that a
// press in the menus is not a thread and a line in the log each.
std::atomic<int64_t> mixRefusedAt{ INT64_MIN / 2 };

int64_t mixNow()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void mixRun()
{
	audioLibrary();
	const int port = sceAudioOutOpen(255, 0, 0, Grain, OutRate, FormatS16Stereo);
	if (port < 0)
	{
		ps5::diag::mark("sound: no port for the interface's sounds (%x)", port);
		mixRefusedAt = mixNow();
		std::lock_guard<std::mutex> lock(mixMutex);
		voices.clear();
		mixRunning = false;
		return;
	}
	alignas(64) s16 grain[Grain * 2];
	float mix[Grain * 2];
	float master = 1;
	int silent = 0;
	for (;;)
	{
		std::memset(mix, 0, sizeof(mix));
		bool idle;
		{
			std::lock_guard<std::mutex> lock(mixMutex);
			for (Voice& voice : voices)
			{
				const size_t frames = voice.pcm->size() / 2;
				for (u32 i = 0; i < Grain && voice.at < frames; i++, voice.at++)
				{
					if (voice.ending)
						voice.level = std::max(0.f, voice.level - 1.f / (OutRate * 0.02f));
					const float g = voice.gain * voice.level;
					mix[i * 2] += (*voice.pcm)[voice.at * 2] * g;
					mix[i * 2 + 1] += (*voice.pcm)[voice.at * 2 + 1] * g;
				}
			}
			voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice& voice) {
				return voice.at >= voice.pcm->size() / 2 || (voice.ending && voice.level <= 0);
			}), voices.end());
			idle = voices.empty();
			// Twenty seconds with nothing to play: the port is given back.
			silent = idle ? silent + 1 : 0;
			if (silent > 20 * (int)(OutRate / Grain) && !mixStop)
			{
				mixRunning = false;
				break;
			}
		}
		for (u32 i = 0; i < Grain; i++)
		{
			// Asked to stop, everything goes out over a fiftieth of a second.
			if (mixStop)
				master = std::max(0.f, master - 1.f / (OutRate * 0.02f));
			grain[i * 2] = (s16)std::clamp(mix[i * 2] * master, -32767.f, 32767.f);
			grain[i * 2 + 1] = (s16)std::clamp(mix[i * 2 + 1] * master, -32767.f, 32767.f);
		}
		sceAudioOutOutput(port, grain);
		if (mixStop && (master <= 0 || idle))
			break;
	}
	sceAudioOutOutput(port, nullptr);	// what is left of it is played
	sceAudioOutClose(port);
	std::lock_guard<std::mutex> lock(mixMutex);
	voices.clear();
	mixRunning = false;
}

void play(Voice voice)
{
	std::lock_guard<std::mutex> one(playMutex);
	if (mixNow() - mixRefusedAt < 10000)
		return;
	std::thread old;
	{
		std::lock_guard<std::mutex> lock(mixMutex);
		if (mixRunning && !mixStop)
		{
			// Never more than a handful at once: a held direction is many ticks.
			if (voices.size() < 12)
				voices.push_back(std::move(voice));
			return;
		}
		old = std::move(mixThread);
	}
	// The port was closed, or is closing: a new one.
	if (old.joinable())
		old.join();
	std::lock_guard<std::mutex> lock(mixMutex);
	voices.clear();
	voices.push_back(std::move(voice));
	mixStop = false;
	mixRunning = true;
	mixThread = std::thread(mixRun);
}

// ---- the menu's sounds, made here: a few sine notes each, from the scale of
// the start-up sound (D major pentatonic).

struct Note
{
	float at, from, to, seconds, gain;		// when, the pitch it glides between, how long, how loud
};

std::shared_ptr<const std::vector<s16>> makeCue(std::initializer_list<Note> notes)
{
	float length = 0;
	for (const Note& note : notes)
		length = std::max(length, note.at + note.seconds);
	std::vector<float> mono((size_t)(length * OutRate) + 1, 0.f);
	for (const Note& note : notes)
	{
		const size_t first = (size_t)(note.at * OutRate), count = (size_t)(note.seconds * OutRate);
		double phase = 0;
		for (size_t i = 0; i < count && first + i < mono.size(); i++)
		{
			const float t = (float)i / count;
			const float pitch = note.from * std::pow(note.to / note.from, t);
			phase += 2 * M_PI * pitch / OutRate;
			// In over three milliseconds, then dying away to nothing at its end.
			const float in = std::min(1.f, (float)i / (OutRate * 0.003f));
			const float out = std::exp(-5.5f * t) * (1 - t);
			mono[first + i] += (float)std::sin(phase) * note.gain * in * out;
		}
	}
	auto pcm = std::make_shared<std::vector<s16>>(mono.size() * 2);
	for (size_t i = 0; i < mono.size(); i++)
		(*pcm)[i * 2] = (*pcm)[i * 2 + 1] = (s16)std::clamp(mono[i] * 32767.f, -32767.f, 32767.f);
	return pcm;
}

std::shared_ptr<const std::vector<s16>> cueSound(ps5::sound::Cue cue)
{
	using ps5::sound::Cue;
	static std::once_flag once;
	static std::shared_ptr<const std::vector<s16>> sounds[(int)Cue::Count];
	std::call_once(once, [] {
		const float d5 = 587.33f, a5 = 880.f, d6 = 1174.66f, e6 = 1318.51f, a6 = 1760.f;
		sounds[(int)Cue::Move] = makeCue({ { 0, d6, d6, 0.05f, 0.060f }, { 0, d6 * 2, d6 * 2, 0.03f, 0.014f } });
		sounds[(int)Cue::Select] = makeCue({ { 0, a5, a5, 0.11f, 0.085f }, { 0.055f, e6, e6, 0.16f, 0.085f } });
		sounds[(int)Cue::Back] = makeCue({ { 0, a5, a5, 0.10f, 0.075f }, { 0.055f, d5, d5, 0.16f, 0.085f } });
		sounds[(int)Cue::Tab] = makeCue({ { 0, a6, a6, 0.05f, 0.045f }, { 0.03f, d6, d6, 0.08f, 0.055f } });
		sounds[(int)Cue::Open] = makeCue({ { 0, d5, d5, 0.10f, 0.070f }, { 0.05f, a5, a5, 0.10f, 0.070f },
				{ 0.10f, d6, d6, 0.18f, 0.070f } });
		sounds[(int)Cue::Refuse] = makeCue({ { 0, 233.08f, 196.f, 0.14f, 0.13f } });
	});
	return sounds[(int)cue];
}

void loadStartup(float fromSeconds, float gain)
{
	const auto began = std::chrono::steady_clock::now();
	auto samples = std::make_shared<std::vector<s16>>();
	std::string why;
	const std::string path = ps5::rootDir + "sounds/startup.wav";
	if (!readWav(path, *samples, why))
	{
		ps5::diag::mark("sound: no start-up sound, %s: %s", path.c_str(), why.c_str());
		return;
	}
	// The animation went on while the file was read: the sound starts where
	// the animation is.
	const double late = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
	const size_t frames = samples->size() / 2;
	const size_t at = (size_t)((std::max(fromSeconds, 0.f) + late) * OutRate);
	ps5::diag::mark("sound: the start-up sound from %.2f s of %.2f s, loudness %.2f (the file was read in %.0f ms)",
			(double)at / OutRate, (double)frames / OutRate, gain, late * 1000);
	if (at < frames)
		play({ samples, at, gain, true, 1.f, false });
}

}

void ps5::sound::playStartup(float fromSeconds, float gain)
{
	if (loadThread.joinable())
		loadThread.join();
	loadThread = std::thread(loadStartup, fromSeconds, gain);
}

void ps5::sound::stopStartup()
{
	std::lock_guard<std::mutex> lock(mixMutex);
	for (Voice& voice : voices)
		if (voice.startup)
			voice.ending = true;
}

void ps5::sound::cue(Cue which)
{
	if (!options().menuSounds || (int)which < 0 || (int)which >= (int)Cue::Count)
		return;
	play({ cueSound(which), 0, 1.f, false, 1.f, false });
}

void ps5::sound::close()
{
	if (loadThread.joinable())
		loadThread.join();
	std::lock_guard<std::mutex> one(playMutex);
	mixStop = true;
	std::thread old;
	{
		std::lock_guard<std::mutex> lock(mixMutex);
		old = std::move(mixThread);
	}
	if (old.joinable())
		old.join();
}
