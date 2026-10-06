/*
	REFSW: reference-style software model of the PowerVR CLX2 "CORE", by
	Stefanos Kornilios Mitsis Poiitidis (skmp).

	Derived from refsw_tile.cc, refsw_lists.cc and gentable.h of nullDC-rust
	(crates/refsw2-cpp/ffi), which say:

		This file is part of libswirl

		Implementes the Reference SoftWare renderer (RefRendInterface) backend for refrend_base.
		This includes buffer operations and rasterization

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

	PSFlyCast changes (2026, the PSFlyCast contributors).

	How it is fed and run:
	- the tile buffers are a TileState (one per worker thread) and the
	  registers a FrameConfig snapshot, instead of globals;
	- a tag is an index into the frame's prepared triangles; GetFpuEntry copies
	  a triangle's surface equations for the tile from that array instead of
	  decoding parameters and vertices from VRAM (decode_pvr_vertex and
	  decode_pvr_vertices are gone);
	- the walk of the region array and of the object lists (refsw_lists.cc) is
	  RenderTilePass, one entry of the region array, given as lists of tags;
	- the writeout has every pack mode of FB_W_CTRL, dithering when FB_W_CTRL
	  asks for it, the pixel clip and the texture memory of a render to a
	  texture (WritePixels);
	- texture dumping and the logging hooks are gone.

	Faster, with the same result for every pixel (the host test compares
	frames with the reference's, bit for bit):
	- a triangle is culled and its edge constants are computed once a frame
	  (SetupTriangle), not once a tile; a tile then visits the pixels of the
	  triangle's bounding box, eight at a time with AVX2 (and, for the modes
	  that only look at the pixel's own depth, tests and writes them eight at a
	  time), instead of all 1024;
	- the function tables of gentable.h are built by the compiler, and a
	  polygon's entries are looked up once a frame (SelectShadeFuncs), not
	  for every pixel;
	- point sampling fetches one texel instead of four (the other three were
	  not used), and bilinear filtering fetches its four by one call and
	  weighs their four components at once with SSE;
	- the four components of a colour are interpolated at once with SSE;
	- a pre-sorted translucent triangle's pixels are looked for where the
	  triangle is, not over the whole tile.

	Different from the reference:
	- trilinear filtering, which the reference refuses, is bilinear;
	- YUV422 textures: the reference took the pair and the luma of a texel
	  from the wrong bits of its position, in both scan orders. They follow
	  Flycast's decoders (rend/texconv.cpp);
	- a punch-through pixel that fails the alpha test is not written. The
	  reference blended it with alpha 0, which is the same thing for the usual
	  source-alpha blend and a stray texel for any other;
	- "outside last" modifier volumes remove their inside from the area in
	  shadow, as Flycast's renderers do. The reference kept the intersection;
	- RM_DEPTH_ONLY: after an auto-sorted translucent list, when the tile has
	  another pass that keeps the depth buffer, the buffer is given the depth
	  of the opaque geometry and of the translucent list. The reference left
	  the last peeled layer in it;
	- a pixel gets at most 256 layers of translucent polygons and 256 of
	  punch-through ones (and no more than the tile's list has triangles), and
	  a triangle whose depth is not a number, or would overflow, is not drawn:
	  the reference loops until nothing is left, which is for ever with a
	  depth that is not a number;
	- a texture coordinate that is not a number or too large for an int is
	  converted as the x86 instruction does (FloatToInt); in C++ the
	  reference's cast is undefined there;
	- the secondary accumulation buffer, the primary one and the persistent
	  offset colour start each tile from fixed values, so that a tile does
	  not depend on the tile the same thread rendered before it.
*/
#include "refsw_core.h"
#include "refsw_texutils.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

#if defined(__SSE__)
#include <immintrin.h>
#endif

namespace refsw
{

FrameConfig cfg;

// PSFlyCast: the most layers of translucent or punch-through polygons drawn on a pixel
constexpr u32 MAX_PEEL_PASSES = 256;
// PSFlyCast: a triangle whose depth (1/w) could overflow a float in a tile, or
// not be a number, is not drawn there: where its depth at the first vertex or
// in the tile's corner is beyond MAX_DEPTH, or changes by more than
// MAX_DEPTH_STEP a pixel. Every depth in the buffers is then a number, which
// the per-pixel sort needs to come to an end. (The reference draws them, and
// its sort then never ends.)
constexpr float MAX_DEPTH = 1e36f;
constexpr float MAX_DEPTH_STEP = 1e35f;

constexpr u32 tagBufferA = 0;
constexpr u32 tagBufferB = 1;
constexpr u32 depthBufferA = 0;
constexpr u32 depthBufferB = 1;
constexpr u32 depthBufferC = 2;

#define always_inline __attribute__((always_inline))

// Z buffer doesn't store sign, and has 19 bits of m
static inline float mask_w(float w) {
	return w;
}

static void ClearBuffers(TileState& s, u32 paramValue, float depthValue, u32 stencilValue)
{
	auto zb = s.depthBuffer[depthBufferA];
	auto stencil = s.stencilBuffer;
	auto pb = s.tagBuffer[tagBufferA];

	for (int i = 0; i < MAX_RENDER_PIXELS; i++) {
		zb[i] = mask_w(depthValue);
		stencil[i] = stencilValue;
		pb[i] = paramValue;
		s.tagStatus[i] = TAG_VALID;
	}
}

static void ClearParamStatusBuffer(TileState& s) {
	memset(s.tagStatus, 0, sizeof(s.tagStatus));
}

static void PeelBuffersPTInitial(TileState& s) {
	memcpy(s.depthBuffer[depthBufferC], s.depthBuffer[depthBufferA], sizeof(ZType) * MAX_RENDER_PIXELS);
	memset(s.tagStatus, 0, sizeof(s.tagStatus));
	memset(s.stencilBuffer, 0, sizeof(s.stencilBuffer));
}

static void PeelBuffersPT(TileState& s) {
	memcpy(s.depthBuffer[depthBufferB], s.depthBuffer[depthBufferA], sizeof(ZType) * MAX_RENDER_PIXELS);
	memcpy(s.tagBuffer[tagBufferB], s.tagBuffer[tagBufferA], sizeof(parameter_tag_t) * MAX_RENDER_PIXELS);
}

static void SetTagToMax(TileState& s)
{
	memset(s.tagBuffer[tagBufferA], 0xFF, sizeof(s.tagBuffer[tagBufferA]));
}

static void PeelBuffers(TileState& s, float depthValue, u32 stencilValue)
{
	memcpy(s.depthBuffer[depthBufferB], s.depthBuffer[depthBufferA], sizeof(ZType) * MAX_RENDER_PIXELS);
	memcpy(s.tagBuffer[tagBufferB], s.tagBuffer[tagBufferA], sizeof(parameter_tag_t) * MAX_RENDER_PIXELS);

	auto zb = s.depthBuffer[depthBufferA];

	for (int i = 0; i < MAX_RENDER_PIXELS; i++)
		zb[i] = mask_w(depthValue);    // set the "closest" test to furthest value possible
	memset(s.tagStatus, 0, sizeof(s.tagStatus));
	memset(s.stencilBuffer, stencilValue, sizeof(s.stencilBuffer));
}

static void SummarizeStencilOr(TileState& s) {
	auto stencil = s.stencilBuffer;

	// post movdol merge INSIDE
	for (int i = 0; i < MAX_RENDER_PIXELS; i++) {
		if (stencil[i] & 0b100) {
			stencil[i] |= (stencil[i] >> 1);
			stencil[i] &= 0b001; // keep only status bit
		}
	}
}

static void SummarizeStencilAnd(TileState& s) {
	auto stencil = s.stencilBuffer;

	for (int i = 0; i < MAX_RENDER_PIXELS; i++) {
		// post movdol merge OUTSIDE
		// PSFlyCast: the volume's inside leaves the area (the reference: stencil &= stencil >> 1)
		if (stencil[i] & 0b100) {
			stencil[i] &= ~(stencil[i] >> 1);
			stencil[i] &= 0b001; // keep only status bit
		}
	}
}

// The surface equations of a triangle in this tile.
// PSFlyCast: from the frame's prepared triangles; the reference decoded them from VRAM.
static inline always_inline const TileEntry& GetFpuEntry(TileState& s, parameter_tag_t tag)
{
	TileEntry& entry = s.entries[tag & (TILE_ENTRY_CACHE - 1)];
	if (entry.tag == tag)
		return entry;

	const Triangle& tri = s.frame->tris[tag];
	const IPs3& ips = s.frame->ips[tag];
	const PolyState& poly = s.frame->polys[tri.poly];
	const float dx = tri.x1 - s.left;
	const float dy = tri.y1 - s.top;

	entry.poly = &poly;
	entry.invW.Set(ips.invW, dx, dy);
	const int volumes = poly.twoVolumes ? 2 : 1;
	for (int v = 0; v < volumes; v++)
	{
		for (int i = 0; i < 4; i++)
			entry.Col[v].Set(i, ips.Col[v][i], dx, dy);
		if (poly.params.isp.Texture)
		{
			entry.U[v].Set(ips.U[v], dx, dy);
			entry.V[v].Set(ips.V[v], dx, dy);
			if (poly.params.isp.Offset)
				for (int i = 0; i < 4; i++)
					entry.Ofs[v].Set(i, ips.Ofs[v][i], dx, dy);
		}
	}
	entry.tag = tag;

	return entry;
}

static void ClearFpuCache(TileState& s) {
	for (TileEntry& entry : s.entries)
		entry.tag = 0xFFFFFFFF;
}

static bool PixelFlush_tsp(TileState& s, bool pp_AlphaTest, const TileEntry *entry, float x, float y, u32 index, float invW, bool InVolume);

// Render to ACCUM from TAG buffer
// TAG holds references to trianes, ACCUM is the tile framebuffer
// PSFlyCast: over a rectangle of the tile (all of it, or the triangle a pre-sorted list just rasterised)
template<RenderMode rm>
static void RenderParamTags(TileState& s, int x0, int y0, int x1, int y1) {
	float halfpixel = cfg.tspHalfOffset ? 0.5f : 0;

	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			auto index = y * 32 + x;
			u8 status = s.tagStatus[index];
			bool TagValid = (status & TAG_VALID) != 0;
			bool Stencil = (s.stencilBuffer[index] & 0b001) == 0b001;

			if (rm == RM_PUNCHTHROUGH_MV) {
				if (!Stencil) {
					continue;
				} else {
					TagValid = (status & TAG_RENDERED) != 0;
				}
			}

			if (TagValid) {
				auto tag = s.tagBuffer[tagBufferA][index];
				const auto& Entry = GetFpuEntry(s, tag);
				bool InVolume = Stencil && Entry.poly->shadow;

				if (rm == RM_PUNCHTHROUGH_MV && !InVolume)
					continue;
				if (rm == RM_PUNCHTHROUGH_PASS0 || rm == RM_PUNCHTHROUGH_PASSN) {
					InVolume = false;
				}

				auto invW = Entry.invW.Ip(x + halfpixel, y + halfpixel);
				bool AlphaTestPassed = PixelFlush_tsp(s, rm == RM_PUNCHTHROUGH_PASS0 || rm == RM_PUNCHTHROUGH_PASSN, &Entry, x + halfpixel, y + halfpixel, index, invW, InVolume);
				s.tspPixels++;

				if (rm == RM_PUNCHTHROUGH_PASS0 || rm == RM_PUNCHTHROUGH_PASSN) {
					// can only happen when rm == RM_PUNCHTHROUGH
					if (!AlphaTestPassed) {
						s.MoreToDraw = true;
						// Feedback Channel
						s.depthBuffer[depthBufferA][index] = s.depthBuffer[depthBufferC][index];
					} else {
						s.tagStatus[index] = (status | TAG_RENDERED) & ~TAG_VALID;
					}
				}

				if (rm == RM_TRANSLUCENT_PRESORT) {
					s.tagStatus[index] = status & ~TAG_VALID;
				}
			}
		}
	}
}

