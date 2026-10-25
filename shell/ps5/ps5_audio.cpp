/*
	PSFlyCast - audio through libSceAudioOut.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The port is opened as both console-proven PS5 projects open it: the system
	user (255), the main port, 256-frame grains at 48 kHz, 16-bit stereo, with
	a thread of its own feeding sceAudioOutOutput, which blocks until the
	previous grain has been taken. Flycast produces 44.1 kHz, so push()
	resamples it with a 16-tap windowed sinc into a ring the thread drains.

	The emulator runs at the display's pace (VSync), the port at its own
	48 kHz clock, and the two drift apart: a ring that filled up blocked the
	emulator for a moment, one that ran dry played a gap, the occasional
	micro-stutter. The resampling ratio is therefore nudged, by at most
	0.5 %, inaudibly, to keep the ring half full (dynamic rate control, as
	RetroArch does). That is the "Sync to display" frame pacing. With "VSync",
	a variable-refresh display, or a game whose rate the display does not
	follow (a 50 Hz game on a 60/120 Hz output) the ratio is the exact one,
	and push() blocks just above half the ring so latency stays low.

	The interface's sounds (ps5::sound, at the end): the start-up sound, a
	WAV file, and the menu's sounds, a few notes each made in code. They are
	mixed into a port of their own, which is closed when they have been
	silent for a while and before the emulator's port is opened.
*/
#include "audio/audiostream.h"
#include "cfg/option.h"
#include "hw/aica/sgc_if.h"
#include "hw/pvr/spg.h"
#include "ps5_frontend.h"
#include "ps5_diag.h"
#include "ps5_pad.h"
#include "ps5_priority.h"
#include "log/Log.h"

#include <algorithm>
#include <array>
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
int sceAudioOutSetVolume(int32_t handle, int32_t flag, int32_t *vol);
// libSceAudioIn (shell/ps5/runtime/stubs/audio_in_link_stub.c): the DualSense's microphone.
int sceAudioInOpen(int32_t userId, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param);
int sceAudioInInput(int32_t handle, void *dest);
int sceAudioInClose(int32_t handle);
}

namespace ps5
{
config::Option<bool> SyncToDisplay("SyncToDisplay", true, "ps5");
config::Option<int> VmuSoundOutput("VmuSoundOutput", 1, "ps5");
config::Option<int> MicVolume("MicVolume", 50, "ps5");
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


// ------------------------------------------- the Dreamcast's microphone, in a controller
//
// A game with the microphone in a controller slot (Settings > Controls) hears
// the DualSense's microphone: the console's audio input of player 1's pad
// (libSceAudioIn, type 0 "voice chat", 16 kHz mono, blocks of 256 samples:
// the call a PS5 title's audio test proved on a console, TokenStation5's
// voice.cpp, which recorded the DualSense's microphone with it), read on
// a thread of its own into a ring, and given to Flycast at the rate the game
// asked for (8 or 11.025 kHz). Not tried on a console yet: the log says what
// the console answered when it refuses.
//
// What is given is the microphone's own level: the Microphone volume matches
// the DualSense to the Dreamcast microphone (50 % by default), and the core
// then applies the game's gain and clips as the real device's converter does
// (maple_microphone in core/hw/maple/maple_devs.cpp).
class PadMicrophone
{
public:
	bool start(u32 rate)
	{
		std::lock_guard<std::mutex> hold(lock);
		outRate = std::max<u32>(rate, 1000);
		phase = 0.f;
		readPos.store(0);
		writePos.store(0);
		// The low-pass below the game's half rate: about 4.8 kHz for 11.025 kHz,
		// 3.4 kHz for 8 kHz (the telephone band, as the real device's filter).
		coefficients = lowPass(outRate >= 11025 ? 4800.f : std::min(3400.f, outRate * 0.425f));
		newFilter = true;
		wanted = true;
		if (!thread.joinable())
			thread = std::thread([this] { run(); });
		return true;
	}

	void stop()
	{
		std::lock_guard<std::mutex> hold(lock);
		wanted = false;
	}

