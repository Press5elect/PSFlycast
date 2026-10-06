/*
	Derived from TexUtils.cc of nullDC-rust (crates/refsw2-cpp/ffi), part of
	libswirl, by Stefanos Kornilios Mitsis Poiitidis (skmp).

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

	PSFlyCast changes (2026, the PSFlyCast contributors): namespace refsw; the
	tables are filled once, whichever thread asks first.
*/
#include "refsw_core.h"
#include "refsw_texutils.h"

#include <cmath>
#include <mutex>

namespace refsw
{

u32 detwiddle[2][11][1024];
s8 BM_SIN90[256];
s8 BM_COS90[256];
s8 BM_COS360[256];

static u32 twiddle_slow(u32 x, u32 y, u32 x_sz, u32 y_sz)
{
	u32 rv = 0;//low 2 bits are directly passed  -> needs some misc stuff to work.However
			 //Pvr internally maps the 64b banks "as if" they were twiddled :p

	u32 sh = 0;
	x_sz >>= 1;
	y_sz >>= 1;
	while (x_sz != 0 || y_sz != 0)
	{
		if (y_sz)
		{
			u32 temp = y & 1;
			rv |= temp << sh;

			y_sz >>= 1;
			y >>= 1;
			sh++;
		}
		if (x_sz)
		{
			u32 temp = x & 1;
			rv |= temp << sh;

			x_sz >>= 1;
			x >>= 1;
			sh++;
		}
	}
	return rv;
}

static void fillTables()
{
	// a double, as M_PI is where the reference is built
	const double pi = 3.14159265358979323846;

	for (u32 s = 0; s < 11; s++)
	{
		u32 x_sz = 1024;
		u32 y_sz = 1 << s;
		for (u32 i = 0; i < x_sz; i++)
		{
			detwiddle[0][s][i] = twiddle_slow(i, 0, x_sz, y_sz);
			detwiddle[1][s][i] = twiddle_slow(0, i, y_sz, x_sz);
		}
	}

	for (int i = 0; i < 256; i++) {
		BM_SIN90[i]  = 127 * sinf((i / 256.0f) * (pi / 2));
		BM_COS90[i]  = 127 * cosf((i / 256.0f) * (pi / 2));
		BM_COS360[i] = 127 * cosf((i / 256.0f) * (2 * pi));
	}
}

void InitTexUtils()
{
	static std::once_flag once;
	std::call_once(once, fillTables);
}

}