template<RenderMode rm>
static void RenderParamTags(TileState& s) {
	RenderParamTags<rm>(s, 0, 0, 31, 31);
}

static inline always_inline bool IsTopLeft(float x, float y) {
	bool IsTop = y == 0 && x > 0;
	bool IsLeft = y < 0;

	return IsTop || IsLeft;
}

// Depth processing for a pixel -- render_mode 0: OPAQ, 1: PT, 2: TRANS
template<RenderMode render_mode>
static inline always_inline void PixelFlush_isp(TileState& s, u32 depth_mode, u32 ZWriteDis, float invW, u32 index, parameter_tag_t tag)
{
	auto pb = s.tagBuffer[tagBufferA] + index;
	auto ts = s.tagStatus + index;
	auto pb2 = s.tagBuffer[tagBufferB] + index;
	auto zb = s.depthBuffer[depthBufferA] + index;
	auto zb2 = s.depthBuffer[depthBufferB] + index;
	auto stencil = s.stencilBuffer + index;

	auto mode = depth_mode;

	if (render_mode == RM_PUNCHTHROUGH_PASS0 || render_mode == RM_PUNCHTHROUGH_PASSN)
		mode = 6;
	else if (render_mode == RM_TRANSLUCENT_AUTOSORT)
		mode = 3;
	else if (render_mode == RM_MODIFIER)
		mode = 6;

	switch(mode) {
		// never
		case 0: return; break;
		// less
		case 1: if (invW >= *zb) { return; } break;
		// equal
		case 2: if (invW != *zb) { return; } break;
		// less or equal
		case 3: if (invW > *zb) {
			if (render_mode == RM_TRANSLUCENT_AUTOSORT) {
				s.MoreToDraw = true;
			}
			return;
		}break;
		// greater
		case 4: if (invW <= *zb) { return; } break;
		// not equal
		case 5: if (invW == *zb) { return; } break;
		// greater or equal
		case 6: if (invW < *zb) { return; } break;
		// always
		case 7: break;
	}

	switch (render_mode)
	{
		// OPAQ
		case RM_OPAQUE:
		{
			// Z pre-pass only
			if (!ZWriteDis) {
				*zb = mask_w(invW);
			}
			*pb = tag;
			*ts |= TAG_VALID;
		}
		break;

		case RM_MODIFIER:
		{
			// Flip on Z pass

			*stencil ^= 0b0010;

			// This pixel has valid stencil for summary
			*stencil |= 0b100;
		}
		break;

		case RM_PUNCHTHROUGH_PASS0:
		{
			*zb = mask_w(invW);
			*pb = tag;

			*ts |= TAG_VALID;
		}
		break;
		// PT
		case RM_PUNCHTHROUGH_PASSN:
		{
			if (*ts & TAG_RENDERED) {
				return;
			}

			if (invW > *zb2) {
				return;
			}

			if (invW == *zb2 || invW == *zb) {
				auto tagRendered = *pb2;

				if ((tag & PARAMETER_TAG_SORT_MASK) <= (tagRendered & PARAMETER_TAG_SORT_MASK)) {
					return;
				}
			}

			s.MoreToDraw = true;

			*zb = mask_w(invW);
			*pb = tag;
		}
		break;

		// Layer Peeling. zb2 holds the reference depth, zb is used to find closest to reference
		case RM_TRANSLUCENT_PRESORT:
		{
			if (!ZWriteDis) {
				*zb = mask_w(invW);
			}
			*pb = tag;
			*ts |= TAG_VALID;
		}
		break;
		case RM_TRANSLUCENT_AUTOSORT:
		{
			if (invW < *zb2) {
				return;
			}

			if (invW == *zb2) {
				auto tagRendered = *pb2;

				// if tag is earlier or same as last rendered, skip
				if ((tag & PARAMETER_TAG_SORT_MASK) <= (tagRendered & PARAMETER_TAG_SORT_MASK) && tagRendered != 0xFFFFFFFF) {
					return;
				}
			}

			if (invW == *zb) {
				auto tagRendered = *pb2;

				// if tag is earlier or same as last rendered, skip
				if ((tag & PARAMETER_TAG_SORT_MASK) <= (tagRendered & PARAMETER_TAG_SORT_MASK) && tagRendered != 0xFFFFFFFF) {
					return;
				}

				if (*ts & TAG_VALID) {
					auto tagPending = *pb;
					// if tag is later than the current pending, skip
					if ((tag & PARAMETER_TAG_SORT_MASK) > (tagPending & PARAMETER_TAG_SORT_MASK)) {
						s.MoreToDraw = true;
						return;
					}
				}
			}

			*zb = mask_w(invW);

			if (*ts & TAG_VALID) {
				s.MoreToDraw = true;
			}
			*ts |= TAG_VALID;
			*pb = tag;
		}
		break;

		// PSFlyCast: only the depth, with the polygon's own compare mode
		case RM_DEPTH_ONLY:
		{
			if (!ZWriteDis) {
				*zb = mask_w(invW);
			}
		}
		break;

		case RM_PUNCHTHROUGH_MV: break;	// this is invalid here
	}
}

static inline float flushNan(float a) {
	return std::isnan(a) ? 0 : a;
}

// The part of the reference's RasterizeTriangle that does not depend on the tile.
bool SetupTriangle(Triangle& tri, u32 cullMode, float x1, float y1, float z1, float x2, float y2, float z2, float x3, float y3, float z3)
{
	const float Y1 = flushNan(y1);
	const float Y2 = flushNan(y2);
	const float Y3 = flushNan(y3);

	const float X1 = flushNan(x1);
	const float X2 = flushNan(x2);
	const float X3 = flushNan(x3);

	int sgn = 1;

	float tri_area = ((X1 - X3) * (Y2 - Y3) - (Y1 - Y3) * (X2 - X3));

	if (tri_area > 0)
		sgn = -1;

	// cull
	if (cullMode != 0) {
		//area: (X1-X3)*(Y2-Y3)-(Y1-Y3)*(X2-X3)

		float abs_area = fabsf(tri_area);

		if (abs_area < cfg.cullVal)
			return false;

		if (cullMode >= 2) {
			u32 mode = cullMode & 1;

			if (
				(mode == 0 && tri_area < 0) ||
				(mode == 1 && tri_area > 0)) {
				return false;
			}
		}
	}

	// Half-edge constants
	tri.DX12 = sgn * (X1 - X2);
	tri.DX23 = sgn * (X2 - X3);
	tri.DX31 = sgn * (X3 - X1);

	tri.DY12 = sgn * (Y1 - Y2);
	tri.DY23 = sgn * (Y2 - Y3);
	tri.DY31 = sgn * (Y3 - Y1);

	bool T1 = IsTopLeft(X2 - X1, Y2 - Y1);
	bool T2 = IsTopLeft(X3 - X2, Y3 - Y2);
	bool T3 = IsTopLeft(X1 - X3, Y1 - Y3);
	tri.topLeft = (T1 ? 1 : 0) | (T2 ? 2 : 0) | (T3 ? 4 : 0);

	tri.x1 = X1;
	tri.y1 = Y1;
	tri.x2 = X2;
	tri.y2 = Y2;
	tri.x3 = X3;
	tri.y3 = Y3;
	tri.Z.Setup(x1, y1, x2, y2, x3, y3, z1, z2, z3);
	// PSFlyCast: a depth that cannot be a number in every pixel of a tile is not
	// drawn (see MAX_DEPTH). The comparison is false for a NaN.
	if (!(fabsf(tri.Z.ddx) < MAX_DEPTH_STEP && fabsf(tri.Z.ddy) < MAX_DEPTH_STEP && fabsf(tri.Z.a1) < MAX_DEPTH))
		return false;

	tri.minX = std::min(X1, std::min(X2, X3));
	tri.maxX = std::max(X1, std::max(X2, X3));
	tri.minY = std::min(Y1, std::min(Y2, Y3));
	tri.maxY = std::max(Y1, std::max(Y2, Y3));

	return true;
}

