/*
	PSFlyCast - the software "reference" renderer: from Flycast's parsed frame
	to tiles, the worker threads, and the frame buffer in the emulated VRAM.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The pixels are REFSW's (refsw_tile.cpp, MIT, skmp / libswirl / nullDC-rust).
	What is here is what the hardware's tile accelerator and the reference's
	walk of the region array do, done from rend_context:

	- every strip of every polygon list becomes triangles in the order the
	  game submitted them (a tag is a triangle's place in that order, which is
	  what the per-pixel sort uses for polygons at the same depth), culled and
	  set up once;
	- each triangle is put in the tiles its bounding box touches, as the tile
	  accelerator does, and a modifier volume in the tiles the whole volume
	  touches; the polygon's tile clipping is applied there too;
	- the tiles are rendered by a pool of threads, each with tile buffers of
	  its own, and each tile is written to VRAM by the thread that rendered it
	  (or, when the scaler of SCALER_CTL is in use, to a picture the scaler
	  then writes).
*/
#include "soft_renderer.h"
#include "refsw_core.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace ps5
{
config::Option<bool> SoftwareRenderer("SoftwareRenderer", false, "ps5");
}

namespace ps5::soft
{
namespace
{
using namespace refsw;

// The most threads that render tiles. The console has 8 cores and 16 threads;
// the emulator itself keeps about three busy.
constexpr int MaxThreads = 12;

double msSince(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The floating point state every thread renders with: round to nearest,
// denormals as IEEE has them. A title's threads start with denormals flushed
// to zero, and the thread the emulator renders on may have another rounding
// mode; a tile must not depend on the thread that renders it.
class FloatState
{
public:
	FloatState()
	{
#if defined(__x86_64__) || defined(__i386__)
		saved = _mm_getcsr();
		_mm_setcsr(0x1F80);
#endif
	}
	~FloatState()
	{
#if defined(__x86_64__) || defined(__i386__)
		_mm_setcsr(saved);
#endif
	}

private:
	unsigned saved = 0;
};

// Threads that all run one job and are waited for. The caller runs it too.
class Pool
{
public:
	~Pool() {
		stop();
	}

	void start(int workers)
	{
		if ((int)threads.size() == workers)
			return;
		stop();
		quit = false;
		// no thread is running: a new one waits for the job after the last one given
		const unsigned seen = generation;
		for (int i = 0; i < workers; i++)
			threads.emplace_back([this, i, seen] { work(i + 1, seen); });
	}

	void stop()
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			quit = true;
		}
		wake.notify_all();
		for (std::thread& thread : threads)
			thread.join();
		threads.clear();
	}

	int size() const {
		return (int)threads.size() + 1;
	}

	// job(context, worker): worker 0 is the caller
	void run(void (*job)(void *, int), void *context)
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			this->job = job;
			this->context = context;
			running = (int)threads.size();
			generation++;
		}
		wake.notify_all();
		job(context, 0);
		std::unique_lock<std::mutex> lock(mutex);
		done.wait(lock, [this] { return running == 0; });
	}

private:
	void work(int worker, unsigned seen)
	{
		FloatState floatState;
		for (;;)
		{
			void (*job)(void *, int);
			void *context;
			{
				std::unique_lock<std::mutex> lock(mutex);
				wake.wait(lock, [&] { return quit || generation != seen; });
				if (quit)
					return;
				seen = generation;
				job = this->job;
				context = this->context;
			}
			job(context, worker);
			{
				std::lock_guard<std::mutex> lock(mutex);
				running--;
			}
			done.notify_one();
		}
	}

	std::vector<std::thread> threads;
	std::mutex mutex;
	std::condition_variable wake;
	std::condition_variable done;
	void (*job)(void *, int) = nullptr;
	void *context = nullptr;
	unsigned generation = 0;
	int running = 0;
	bool quit = false;
};

// One polygon list of one pass, by tile: tile t has tags[start[t]] to tags[start[t + 1]]
struct BinList
{
	std::vector<u32> start;
	std::vector<u32> tags;

	TileList of(int tile) const
	{
		TileList list;
		if (!start.empty())
		{
			list.tags = tags.data() + start[tile];
			list.count = start[tile + 1] - start[tile];
		}
		return list;
	}
};

// A triangle and the tiles it goes into
struct Placement
{
	u32 tag;
	s16 tx0, ty0, tx1, ty1;		// tiles, inclusive, of the whole picture
	u32 outside;				// tile clipping "outside": not in this rectangle (as PolyParam::tileclip), or 0
};

enum { ListOpaque, ListPunchThrough, ListTranslucent, ListOpaqueMod, ListTranslucentMod, ListCount };

struct Pass
{
	bool zClear;
	bool autosort;
	bool sharedModVols;		// the translucent list uses the opaque modifier volumes
	BinList lists[ListCount];
};

