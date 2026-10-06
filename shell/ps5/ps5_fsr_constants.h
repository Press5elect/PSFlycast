/*
	PSFlyCast - the constants FSR 1's two passes take.

	Copyright 2026 the PSFlyCast contributors
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

struct RcasConstants
{
	uint32_t con[4];
	int32_t origin[4];		// of the picture, in the framebuffer
};

// What the picture filters' shader takes (ps5_crt.glsl.h), as it reads them.
struct CrtConstants
{
	float origin[2];	// of the picture in the framebuffer, in pixels
	float size[2];		// of the picture there, in pixels
	float beam[4];		// the game's lines; a line's first and second harmonics; the lines left in white
	float tube[4];		// the mask's pitch, in pixels, and its depth; the glow; the vignette
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

}