#if defined(__AVX2__)
// For each eight bits, eight bytes: 1 where the bit is set. (A table: the
// instruction that does this, PDEP, is slow on the console's processor.)
static constexpr std::array<u64, 256> MakeMaskBytes()
{
	std::array<u64, 256> table{};
	for (u32 mask = 0; mask < 256; mask++)
		for (u32 bit = 0; bit < 8; bit++)
			if (mask & (1u << bit))
				table[mask] |= 1ull << (bit * 8);
	return table;
}
static constexpr std::array<u64, 256> maskBytes = MakeMaskBytes();
#endif

struct TileRect
{
	int x0, y0, x1, y1;

	bool empty() const { return x0 > x1 || y0 > y1; }
};

// The pixels of the tile a triangle can cover: its bounding box and one more on each side.
static inline TileRect TriangleRect(const TileState& s, const Triangle& tri)
{
	TileRect r;
	r.x0 = (int)std::min(32.f, std::max(0.f, floorf(tri.minX - s.left) - 1));
	r.x1 = (int)std::max(-1.f, std::min(31.f, ceilf(tri.maxX - s.left) + 1));
	r.y0 = (int)std::min(32.f, std::max(0.f, floorf(tri.minY - s.top) - 1));
	r.y1 = (int)std::max(-1.f, std::min(31.f, ceilf(tri.maxY - s.top) + 1));
	return r;
}

// Rasterize a single triangle to ISP (or ISP+TSP for PT)
template<RenderMode render_mode>
static TileRect RasterizeTriangle(TileState& s, const Triangle& tri, parameter_tag_t tag)
{
	const TileRect rect = TriangleRect(s, tri);
	if (rect.empty())
		return rect;
	s.ispTriangles++;

	const float DX12 = tri.DX12;
	const float DX23 = tri.DX23;
	const float DX31 = tri.DX31;

	const float DY12 = tri.DY12;
	const float DY23 = tri.DY23;
	const float DY31 = tri.DY31;

	const float C1 = DY12 * (tri.x1 - s.left) - DX12 * (tri.y1 - s.top);
	const float C2 = DY23 * (tri.x2 - s.left) - DX23 * (tri.y2 - s.top);
	const float C3 = DY31 * (tri.x3 - s.left) - DX31 * (tri.y3 - s.top);

	const bool T1 = (tri.topLeft & 1) != 0;
	const bool T2 = (tri.topLeft & 2) != 0;
	const bool T3 = (tri.topLeft & 4) != 0;

	TilePlane Z;
	Z.Set(tri.Z, tri.x1 - s.left, tri.y1 - s.top);
	// PSFlyCast: see MAX_DEPTH
	if (!(fabsf(Z.c) < MAX_DEPTH)) {
		TileRect none = { 0, 0, -1, -1 };
		return none;
	}

	const float halfpixel = cfg.fpuHalfOffset ? 0.5f : 0;
	const u32 depthMode = tri.depthMode;
	const u32 zWriteDis = tri.zWriteDis;

#if defined(__AVX2__)
	// Eight pixels of a row at a time: the same operations, in the same order, as one at a time below
	const __m256 vDY12 = _mm256_set1_ps(DY12);
	const __m256 vDY23 = _mm256_set1_ps(DY23);
	const __m256 vDY31 = _mm256_set1_ps(DY31);
	const __m256 vZddx = _mm256_set1_ps(Z.ddx);
	const __m256 vZc = _mm256_set1_ps(Z.c);
	const __m256 vT1 = _mm256_castsi256_ps(_mm256_set1_epi32(T1 ? -1 : 0));
	const __m256 vT2 = _mm256_castsi256_ps(_mm256_set1_epi32(T2 ? -1 : 0));
	const __m256 vT3 = _mm256_castsi256_ps(_mm256_set1_epi32(T3 ? -1 : 0));
	const __m256 zero = _mm256_setzero_ps();
	const __m256 lane = _mm256_setr_ps(0, 1, 2, 3, 4, 5, 6, 7);
	const int xFirst = rect.x0 & ~7;

	// The modes whose depth test and writes need nothing but the pixel's own
	// depth are done eight pixels at a time too. The test, as PixelFlush_isp
	// has it: the pixel fails when its depth is less than, equal to, greater
	// than or not comparable with the buffer's, as the mode says.
	constexpr bool wide = render_mode == RM_OPAQUE || render_mode == RM_PUNCHTHROUGH_PASS0
			|| render_mode == RM_MODIFIER || render_mode == RM_DEPTH_ONLY;
	enum { FailLess = 1, FailEqual = 2, FailGreater = 4, FailUnordered = 8 };
	static const u8 failsWhen[8] = {
		FailLess | FailEqual | FailGreater | FailUnordered,	// never
		FailEqual | FailGreater,							// less
		FailLess | FailGreater | FailUnordered,				// equal
		FailGreater,										// less or equal
		FailLess | FailEqual,								// greater
		FailEqual,											// not equal
		FailLess,											// greater or equal
		0,													// always
	};
	const u32 wideMode = (render_mode == RM_PUNCHTHROUGH_PASS0 || render_mode == RM_MODIFIER) ? 6 : depthMode;
	const u8 fails = failsWhen[wideMode & 7];
	const __m256 vFailLess = _mm256_castsi256_ps(_mm256_set1_epi32((fails & FailLess) ? -1 : 0));
	const __m256 vFailEqual = _mm256_castsi256_ps(_mm256_set1_epi32((fails & FailEqual) ? -1 : 0));
	const __m256 vFailGreater = _mm256_castsi256_ps(_mm256_set1_epi32((fails & FailGreater) ? -1 : 0));
	const __m256 vFailUnordered = _mm256_castsi256_ps(_mm256_set1_epi32((fails & FailUnordered) ? -1 : 0));
	const __m256i vTag = _mm256_set1_epi32((int)tag);

	for (int y = rect.y0; y <= rect.y1; y++)
	{
		const float y_ps = y + halfpixel;
		const __m256 r12 = _mm256_set1_ps(C1 + DX12 * y_ps);
		const __m256 r23 = _mm256_set1_ps(C2 + DX23 * y_ps);
		const __m256 r31 = _mm256_set1_ps(C3 + DX31 * y_ps);
		const __m256 zy = _mm256_set1_ps(y_ps * Z.ddy);

		for (int x = xFirst; x <= rect.x1; x += 8)
		{
			const __m256 x_ps = _mm256_add_ps(_mm256_add_ps(_mm256_set1_ps((float)x), lane), _mm256_set1_ps(halfpixel));
			const __m256 Xhs12 = _mm256_sub_ps(r12, _mm256_mul_ps(vDY12, x_ps));
			const __m256 Xhs23 = _mm256_sub_ps(r23, _mm256_mul_ps(vDY23, x_ps));
			const __m256 Xhs31 = _mm256_sub_ps(r31, _mm256_mul_ps(vDY31, x_ps));

			const __m256 in12 = _mm256_or_ps(_mm256_cmp_ps(Xhs12, zero, _CMP_GT_OQ), _mm256_and_ps(vT1, _mm256_cmp_ps(Xhs12, zero, _CMP_EQ_OQ)));
			const __m256 in23 = _mm256_or_ps(_mm256_cmp_ps(Xhs23, zero, _CMP_GT_OQ), _mm256_and_ps(vT2, _mm256_cmp_ps(Xhs23, zero, _CMP_EQ_OQ)));
			const __m256 in31 = _mm256_or_ps(_mm256_cmp_ps(Xhs31, zero, _CMP_GT_OQ), _mm256_and_ps(vT3, _mm256_cmp_ps(Xhs31, zero, _CMP_EQ_OQ)));
			const __m256 inTriangle = _mm256_and_ps(in12, _mm256_and_ps(in23, in31));
			u32 mask = (u32)_mm256_movemask_ps(inTriangle);
			if (mask == 0)
				continue;

			const __m256 vInvW = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(x_ps, vZddx), zy), vZc);
			const u32 index = y * 32 + x;

			if (wide)
			{
				float *zb = s.depthBuffer[depthBufferA] + index;
				const __m256 depth = _mm256_loadu_ps(zb);
				const __m256 failed = _mm256_or_ps(
						_mm256_or_ps(_mm256_and_ps(vFailLess, _mm256_cmp_ps(vInvW, depth, _CMP_LT_OQ)),
								_mm256_and_ps(vFailEqual, _mm256_cmp_ps(vInvW, depth, _CMP_EQ_OQ))),
						_mm256_or_ps(_mm256_and_ps(vFailGreater, _mm256_cmp_ps(vInvW, depth, _CMP_GT_OQ)),
								_mm256_and_ps(vFailUnordered, _mm256_cmp_ps(vInvW, depth, _CMP_UNORD_Q))));
				const __m256 passed = _mm256_andnot_ps(failed, inTriangle);
				const u32 passedMask = (u32)_mm256_movemask_ps(passed);
				if (passedMask == 0)
					continue;
				// one byte of a pixel's status or stencil for each pixel that passed
				const u64 bytes = maskBytes[passedMask];

				if (render_mode == RM_MODIFIER)
				{
					// Flip on Z pass; this pixel has valid stencil for summary
					u64 stencil;
					memcpy(&stencil, s.stencilBuffer + index, 8);
					stencil ^= bytes * 0b0010;
					stencil |= bytes * 0b100;
					memcpy(s.stencilBuffer + index, &stencil, 8);
				}
				else
				{
					if (render_mode == RM_PUNCHTHROUGH_PASS0 || !zWriteDis)
						_mm256_storeu_ps(zb, _mm256_blendv_ps(depth, vInvW, passed));
					if (render_mode != RM_DEPTH_ONLY)
					{
						_mm256_maskstore_epi32((int *)(s.tagBuffer[tagBufferA] + index), _mm256_castps_si256(passed), vTag);
						u64 status;
						memcpy(&status, s.tagStatus + index, 8);
						status |= bytes * TAG_VALID;
						memcpy(s.tagStatus + index, &status, 8);
					}
				}
				continue;
			}

			if (render_mode == RM_TRANSLUCENT_AUTOSORT)
			{
				// What PixelFlush_isp does first, for the eight: a pixel nearer than
				// the nearest found so far waits for a later pass, and one behind
				// the layer drawn last was drawn before
				const __m256 nearer = _mm256_and_ps(inTriangle, _mm256_cmp_ps(vInvW, _mm256_loadu_ps(s.depthBuffer[depthBufferA] + index), _CMP_GT_OQ));
				if (_mm256_movemask_ps(nearer) != 0)
					s.MoreToDraw = true;
				const __m256 behind = _mm256_cmp_ps(vInvW, _mm256_loadu_ps(s.depthBuffer[depthBufferB] + index), _CMP_LT_OQ);
				mask = (u32)_mm256_movemask_ps(_mm256_andnot_ps(behind, _mm256_andnot_ps(nearer, inTriangle)));
				if (mask == 0)
					continue;
			}

			alignas(32) float invW[8];
			_mm256_store_ps(invW, vInvW);
			do {
				const int i = __builtin_ctz(mask);
				mask &= mask - 1;
				PixelFlush_isp<render_mode>(s, depthMode, zWriteDis, invW[i], index + i, tag);
			} while (mask != 0);
		}
	}