// A triangle before it is set up: its vertices, its polygon, its place in the order of submission
struct Candidate
{
	u32 a, b, c;
	u32 poly;		// index in its list of PolyParams
	u32 key;
};

bool isVertexInf(const Vertex& vtx)
{
	// as ta_util.cpp
	return std::isnan(vtx.x) || fabsf(vtx.x) > 1e25f
			|| std::isnan(vtx.y) || fabsf(vtx.y) > 1e25f
			|| std::isnan(vtx.z) || vtx.z > 3.4e37f;
}

VertexIn vertexIn(const Vertex& v)
{
	VertexIn in;
	in.x = v.x;
	in.y = v.y;
	in.z = v.z;
	in.u[0] = v.u;
	in.v[0] = v.v;
	in.u[1] = v.u1;
	in.v[1] = v.v1;
	in.col[0] = v.col;
	in.col[1] = v.col1;
	in.spc[0] = v.spc;
	in.spc[1] = v.spc1;
	return in;
}

class SoftRenderer
{
public:
	void render(const rend_context& ctx);
	void term()
	{
		pool.stop();
		states.reset();
		stateCount = 0;
		frame = Frame();
		passes.clear();
		image.clear();
		image.shrink_to_fit();
	}

	int wantedThreads = 0;
	int profiling = 0;
	Stats stats{};

private:
	void snapshot(const rend_context& ctx);
	bool setArea(const rend_context& ctx);
	void prepare(const rend_context& ctx);
	u32 addPoly(const PolyParam& pp);
	void addList(const rend_context& ctx, const std::vector<PolyParam>& polys, u32 first, u32 end, bool indexed, bool opaque, BinList& bins);
	void addModVols(const rend_context& ctx, const std::vector<ModifierVolumeParam>& params, u32 first, u32 end, BinList& bins);
	void place(const Triangle& tri, u32 tag, u32 tileclip);
	void bin(BinList& bins);
	void renderTile(TileState& state, int tile);
	void scaledWriteout(const rend_context& ctx);
	static void tileJob(void *context, int worker);

	Pool pool;
	std::unique_ptr<TileState[]> states;
	int stateCount = 0;
	Frame frame;
	std::vector<Pass> passes;
	std::vector<Candidate> candidates;
	struct Owner { u32 firstVertex; u32 poly; };
	std::vector<Owner> owners;
	std::vector<Placement> placements;
	std::vector<Placement> volumeMembers;
	std::vector<u32> cursor;
	// The tiles of the picture: tile t is at tileX0 + t % tilesW, tileY0 + t / tilesW
	int tileX0 = 0, tileY0 = 0, tilesW = 0, tilesH = 0;
	std::atomic<int> nextTile{0};
	WriteOut out{};
	// With the scaler in use: the picture as rendered, for scaledWriteout
	bool scaled = false;
	std::vector<u32> image;
};

void SoftRenderer::snapshot(const rend_context& ctx)
{
	cfg.vram = &vram[0];
	cfg.vramMask = VRAM_MASK;
	memcpy(cfg.palette, PALETTE_RAM, sizeof(cfg.palette));
	cfg.palCtrl = PAL_RAM_CTRL & 3;
	cfg.textControl = TEXT_CONTROL;
	memcpy(cfg.fogTable, FOG_TABLE, sizeof(cfg.fogTable));
	{
		// as the reference's LookupFogTable
		const u32 density = FOG_DENSITY.full;
		float fog_den_mant = ((density >> 8) & 0xFF) / 128.0f;  //bit 7 -> x. bit, so [6:0] -> fraction -> /128
		s32 fog_den_exp = (s8)(density & 0xFF);
		cfg.fogDensity = fog_den_mant * powf(2.0f, fog_den_exp);
	}
	cfg.fogColRam = FOG_COL_RAM.full;
	cfg.fogColVert = FOG_COL_VERT.full;
	cfg.fogClampMax = ctx.fog_clamp_max.full;
	cfg.fogClampMin = ctx.fog_clamp_min.full;
	cfg.ptAlphaRef = PT_ALPHA_REF & 0xFF;
	cfg.shadScale = FPU_SHAD_SCALE.scale_factor;
	cfg.intensityShadow = FPU_SHAD_SCALE.intensity_shadow != 0;
	cfg.cullVal = FPU_CULL_VAL;
	const u32 halfOffset = HALF_OFFSET;
	cfg.fpuHalfOffset = (halfOffset & 1) != 0;
	cfg.tspHalfOffset = (halfOffset & 2) != 0;
	cfg.texHalfOffset = (halfOffset & 4) != 0;
	// The background plane's depth as Flycast has it (ISP_BACKGND_D, a little lower: FillBGP)
	cfg.backgroundDepth = ctx.verts[0].z;
}

