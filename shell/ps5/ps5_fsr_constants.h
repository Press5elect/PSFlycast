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

}