#else
	for (int y = rect.y0; y <= rect.y1; y++)
	{
		const float y_ps = y + halfpixel;
		const float r12 = C1 + DX12 * y_ps;
		const float r23 = C2 + DX23 * y_ps;
		const float r31 = C3 + DX31 * y_ps;

		for (int x = rect.x0; x <= rect.x1; x++)
		{
			const float x_ps = x + halfpixel;
			float Xhs12 = r12 - DY12 * x_ps;
			float Xhs23 = r23 - DY23 * x_ps;
			float Xhs31 = r31 - DY31 * x_ps;

			bool inTriangle = (Xhs12 > 0 || (T1 && Xhs12 == 0)) &&
							  (Xhs23 > 0 || (T2 && Xhs23 == 0)) &&
							  (Xhs31 > 0 || (T3 && Xhs31 == 0));

			if (inTriangle) {
				u32 index = y * 32 + x;
				float invW = Z.Ip(x_ps, y_ps);
				PixelFlush_isp<render_mode>(s, depthMode, zWriteDis, invW, index, tag);
			}
		}
	}
#endif

	return rect;
}

// Clamp and flip a texture coordinate
template<bool pp_Clamp, bool pp_Flip>
static inline always_inline int ClampFlip(int coord, int size) {
	if (pp_Clamp) { // clamp
		if (coord < 0) {
			coord = 0;
		} else if (coord >= size) {
			coord = size-1;
		}
	} else if (pp_Flip) { // flip
		coord &= size*2-1;
		if (coord & size) {
			coord ^= size*2-1;
		}
	} else { //wrap
		coord &= size-1;
	}

	return coord;
}


const u32 MipPoint[11] =
{
	0x00003,//1
	0x00001 * 4,//2
	0x00002 * 4,//4
	0x00006 * 4,//8
	0x00016 * 4,//16
	0x00056 * 4,//32
	0x00156 * 4,//64
	0x00556 * 4,//128
	0x01556 * 4,//256
	0x05556 * 4,//512
	0x15556 * 4//1024
};

static inline always_inline u32 ExpandToARGB8888(u32 color, u32 mode) {
	switch(mode)
	{
		case 0: return ARGB1555_32(color);
		case 1: return ARGB565_32(color);
		case 2: return ARGB4444_32(color);
		case 3: return ARGB8888_32(color);  // this one just shuffles
	}
	return 0xDEADBEEF;
}

template<bool VQ_Comp>
static inline always_inline u32 TexAddressGen(TCW tcw) {
	u32 base_address = tcw.TexAddr << 3;

	if (VQ_Comp) {
		base_address += 256 * 4 * 2;
	}

	return base_address;
}

template<bool VQ_Comp, bool MipMapped, bool ScanOrder>
static inline always_inline u32 TexOffsetGen(TSP tsp, int u, int v, u32 stride, u32 MipLevel) {
	u32 mip_offset;

	if (MipMapped) {
		mip_offset = MipPoint[3 + tsp.TexU - MipLevel];
	} else {
		mip_offset = 0;
	}

	if (VQ_Comp || !ScanOrder) {
		if (MipMapped) {
			return mip_offset + twop(u, v, (tsp.TexU - MipLevel), (tsp.TexU - MipLevel));
		} else {
			return mip_offset + twop(u, v, tsp.TexU, tsp.TexV);
		}
	} else {
		return mip_offset + u + stride * v;
	}
}

// 4.1 format
template<bool VQ_Comp, u8 PixelFmt>
constexpr u32 fBitsPerPixel() {
	u32 rv = 16;
	if (PixelFmt == PixelPal8) {
		rv = 8;
	}
	else if (PixelFmt == PixelPal4) {
		rv = 4;
	}
	else {
		rv = 16;
	}

	if (VQ_Comp) {
		return 8 * 2 / (64 / rv); // 8 bpp / (pixels per 64 bits)
	} else {
		return rv * 2;
	}
}

static inline always_inline u64 ReadVram64(u32 address) {
	u64 rv;
	memcpy(&rv, &cfg.vram[address & (cfg.vramMask - 7)], sizeof(rv));
	return rv;
}

static inline always_inline u64 VQLookup(u32 start_address, u64 memtel, u32 offset) {
	u8* memtel8 = (u8*)&memtel;

	u8 index = memtel8[offset & 7];
	return ReadVram64((start_address & ~7u) + index * 8);
}

template<u32 StrideSel, u32 ScanOrder>
static inline always_inline u32 TexStride(u32 TexU, u32 MipLevel) {
	if (StrideSel && ScanOrder)
		return (cfg.textControl&31)*32;
	else
		return (8U << TexU) >> MipLevel;
}

template<u32 PixelFmt, bool ScanOrder>
static inline always_inline u32 DecodeTextel(u32 PalSelect, u64 memtel, u32 offset) {
	auto memtel_16 = (u16*)&memtel;
	auto memtel_8 = (u8*)&memtel;

	switch (PixelFmt)
	{
		case PixelReserved:
		case Pixel1555:
		case Pixel565:
		case Pixel4444:
		case PixelBumpMap:
			return memtel_16[offset & 3]; break;

		case PixelYUV: {
			// PSFlyCast: a texel is Y in its high byte and, in its low byte, U (the
			// first of a pair) or V (the second). The pair is the next texel in
			// memory in scan order, and the next in x (two further) when twiddled.
			const u32 own = offset & 3;
			const u32 first = ScanOrder ? (own & 2) : (own & 1);
			const u32 second = ScanOrder ? first + 1 : first + 2;
			return YUV422(memtel_16[own] >> 8, memtel_16[first] & 255, memtel_16[second] & 255);
			}

		case PixelPal4: {
			auto local_idx = (memtel >> (offset & 15)*4) & 15;
			auto idx = PalSelect * 16 | local_idx;
			return cfg.palette[idx];
		}
		break;
		case PixelPal8: {
			auto local_idx = memtel_8[offset & 7];
			auto idx = (PalSelect / 16) * 256 | local_idx;
			return cfg.palette[idx];
		}
		break;
	}
	return 0xDEADBEEF;
}

template<u32 PixelFmt>
static inline always_inline u32 GetExpandFormat() {
	if (PixelFmt == PixelPal4 || PixelFmt == PixelPal8) {
		return cfg.palCtrl;
	} else if (PixelFmt == PixelBumpMap || PixelFmt == PixelYUV) {
		return 3;
	} else {
		return PixelFmt & 3;
	}
}

template<bool VQ_Comp, bool MipMapped, bool ScanOrder_, bool StrideSel_, u8 PixelFmt>
static inline always_inline Color FetchTextel(TSP tsp, TCW tcw, int u, int v, u32 MipLevel) {

	if (MipLevel == (tsp.TexU + 3)) {
		if (PixelFmt == PixelYUV) {
			return FetchTextel<VQ_Comp, MipMapped, ScanOrder_, StrideSel_, Pixel565>(tsp, tcw, u, v, MipLevel);
		}
	 }

	// These are fixed to zero for pal4/pal8
	constexpr u32 ScanOrder = ScanOrder_ && !(PixelFmt == PixelPal4 || PixelFmt == PixelPal8);
	constexpr u32 StrideSel = StrideSel_ && !(PixelFmt == PixelPal4 || PixelFmt == PixelPal8);

	u32 stride = TexStride<StrideSel, ScanOrder>(tsp.TexU, MipLevel);

	u32 start_address = tcw.TexAddr << 3;

	auto fbpp = fBitsPerPixel<VQ_Comp, PixelFmt>();

	auto base_address = TexAddressGen<VQ_Comp>(tcw);
	auto offset = TexOffsetGen<VQ_Comp, MipMapped, ScanOrder>(tsp, u, v, stride, MipLevel);

	u64 memtel = ReadVram64(base_address + offset * fbpp / 16);

	if (VQ_Comp) {
		memtel = VQLookup(start_address, memtel, offset * fbpp / 16);
	}

	u32 textel = DecodeTextel<PixelFmt, (VQ_Comp || !ScanOrder) ? false : true>(tcw.PalSelect, memtel, offset);

	u32 expand_format = GetExpandFormat<PixelFmt>();

	textel = ExpandToARGB8888(textel, expand_format);

	Color rv;
	rv.raw = textel;
	return rv;
}

