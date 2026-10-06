/*
	REFSW: reference-style software model of the PowerVR CLX2 "CORE"
	(ISP, TSP, tile buffers), by Stefanos Kornilios Mitsis Poiitidis (skmp).

	Derived from refsw_tile.h, refsw_lists.h and core_structs.h of
	nullDC-rust (crates/refsw2-cpp/ffi), which say:

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

	PSFlyCast changes (2026, the PSFlyCast contributors):
	- everything is in namespace refsw; the tile buffers, the "more to draw"
	  flag and the persistent offset colour are a TileState, one per worker
	  thread, instead of globals;
	- a tag is an index into a per-frame array of triangles prepared from
	  Flycast's rend_context instead of an address in the parameter buffer in
	  VRAM, and the parameters and the interpolation set-up come from that
	  array (Frame) instead of being decoded from VRAM;
	- a surface equation keeps its value at the first vertex, so that its
	  constant can be derived for any tile with the reference's arithmetic;
	- the registers the reference reads while it renders are a snapshot
	  (FrameConfig) taken once a frame.
*/
#pragma once
#include "types.h"
#include "hw/pvr/ta_structs.h"

#include <chrono>
#include <ctime>
#include <vector>

namespace refsw
{

// PSFlyCast: the processor time this thread has used, for the measurements
// (the time of day where the system has no such clock).
static inline u64 ThreadTimeNs()
{
#if defined(CLOCK_THREAD_CPUTIME_ID)
	timespec ts;
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
		return (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr int MAX_RENDER_WIDTH = 32;
constexpr int MAX_RENDER_HEIGHT = 32;
constexpr int MAX_RENDER_PIXELS = MAX_RENDER_WIDTH * MAX_RENDER_HEIGHT;

typedef float ZType;
typedef u8 StencilType;
typedef u32 parameter_tag_t;

constexpr u32 PARAMETER_TAG_SORT_MASK = 0x00FFFFFF;
// The most triangles a frame may hold: a tag's sort key is 24 bits.
constexpr u32 MAX_FRAME_TRIANGLES = PARAMETER_TAG_SORT_MASK;

enum RenderMode {
	RM_OPAQUE,
	RM_PUNCHTHROUGH_PASS0,
	RM_PUNCHTHROUGH_PASSN,
	RM_PUNCHTHROUGH_MV, // PT MODVOL 2nd pass
	RM_TRANSLUCENT_AUTOSORT,
	RM_TRANSLUCENT_PRESORT,
	RM_MODIFIER,
	// PSFlyCast: depth of an auto-sorted translucent list, for the pass that follows it
	RM_DEPTH_ONLY,
};

// TagState of the reference, as two bits
constexpr u8 TAG_VALID = 1;
constexpr u8 TAG_RENDERED = 2;

union Color {
	u32 raw;
	u8 bgra[4];
	struct {
		u8 b;
		u8 g;
		u8 r;
		u8 a;
	};
};

struct DrawParameters
{
	ISP_TSP isp;
	TSP tsp[2];
	TCW tcw[2];
};

/*
	Surface equation solver
*/
struct PlaneStepper3
{
	float ddx, ddy;
	float a1;	// PSFlyCast: the value at the first vertex

	void Setup(float x1, float y1, float x2, float y2, float x3, float y3, float v1_a, float v2_a, float v3_a)
	{
		float Aa = ((v3_a - v1_a) * (y2 - y1) - (v2_a - v1_a) * (y3 - y1));
		float Ba = ((x3 - x1) * (v2_a - v1_a) - (x2 - x1) * (v3_a - v1_a));

		float C = ((x2 - x1) * (y3 - y1) - (x3 - x1) * (y2 - y1));

		if (C == 0) {
			C = 1; // avoid divide by zero
		}

		ddx = -Aa / C;
		ddy = -Ba / C;
		a1 = v1_a;
	}

	// The constant for a tile: dx and dy are the first vertex's position in it.
	float C(float dx, float dy) const
	{
		return a1 - ddx * dx - ddy * dy;
	}
};

// A surface equation in one tile
struct TilePlane
{
	float ddx, ddy;
	float c;

	void Set(const PlaneStepper3& p, float dx, float dy)
	{
		ddx = p.ddx;
		ddy = p.ddy;
		c = p.C(dx, dy);
	}

	float Ip(float x, float y) const
	{
		return x * ddx + y * ddy + c;
	}

	float Ip(float x, float y, float W) const
	{
		return Ip(x, y) * W;
	}

	float IpU8(float x, float y, float W) const
	{
		float rv = Ip(x, y, W);

		if (rv < 0) rv = 0;
		if (rv > 255) rv = 255;

		return rv;
	}
};

// PSFlyCast: the four surface equations of a colour (B, G, R, A) in one tile,
// laid out for computing the four at once
struct alignas(16) TilePlane4
{
	float ddx[4];
	float ddy[4];
	float c[4];

	void Set(int i, const PlaneStepper3& p, float dx, float dy)
	{
		ddx[i] = p.ddx;
		ddy[i] = p.ddy;
		c[i] = p.C(dx, dy);
	}
};

/*
	Interpolation helper
*/
struct IPs3
{
	PlaneStepper3 invW;
	PlaneStepper3 U[2];
	PlaneStepper3 V[2];
	PlaneStepper3 Col[2][4];
	PlaneStepper3 Ofs[2][4];
};

struct TileState;
struct TileEntry;

typedef Color (*TextureFetch_fp)(TSP tsp, TCW tcw, int u, int v, u32 MipLevel);
// PSFlyCast: the four texels of a bilinear sample in one call: (u1,v1) (u0,v1) (u1,v0) (u0,v0)
typedef void (*TextureFetch4_fp)(TSP tsp, TCW tcw, int u0, int u1, int v0, int v1, u32 MipLevel, Color *textels);
// What fetches a polygon's texels
struct TextureFetchFuncs
{
	TextureFetch_fp one;
	TextureFetch4_fp four;
};
typedef Color (*TextureFilter_fp)(TSP tsp, TCW tcw, float u, float v, u32 MipLevel, float dTrilinear, const TextureFetchFuncs& fetch);
typedef Color (*ColorCombiner_fp)(Color base, Color textel, Color offset);
typedef bool (*BlendingUnit_fp)(TileState& state, u32 index, Color col);
typedef bool (*PixelFlush_tsp_fp)(TileState& state, const TileEntry *entry, float x, float y, float W, bool InVolume, u32 volume, u32 index,
		const TextureFetchFuncs& fetch, TextureFilter_fp filter, ColorCombiner_fp combiner, BlendingUnit_fp blending);

// What the reference looks up in its tables for every pixel, looked up once for a polygon
struct ShadeFuncs
{
	TextureFetchFuncs fetch;
	TextureFilter_fp filter;
	ColorCombiner_fp combiner;
	BlendingUnit_fp blending[2];	// [alpha test]
	PixelFlush_tsp_fp pixel;
};

// The parameters of an object (a Flycast PolyParam)
struct PolyState
{
	DrawParameters params;
	bool shadow;		// the object's "shadow" bit: modifier volumes affect it
	bool twoVolumes;	// it has a second set of parameters for inside them
	ShadeFuncs funcs[2];	// [volume]
};

// A prepared triangle: what the ISP needs. tris[tag] and ips[tag] are one triangle.
struct Triangle
{
	float x1, y1, x2, y2, x3, y3;
	float DX12, DX23, DX31;
	float DY12, DY23, DY31;
	PlaneStepper3 Z;
	u32 poly;			// index of its PolyState
	u8 topLeft;			// IsTopLeft of its three edges: bits 0 to 2
	u8 depthMode;
	u8 zWriteDis;
	u8 volumeOp;		// modifier volume triangles: 1 inside last, 2 outside last
	float minX, maxX, minY, maxY;
};

// The registers and memories the reference reads while rendering, as they were when the frame started
struct FrameConfig
{
	u8 *vram;
	u32 vramMask;

	u32 palette[1024];		// PALETTE_RAM
	u32 palCtrl;			// PAL_RAM_CTRL & 3
	u32 textControl;		// TEXT_CONTROL
	u32 fogTable[128];		// FOG_TABLE
	float fogDensity;		// FOG_DENSITY, as a number
	u32 fogColRam;
	u32 fogColVert;
	u32 fogClampMax;
	u32 fogClampMin;
	u32 ptAlphaRef;
	u32 shadScale;			// FPU_SHAD_SCALE.scale_factor
	bool intensityShadow;	// FPU_SHAD_SCALE.intensity_shadow
	float cullVal;			// FPU_CULL_VAL
	bool fpuHalfOffset;		// HALF_OFFSET
	bool tspHalfOffset;
	bool texHalfOffset;
	float backgroundDepth;	// ISP_BACKGND_D
};
extern FrameConfig cfg;

// One list of a tile: tags, in the order they were submitted
struct TileList
{
	const u32 *tags = nullptr;
	u32 count = 0;

	bool empty() const { return count == 0; }
};

// A region array entry: one pass of one tile
struct TilePass
{
	bool zKeep;
	bool preSort;
	bool keepDepth;		// PSFlyCast: the next pass of this tile keeps the depth buffer
	TileList opaque;
	TileList opaque_mod;
	TileList trans;
	TileList trans_mod;
	TileList puncht;
};

struct Frame
{
	std::vector<Triangle> tris;		// tag 0 is the background plane
	std::vector<IPs3> ips;
	std::vector<PolyState> polys;
};

// A triangle's surface equations in the tile being rendered (the reference's FpuEntry)
struct TileEntry
{
	u32 tag;
	const PolyState *poly;
	TilePlane invW;
	TilePlane U[2];
	TilePlane V[2];
	TilePlane4 Col[2];
	TilePlane4 Ofs[2];
};

constexpr u32 TILE_ENTRY_CACHE = 64;

// The tile buffers of the CORE: one set per worker thread
struct TileState
{
	u8 tagStatus[MAX_RENDER_PIXELS];
	parameter_tag_t tagBuffer[2][MAX_RENDER_PIXELS];
	StencilType stencilBuffer[MAX_RENDER_PIXELS];
	u32 colorBuffer1[MAX_RENDER_PIXELS];
	u32 colorBuffer2[MAX_RENDER_PIXELS];
	ZType depthBuffer[3][MAX_RENDER_PIXELS];
	ZType depthSave[MAX_RENDER_PIXELS];		// PSFlyCast, see RM_DEPTH_ONLY
	bool MoreToDraw;
	Color offs;		// this one persists across invocations, as tested via bump maps
	float left, top;	// the tile's position
	const Frame *frame;
	TileEntry entries[TILE_ENTRY_CACHE];
	// counters, for the statistics
	u32 ispTriangles;
	u32 tspPixels;
	u32 peelPasses;
	// PSFlyCast: where the time goes, when it is measured (see PROFILE_ below)
	bool profile;
	u64 profileNs[10];
	u64 jobNs;		// all of this thread's tiles
};

enum {
	PROFILE_CLEAR, PROFILE_ISP_OPAQUE, PROFILE_ISP_PUNCHTHROUGH, PROFILE_ISP_TRANSLUCENT, PROFILE_ISP_MODIFIER,
	PROFILE_TSP_OPAQUE, PROFILE_TSP_PUNCHTHROUGH, PROFILE_TSP_TRANSLUCENT, PROFILE_PRESORT, PROFILE_WRITEOUT,
};

void InitTexUtils();
// Sets a polygon's ShadeFuncs from its parameters.
void SelectShadeFuncs(PolyState& poly);
// Fills a triangle's interpolation set-up (the reference's IPs3::Setup).
struct VertexIn
{
	float x, y, z;
	float u[2], v[2];
	const u8 *col[2];	// in memory order: R, G, B, A
	const u8 *spc[2];
};
void SetupInterpolation(IPs3& ips, const DrawParameters& params, const VertexIn& v1, const VertexIn& v2, const VertexIn& v3, bool TwoVolumes);
// Fills a triangle's ISP set-up. False if it is culled (the reference culls when it rasterises).
bool SetupTriangle(Triangle& tri, u32 cullMode, float x1, float y1, float z1, float x2, float y2, float z2, float x3, float y3, float z3);

// Starts a tile: its position, and buffers that hold nothing.
void BeginTile(TileState& state, const Frame& frame, int tileX, int tileY);
// Renders one pass (region array entry) of the tile into its buffers.
void RenderTilePass(TileState& state, const TilePass& pass);

// The frame buffer the tiles are written to
struct WriteOut
{
	u8 *vram;
	u32 vramMask;
	bool area64;		// render to texture: the 64-bit (texture) area, else the 32-bit one
	u32 base;			// FB_W_SOF1 or FB_W_SOF2
	u32 stride;			// bytes
	u32 packmode;		// FB_W_CTRL
	bool dither;
	u32 kval;
	u32 alphaThreshold;
	int clipX0, clipX1;	// FB_X_CLIP, FB_Y_CLIP: inclusive
	int clipY0, clipY1;
};
// Packs count pixels (the accumulation buffer's B, G, R, A) and writes them at x, y of the frame buffer.
void WritePixels(const WriteOut& out, int x, int y, const u32 *pixels, int count);

}