// Which tiles are rendered and where they are written. False if there is nothing to do.
bool SoftRenderer::setArea(const rend_context& ctx)
{
	// The tiles the region array has, within the global tile clip (setTileClipping)
	const int x0 = std::max(ctx.tileClip.origin.x, 0);
	const int y0 = std::max(ctx.tileClip.origin.y, 0);
	const int x1 = std::min(ctx.tileClip.origin.x + ctx.tileClip.size.x, std::min(ctx.globClip.x, 64 * 32));
	const int y1 = std::min(ctx.tileClip.origin.y + ctx.tileClip.size.y, std::min(ctx.globClip.y, 16 * 32));
	if (x1 <= x0 || y1 <= y0)
		return false;
	tileX0 = x0 / 32;
	tileY0 = y0 / 32;
	tilesW = (x1 + 31) / 32 - tileX0;
	tilesH = (y1 + 31) / 32 - tileY0;

	out.vram = cfg.vram;
	out.vramMask = cfg.vramMask;
	out.area64 = ctx.isRTT;
	out.base = ctx.fb_W_SOF1;
	// The scaler writes the second field of a flicker-free interlaced picture at FB_W_SOF2 (as the reference)
	if (!ctx.isRTT && ctx.scaler_ctl.interlace && ctx.scaler_ctl.fieldselect)
		out.base = FB_W_SOF2;
	out.base &= cfg.vramMask;
	out.packmode = ctx.fb_W_CTRL.fb_packmode;
	out.dither = ctx.fb_W_CTRL.fb_dither != 0;
	out.kval = ctx.fb_W_CTRL.fb_kval;
	out.alphaThreshold = ctx.fb_W_CTRL.fb_alpha_threshold;
	out.clipX0 = ctx.fbClip.origin.x;
	out.clipX1 = ctx.fbClip.origin.x + ctx.fbClip.size.x - 1;
	out.clipY0 = ctx.fbClip.origin.y;
	out.clipY1 = ctx.fbClip.origin.y + ctx.fbClip.size.y - 1;

	const u32 vscale = ctx.scaler_ctl.vscalefactor;
	scaled = ctx.scaler_ctl.hscale != 0 || (vscale != 0 && vscale != 0x400 && vscale != 0x401);
	const u32 bpp = out.packmode <= 3 ? 2 : out.packmode == 4 ? 3 : 4;
	out.stride = ctx.fb_W_LINESTRIDE * 8;
	if (out.stride == 0)
		// no stride: the lines follow each other (as Flycast's WriteFramebuffer and WriteTextureToVRam)
		out.stride = (ctx.scaler_ctl.hscale ? ctx.globClip.x / 2 : ctx.globClip.x) * bpp;
	if (scaled)
		image.resize((size_t)tilesW * 32 * tilesH * 32);

	return true;
}

u32 SoftRenderer::addPoly(const PolyParam& pp)
{
	frame.polys.emplace_back();
	PolyState& poly = frame.polys.back();
	poly.params.isp = pp.isp;
	// In the TA's output these four are the object control's (PCW)
	poly.params.isp.Texture = pp.pcw.Texture;
	poly.params.isp.Offset = pp.pcw.Offset;
	poly.params.isp.Gouraud = pp.pcw.Gouraud;
	poly.params.isp.UV_16b = pp.pcw.UV_16bit;
	poly.params.tsp[0] = pp.tsp;
	poly.params.tcw[0] = pp.tcw;
	poly.params.tsp[1] = pp.tsp1;
	poly.params.tcw[1] = pp.tcw1;
	poly.shadow = pp.pcw.Shadow != 0;
	// A second set of parameters: "with two volumes", when shadows are not by intensity
	poly.twoVolumes = poly.shadow && !cfg.intensityShadow && pp.tsp1.full != (u32)-1;
	SelectShadeFuncs(poly);
	return (u32)frame.polys.size() - 1;
}