	// Up to `count` samples the game is to have now, at outRate; how many there are.
	u32 take(s16 *out, u32 count)
	{
		if (!working.load())
			return 0;
		// 16 kHz in, the game's rate out, each sample between the two it falls between
		// (the input was low-passed below the game's half rate as it came in: run()).
		const float step = (float)InRate / outRate;
		u32 n = 0;
		for (; n < count; n++)
		{
			while (phase >= 1.f)
			{
				const u32 r = readPos.load(std::memory_order_relaxed);
				if (r == writePos.load(std::memory_order_acquire))
				{
					given.fetch_add(n, std::memory_order_relaxed);
					return n;
				}
				previous = last;
				last = ring[r];
				readPos.store((r + 1) & Mask, std::memory_order_release);
				phase -= 1.f;
			}
			out[n] = (s16)std::lround(previous + (last - previous) * phase);
			phase += step;
		}
		given.fetch_add(n, std::memory_order_relaxed);
		return n;
	}

private:
	static constexpr u32 Size = 1 << 14, Mask = Size - 1;
	static constexpr u32 InRate = 16000;
	static constexpr u32 Block = 256;
	static constexpr u32 TypeVoice = 0, FormatS16Mono = 0;
	static constexpr u32 DeviceSamples = 240;	// what the real microphone holds, at the game's rate

	// A two-pole Butterworth low-pass at `cutoff` Hz of 16 kHz (the bilinear
	// transform, as in R. Bristow-Johnson's audio EQ cookbook): b0 b1 b2 a1 a2.
	static std::array<float, 5> lowPass(float cutoff)
	{
		const double w = 2.0 * M_PI * cutoff / InRate;
		const double alpha = std::sin(w) / (2.0 * M_SQRT1_2);	// Q = 1/sqrt(2)
		const double c = std::cos(w);
		const double a0 = 1.0 + alpha;
		return { (float)((1.0 - c) / 2.0 / a0), (float)((1.0 - c) / a0), (float)((1.0 - c) / 2.0 / a0),
				(float)(-2.0 * c / a0), (float)((1.0 - alpha) / a0) };
	}

