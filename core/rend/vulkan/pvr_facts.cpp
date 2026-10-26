/*
	Copyright 2026 Press5elect

	This file is part of PSFlyCast, a port of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
// The game's graphics facts (rend/pvr_facts.h). It lives with the Vulkan renderers, which call the
// collector, since those are the renderers the PS5 build has; any renderer may call it.
#include "rend/pvr_facts.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_regs.h"
#include "stdclass.h"

#include <mutex>

namespace
{

// How often the polygon counts are taken again after the first frame
constexpr u64 CountIntervalMs = 10000;

std::mutex factsMutex;
PvrFacts facts;			// guarded by factsMutex
bool haveFacts = false;	// a screen frame of the current game has been seen
std::string currentGameId;		// the game the facts are of (settings.content)
std::string currentGameFile;

// The facts that bump the serial when they change: registers and the set of features in use
// (not the raw counts, which move every frame)
bool sameModes(const PvrFacts& a, const PvrFacts& b)
{
	return a.fbDepth == b.fbDepth && a.vgaClock == b.vgaClock && a.lineDouble == b.lineDouble
			&& a.pal == b.pal && a.ntsc == b.ntsc && a.interlace == b.interlace
			&& a.hscale == b.hscale && a.vscale == b.vscale && a.yCoeff == b.yCoeff
			&& a.screenSeen == b.screenSeen && a.screenPackMode == b.screenPackMode && a.screenDither == b.screenDither
			&& a.rttSeen == b.rttSeen && a.rttPackMode == b.rttPackMode && a.rttDither == b.rttDither
			&& a.shadowScale == b.shadowScale && a.cheapShadow == b.cheapShadow && a.cullValue == b.cullValue
			&& a.passCount == b.passCount && a.autosortMask == b.autosortMask && a.typeTwoRegions == b.typeTwoRegions
			&& a.fogTable == b.fogTable
			&& (a.superSamplePolygons != 0) == (b.superSamplePolygons != 0)
			&& (a.filterPolygons[0] != 0) == (b.filterPolygons[0] != 0)
			&& (a.filterPolygons[1] != 0) == (b.filterPolygons[1] != 0)
			&& (a.filterPolygons[2] != 0) == (b.filterPolygons[2] != 0)
			&& (a.filterPolygons[3] != 0) == (b.filterPolygons[3] != 0)
			&& (a.dAdjustPolygons != 0) == (b.dAdjustPolygons != 0)
			&& a.yuvUsed == b.yuvUsed && a.yuvMulti == b.yuvMulti && a.yuvForm422 == b.yuvForm422;
}

void countList(const std::vector<PolyParam>& list, PvrFacts& f)
{
	for (const PolyParam& pp : list)
	{
		if (pp.count == 0)
			continue;
		f.polygons++;
		// Fog look-up table: FogCtrl 0 (table) or 3 (table mode 2)
		if (pp.tsp.FogCtrl == 0 || pp.tsp.FogCtrl == 3)
			f.fogTablePolygons++;
		if (!pp.pcw.Texture)
			continue;
		f.texturedPolygons++;
		if (pp.tsp.SupSample)
			f.superSamplePolygons++;
		f.filterPolygons[pp.tsp.FilterMode]++;
		if (pp.tsp.MipMapD != 4 && pp.tsp.MipMapD != 0)
			f.dAdjustPolygons++;
	}
}

}	// namespace

void pvrFactsCollect(const rend_context& ctx)
{
	std::lock_guard<std::mutex> _(factsMutex);
	if (settings.content.gameId != currentGameId || settings.content.fileName != currentGameFile)
	{
		// A new game: start over, and count on its first screen frame
		currentGameId = settings.content.gameId;
		currentGameFile = settings.content.fileName;
		facts = PvrFacts{ facts.serial };
		facts.gameId = settings.content.gameId;
		haveFacts = false;
	}
	PvrFacts f = facts;

	// Write side, kept for screen and render to texture apart
	if (ctx.isRTT)
	{
		f.rttSeen = true;
		f.rttPackMode = ctx.fb_W_CTRL.fb_packmode;
		f.rttDither = ctx.fb_W_CTRL.fb_dither;
	}
	else
	{
		f.screenSeen = true;
		f.screenPackMode = ctx.fb_W_CTRL.fb_packmode;
		f.screenDither = ctx.fb_W_CTRL.fb_dither;
	}

	// Registers: a handful of reads per frame
	const FB_R_CTRL_type fbr = FB_R_CTRL;
	f.fbDepth = fbr.fb_depth;
	f.vgaClock = fbr.vclk_div;
	f.lineDouble = fbr.fb_line_double;
	const SPG_CONTROL_type spg = SPG_CONTROL;
	f.pal = spg.PAL;
	f.ntsc = spg.NTSC;
	f.interlace = spg.interlace;
	f.yCoeff = Y_COEFF & 0xffff;
	f.shadowScale = FPU_SHAD_SCALE.scale_factor;
	f.cheapShadow = FPU_SHAD_SCALE.intensity_shadow;
	f.cullValue = FPU_CULL_VAL;
	f.yuvUsed = YUV_active();
	f.yuvMulti = TA_YUV_TEX_CTRL.yuv_tex;
	f.yuvForm422 = TA_YUV_TEX_CTRL.yuv_form;

	if (!ctx.isRTT)
	{
		f.hscale = ctx.scaler_ctl.hscale;
		f.vscale = ctx.scaler_ctl.vscalefactor;
		f.typeTwoRegions = ((FPU_PARAM_CFG >> 21) & 1) != 0;
		f.passCount = (u32)ctx.render_passes.size();
		f.autosortMask = 0;
		for (u32 i = 0; i < f.passCount && i < 32; i++)
			if (ctx.render_passes[i].autosort)
				f.autosortMask |= 1u << i;

		// The polygon walk: the first screen frame of the game, then every CountIntervalMs
		const u64 now = getTimeMs();
		if (!haveFacts || now - f.countTimeMs >= CountIntervalMs)
		{
			f.countTimeMs = now;
			f.polygons = f.texturedPolygons = f.superSamplePolygons = 0;
			f.filterPolygons[0] = f.filterPolygons[1] = f.filterPolygons[2] = f.filterPolygons[3] = 0;
			f.dAdjustPolygons = f.fogTablePolygons = 0;
			countList(ctx.global_param_op, f);
			countList(ctx.global_param_pt, f);
			countList(ctx.global_param_tr, f);
			f.fogTable = f.fogTablePolygons != 0;
		}
	}

	if (!haveFacts)
	{
		// Only a screen frame makes the facts complete; render to texture before it is kept
		if (!ctx.isRTT)
		{
			haveFacts = true;
			f.serial = facts.serial + 1;
		}
	}
	else if (!sameModes(f, facts))
		f.serial = facts.serial + 1;
	facts = f;
}

bool pvrGameFacts(PvrFacts& out)
{
	std::lock_guard<std::mutex> _(factsMutex);
	if (!haveFacts)
		return false;
	out = facts;
	return true;
}