// The tiles a triangle is in: those its bounding box touches, less its polygon's tile clipping.
void SoftRenderer::place(const Triangle& tri, u32 tag, u32 tileclip)
{
	// one pixel more on each side, as the rasteriser's own bounds
	const float fx0 = std::min(std::max(floorf(tri.minX) - 1, -32.f), 65536.f);
	const float fy0 = std::min(std::max(floorf(tri.minY) - 1, -32.f), 65536.f);
	const float fx1 = std::min(std::max(ceilf(tri.maxX) + 1, -32.f), 65536.f);
	const float fy1 = std::min(std::max(ceilf(tri.maxY) + 1, -32.f), 65536.f);
	int tx0 = std::max((int)floorf(fx0 / 32), tileX0);
	int ty0 = std::max((int)floorf(fy0 / 32), tileY0);
	int tx1 = std::min((int)floorf(fx1 / 32), tileX0 + tilesW - 1);
	int ty1 = std::min((int)floorf(fy1 / 32), tileY0 + tilesH - 1);

	u32 outside = 0;
	const u32 clipMode = tileclip >> 28;
	if (clipMode >= 2)
	{
		const int cx0 = tileclip & 63;
		const int cx1 = (tileclip >> 6) & 63;
		const int cy0 = (tileclip >> 12) & 31;
		const int cy1 = (tileclip >> 17) & 31;
		if (clipMode == 2)
		{
			// inside the rectangle only
			tx0 = std::max(tx0, cx0);
			ty0 = std::max(ty0, cy0);
			tx1 = std::min(tx1, cx1);
			ty1 = std::min(ty1, cy1);
		}
		else
			outside = tileclip | 0x80000000;
	}
	if (tx0 > tx1 || ty0 > ty1)
		return;
	placements.push_back({ tag, (s16)tx0, (s16)ty0, (s16)tx1, (s16)ty1, outside });
}

static inline bool clippedOut(u32 outside, int tx, int ty)
{
	return (int)(outside & 63) <= tx && tx <= (int)((outside >> 6) & 63)
			&& (int)((outside >> 12) & 31) <= ty && ty <= (int)((outside >> 17) & 31);
}

// From the placements, in order, to each tile's list.
void SoftRenderer::bin(BinList& bins)
{
	const int tiles = tilesW * tilesH;
	bins.start.assign(tiles + 1, 0);
	for (const Placement& p : placements)
		for (int ty = p.ty0; ty <= p.ty1; ty++)
			for (int tx = p.tx0; tx <= p.tx1; tx++)
				if (p.outside == 0 || !clippedOut(p.outside, tx, ty))
					bins.start[(ty - tileY0) * tilesW + (tx - tileX0) + 1]++;
	for (int t = 0; t < tiles; t++)
		bins.start[t + 1] += bins.start[t];
	bins.tags.resize(bins.start[tiles]);
	cursor.assign(bins.start.begin(), bins.start.end() - 1);
	for (const Placement& p : placements)
		for (int ty = p.ty0; ty <= p.ty1; ty++)
			for (int tx = p.tx0; tx <= p.tx1; tx++)
				if (p.outside == 0 || !clippedOut(p.outside, tx, ty))
					bins.tags[cursor[(ty - tileY0) * tilesW + (tx - tileX0)]++] = p.tag;
	placements.clear();
}

