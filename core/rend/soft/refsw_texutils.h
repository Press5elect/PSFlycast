/*
	Derived from TexUtils.h of nullDC-rust (crates/refsw2-cpp/ffi), which says:

		This file is part of libswirl

	nullDC-rust (https://github.com/skmp/nullDC-rust, commit 6b76ff0) is under
	the MIT licence:

	Copyright (c) 2025 emudev-org

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files (the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions:

	The above copyright notice and this permission notice shall be included in all
	copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
	SOFTWARE.

	PSFlyCast changes (2026, the PSFlyCast contributors): namespace refsw (Flycast
	has a detwiddle table of its own with the same name); the macros are functions.
*/
#pragma once
#include "types.h"

#include <algorithm>

namespace refsw
{

extern u32 detwiddle[2][11][1024];
extern s8 BM_SIN90[256];
extern s8 BM_COS90[256];
extern s8 BM_COS360[256];

template<class pixel_type>
pixel_type cclamp(pixel_type minv, pixel_type maxv, pixel_type x) {
	return std::min(maxv, std::max(minv, x));
}

// Unpack to 32-bit word

static inline u32 ARGB1555_32(u32 word) {
	return ((word & 0x8000) ? 0xFF000000 : 0) | (((word >> 0) & 0x1F) << 3) | (((word >> 5) & 0x1F) << 11) | (((word >> 10) & 0x1F) << 19);
}

static inline u32 ARGB565_32(u32 word) {
	return (((word >> 0) & 0x1F) << 3) | (((word >> 5) & 0x3F) << 10) | (((word >> 11) & 0x1F) << 19) | 0xFF000000;
}

static inline u32 ARGB4444_32(u32 word) {
	return (((word >> 12) & 0xF) << 28) | (((word >> 0) & 0xF) << 4) | (((word >> 4) & 0xF) << 12) | (((word >> 8) & 0xF) << 20);
}

static inline u32 ARGB8888_32(u32 word) {
	return word;
}

static inline u32 packRGB(u8 R, u8 G, u8 B)
{
	return (R << 0) | (G << 8) | (B << 16) | 0xFF000000;
}

static inline u32 YUV422(s32 Y, s32 Yu, s32 Yv)
{
	Yu -= 128;
	Yv -= 128;

	s32 R = Y + Yv * 11 / 8;            // Y + (Yv-128) * (11/8) ?
	s32 G = Y - (Yu * 11 + Yv * 22) / 32; // Y - (Yu-128) * (11/8) * 0.25 - (Yv-128) * (11/8) * 0.5 ?
	s32 B = Y + Yu * 110 / 64;          // Y + (Yu-128) * (11/8) * 1.25 ?

	return packRGB(cclamp<s32>(0, 255, R), cclamp<s32>(0, 255, G), cclamp<s32>(0, 255, B));
}

static inline u32 twop(u32 x, u32 y, u32 bcx, u32 bcy) {
	return detwiddle[0][bcy + 3][x] + detwiddle[1][bcx + 3][y];
}

}