	void run()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
		int handle = -1;
		int user = -1;
		alignas(64) s16 block[Block];
		while (true)
		{
			if (!wanted.load())
			{
				if (handle >= 0)
				{
					sceAudioInClose(handle);
					handle = -1;
					working = false;
					ps5::diag::mark("microphone: closed");
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				continue;
			}
			if (handle < 0)
			{
				user = ps5::pad::userOfPort(0);
				if (user < 0)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(500));
					continue;
				}
				// Voice chat first, then general, then voice recognition: the kinds a PS5
				// title's console-proven audio test tries, in its order (TokenStation5's
				// audio_probe.cpp; voice chat opened the DualSense's microphone there).
				static constexpr u32 types[] = { TypeVoice, 1, 5 };
				for (u32 type : types)
				{
					handle = sceAudioInOpen(user, type, 0, Block, InRate, FormatS16Mono);
					ps5::diag::mark("microphone: audio in of user %d, type %u, %u Hz -> %x", user, type, InRate, handle);
					if (handle >= 0)
						break;
				}
				if (handle < 0)
				{
					// The console says no: the game hears silence. Tried again in a while.
					working = false;
					std::this_thread::sleep_for(std::chrono::seconds(3));
					handle = -1;
					continue;
				}
				working = true;
			}
			// Blocks until a block is in: 16 ms.
			const int rc = sceAudioInInput(handle, block);
			if (rc < 0)
			{
				ps5::diag::mark("microphone: input -> %x", rc);
				sceAudioInClose(handle);
				handle = -1;
				working = false;
				std::this_thread::sleep_for(std::chrono::seconds(1));
				continue;
			}
			// The game asked for another rate: its low-pass, from a quiet start.
			if (newFilter.exchange(false))
			{
				std::lock_guard<std::mutex> hold(lock);
				filter = coefficients;
				x1 = x2 = y1 = y2 = 0.f;
			}
			// What the microphone hears, every five seconds: whether sound comes in at
			// all, how loud, how much of it the game took, and the gain the game set.
			for (u32 i = 0; i < Block; i++)
			{
				const int v = block[i];
				peak = std::max(peak, std::abs(v));
				squares += (double)v * v;
			}
			heard += Block;
			if (heard >= InRate * 5)
			{
				ps5::diag::mark("microphone: 5 s heard, loudest %d, average %.0f (of 32767) before the volume (%d%%); the game took %u samples at %u Hz, its gain %+d dB",
						peak, std::sqrt(squares / heard), (int)ps5::MicVolume, given.exchange(0), outRate, AudioRecordingGainDb.load());
				peak = 0;
				squares = 0;
				heard = 0;
			}
			// What the game hears: below its half rate (the low-pass, two poles, so that
			// the step down to 11.025 or 8 kHz folds nothing back into the voice), at the
			// Microphone volume. The core clips after the game's gain as the real
			// converter does; here only the very top (above 0.9 of full scale) is
			// rounded off, so that a loud DualSense is not cut flat before the gain.
			const float volume = std::clamp((int)ps5::MicVolume, 0, 400) / 100.f;
			const float b0 = filter[0], b1 = filter[1], b2 = filter[2], a1 = filter[3], a2 = filter[4];
			for (u32 i = 0; i < Block; i++)
			{
				const float x = block[i] / 32768.f;
				const float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
				x2 = x1;
				x1 = x;
				y2 = y1;
				y1 = y;
				float v = y * volume;
				const float m = std::fabs(v);
				if (m > 0.9f)
					v = std::copysign(0.9f + 0.1f * std::tanh((m - 0.9f) * 10.f), v);
				block[i] = (s16)std::lround(std::clamp(v, -1.f, 1.f) * 32767.f);
			}
			for (u32 i = 0; i < Block; i++)
			{
				const u32 w = writePos.load(std::memory_order_relaxed);
				if (((w + 1) & Mask) == readPos.load(std::memory_order_acquire))
					break;		// the ring is full (the game stopped taking): the newest are dropped
				ring[w] = block[i];
				writePos.store((w + 1) & Mask, std::memory_order_release);
			}
			// The real microphone holds 240 samples; a game that falls behind hears
			// what is said now, not what was said a second ago. Once more than that
			// and a block wait (about 38 ms at 11.025 kHz, 46 ms at 8 kHz), the oldest
			// are dropped down to 240 samples' worth. If the game takes one at the
			// same moment, the drop is simply tried again after the next block.
			const u32 keep = DeviceSamples * InRate / outRate;
			const u32 w = writePos.load(std::memory_order_relaxed);
			u32 r = readPos.load(std::memory_order_acquire);
			if (((w - r) & Mask) > keep + Block)
				readPos.compare_exchange_strong(r, (w - keep) & Mask, std::memory_order_acq_rel);
		}
	}

	std::array<s16, Size> ring{};
	std::atomic<u32> readPos{ 0 }, writePos{ 0 };
	std::atomic<bool> wanted{ false }, working{ false };
	std::atomic<bool> newFilter{ false };	// start() set new coefficients for the reader
	std::atomic<u32> given{ 0 };		// samples the game took since the last report
	int peak = 0;						// the reader's: the loudest sample since the last report
	double squares = 0;
	u32 heard = 0;
	u32 outRate = 11025;
	float phase = 0.f;
	float last = 0.f, previous = 0.f;
	// The low-pass: start()'s coefficients (under the lock), and the reader's copy and state.
	std::array<float, 5> coefficients = lowPass(4800.f);
	std::array<float, 5> filter = lowPass(4800.f);
	float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
	std::mutex lock;
	std::thread thread;
};