// One polygon list of one pass: polys[first] to polys[end - 1].
// indexed: ta_parse made strips with primitive restart in ctx.idx; else the
// polygons still name their vertices (it sorted triangles for the graphics
// processor's renderer and left the list alone).
//
// What ta_parse did to an indexed list is undone here, because the hardware
// gets the strips as the game sent them. It merges a polygon into the one
// before when their parameters draw alike, turning its strips over with a
// repeated vertex if it culls the other way, and with strip sorting it puts
// the polygons in order of depth first. A merged polygon keeps its own
// parameters and the number of its first vertex, and vertices are numbered
// in the order of submission: so a strip belongs to the polygon with the
// last first vertex not after the strip's, a triangle's place in its strip
// says which way round it was, and its vertices say where it was in the list.
void SoftRenderer::addList(const rend_context& ctx, const std::vector<PolyParam>& polys, u32 first, u32 end, bool indexed, bool opaque, BinList& bins)
{
	const u32 vertexCount = (u32)ctx.verts.size();
	const u32 indexCount = (u32)ctx.idx.size();
	const Vertex *verts = ctx.verts.data();

	candidates.clear();
	end = std::min<u32>(end, (u32)polys.size());
	if (indexed)
	{
		// each polygon's first vertex
		owners.clear();
		for (u32 p = first; p < end; p++)
		{
			const PolyParam& pp = polys[p];
			if (pp.isNaomi2())
				continue;
			if (pp.count == 0)
				// merged into another (or empty)
				owners.push_back({ pp.first, p });
			else if (pp.first < indexCount && ctx.idx[pp.first] != (u32)-1)
			{
				// Its first vertex that was not dropped. ta_parse repeats it when
				// vertices before it were dropped and the strip, which is culled by
				// its winding, would otherwise start the other way round: the strip
				// started an odd number of vertices earlier.
				u32 firstVertex = ctx.idx[pp.first];
				if (pp.count >= 2 && pp.first + 1 < indexCount && ctx.idx[pp.first + 1] == firstVertex && firstVertex > 0)
					firstVertex--;
				owners.push_back({ firstVertex, p });
			}
		}
		std::stable_sort(owners.begin(), owners.end(), [](const Owner& left, const Owner& right) { return left.firstVertex < right.firstVertex; });

		for (u32 p = first; p < end; p++)
		{
			const PolyParam& pp = polys[p];
			if (pp.count < 3 || pp.isNaomi2())
				continue;
			if (pp.first > indexCount || pp.count > indexCount - pp.first)
				continue;
			const u32 *idx = ctx.idx.data() + pp.first;
			u32 start = 0;
			while (start < pp.count)
			{
				u32 stop = start;
				while (stop < pp.count && idx[stop] != (u32)-1)
					stop++;
				// a vertex ta_parse repeated to turn the strip over, or to keep it the way it was
				if (start + 1 < stop && idx[start] == idx[start + 1])
					start++;
				const u32 firstVertex = idx[start];
				// whose strip it is
				auto owner = std::upper_bound(owners.begin(), owners.end(), firstVertex,
						[](u32 vertex, const Owner& candidate) { return vertex < candidate.firstVertex; });
				if (owner != owners.begin() && firstVertex < vertexCount)
				{
					--owner;
					// the first triangle's place in the strip as it was submitted (vertices that were dropped come before it)
					const u32 place = firstVertex - owner->firstVertex;
					for (u32 i = start; i + 2 < stop; i++)
					{
						u32 a = idx[i], b = idx[i + 1];
						const u32 c = idx[i + 2];
						if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
							continue;
						if (a == b || b == c || a == c)
							continue;
						// the background plane (the first polygon of the opaque list) is tag 0, not a triangle of the list
						if (opaque && a < 4 && b < 4 && c < 4)
							continue;
						if ((place + i - start) & 1)
							std::swap(a, b);
						candidates.push_back({ a, b, c, owner->poly, std::min(a, std::min(b, c)) });
					}
				}
				start = stop + 1;
			}
		}
	}
	else
	{
		for (u32 p = first; p < end; p++)
		{
			const PolyParam& pp = polys[p];
			if (pp.count < 3 || pp.isNaomi2())
				continue;
			if (pp.first > vertexCount || pp.count > vertexCount - pp.first)
				continue;
			for (u32 i = 0; i + 2 < pp.count; i++)
			{
				u32 a = pp.first + i, b = a + 1;
				const u32 c = a + 2;
				if (isVertexInf(verts[a]) || isVertexInf(verts[b]) || isVertexInf(verts[c]))
					continue;
				if (i & 1)
					std::swap(a, b);
				candidates.push_back({ a, b, c, p, pp.first + i });
			}
		}
	}

	// The order of submission is the order of the vertices
	const auto earlier = [](const Candidate& left, const Candidate& right) { return left.key < right.key; };
	if (!std::is_sorted(candidates.begin(), candidates.end(), earlier))
		std::stable_sort(candidates.begin(), candidates.end(), earlier);

	u32 lastParam = (u32)-1;
	u32 polyIndex = 0;
	for (const Candidate& candidate : candidates)
	{
		if (frame.tris.size() >= MAX_FRAME_TRIANGLES)
			break;
		const PolyParam& pp = polys[candidate.poly];
		const Vertex& v1 = verts[candidate.a];
		const Vertex& v2 = verts[candidate.b];
		const Vertex& v3 = verts[candidate.c];
		Triangle tri;
		if (!SetupTriangle(tri, pp.isp.CullMode, v1.x, v1.y, v1.z, v2.x, v2.y, v2.z, v3.x, v3.y, v3.z))
			continue;
		if (candidate.poly != lastParam) {
			polyIndex = addPoly(pp);
			lastParam = candidate.poly;
		}
		tri.poly = polyIndex;
		tri.depthMode = pp.isp.DepthMode;
		tri.zWriteDis = pp.isp.ZWriteDis;
		tri.volumeOp = 0;
		const u32 tag = (u32)frame.tris.size();
		frame.tris.push_back(tri);
		frame.ips.emplace_back();
		const PolyState& poly = frame.polys[polyIndex];
		SetupInterpolation(frame.ips.back(), poly.params, vertexIn(v1), vertexIn(v2), vertexIn(v3), poly.twoVolumes);
		place(tri, tag, pp.tileclip);
	}
	bin(bins);
}

