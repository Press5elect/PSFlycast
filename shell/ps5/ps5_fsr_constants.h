/*
	PSFlyCast - the constants FSR 1's two passes take.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	What FsrEasuCon() and FsrRcasCon() of fsr/ffx_fsr1.h compute, for a
	picture that is the whole of its image: floats, passed to the shaders as
	their bits. Nothing here knows about Vulkan, so a test on the build
	machine compares it with AMD's functions.
*/
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ps5::fsr
{

struct EasuConstants
{
	uint32_t con0[4], con1[4], con2[4], con3[4];
};

// The picture's look, which every shader that puts the picture on the screen
// ends with (lookTail in ps5_fsr.cpp): colour is brightness, contrast,
// saturation and gamma (1 1 1 1 leaves the colours as they are); grain is its
// strength (0 for none), a grain's side in pixels, the frame's seed, 0.
struct LookConstants
{
	float colour[4] = { 1.f, 1.f, 1.f, 1.f };
	float grain[4] = { 0.f, 1.f, 0.f, 0.f };
};

struct RcasConstants
{
	uint32_t con[4];
	int32_t origin[4];		// of the picture, in the framebuffer
	LookConstants look;
};

// Sharp bilinear, CAS and the plain stretch, in one shader (kinds in ps5_fsr.cpp).
struct LookShaderConstants
{
	float origin[2];	// of the picture in the framebuffer, in pixels
	float size[2];		// of the picture there, in pixels
	uint32_t const0[4], const1[4];		// CAS's, from casConstants
	LookConstants look;
};

// What the picture filters' shader takes (ps5_crt.glsl.h), as it reads them.
struct CrtConstants
{
	float origin[2];	// of the picture in the framebuffer, in pixels
	float size[2];		// of the picture there, in pixels
	float beam[4];		// the game's lines; a line's first and second harmonics; the lines left in white
	float tube[4];		// the mask's pitch, in pixels, and its depth; the glow; the vignette
	LookConstants look;
};

inline uint32_t floatBits(float v)
{
	uint32_t u;
	memcpy(&u, &v, 4);
	return u;
}

// inW x inH: the picture; outW x outH: what it is upscaled to.
inline EasuConstants easuConstants(float inW, float inH, float outW, float outH)
{
	EasuConstants c;
	c.con0[0] = floatBits(inW * (1.f / outW));
	c.con0[1] = floatBits(inH * (1.f / outH));
	c.con0[2] = floatBits(0.5f * inW * (1.f / outW) - 0.5f);
	c.con0[3] = floatBits(0.5f * inH * (1.f / outH) - 0.5f);
	c.con1[0] = floatBits(1.f / inW);
	c.con1[1] = floatBits(1.f / inH);
	c.con1[2] = floatBits(1.f * (1.f / inW));
	c.con1[3] = floatBits(-1.f * (1.f / inH));
	c.con2[0] = floatBits(-1.f * (1.f / inW));
	c.con2[1] = floatBits(2.f * (1.f / inH));
	c.con2[2] = floatBits(1.f * (1.f / inW));
	c.con2[3] = floatBits(2.f * (1.f / inH));
	c.con3[0] = floatBits(0.f * (1.f / inW));
	c.con3[1] = floatBits(4.f * (1.f / inH));
	c.con3[2] = c.con3[3] = 0;
	return c;
}

// stops: how many halvings the sharpening is reduced by; 0 is the sharpest.
// (The second word is the half-float form, which the float pass does not read.)
inline RcasConstants rcasConstants(float stops, int originX, int originY)
{
	RcasConstants c{};
	c.con[0] = floatBits(std::exp2(-stops));
	c.origin[0] = originX;
	c.origin[1] = originY;
	return c;
}

// The picture filters. tube: the CRT one (a mask, a glow and a vignette as
// well as the lines). left, top, width, height: where the picture is drawn,
// in pixels; lines: how many the game's video mode has (240 or 480).
inline CrtConstants crtConstants(bool tube, float left, float top, float width, float height, float lines)
{
	CrtConstants c{};
	c.origin[0] = left;
	c.origin[1] = top;
	c.size[0] = width;
	c.size[1] = height;
	c.beam[0] = lines;
	// A line's profile is 1 + beam[1] cos(a) + beam[2] cos(2a), a being 0
	// along its middle: it must stay positive (beam[1] <= 1 + beam[2]).
	// beam[3]: how much darker the gaps are left in plain white, which
	// would otherwise show no lines at all; half of it is light lost there.
	if (!tube)
	{
		// Gentle: text stays easy to read.
		c.beam[1] = 0.60f;
		c.beam[2] = 0.08f;
		c.beam[3] = 0.05f;
		c.tube[0] = 1.f;
		return c;
	}
	c.beam[1] = 0.75f;
	c.beam[2] = 0.12f;
	c.beam[3] = 0.05f;
	// A period of three pixels at 2160 lines and of two at 1080 and 1440,
	// which the console doubles, or nearly, on a 4K screen.
	c.tube[0] = std::fmax(2.f, std::round(height / 720.f));
	c.tube[1] = 0.30f;		// a stripe's own colour up to 30% brighter, the other two 15% darker
	c.tube[2] = 0.05f;		// the share of a pixel's light that is the glow of those around
	c.tube[3] = 0.10f;		// how much darker the very corners are
	return c;
}


// CAS's constants for a picture of in x in pixels drawn at out x out
// (CasSetup of fsr/ffx_cas.h, on the CPU). sharpness: 0 is the default (less
// ringing), 1 the most.
inline void casConstants(uint32_t const0[4], uint32_t const1[4], float sharpness, float inW, float inH, float outW, float outH)
{
	const0[0] = floatBits(inW * (1.f / outW));
	const0[1] = floatBits(inH * (1.f / outH));
	const0[2] = floatBits(0.5f * inW * (1.f / outW) - 0.5f);
	const0[3] = floatBits(0.5f * inH * (1.f / outH) - 0.5f);
	const float s = std::fmin(std::fmax(sharpness, 0.f), 1.f);
	const float sharp = -1.f / (8.f + (5.f - 8.f) * s);
	const1[0] = floatBits(sharp);
	const1[1] = 0;		// the half-float form, which the float pass does not read
	const1[2] = floatBits(8.f * inW * (1.f / outW));
	const1[3] = 0;
}

// Whether CAS can scale from this size to that itself (its limit is 4 times the area).
inline bool casCanScale(float inW, float inH, float outW, float outH)
{
	return outW * outH <= 4.f * inW * inH;
}

// The look the settings ask for. grain: 0 to 3; the colours in per cent; frame
// counts the pictures shown, for the grain's pattern; outHeight is the picture's
// height on the screen.
inline LookConstants lookConstants(int grain, int brightness, int contrast, int saturation, int gamma, float outHeight,
		unsigned frame)
{
	LookConstants c;
	c.colour[0] = std::fmax(brightness, 10) / 100.f;
	c.colour[1] = std::fmax(contrast, 10) / 100.f;
	c.colour[2] = std::fmax(saturation, 0) / 100.f;
	c.colour[3] = std::fmax(gamma, 10) / 100.f;
	static const float strength[3] = { 0.045f, 0.085f, 0.14f };
	if (grain > 0)
	{
		c.grain[0] = strength[(grain > 3 ? 3 : grain) - 1];
		// A grain is two pixels at 1080 lines, four at 2160.
		c.grain[1] = std::fmax(std::floor(outHeight / 540.f), 1.f);
		c.grain[2] = (float)(frame % 1024u) * 0.731f;
	}
	return c;
}

}