PadMicrophone padMicrophone;

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
		std::fill(std::begin(histL), std::end(histL), 0.f);
		std::fill(std::begin(histR), std::end(histR), 0.f);
		histPos = 0;
		sincTable();	// built once, here rather than in the first push()
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

	// The Dreamcast's microphone (hw/maple/maple_devs.cpp asks for 8 or 11.025 kHz).
	bool initRecord(u32 rate) override
	{
		return padMicrophone.start(rate);
	}

	u32 record(void *buffer, u32 samples) override
	{
		return padMicrophone.take((s16 *)buffer, samples);
	}

	void termRecord() override
	{
		padMicrophone.stop();
	}

	u32 push(const void *data, u32 frames, bool wait) override
	{
		const s16 *in = static_cast<const s16 *>(data);
		std::unique_lock<std::mutex> lock(mutex);
		// Resample 44.1 -> 48 kHz with a windowed-sinc filter (see sincTable()),
		// at a ratio that steers the ring back towards half full.
		const float target = capacity() / 2.f;
		float deviation = ((float)fill() - target) / target;
		deviation = std::clamp(deviation, -1.f, 1.f);
		// "VSync", or a display that shows each frame when it comes
		// (variable refresh), or a game whose field rate the display does not
		// follow (a 50 Hz PAL game on a 59.94 / 119.88 Hz output): the sound
		// at its own, exact rate, and push() blocking paces the emulator.
		// Steering there could only saturate at +0.5 %, fill the ring and run
		// the game 0.5 % fast with the whole ring as latency.
		const bool steer = ps5::SyncToDisplay && !ps5::variableRefresh && displayFollowsGame();
		if (!steer)
			deviation = 0.f;
		const float step = (float)InRate / OutRate * (1.f + MaxRateDelta * deviation);
		// Where push() stops writing. Steering keeps the ring near half full
		// by itself and the spare half absorbs the display's hitches. At the
		// exact rate nothing pulls the fill back down, so push() is held just
		// above half full instead of at capacity: the latency stays about half
		// the ring. A small margin above the target (two grains), and blocking
		// a grain at a time, rather than waiting for the fill to fall all the
		// way back to the target, which would stall the emulator for tens of
		// milliseconds and then let it rush to refill.
		const u32 limit = steer ? capacity() - 1 : std::min(capacity() - 1, (u32)target + Grain * 2);
		const SincTable& table = sincTable();
		for (u32 i = 0; i < frames; i++)
		{
			// The newest input frame enters the history; the output is taken
			// between the frames Taps/2 and Taps/2 - 1 before it (a fixed delay
			// of 8 input frames, 0.18 ms).
			histL[histPos] = histL[histPos + Taps] = in[i * 2];
			histR[histPos] = histR[histPos + Taps] = in[i * 2 + 1];
			histPos = (histPos + 1) % Taps;
			const float *hl = histL + histPos, *hr = histR + histPos;	// oldest first
			while (phase < 1.f)
			{
				if (fill() >= limit)
				{
					if (!wait)
						goto done;	// running fast: drop the rest
					space.wait(lock, [this, limit] { return quit || fill() < limit; });
					if (quit)
						return 1;
				}
				// The filter for this fractional position: linear between the
				// two nearest of the table's phases.
				const float pos = phase * Phases;
				const int row = std::min((int)pos, Phases - 1);
				const float frac = pos - row;
				const float *c0 = table.c[row], *c1 = table.c[row + 1];
				float sl = 0.f, sr = 0.f;
				for (int j = 0; j < Taps; j++)
				{
					const float c = c0[j] + (c1[j] - c0[j]) * frac;
					sl += c * hl[j];
					sr += c * hr[j];
				}
				ring[writePos * 2] = toS16(sl);
				ring[writePos * 2 + 1] = toS16(sr);
				writePos = (writePos + 1) % capacity();
				phase += step;
			}
			phase -= 1.f;
		}
done:
		return 1;
	}