template<bool VQ_Comp, bool MipMapped, bool ScanOrder_, bool StrideSel_, u8 PixelFmt>
static Color TextureFetch(TSP tsp, TCW tcw, int u, int v, u32 MipLevel) {
	return FetchTextel<VQ_Comp, MipMapped, ScanOrder_, StrideSel_, PixelFmt>(tsp, tcw, u, v, MipLevel);
}

// PSFlyCast: the four fetches of TextureFilter, in its order, as one function
template<bool VQ_Comp, bool MipMapped, bool ScanOrder_, bool StrideSel_, u8 PixelFmt>
static void TextureFetch4(TSP tsp, TCW tcw, int u0, int u1, int v0, int v1, u32 MipLevel, Color *textels) {
	textels[0] = FetchTextel<VQ_Comp, MipMapped, ScanOrder_, StrideSel_, PixelFmt>(tsp, tcw, u1, v1, MipLevel);
	textels[1] = FetchTextel<VQ_Comp, MipMapped, ScanOrder_, StrideSel_, PixelFmt>(tsp, tcw, u0, v1, MipLevel);
	textels[2] = FetchTextel<VQ_Comp, MipMapped, ScanOrder_, StrideSel_, PixelFmt>(tsp, tcw, u1, v0, MipLevel);
	textels[3] = FetchTextel<VQ_Comp, MipMapped, ScanOrder_, StrideSel_, PixelFmt>(tsp, tcw, u0, v0, MipLevel);
}

static inline u32 to_u8_256(u8 v) {
	return v + (v >> 7);
}

// PSFlyCast: a float to an int the way the x86 conversion does it, which is
// what the reference's casts come to there: towards zero, and the most
// negative int for what is not a number or does not fit.
static inline always_inline int FloatToInt(float v) {
#if defined(__SSE__)
	return _mm_cvttss_si32(_mm_set_ss(v));
#else
	if (!(v > -2147483648.f && v < 2147483648.f))
		return (int)0x80000000;
	return (int)v;
#endif
}

// Fetch pixels from UVs, interpolate
template<bool pp_IgnoreTexA,  bool pp_ClampU, bool pp_ClampV, bool pp_FlipU, bool pp_FlipV, u32 pp_FilterMode>
static Color TextureFilter(TSP tsp, TCW tcw, float u, float v, u32 MipLevel, float dTrilinear, const TextureFetchFuncs& fetch) {

	int halfpixel = cfg.texHalfOffset ? 0 : 127;

	if (MipLevel >= (tsp.TexU + 3)) {
		MipLevel = tsp.TexU+3;
	}
	int sizeU, sizeV;

	if (tcw.MipMapped) {
		sizeU = (8 << tsp.TexU) >> MipLevel;
		sizeV = (8 << tsp.TexU) >> MipLevel;
	} else {
		sizeU = 8 << tsp.TexU;
		sizeV = 8 << tsp.TexV;
	}

	// PSFlyCast: FloatToInt, for coordinates that are not numbers or too large for an int
	int ui = FloatToInt(u * sizeU * 256 + halfpixel);
	int vi = FloatToInt(v * sizeV * 256 + halfpixel);

	Color textel;

	if (pp_FilterMode == 0) {
		// Point sampling
		// PSFlyCast: only this texel is fetched
		textel = fetch.one(tsp, tcw, ClampFlip<pp_ClampU, pp_FlipU>((ui >> 8) + 0, sizeU), ClampFlip<pp_ClampV, pp_FlipV>((vi >> 8) + 0, sizeV), MipLevel);
	} else {
		// Bilinear filtering
		// PSFlyCast: also for trilinear filtering A and B, which the reference does not have
		// PSFlyCast: the four texels by one call
		Color textels[4];
		fetch.four(tsp, tcw,
				ClampFlip<pp_ClampU, pp_FlipU>((ui >> 8) + 0, sizeU), ClampFlip<pp_ClampU, pp_FlipU>((ui >> 8) + 1, sizeU),
				ClampFlip<pp_ClampV, pp_FlipV>((vi >> 8) + 0, sizeV), ClampFlip<pp_ClampV, pp_FlipV>((vi >> 8) + 1, sizeV), MipLevel, textels);
		const Color offset00 = textels[0];
		const Color offset01 = textels[1];
		const Color offset10 = textels[2];
		const Color offset11 = textels[3];

		int ublend = to_u8_256(ui & 255);
		int vblend = to_u8_256(vi & 255);
		int nublend = 256 - ublend;
		int nvblend = 256 - vblend;

#if defined(__SSE4_1__)
		// PSFlyCast: the four components at once (the sums are below 2^24)
		const __m128i sum = _mm_add_epi32(
				_mm_add_epi32(_mm_mullo_epi32(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)offset00.raw)), _mm_set1_epi32(ublend * vblend)),
						_mm_mullo_epi32(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)offset01.raw)), _mm_set1_epi32(nublend * vblend))),
				_mm_add_epi32(_mm_mullo_epi32(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)offset10.raw)), _mm_set1_epi32(ublend * nvblend)),
						_mm_mullo_epi32(_mm_cvtepu8_epi32(_mm_cvtsi32_si128((int)offset11.raw)), _mm_set1_epi32(nublend * nvblend))));
		textel.raw = (u32)_mm_cvtsi128_si32(_mm_shuffle_epi8(_mm_srli_epi32(sum, 16), _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)));
#else
		for (int i = 0; i < 4; i++)
		{
			textel.bgra[i] = (
				(offset00.bgra[i] * ublend * vblend) +
				(offset01.bgra[i] * nublend * vblend) +
				(offset10.bgra[i] * ublend * nvblend) +
				(offset11.bgra[i] * nublend * nvblend)
			) / 65536;
		};
#endif
	}

	if (pp_IgnoreTexA)
	{
		textel.a = 255;
	}

	return textel;
}

// Combine Base, Textel and Offset colors
template<bool pp_Texture, bool pp_Offset, u32 pp_ShadInstr>
static Color ColorCombiner(Color base, Color textel, Color offset) {

	Color rv = base;
	if (pp_Texture)
	{
		if (pp_ShadInstr == 0)
		{
			//color.rgb = texcol.rgb;
			//color.a = texcol.a;

			rv = textel;
		}
		else if (pp_ShadInstr == 1)
		{
			//color.rgb *= texcol.rgb;
			//color.a = texcol.a;
			for (int i = 0; i < 3; i++)
			{
				rv.bgra[i] = textel.bgra[i] * to_u8_256(base.bgra[i]) / 256;
			}

			rv.a = textel.a;
		}
		else if (pp_ShadInstr == 2)
		{
			//color.rgb=mix(color.rgb,texcol.rgb,texcol.a);
			u32 tb = to_u8_256(textel.a);
			u32 cb = 256 - tb;

			for (int i = 0; i < 3; i++)
			{
				rv.bgra[i] = (textel.bgra[i] * tb + base.bgra[i] * cb) / 256;
			}

			rv.a = base.a;
		}
		else if (pp_ShadInstr == 3)
		{
			//color*=texcol
			for (int i = 0; i < 4; i++)
			{
				rv.bgra[i] = textel.bgra[i] * to_u8_256(base.bgra[i]) / 256;
			}
		}

		if (pp_Offset) {
			// mix only color, saturate
			for (int i = 0; i < 3; i++)
			{
				rv.bgra[i] = std::min(rv.bgra[i] + offset.bgra[i], 255);
			}
		}
	}

	return rv;
}

static Color BumpMapper(Color textel, Color offset) {
	u8 K1 = offset.a;
	u8 K2 = offset.r;
	u8 K3 = offset.g;
	u8 Q = offset.b;

	u8 R = textel.b;
	u8 S = textel.g;

	s32 I = (K1*127*127 + K2*BM_SIN90[S]*127 + K3*BM_COS90[S]*BM_COS360[(R - Q) & 255])/127/127;
	if (I < 0) {
		I = 0;
	} else if (I > 255) {
		I = 255;
	}

	Color res;
	res.b = 255;
	res.g = 255;
	res.r = 255;
	res.a = I;

	return res;
}

// PSFlyCast: the reference's PlaneStepper3::IpU8 for the four components of a
// colour, then "0.5f + IpU8 * mult / 256" for each and the conversion to a
// byte. With SSE the four are computed at once: the same operations in the
// same order (a division by 256 and a multiplication by 1/256 round alike).
// multAlpha: the alpha component is scaled too.
static inline always_inline Color InterpolateColor(const TilePlane4& planes, float x, float y, float W, u32 mult, bool multAlpha) {
	Color rv;
#if defined(__SSE4_1__)
	__m128 v = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_set1_ps(x), _mm_load_ps(planes.ddx)), _mm_mul_ps(_mm_set1_ps(y), _mm_load_ps(planes.ddy))), _mm_load_ps(planes.c));
	v = _mm_mul_ps(v, _mm_set1_ps(W));
	// if (rv < 0) rv = 0; if (rv > 255) rv = 255; and what is not a number stays so
	v = _mm_max_ps(_mm_setzero_ps(), v);
	v = _mm_min_ps(_mm_set1_ps(255), v);
	const float m = (float)mult;
	const __m128 vmult = multAlpha ? _mm_set1_ps(m) : _mm_setr_ps(m, m, m, 256.f);
	__m128 scaled = _mm_mul_ps(_mm_mul_ps(v, vmult), _mm_set1_ps(1 / 256.f));
	if (!multAlpha)
		// the alpha is not scaled at all: 0.5f + IpU8
		scaled = _mm_blend_ps(scaled, v, 8);
	const __m128i bytes = _mm_cvttps_epi32(_mm_add_ps(_mm_set1_ps(0.5f), scaled));
	rv.raw = (u32)_mm_cvtsi128_si32(_mm_shuffle_epi8(bytes, _mm_setr_epi8(0, 4, 8, 12, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1)));