// The modifier volumes of one pass. A volume is the triangles up to one that
// closes it ("inside last" or "outside last"); the tile accelerator puts all
// of them in every tile the volume touches, so that each tile sees the whole
// volume and the triangle that closes it.
void SoftRenderer::addModVols(const rend_context& ctx, const std::vector<ModifierVolumeParam>& params, u32 first, u32 end, BinList& bins)
{
	const u32 triangleCount = (u32)ctx.modtrig.size();
	size_t volumeStart = placements.size();
	float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;

	const auto closeVolume = [&]() {
		// every triangle of the volume in every tile of the volume
		Triangle box;
		box.minX = minX;
		box.minY = minY;
		box.maxX = maxX;
		box.maxY = maxY;
		volumeMembers.assign(placements.begin() + volumeStart, placements.end());
		placements.resize(volumeStart);
		if (minX <= maxX)
			for (const Placement& member : volumeMembers)
				// member.outside holds the triangle's tile clipping here
				place(box, member.tag, member.outside);
		volumeStart = placements.size();
		minX = minY = 1e30f;
		maxX = maxY = -1e30f;
	};

	end = std::min<u32>(end, (u32)params.size());
	for (u32 p = first; p < end; p++)
	{
		const ModifierVolumeParam& param = params[p];
		if (param.count == 0 || param.isNaomi2())
			continue;
		if (param.first > triangleCount || param.count > triangleCount - param.first)
			continue;
		// 0 normal polygon, 1 inside last, 2 outside last
		const u32 volumeMode = param.isp.DepthMode;
		const bool closes = volumeMode == 1 || volumeMode == 2;
		for (u32 i = 0; i < param.count; i++)
		{
			if (frame.tris.size() >= MAX_FRAME_TRIANGLES)
				break;
			const ModTriangle& mt = ctx.modtrig[param.first + i];
			Triangle tri;
			const bool finite = !std::isnan(mt.x0 + mt.y0 + mt.z0 + mt.x1 + mt.y1 + mt.z1 + mt.x2 + mt.y2 + mt.z2)
					&& fabsf(mt.x0) <= 1e25f && fabsf(mt.y0) <= 1e25f && fabsf(mt.x1) <= 1e25f && fabsf(mt.y1) <= 1e25f
					&& fabsf(mt.x2) <= 1e25f && fabsf(mt.y2) <= 1e25f;
			if (!finite || !SetupTriangle(tri, param.isp.CullMode, mt.x0, mt.y0, mt.z0, mt.x1, mt.y1, mt.z1, mt.x2, mt.y2, mt.z2))
			{
				// culled: it covers nothing, but it may still close the volume
				tri = Triangle();
				tri.minX = tri.minY = 1e30f;
				tri.maxX = tri.maxY = -1e30f;
			}
			else
			{
				minX = std::min(minX, tri.minX);
				minY = std::min(minY, tri.minY);
				maxX = std::max(maxX, tri.maxX);
				maxY = std::max(maxY, tri.maxY);
			}
			tri.poly = 0;
			tri.depthMode = 6;
			tri.zWriteDis = 1;
			// The volume is summed up after the triangle that closes it. When a
			// parameter that closes a volume is not marked as its last (an open
			// volume or a quad), each of its triangles is one, as in the
			// reference; when it is, the whole parameter is (as Flycast).
			tri.volumeOp = 0;
			if (closes && (!param.isp.VolumeLast || i == param.count - 1))
				tri.volumeOp = volumeMode;
			const u32 tag = (u32)frame.tris.size();
			frame.tris.push_back(tri);
			frame.ips.emplace_back();
			placements.push_back({ tag, 0, 0, 0, 0, param.tileclip });
		}
		if (closes)
			closeVolume();
	}
	closeVolume();
	bin(bins);
}

void SoftRenderer::prepare(const rend_context& ctx)
{
	frame.tris.clear();
	frame.ips.clear();
	frame.polys.clear();
	placements.clear();

	// Tag 0: the background plane, from the three vertices FillBGP decoded
	{
		const PolyParam& pp = ctx.global_param_op[0];
		const u32 polyIndex = addPoly(pp);
		const Vertex *v = ctx.verts.data();
		Triangle tri{};
		tri.x1 = v[0].x;
		tri.y1 = v[0].y;
		tri.poly = polyIndex;
		tri.minX = tri.minY = 1e30f;
		tri.maxX = tri.maxY = -1e30f;
		frame.tris.push_back(tri);
		frame.ips.emplace_back();
		SetupInterpolation(frame.ips.back(), frame.polys[polyIndex].params, vertexIn(v[0]), vertexIn(v[1]), vertexIn(v[2]), false);
	}

	passes.resize(ctx.render_passes.size());
	RenderPass previous{};
	for (size_t p = 0; p < ctx.render_passes.size(); p++)
	{
		const RenderPass& current = ctx.render_passes[p];
		Pass& pass = passes[p];
		pass.zClear = current.z_clear;
		pass.autosort = current.autosort;
		pass.sharedModVols = current.mv_op_tr_shared;

		addList(ctx, ctx.global_param_op, previous.op_count, current.op_count, true, true, pass.lists[ListOpaque]);
		addList(ctx, ctx.global_param_pt, previous.pt_count, current.pt_count, true, false, pass.lists[ListPunchThrough]);
		// ta_parse sorted this pass's translucent triangles itself (for a
		// renderer that does not sort by pixel) when it added to sortedTriangles
		const bool sortedByParser = current.sorted_tr_count > previous.sorted_tr_count;
		addList(ctx, ctx.global_param_tr, previous.tr_count, current.tr_count, !sortedByParser, false, pass.lists[ListTranslucent]);
		addModVols(ctx, ctx.global_param_mvo, previous.mvo_count, current.mvo_count, pass.lists[ListOpaqueMod]);
		if (pass.sharedModVols) {
			pass.lists[ListTranslucentMod].start.clear();
			pass.lists[ListTranslucentMod].tags.clear();
		}
		else
			addModVols(ctx, ctx.global_param_mvo_tr, previous.mvo_tr_count, current.mvo_tr_count, pass.lists[ListTranslucentMod]);
		previous = current;
	}
}