private:
	u32 capacity() const { return (u32)(ring.size() / 2); }
	u32 fill() const { return (writePos + capacity() - readPos) % capacity(); }

	// Whether the display's pace follows the game's: the output's refresh
	// divided by the presents per frame (2 on 119.88 Hz, as vulkan_context.cpp
	// sets the swap interval: refresh / 60, rounded) against the field rate
	// the emulated machine produces vblanks at, within 0.3 %. A 59.94 Hz NTSC
	// or VGA game on a 59.94 or 119.88 Hz output follows; a 50 Hz PAL one does
	// not. Unknown rates keep the steering, as before.
	static bool displayFollowsGame()
	{
		const float refresh = settings.display.refreshRate;
		const float field = spg_GetFieldRate();
		if (refresh <= 1.f || field <= 1.f)
			return true;
		const float pace = refresh / (float)std::max(1L, std::lround(refresh / 60.f));
		return std::fabs(field / pace - 1.f) <= 0.003f;
	}

	static s16 toS16(float v)
	{
		return (s16)std::lrint(std::clamp(v, -32768.f, 32767.f));
	}

	// The resampler: a 16-tap Kaiser-windowed sinc (beta 6, cutoff 0.45 of
	// the input rate, about 19.8 kHz), tabulated at 128 fractional positions
	// (plus the position 1.0, so two neighbouring rows can always be
	// interpolated), each row normalised to a gain of exactly 1. Flat within
	// 0.1 dB to 15 kHz, about -2 dB at 18 kHz; the images that linear
	// interpolation left at 24-44 kHz (folded back into the audible band by
	// the 48 kHz output) are down about 64 dB from 26 kHz up. 16 taps by two
	// channels, plus the row interpolation, per output frame: around 3
	// million multiply-adds a second. The fractional position is continuous,
	// so any ratio works, including the steered ones.
	static constexpr int Taps = 16;
	static constexpr int Phases = 128;
	struct SincTable
	{
		float c[Phases + 1][Taps];
	};

	static const SincTable& sincTable()
	{
		static const SincTable table = [] {
			constexpr double Pi = 3.14159265358979323846;
			constexpr double Beta = 6.0;
			constexpr double Cutoff = 0.45;	// of the input rate
			// The modified Bessel function I0, by its series (libc++ has no
			// std::cyl_bessel_i).
			auto i0 = [](double x) {
				double sum = 1.0, term = 1.0;
				for (int k = 1; k < 64; k++)
				{
					const double t = x / (2.0 * k);
					term *= t * t;
					sum += term;
					if (term < sum * 1e-15)
						break;
				}
				return sum;
			};
			const double half = Taps / 2.0;
			const double i0Beta = i0(Beta);
			SincTable t{};
			for (int r = 0; r <= Phases; r++)
			{
				const double frac = (double)r / Phases;
				double row[Taps];
				double sum = 0.0;
				for (int j = 0; j < Taps; j++)
				{
					// Tap j is the input frame j - (Taps/2 - 1) after the
					// output position's integer part.
					const double d = j - (half - 1.0) - frac;
					const double x = 2.0 * Cutoff * d;
					const double sinc = x == 0.0 ? 1.0 : std::sin(Pi * x) / (Pi * x);
					const double u = d / half;
					const double w = u * u >= 1.0 ? 0.0 : i0(Beta * std::sqrt(1.0 - u * u)) / i0Beta;
					row[j] = sinc * w;
					sum += row[j];
				}
				for (int j = 0; j < Taps; j++)
					t.c[r][j] = (float)(row[j] / sum);
			}
			return t;
		}();
		return table;
	}

	void run()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
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
	// The last Taps input frames, each stored twice (at i and i + Taps) so
	// the filter always reads them in one contiguous run from histPos.
	float histL[Taps * 2] = {};
	float histR[Taps * 2] = {};
	int histPos = 0;
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
// A video's sound (pushStream): 48 kHz stereo, mixed in with the voices.
std::vector<s16> streamPcm;
size_t streamAt = 0;			// the next frame
float streamGain = 1.f;
float streamLast[2] = { 0, 0 };
float streamPhase = 0.f;
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
	PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
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
			{
				const size_t frames = streamPcm.size() / 2;
				for (u32 i = 0; i < Grain && streamAt < frames; i++, streamAt++)
				{
					mix[i * 2] += streamPcm[streamAt * 2] * streamGain;
					mix[i * 2 + 1] += streamPcm[streamAt * 2 + 1] * streamGain;
				}
				if (streamAt >= frames)
				{
					streamPcm.clear();
					streamAt = 0;
				}
			}
			voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice& voice) {
				return voice.at >= voice.pcm->size() / 2 || (voice.ending && voice.level <= 0);
			}), voices.end());
			idle = voices.empty() && streamPcm.empty();
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

// ---- the menu's sounds, made here: a few sine notes each, in D major
// pentatonic.

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

// The start-up sound as it was last read, for the animation that draws it.
std::mutex shapeMutex;
std::shared_ptr<const std::vector<s16>> startupShape;

void loadStartup(float fromSeconds, float gain, bool heard)
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
	{
		std::lock_guard<std::mutex> lock(shapeMutex);
		startupShape = samples;
	}
	if (!heard)
		return;
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
	loadThread = std::thread(loadStartup, fromSeconds, gain, true);
}