#else
	for (int i = 0; i < 4; i++)
	{
		float v = (x * planes.ddx[i] + y * planes.ddy[i] + planes.c[i]) * W;
		if (v < 0) v = 0;
		if (v > 255) v = 255;
		if (i < 3 || multAlpha)
			rv.bgra[i] = (u8)FloatToInt(0.5f + v * mult / 256);
		else
			rv.bgra[i] = (u8)FloatToInt(0.5f + v);
	}
#endif
	return rv;
}

// Interpolate the base color, also cheap shadows modifier
template<bool pp_UseAlpha, bool pp_CheapShadows>
static inline always_inline Color InterpolateBase(const TilePlane4& Col, float x, float y, float W, bool InVolume) {
	u32 mult = 256;

	if (pp_CheapShadows) {
		if (InVolume) {
			mult = to_u8_256(cfg.shadScale);
		}
	}

	Color rv = InterpolateColor(Col, x, y, W, mult, true);

	if (!pp_UseAlpha)
	{
		rv.a = 255;
	}

	return rv;
}

// Interpolate the offset color, also cheap shadows modifier
template<bool pp_CheapShadows>
static inline always_inline Color InterpolateOffs(const TilePlane4& Ofs, float x, float y, float W, bool InVolume) {
	u32 mult = 256;

	if (pp_CheapShadows) {
		if (InVolume) {
			mult = to_u8_256(cfg.shadScale);
		}
	}

	return InterpolateColor(Ofs, x, y, W, mult, false);
}

// select/calculate blend coefficient for the blend unit
template<u32 pp_AlphaInst, bool srcOther>
static inline always_inline Color BlendCoefs(Color src, Color dst) {
	Color rv;

	switch(pp_AlphaInst>>1) {
		// zero
		case 0: rv.raw = 0; break;
		// other color
		case 1: rv = srcOther? src : dst; break;
		// src alpha
		case 2: for (int i = 0; i < 4; i++) rv.bgra[i] = src.a; break;
		// dst alpha
		case 3: for (int i = 0; i < 4; i++) rv.bgra[i] = dst.a; break;
	}

	if (pp_AlphaInst & 1) {
		for (int i = 0; i < 4; i++)
			rv.bgra[i] = 255 - rv.bgra[i];
	}

	return rv;
}

// Blending Unit implementation. Alpha blend, accum buffers and such
template<u32 pp_SrcSel, u32 pp_DstSel, u32 pp_SrcInst, u32 pp_DstInst, bool pp_AlphaTest>
static bool BlendingUnit(TileState& s, u32 index, Color col)
{
	if (pp_AlphaTest) {
		if (col.a < cfg.ptAlphaRef) {
			// PSFlyCast: nothing is written (the reference blended the pixel with alpha 0)
			return false;
		} else {
			col.a = 255;
		}
	}

	Color rv;
	Color src;
	src.raw = pp_SrcSel ? s.colorBuffer2[index] : col.raw;
	Color dst;
	dst.raw = pp_DstSel ? s.colorBuffer2[index] : s.colorBuffer1[index];

	Color src_blend = BlendCoefs<pp_SrcInst, false>(src, dst);
	Color dst_blend = BlendCoefs<pp_DstInst, true>(src, dst);

	for (int j = 0; j < 4; j++)
	{
		rv.bgra[j] = std::min((src.bgra[j] * to_u8_256(src_blend.bgra[j]) + dst.bgra[j] * to_u8_256(dst_blend.bgra[j])) >> 8, 255U);
	}

	(pp_DstSel ? s.colorBuffer2[index] : s.colorBuffer1[index]) = rv.raw;

	return true;
}

static inline always_inline u8 LookupFogTable(float invW) {
	float fog_den = cfg.fogDensity;

	float fogW = fog_den * invW;

	fogW = std::max((float)fogW, 1.0f);
	fogW = std::min((float)fogW, 255.999985f);

	union f32_fields {
		float full;
		struct {
			u32 m: 23;
			u32 e: 8;
			u32 s: 1;
		};
	};

	f32_fields fog_fields = { fogW };

	u32 index = (((fog_fields.e +1) & 7) << 4) |  ((fog_fields.m>>19) & 15);

	u8 blendFactor = (fog_fields.m>>11) & 255;
	u8 blend_inv = 255^blendFactor;

	auto fog_entry = (const u8*)&cfg.fogTable[index];

	u8 fog_alpha = (fog_entry[0] * to_u8_256(blendFactor) + fog_entry[1] * to_u8_256(blend_inv)) >> 8;

	return fog_alpha;
}

// Color Clamp and Fog a pixel
template<bool pp_Offset, bool pp_ColorClamp, u32 pp_FogCtrl>
static inline always_inline Color FogUnit(Color col, float invW, u8 offs_a) {
	if (pp_ColorClamp) {
		Color clamp_max = { cfg.fogClampMax };
		Color clamp_min = { cfg.fogClampMin };

		for (int i = 0; i < 4; i++)
		{
			col.bgra[i] = std::min(col.bgra[i], clamp_max.bgra[i]);
			col.bgra[i] = std::max(col.bgra[i], clamp_min.bgra[i]);
		}
	}

	switch(pp_FogCtrl) {
		// Look up mode 1
		case 0b00:
		// look up mode 2
		case 0b11:
			{
				u8 fog_alpha = LookupFogTable(invW);

				u8 fog_inv = 255^fog_alpha;

				Color col_ram = { cfg.fogColRam };

				if (pp_FogCtrl == 0b00) {
					for (int i = 0; i < 3; i++) {
						col.bgra[i] = (col.bgra[i] * to_u8_256(fog_inv) + col_ram.bgra[i] * to_u8_256(fog_alpha))>>8;
					}
				} else {
					for (int i = 0; i < 3; i++) {
						col.bgra[i] = col_ram.bgra[i];
					}
					col.a = fog_alpha;
				}
			}
			break;

		// Per Vertex
		case 0b01:
			if (pp_Offset) {
				Color col_vert = { cfg.fogColVert };
				u8 alpha = offs_a;
				u8 inv = 255^alpha;

				for (int i = 0; i < 3; i++)
				{
					col.bgra[i] = (col.bgra[i] * to_u8_256(inv) + col_vert.bgra[i] * to_u8_256(alpha))>>8;
				}
			}
			break;


		// No Fog
		case 0b10:
			break;
	}

	return col;
}

// Implement the full texture/shade pipeline for a pixel
template<bool pp_UseAlpha, bool pp_Texture, bool pp_Offset, bool pp_ColorClamp, u32 pp_FogCtrl, bool pp_CheapShadows>
static bool PixelFlush_tsp(TileState& s, const TileEntry *entry, float x, float y, float W, bool InVolume, u32 two_voume_index, u32 index,
		const TextureFetchFuncs& fetch, TextureFilter_fp filter, ColorCombiner_fp combiner, BlendingUnit_fp blending)
{
	const DrawParameters& params = entry->poly->params;
	Color base = { 0 }, textel = { 0 };

	base = InterpolateBase<pp_UseAlpha, pp_CheapShadows>(entry->Col[two_voume_index], x, y, W, InVolume);

	float dTrilinear;
	u32 MipLevel;
	if (pp_Texture) {
		float u = entry->U[two_voume_index].Ip(x, y, W);
		float v = entry->V[two_voume_index].Ip(x, y, W);

		if (params.tcw[two_voume_index].MipMapped) {
			int sizeU = 8 << params.tsp[two_voume_index].TexU;
			// faux mip map cals
			// these really don't follow hw
			float ddx = (entry->U[two_voume_index].ddx + entry->V[two_voume_index].ddx);
			float ddy = (entry->U[two_voume_index].ddy + entry->V[two_voume_index].ddy);

			float dMip = fminf(fabsf(ddx), fabsf(ddy)) * W * sizeU * params.tsp[two_voume_index].MipMapD / 4.0f;

			MipLevel = 0; // biggest
			while(dMip > 1.5 && MipLevel < 11) {
				MipLevel ++;
				dMip = dMip / 2;
			}
			dTrilinear = dMip;
		} else {
			dTrilinear = 0;
			MipLevel = 0;
		}

		textel = filter(params.tsp[two_voume_index], params.tcw[two_voume_index], u, v, MipLevel, dTrilinear, fetch);
		if (pp_Offset) {
			s.offs = InterpolateOffs<pp_CheapShadows>(entry->Ofs[two_voume_index], x, y, W, InVolume);
		}
	}

	Color col;
	if (pp_Texture && params.tcw[two_voume_index].PixelFmt == PixelBumpMap) {
		col = BumpMapper(textel, s.offs);
	} else {
		col = combiner(base, textel, s.offs);
	}

	col = FogUnit<pp_Offset, pp_ColorClamp, pp_FogCtrl>(col, 1/W, s.offs.a);

	return blending(s, index, col);
}

// The reference's gentable.h: a table of every instantiation, indexed by the
// template parameters as the digits of one number (the first is the most significant).
template<size_t... I>
static constexpr std::array<PixelFlush_tsp_fp, sizeof...(I)> MakePixelFlushTable(std::index_sequence<I...>) {
	// [UseAlpha][Texture][Offset][ColorClamp][FogCtrl][intensity_shadow]
	return {{ &PixelFlush_tsp<((I >> 6) & 1) != 0, ((I >> 5) & 1) != 0, ((I >> 4) & 1) != 0, ((I >> 3) & 1) != 0, (u32)((I >> 1) & 3), (I & 1) != 0>... }};
}
static const auto PixelFlush_tsp_table = MakePixelFlushTable(std::make_index_sequence<128>{});