void SoftRenderer::renderTile(TileState& state, int tile)
{
	const int tileX = (tileX0 + tile % tilesW) * 32;
	const int tileY = (tileY0 + tile / tilesW) * 32;

	BeginTile(state, frame, tileX, tileY);
	for (size_t p = 0; p < passes.size(); p++)
	{
		const Pass& pass = passes[p];
		TilePass entry;
		// The first pass of a tile always starts from the background plane
		entry.zKeep = p != 0 && !pass.zClear;
		entry.preSort = !pass.autosort;
		entry.keepDepth = p + 1 < passes.size() && !passes[p + 1].zClear;
		entry.opaque = pass.lists[ListOpaque].of(tile);
		entry.opaque_mod = pass.lists[ListOpaqueMod].of(tile);
		entry.puncht = pass.lists[ListPunchThrough].of(tile);
		entry.trans = pass.lists[ListTranslucent].of(tile);
		entry.trans_mod = pass.lists[pass.sharedModVols ? ListOpaqueMod : ListTranslucentMod].of(tile);
		RenderTilePass(state, entry);
	}

	// Copy to vram
	u64 start = 0;
	if (state.profile)
		start = ThreadTimeNs();
	if (scaled)
	{
		const int x = (tile % tilesW) * 32;
		const int y = (tile / tilesW) * 32;
		for (int line = 0; line < 32; line++)
			memcpy(&image[(size_t)(y + line) * tilesW * 32 + x], state.colorBuffer1 + line * 32, 32 * sizeof(u32));
	}
	else
	{
		for (int line = 0; line < 32; line++)
			WritePixels(out, tileX, tileY + line, state.colorBuffer1 + line * 32, 32);
	}
	if (state.profile)
		state.profileNs[PROFILE_WRITEOUT] += ThreadTimeNs() - start;
}

void SoftRenderer::tileJob(void *context, int worker)
{
	SoftRenderer *self = (SoftRenderer *)context;
	if (worker >= self->stateCount)
		return;
	TileState& state = self->states[worker];
	const int tiles = self->tilesW * self->tilesH;
	const u64 start = self->profiling ? ThreadTimeNs() : 0;
	for (;;)
	{
		const int tile = self->nextTile.fetch_add(1, std::memory_order_relaxed);
		if (tile >= tiles)
			break;
		self->renderTile(state, tile);
	}
	if (self->profiling)
		state.jobNs = ThreadTimeNs() - start;
}

// The scaler (SCALER_CTL): the picture is halved horizontally (hscale) and
// scaled vertically by 1024 / vscalefactor on its way to the frame buffer.
// The reference has no scaler; this one averages the lines and pixels a
// frame buffer pixel covers when the picture shrinks and interpolates between
// lines when it grows. The hardware's filter coefficients (Y_COEFF) and the
// half line between the fields of a flicker-free interlaced picture are not
// modelled.
void SoftRenderer::scaledWriteout(const rend_context& ctx)
{
	const int width = tilesW * 32;
	const int height = tilesH * 32;
	const bool hscale = ctx.scaler_ctl.hscale != 0;
	u32 vscale = ctx.scaler_ctl.vscalefactor;
	if (vscale == 0 || vscale == 0x401)
		vscale = 0x400;
	const int outWidth = hscale ? width / 2 : width;
	const int outHeight = (int)(((u64)height * 1024 + vscale - 1) / vscale);
	const int originX = hscale ? tileX0 * 32 / 2 : tileX0 * 32;
	const int originY = (int)((u64)tileY0 * 32 * 1024 / vscale);

	std::vector<u32> sum(width * 4);
	std::vector<u32> line(width);
	for (int y = 0; y < outHeight; y++)
	{
		// the source lines this line covers, in 1/1024ths of a line
		const u64 from = (u64)y * vscale;
		const u64 to = from + vscale;
		if (vscale > 0x400)
		{
			// shrinking: the average of the lines covered, by how much of each is
			std::fill(sum.begin(), sum.end(), 0);
			u64 total = 0;
			for (u64 at = from; at < to; )
			{
				const int source = (int)(at >> 10);
				const u64 next = std::min<u64>(to, ((u64)source + 1) << 10);
				const u32 weight = (u32)(next - at);
				if (source < height)
				{
					const u8 *src = (const u8 *)&image[(size_t)source * width];
					for (int i = 0; i < width * 4; i++)
						sum[i] += src[i] * weight;
					total += weight;
				}
				at = next;
			}
			if (total == 0)
				break;
			u8 *dst = (u8 *)line.data();
			for (int i = 0; i < width * 4; i++)
				dst[i] = (u8)((sum[i] + total / 2) / total);
		}
		else if (vscale == 0x400)
		{
			memcpy(line.data(), &image[(size_t)y * width], width * sizeof(u32));
		}
		else
		{
			// growing: between two lines
			const int source = (int)(from >> 10);
			const u32 frac = (u32)(from & 1023);
			const u8 *src0 = (const u8 *)&image[(size_t)std::min(source, height - 1) * width];
			const u8 *src1 = (const u8 *)&image[(size_t)std::min(source + 1, height - 1) * width];
			u8 *dst = (u8 *)line.data();
			for (int i = 0; i < width * 4; i++)
				dst[i] = (u8)((src0[i] * (1024 - frac) + src1[i] * frac + 512) >> 10);
		}
		if (hscale)
		{
			u8 *p = (u8 *)line.data();
			for (int x = 0; x < outWidth; x++)
				for (int i = 0; i < 4; i++)
					p[x * 4 + i] = (u8)((p[x * 8 + i] + p[x * 8 + 4 + i] + 1) >> 1);
		}
		WritePixels(out, originX, originY + y, line.data(), outWidth);
	}
}