void ps5::sound::prepareStartup()
{
	if (loadThread.joinable())
		loadThread.join();
	loadThread = std::thread(loadStartup, 0.f, 0.f, false);
}

bool ps5::sound::startupWave(double seconds, float *out, int count)
{
	std::fill(out, out + count, 0.f);
	std::shared_ptr<const std::vector<s16>> pcm;
	{
		std::lock_guard<std::mutex> lock(shapeMutex);
		pcm = startupShape;
	}
	if (pcm == nullptr || seconds < 0)
		return false;
	const size_t frames = pcm->size() / 2, each = 4, smooth = 48, look = 400;
	const size_t need = (size_t)count * each;
	size_t from = (size_t)(seconds * OutRate);
	if (frames < need + look + smooth || from + need + look + smooth >= frames)
		return false;
	auto mono = [&](size_t i) { return ((*pcm)[i * 2] + (*pcm)[i * 2 + 1]) * (0.5f / 32768.f); };
	// The shape starts where the sound, smoothed, next rises through zero.
	float sum = 0;
	for (size_t i = 0; i < smooth; i++)
		sum += mono(from + i);
	for (size_t i = 0; i < look; i++)
	{
		const float next = sum - mono(from + i) + mono(from + i + smooth);
		if (sum <= 0 && next > 0)
		{
			from += i + smooth / 2;
			break;
		}
		sum = next;
	}
	for (int i = 0; i < count; i++)
	{
		float mean = 0;
		for (size_t j = 0; j < each; j++)
			mean += mono(from + (size_t)i * each + j);
		out[i] = mean / each;
	}
	return true;
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

// ------------------------------------------------ the VMU's beeps, in a controller
//
// With "VMU sounds" going to the controller, the beep is played on the speaker
// of the DualSense whose memory card made it: a port of type 4 (the pad
// speaker) of that pad's user, mono at 48 kHz. The beep is not streamed from
// the sound chip's 44.1 kHz mixer, which would need resampling and a ring kept
// in step with the emulator: its tone (period and on, from the VMU's alarm)
// is read from the core and the same band-limited square made here at 48 kHz,
// with the same fade in and out. The mixer still calls sample() for every
// sample it would play, which says whether the TV keeps the beep and that the
// emulator is running: paused, muted or fast-forwarding, nothing is called and
// the speaker goes quiet. The port is opened when a beep starts, and opened
// again for the user of another pad when another memory card beeps. Not tried
// on a console yet: when the console refuses the port, the beep stays on the
// TV and the reason is in the log.
namespace
{

class PadSpeaker
{
public:
	// Called for the beep's every sample in the mixer; true when the TV is
	// not to have it.
	bool sample(int value, int bus)
	{
		(void)value;
		const int where = ps5::VmuSoundOutput;
		if (where == 0)
			return false;
		if (!started.exchange(true))
			thread = std::thread([this] { run(); });
		if (bus >= 0)
			wanted = bus;
		fed.fetch_add(1, std::memory_order_relaxed);
		return where == 1 && !refused.load() && !noPad.load();
	}

private:
	static constexpr u32 SpeakerRate = 48000;
	static constexpr int PortPadSpeaker = 4;
	static constexpr u32 FormatS16Mono = 0;

	void run()
	{
		PS5Priority::SetCurrentThread(PS5Priority::Role::Sound);
		using clock = std::chrono::steady_clock;
		audioLibrary();
		int port = -1;
		int user = -1;
		alignas(64) s16 grain[Grain];
		aica::sgc::VmuBeepWave wave;
		// The fade, as the sound chip's: 256 silent, 0 full, 2 steps a 44.1 kHz sample.
		float att = 256.f;
		const float attStep = 2.f * InRate / SpeakerRate;
		bool wasActive = false;
		bool toldNoPad = false;
		u32 lastFed = fed.load();
		clock::time_point fedAt = clock::now();
		clock::time_point checkedAt = fedAt;
		while (true)
		{
			int period = 0, on = 0, bus = -1;
			bool active = aica::sgc::vmuBeepParams(period, on, bus);
			if (bus < 0)
				bus = wanted.load();
			const clock::time_point now = clock::now();
			const u32 count = fed.load(std::memory_order_relaxed);
			if (count != lastFed)
			{
				lastFed = count;
				fedAt = now;
			}
			// The mixer has stopped calling: the emulator is paused, muted or
			// fast-forwarding, or the beep went back to the TV.
			if (now - fedAt > std::chrono::milliseconds(100))
				active = false;
			const bool starts = active && !wasActive;
			wasActive = active;

			// Whose speaker: looked up when a beep starts, and now and then
			// while it sounds, so that another pad's beep goes to that pad.
			if (active && (port < 0 || starts || now - checkedAt > std::chrono::milliseconds(500)))
			{
				checkedAt = now;
				const int u = ps5::pad::userOfPort(bus);
				noPad = u < 0;
				if (u < 0)
				{
					if (!toldNoPad)
						ps5::diag::mark("vmu sound: no pad for the speaker of port %d", bus);
					toldNoPad = true;
				}
				else
				{
					toldNoPad = false;
					if (port >= 0 && u != user)
					{
						sceAudioOutClose(port);
						ps5::diag::mark("vmu sound: pad speaker of user %d closed for user %d", user, u);
						port = -1;
					}
					if (port < 0)
					{
						port = sceAudioOutOpen(u, PortPadSpeaker, 0, Grain, SpeakerRate, FormatS16Mono);
						ps5::diag::mark("vmu sound: pad speaker of user %d -> %x", u, port);
						if (port < 0)
						{
							// The console says no: the TV keeps the beep. Not tried again.
							refused = true;
							return;
						}
						int32_t volume[2] = { 0x8000, 0x8000 };
						sceAudioOutSetVolume(port, 3, volume);
						user = u;
					}
				}
			}
			if (port < 0)
			{
				// Nothing to play into yet: wait for a beep.
				att = 256.f;
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
				continue;
			}
			for (u32 i = 0; i < Grain; i++)
			{
				if (!active && att >= 256.f)
				{
					grain[i] = 0;
					continue;
				}
				// About the level of the recording of a real VMU, as on the TV.
				const float v = wave.next(period, on, (float)SpeakerRate) * 16384.f * std::exp2(-att / 16.f);
				grain[i] = (s16)std::clamp((int)std::lround(v), -32768, 32767);
				att = active ? std::max(att - attStep, 0.f) : std::min(att + attStep, 256.f);
			}
			sceAudioOutOutput(port, grain);
		}
	}

	std::atomic<u32> fed{ 0 };		// the mixer's calls, counted
	std::atomic<bool> started{ false }, refused{ false }, noPad{ false };
	std::atomic<int> wanted{ -1 };
	std::thread thread;
};

PadSpeaker padSpeaker;

}

namespace ps5
{

bool vmuBeepSample(int sample, int bus)
{
	return padSpeaker.sample(sample, bus);
}

}

namespace ps5::sound
{

void pushStream(const short *samples, size_t frames, int channels, int rate, float gain)
{
	if (samples == nullptr || frames == 0 || channels < 1 || rate < 8000)
		return;
	// The mixer runs while a video plays: an empty voice keeps its port open.
	static const auto silence = std::make_shared<const std::vector<s16>>(std::vector<s16>(2, 0));
	::play(Voice{ silence, 0, 0.f, false, 1.f, false });
	std::lock_guard<std::mutex> lock(mixMutex);
	if (!mixRunning)
		return;
	streamGain = gain;
	const float step = (float)rate / OutRate;
	const size_t room = (size_t)OutRate * 2 * 2;		// two seconds of stereo
	for (size_t i = 0; i < frames; i++)
	{
		const float l = samples[i * channels], r = samples[i * channels + (channels > 1 ? 1 : 0)];
		while (streamPhase < 1.f)
		{
			if (streamPcm.size() - streamAt * 2 >= room)
				return;		// far ahead: the rest is dropped
			const float a = streamPhase;
			streamPcm.push_back((s16)(streamLast[0] + (l - streamLast[0]) * a));
			streamPcm.push_back((s16)(streamLast[1] + (r - streamLast[1]) * a));
			streamPhase += step;
		}
		streamPhase -= 1.f;
		streamLast[0] = l;
		streamLast[1] = r;
	}
}

void clearStream()
{
	std::lock_guard<std::mutex> lock(mixMutex);
	streamPcm.clear();
	streamAt = 0;
	streamPhase = 0.f;
}

}