template<size_t... I>
static constexpr std::array<TextureFilter_fp, sizeof...(I)> MakeTextureFilterTable(std::index_sequence<I...>) {
	// [IgnoreTexA][ClampU][ClampV][FlipU][FlipV][FilterMode]
	return {{ &TextureFilter<((I >> 6) & 1) != 0, ((I >> 5) & 1) != 0, ((I >> 4) & 1) != 0, ((I >> 3) & 1) != 0, ((I >> 2) & 1) != 0, (u32)(I & 3)>... }};
}
static const auto TextureFilter_table = MakeTextureFilterTable(std::make_index_sequence<128>{});

template<size_t... I>
static constexpr std::array<ColorCombiner_fp, sizeof...(I)> MakeColorCombinerTable(std::index_sequence<I...>) {
	// [Texture][Offset][ShadInstr]
	return {{ &ColorCombiner<((I >> 3) & 1) != 0, ((I >> 2) & 1) != 0, (u32)(I & 3)>... }};
}
static const auto ColorCombiner_table = MakeColorCombinerTable(std::make_index_sequence<16>{});

template<size_t... I>
static constexpr std::array<BlendingUnit_fp, sizeof...(I)> MakeBlendingUnitTable(std::index_sequence<I...>) {
	// [SrcSelect][DstSelect][SrcInstr][DstInstr][AlphaTest]
	return {{ &BlendingUnit<(u32)((I >> 8) & 1), (u32)((I >> 7) & 1), (u32)((I >> 4) & 7), (u32)((I >> 1) & 7), (I & 1) != 0>... }};
}
static const auto BlendingUnit_table = MakeBlendingUnitTable(std::make_index_sequence<512>{});

template<size_t... I>
static constexpr std::array<TextureFetch_fp, sizeof...(I)> MakeTextureFetchTable(std::index_sequence<I...>) {
	// [VQ_Comp][MipMapped][ScanOrder][StrideSel][PixelFmt]
	return {{ &TextureFetch<((I >> 6) & 1) != 0, ((I >> 5) & 1) != 0, ((I >> 4) & 1) != 0, ((I >> 3) & 1) != 0, (u8)(I & 7)>... }};
}
static const auto TextureFetch_table = MakeTextureFetchTable(std::make_index_sequence<128>{});

template<size_t... I>
static constexpr std::array<TextureFetch4_fp, sizeof...(I)> MakeTextureFetch4Table(std::index_sequence<I...>) {
	return {{ &TextureFetch4<((I >> 6) & 1) != 0, ((I >> 5) & 1) != 0, ((I >> 4) & 1) != 0, ((I >> 3) & 1) != 0, (u8)(I & 7)>... }};
}
static const auto TextureFetch4_table = MakeTextureFetch4Table(std::make_index_sequence<128>{});

// PSFlyCast: what the reference's PixelFlush_tsp looked up for every pixel
void SelectShadeFuncs(PolyState& poly)
{
	const DrawParameters& params = poly.params;
	for (u32 volume = 0; volume < 2; volume++)
	{
		const u32 v = poly.twoVolumes ? volume : 0;
		const TSP tsp = params.tsp[v];
		const TCW tcw = params.tcw[v];
		ShadeFuncs& funcs = poly.funcs[volume];

		const u32 fetch = (tcw.VQ_Comp << 6) | (tcw.MipMapped << 5) | (tcw.ScanOrder << 4) | (tcw.StrideSel << 3) | tcw.PixelFmt;
		funcs.fetch.one = TextureFetch_table[fetch];
		funcs.fetch.four = TextureFetch4_table[fetch];
		funcs.filter = TextureFilter_table[(tsp.IgnoreTexA << 6) | (tsp.ClampU << 5) | (tsp.ClampV << 4) | (tsp.FlipU << 3) | (tsp.FlipV << 2) | tsp.FilterMode];
		funcs.combiner = ColorCombiner_table[(params.isp.Texture << 3) | (params.isp.Offset << 2) | tsp.ShadInstr];
		for (u32 alphaTest = 0; alphaTest < 2; alphaTest++)
			funcs.blending[alphaTest] = BlendingUnit_table[(tsp.SrcSelect << 8) | (tsp.DstSelect << 7) | (tsp.SrcInstr << 4) | (tsp.DstInstr << 1) | alphaTest];
		funcs.pixel = PixelFlush_tsp_table[(tsp.UseAlpha << 6) | (params.isp.Texture << 5) | (params.isp.Offset << 4) | (tsp.ColorClamp << 3) | (tsp.FogCtrl << 1)
				| (cfg.intensityShadow ? 1 : 0)];
	}
}

// Lookup/create cached TSP parameters, and call PixelFlush_tsp
static inline always_inline bool PixelFlush_tsp(TileState& s, bool pp_AlphaTest, const TileEntry* entry, float x, float y, u32 index, float invW, bool InVolume)
{
	// PSFlyCast: the second set of parameters, where the polygon has one
	// (the reference: InVolume & !FPU_SHAD_SCALE.intensity_shadow)
	u32 two_voume_index = InVolume && entry->poly->twoVolumes;
	const ShadeFuncs& funcs = entry->poly->funcs[two_voume_index];

	return funcs.pixel(s, entry, x, y, 1/invW, InVolume, two_voume_index, index, funcs.fetch, funcs.filter, funcs.combiner, funcs.blending[pp_AlphaTest]);
}

// The reference's IPs3::Setup. The colours of a Flycast vertex are R, G, B, A
// in memory; the planes are B, G, R, A as the reference's (and the Color union).
void SetupInterpolation(IPs3& ips, const DrawParameters& params, const VertexIn& v1, const VertexIn& v2, const VertexIn& v3, bool TwoVolumes)
{
	static const int channel[4] = { 2, 1, 0, 3 };

	ips.invW.Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.z, v2.z, v3.z);
	const int volumes = TwoVolumes ? 2 : 1;
	for (int v = 0; v < volumes; v++)
	{
		if (params.isp.Texture)
		{
			ips.U[v].Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.u[v] * v1.z, v2.u[v] * v2.z, v3.u[v] * v3.z);
			ips.V[v].Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.v[v] * v1.z, v2.v[v] * v2.z, v3.v[v] * v3.z);
		}
		const bool offset = params.isp.Texture && params.isp.Offset;
		if (params.isp.Gouraud) {
			for (int i = 0; i < 4; i++)
				ips.Col[v][i].Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.col[v][channel[i]] * v1.z, v2.col[v][channel[i]] * v2.z, v3.col[v][channel[i]] * v3.z);

			if (offset)
				for (int i = 0; i < 4; i++)
					ips.Ofs[v][i].Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v1.spc[v][channel[i]] * v1.z, v2.spc[v][channel[i]] * v2.z, v3.spc[v][channel[i]] * v3.z);
		} else {
			for (int i = 0; i < 4; i++)
				ips.Col[v][i].Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v3.col[v][channel[i]] * v1.z, v3.col[v][channel[i]] * v2.z, v3.col[v][channel[i]] * v3.z);

			if (offset)
				for (int i = 0; i < 4; i++)
					ips.Ofs[v][i].Setup(v1.x, v1.y, v2.x, v2.y, v3.x, v3.y, v3.spc[v][channel[i]] * v1.z, v3.spc[v][channel[i]] * v2.z, v3.spc[v][channel[i]] * v3.z);
		}
	}
}

/*
	Derived from refsw_lists.cc of the same reference (RenderTriangle,
	RenderObjectList and the body of RenderCORE's loop over the region array).
*/

// Render an object list
// PSFlyCast: a list of tags; the reference followed the object list in VRAM
template<RenderMode render_mode>
static void RenderObjectList(TileState& s, const TileList& list)
{
	const Triangle *tris = s.frame->tris.data();

	for (u32 i = 0; i < list.count; i++)
	{
		const parameter_tag_t tag = list.tags[i];
		const Triangle& tri = tris[tag];
		const TileRect rect = RasterizeTriangle<render_mode>(s, tri, tag);

		if (render_mode == RM_TRANSLUCENT_PRESORT) {
			// PSFlyCast: only where the triangle is (nothing else has a valid tag)
			if (!rect.empty())
				RenderParamTags<RM_TRANSLUCENT_PRESORT>(s, rect.x0, rect.y0, rect.x1, rect.y1);
		}

		if (render_mode == RM_MODIFIER)
		{
			// 0 normal polygon, 1 inside last, 2 outside last
			if (tri.volumeOp == 1)
			{
				SummarizeStencilOr(s);
			}
			else if (tri.volumeOp == 2)
			{
				SummarizeStencilAnd(s);
			}
		}
	}
}

// PSFlyCast: the time of one stage of a tile, when it is measured
struct StageTimer
{
	StageTimer(TileState& s, int stage) : s(s), stage(stage) {
		if (s.profile)
			start = ThreadTimeNs();
	}
	~StageTimer() {
		if (s.profile)
			s.profileNs[stage] += ThreadTimeNs() - start;
	}
	TileState& s;
	int stage;
	u64 start = 0;
};

template<RenderMode render_mode>
static void RenderObjectList(TileState& s, const TileList& list, int stage)
{
	StageTimer timer(s, stage);
	RenderObjectList<render_mode>(s, list);
}

template<RenderMode rm>
static void RenderParamTags(TileState& s, int stage)
{
	StageTimer timer(s, stage);
	RenderParamTags<rm>(s);
}

void BeginTile(TileState& s, const Frame& frame, int tileX, int tileY)
{
	s.frame = &frame;
	s.left = tileX;
	s.top = tileY;
	s.MoreToDraw = false;
	s.offs.raw = 0x20004080;	// Default value was randomly chosen.
	memset(s.colorBuffer1, 0, sizeof(s.colorBuffer1));
	memset(s.colorBuffer2, 0, sizeof(s.colorBuffer2));
	// register BGPOLY to fpu
	ClearFpuCache(s);
}