void SoftRenderer::render(const rend_context& ctx)
{
	if (ctx.verts.size() < 4 || ctx.global_param_op.empty() || ctx.render_passes.empty())
		return;
	FloatState floatState;
	InitTexUtils();

	const auto prepareStart = std::chrono::steady_clock::now();
	const u64 prepareCpuStart = profiling ? ThreadTimeNs() : 0;
	snapshot(ctx);
	if (!setArea(ctx))
		return;
	prepare(ctx);
	stats.prepareMs = msSince(prepareStart);
	stats.prepareCpuMs = profiling ? (ThreadTimeNs() - prepareCpuStart) / 1e6 : 0;

	// The threads: all there are less a few for the emulator, and no more than the tiles
	int threads = wantedThreads;
	if (threads <= 0)
	{
#ifdef USE_PS5
		const int processors = 16;
#else
		const int processors = (int)std::thread::hardware_concurrency();
#endif
		threads = processors - 4;
	}
	threads = std::max(1, std::min(threads, MaxThreads));
	if (pool.size() != threads)
		pool.start(threads - 1);
	if (stateCount != threads)
	{
		states.reset(new TileState[threads]);
		stateCount = threads;
	}
	for (int i = 0; i < stateCount; i++)
	{
		TileState& state = states[i];
		state.ispTriangles = 0;
		state.tspPixels = 0;
		state.peelPasses = 0;
		state.profile = profiling >= 2;
		state.jobNs = 0;
		memset(state.profileNs, 0, sizeof(state.profileNs));
	}

	const auto tilesStart = std::chrono::steady_clock::now();
	nextTile = 0;
	pool.run(tileJob, this);
	stats.tilesMs = msSince(tilesStart);

	stats.writeMs = 0;
	if (scaled)
	{
		const auto writeStart = std::chrono::steady_clock::now();
		scaledWriteout(ctx);
		stats.writeMs = msSince(writeStart);
	}

	stats.triangles = (unsigned)frame.tris.size();
	stats.tiles = tilesW * tilesH;
	stats.tileTriangles = 0;
	stats.shadedPixels = 0;
	stats.peelPasses = 0;
	stats.tilesCpuMs = 0;
	for (double& ms : stats.profileMs)
		ms = 0;
	for (int i = 0; i < stateCount; i++)
	{
		stats.tileTriangles += states[i].ispTriangles;
		stats.shadedPixels += states[i].tspPixels;
		stats.peelPasses += states[i].peelPasses;
		stats.tilesCpuMs += states[i].jobNs / 1e6;
		for (int stage = 0; stage < 10; stage++)
			stats.profileMs[stage] += states[i].profileNs[stage] / 1e6;
	}
}

SoftRenderer& instance()
{
	static SoftRenderer renderer;
	return renderer;
}

}	// namespace

void render(const rend_context& ctx) {
	instance().render(ctx);
}

void term() {
	instance().term();
}

void setThreadCount(int count) {
	instance().wantedThreads = count;
}

int threadCount() {
	return instance().wantedThreads;
}

const Stats& lastStats() {
	return instance().stats;
}

void setProfiling(int level) {
	instance().profiling = level;
}

}
