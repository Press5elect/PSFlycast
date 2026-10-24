#pragma once
#include "types.h"

namespace aica::sgc
{

void AICA_Sample();

void WriteChannelReg(u32 channel, u32 reg, int size);

void init();
void term();

union fp_22_10
{
	struct
	{
		u32 fp:10;
		u32 ip:22;
	};
	u32 full;
};
union fp_s_22_10
{
	struct
	{
		u32 fp:10;
		s32 ip:22;
	};
	s32 full;
};
union fp_20_12
{
	struct
	{
		u32 fp:12;
		u32 ip:20;
	};
	u32 full;
};

typedef s32 SampleType;

void ReadCommonReg(u32 reg, bool byte);
void serialize(Serializer& ctx);
void deserialize(Deserializer& ctx);
// The VMU's alarm: period and on are its two bytes (ALw then ALd), the tone
// lasting period clock ticks of which it is high for on.
// bus: the controller port of the memory card that beeps (-1: not known).
void vmuBeep(int period, int on, int bus = -1);
// The beep as it stands, for a host that makes it itself (the PS5's DualSense
// speaker): true while it sounds. period and on stay the last tone's while it
// fades out.
bool vmuBeepParams(int& period, int& on, int& bus);

// The VMU's beep: a square wave high for on / period of each cycle, band-limited
// with PolyBLEP so that its edges do not alias at the output's rate. Shared by
// the sound chip's mixer (44.1 kHz) and the PS5's pad speaker (48 kHz).
struct VmuBeepWave
{
	double phase = 0.0;	// where in the cycle, 0 to 1

	// The VMU's 6 MHz clock divided by 6, then by the period.
	static int frequency(int period) {
		return period > 0 ? 6000000 / 6 / period : 0;
	}

	// The next sample: -1 to 1 for a duty of one half, with the DC of other
	// duties taken out (a speaker does not pass it, and the fades do not thump).
	float next(int period, int on, float rate)
	{
		if (period <= 0 || on <= 0 || on >= period)
			return 0.f;
		const double dt = frequency(period) / (double)rate;
		// Above the output's Nyquist frequency there is nothing to hear.
		if (dt <= 0.0 || dt >= 0.5)
			return 0.f;
		const double duty = (double)on / period;
		double v = phase < duty ? 1.0 : -1.0;
		v += blep(phase, dt);			// the rising edge, at 0
		double fall = phase - duty;		// the falling edge, at the duty
		if (fall < 0.0)
			fall += 1.0;
		v -= blep(fall, dt);
		phase += dt;
		if (phase >= 1.0)
			phase -= 1.0;
		return (float)(v - (2.0 * duty - 1.0));
	}

private:
	static double blep(double t, double dt)
	{
		if (t < dt)
		{
			t /= dt;
			return t + t - t * t - 1.0;
		}
		if (t > 1.0 - dt)
		{
			t = (t - 1.0) / dt;
			return t * t + t + t + 1.0;
		}
		return 0.0;
	}
};

} // namespace aica::sgc