// One entry of the region array
void RenderTilePass(TileState& s, const TilePass& entry)
{
	// Tile needs clear?
	if (!entry.zKeep)
	{
		// Clear Param + Z + stencil buffers
		// PSFlyCast: tag 0 is the background plane
		ClearBuffers(s, 0, cfg.backgroundDepth, 0);
	} else {
		ClearParamStatusBuffer(s);
	}

	// Render OPAQ to TAGS
	if (!entry.opaque.empty())
	{
		RenderObjectList<RM_OPAQUE>(s, entry.opaque, PROFILE_ISP_OPAQUE);

		if (!entry.opaque_mod.empty())
		{
			RenderObjectList<RM_MODIFIER>(s, entry.opaque_mod, PROFILE_ISP_MODIFIER);
		}
	}

	// Render TAGS to ACCUM
	RenderParamTags<RM_OPAQUE>(s, PROFILE_TSP_OPAQUE);

	// render PT to TAGS
	if (!entry.puncht.empty())
	{
		PeelBuffersPTInitial(s);

		s.MoreToDraw = false;

		// Render to TAGS
		RenderObjectList<RM_PUNCHTHROUGH_PASS0>(s, entry.puncht, PROFILE_ISP_PUNCHTHROUGH);

		// keep reference Z buffer
		PeelBuffersPT(s);

		// Render TAGS to ACCUM, making Z holes as-needed
		RenderParamTags<RM_PUNCHTHROUGH_PASS0>(s, PROFILE_TSP_PUNCHTHROUGH);

		// PSFlyCast: each pass draws one more layer of a pixel, and a pixel has no
		// more layers than the list has triangles. Without the limit a depth that
		// is not a number (from coordinates beyond what a float can multiply)
		// would have the loop go on for ever.
		u32 passesLeft = std::min<u32>(entry.puncht.count, MAX_PEEL_PASSES);
		while (s.MoreToDraw && passesLeft-- != 0) {
			s.MoreToDraw = false;
			s.peelPasses++;

			// Render to TAGS
			RenderObjectList<RM_PUNCHTHROUGH_PASSN>(s, entry.puncht, PROFILE_ISP_PUNCHTHROUGH);

			if (!s.MoreToDraw)
				break;

			s.MoreToDraw = false;
			// keep reference Z buffer
			PeelBuffersPT(s);

			// Render TAGS to ACCUM, making Z holes as-needed
			RenderParamTags<RM_PUNCHTHROUGH_PASS0>(s, PROFILE_TSP_PUNCHTHROUGH);
		}
		if (!entry.opaque_mod.empty())
		{
			RenderObjectList<RM_MODIFIER>(s, entry.opaque_mod, PROFILE_ISP_MODIFIER);
			RenderParamTags<RM_PUNCHTHROUGH_MV>(s, PROFILE_TSP_PUNCHTHROUGH);
		}
	}

	// layer peeling rendering
	if (!entry.trans.empty())
	{
		if (entry.preSort) {
			// clear the param buffer
			ClearParamStatusBuffer(s);

			// render to TAGS
			{
				RenderObjectList<RM_TRANSLUCENT_PRESORT>(s, entry.trans, PROFILE_PRESORT);
			}

			// what happens with modvols here?
		} else {
			// PSFlyCast: see RM_DEPTH_ONLY
			if (entry.keepDepth)
				memcpy(s.depthSave, s.depthBuffer[depthBufferA], sizeof(s.depthSave));

			SetTagToMax(s);
			// PSFlyCast: as for the punch-through list
			u32 passesLeft = std::min<u32>(entry.trans.count + 1, MAX_PEEL_PASSES);
			do
			{
				// prepare for a new pass
				s.MoreToDraw = false;
				s.peelPasses++;

				// copy depth test to depth reference buffer, clear depth test buffer, clear stencil
				PeelBuffers(s, FLT_MAX, 0);

				// render to TAGS
				{
					RenderObjectList<RM_TRANSLUCENT_AUTOSORT>(s, entry.trans, PROFILE_ISP_TRANSLUCENT);
				}

				if (!entry.trans_mod.empty())
				{
					RenderObjectList<RM_MODIFIER>(s, entry.trans_mod, PROFILE_ISP_MODIFIER);
				}

				// render TAGS to ACCUM
				RenderParamTags<RM_TRANSLUCENT_AUTOSORT>(s, PROFILE_TSP_TRANSLUCENT);
			} while (s.MoreToDraw != 0 && --passesLeft != 0);

			if (entry.keepDepth)
			{
				memcpy(s.depthBuffer[depthBufferA], s.depthSave, sizeof(s.depthSave));
				RenderObjectList<RM_DEPTH_ONLY>(s, entry.trans, PROFILE_ISP_TRANSLUCENT);
			}
		}
	}
}

/*
	The tile writeout of RenderCORE.
	PSFlyCast: every pack mode of FB_W_CTRL (the reference has 565 and 8888),
	dithering only when FB_W_CTRL asks for it (the reference always dithers
	565) and with the same thresholds for every 16-bit mode, the pixel clip of
	FB_X_CLIP and FB_Y_CLIP, and the 64-bit area for a render to a texture.
*/

// Precomputed "threshold biases" = bias4[bayer4[i][j]]
static constexpr u8 bayerBias[4][4] = {
	{   8, 136,  40, 168 },  // 0->8, 8->136, 2->40, 10->168
	{ 200,  72, 232, 104 },  //12->200,4->72, 14->232,6->104
	{  56, 184,  24, 152 },  // 3->56,11->184,1->24, 9->152
	{ 248, 120, 216,  88 }   //15->248,7->120,13->216,5->88
};

// The 32-bit area: the 64-bit wide bus is achieved by interleaving the banks every 32 bits
static inline u32 map32(const WriteOut& out, u32 offset32)
{
	const u32 bank_bit = 0x400000;
	const u32 static_bits = (out.vramMask - (bank_bit * 2 - 1)) | 3;
	const u32 offset_bits = (bank_bit - 1) & ~3;

	u32 bank = (offset32 & bank_bit) / bank_bit;

	u32 rv = offset32 & static_bits;

	rv |= (offset32 & offset_bits) * 2;

	rv |= bank * 4;

	return rv;
}

static inline u8 *pixelAddress(const WriteOut& out, u32 address)
{
	if (out.area64)
		return &out.vram[address & out.vramMask];
	else
		return &out.vram[map32(out, address) & out.vramMask];
}

// One colour component to bits bits
static inline int quantize(int c8, int max, int shift, bool dither, int T)
{
	if (!dither)
		return c8 >> shift;
	// integer quantize exactly as before
	int c = (c8 * max + T) / 255;
	// clamp (just in case)
	if (c < 0) c = 0; else if (c > max) c = max;
	return c;
}

void WritePixels(const WriteOut& out, int x, int y, const u32 *pixels, int count)
{
	if (y < out.clipY0 || y > out.clipY1)
		return;
	if (x < out.clipX0) {
		pixels += out.clipX0 - x;
		count -= out.clipX0 - x;
		x = out.clipX0;
	}
	if (x + count - 1 > out.clipX1)
		count = out.clipX1 - x + 1;
	if (count <= 0)
		return;

	const u32 fb_packmode = out.packmode;
	const u32 bpp = fb_packmode <= 3 ? 2 : fb_packmode == 4 ? 3 : 4;
	u32 dst = out.base + y * out.stride + x * bpp;
	const u8 *src = (const u8 *)pixels;
	const bool dither = out.dither;

	for (int i = 0; i < count; i++, x++)
	{
		const int b8 = src[0];
		const int g8 = src[1];
		const int r8 = src[2];
		const int a8 = src[3];
		const int T = bayerBias[y & 3][x & 3];

		switch (fb_packmode)
		{
		case 0: // 0555 KRGB 16 bit: bit 15 is the value of fb_kval[7]
			{
				u16 pixel = (quantize(b8, 31, 3, dither, T) << 0) | (quantize(g8, 31, 3, dither, T) << 5) | (quantize(r8, 31, 3, dither, T) << 10)
						| ((out.kval & 0x80) << 8);
				memcpy(pixelAddress(out, dst), &pixel, 2);
			}
			break;
		case 1: // 565 RGB 16 bit
			{
				u16 pixel = (quantize(b8, 31, 3, dither, T) << 0) | (quantize(g8, 63, 2, dither, T) << 5) | (quantize(r8, 31, 3, dither, T) << 11);
				memcpy(pixelAddress(out, dst), &pixel, 2);
			}
			break;
		case 2: // 4444 ARGB 16 bit
			{
				u16 pixel = (quantize(b8, 15, 4, dither, T) << 0) | (quantize(g8, 15, 4, dither, T) << 4) | (quantize(r8, 15, 4, dither, T) << 8)
						| (quantize(a8, 15, 4, dither, T) << 12);
				memcpy(pixelAddress(out, dst), &pixel, 2);
			}
			break;
		case 3: // 1555 ARGB 16 bit: alpha by comparison with fb_alpha_threshold
			{
				u16 pixel = (quantize(b8, 31, 3, dither, T) << 0) | (quantize(g8, 31, 3, dither, T) << 5) | (quantize(r8, 31, 3, dither, T) << 10)
						| ((u32)a8 >= out.alphaThreshold ? 0x8000 : 0);
				memcpy(pixelAddress(out, dst), &pixel, 2);
			}
			break;
		case 4: // 888 RGB 24 bit packed
			*pixelAddress(out, dst) = b8;
			*pixelAddress(out, dst + 1) = g8;
			*pixelAddress(out, dst + 2) = r8;
			break;
		case 5: // 0888 KRGB 32 bit
			{
				u32 pixel = b8 | (g8 << 8) | (r8 << 16) | (out.kval << 24);
				memcpy(pixelAddress(out, dst), &pixel, 4);
			}
			break;
		default: // 8888 ARGB 32 bit
			{
				u32 pixel = b8 | (g8 << 8) | (r8 << 16) | ((u32)a8 << 24);
				memcpy(pixelAddress(out, dst), &pixel, 4);
			}
			break;
		}

		dst += bpp;
		src += 4;
	}
}

}
