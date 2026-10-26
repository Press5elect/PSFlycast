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
// The graphics facts of the running game, as the PowerVR2 registers and the frame's display lists
// show them: what the game asks the hardware for (colour depths, video mode, scaler, dither, sorting,
// filters...). For a boot log, so that a picture problem can be matched to the modes a game uses.
//
// The renderer collects them (pvrFactsCollect, Vulkan renderers) on the first frame after a game
// starts and when a register fact changes; the polygon counts are taken on that first frame and then
// once about every 10 seconds, so the per-frame cost is a few register reads. The frontend reads them
// with pvrGameFacts and logs them when the serial changes. This module does not log them itself.
#pragma once
#include "types.h"

#include <string>

struct rend_context;

struct PvrFacts
{
	// Increases each time a fact other than the counts changes (and on the first frame of a game)
	u32 serial = 0;
	std::string gameId;

	// Video output (read side)
	u32 fbDepth = 0;		// FB_R_CTRL.fb_depth: 0 = 0555 (15-bit), 1 = 565, 2 = 888 packed (24-bit), 3 = 0888 (32-bit)
	bool vgaClock = false;	// FB_R_CTRL.vclk_div: 1 = full pixel clock (VGA / 31 kHz), 0 = half (TV / 15 kHz)
	bool lineDouble = false;// FB_R_CTRL.fb_line_double
	bool pal = false;		// SPG_CONTROL.PAL
	bool ntsc = false;		// SPG_CONTROL.NTSC
	bool interlace = false;	// SPG_CONTROL.interlace
	bool hscale = false;	// SCALER_CTL.hscale: horizontal halving (the 1280-wide super-sampling render)
	u32 vscale = 0;			// SCALER_CTL.vscalefactor, 0x400 = 1.0 (0x401: the flicker filter at full size)
	u32 yCoeff = 0;			// Y_COEFF: bits 15-8 centre line, 7-0 outer lines of the flicker filter

	// Write side: the last screen frame and the last render to texture
	bool screenSeen = false;
	u32 screenPackMode = 0;	// FB_W_CTRL.fb_packmode: 0 0555 KRGB, 1 565, 2 4444, 3 1555, 4 888, 5 0888 KRGB, 6 8888
	bool screenDither = false;
	bool rttSeen = false;
	u32 rttPackMode = 0;
	bool rttDither = false;

	// Shadows and culling
	u32 shadowScale = 0;		// FPU_SHAD_SCALE.scale_factor (brightness inside a cheap shadow, /256)
	bool cheapShadow = false;	// FPU_SHAD_SCALE.intensity_shadow: modifier volumes as cheap shadow
	float cullValue = 0.f;		// FPU_CULL_VAL: polygons smaller than this are culled

	// Render passes of the last screen frame (the region array)
	u32 passCount = 0;
	u32 autosortMask = 0;		// bit n set: pass n sorts translucent polygons per pixel (auto sort); clear: presort
	bool typeTwoRegions = false;// FPU_PARAM_CFG bit 21: type 2 region headers, sort mode per pass; else ISP_FEED_CFG for all

	// Fog
	bool fogTable = false;		// some polygons use the fog look-up table (FogCtrl 0 or 3)

	// Polygon counts of the last sampled screen frame (opaque, punch-through and translucent lists)
	u64 countTimeMs = 0;		// when they were taken (getTimeMs)
	u32 polygons = 0;
	u32 texturedPolygons = 0;
	u32 superSamplePolygons = 0;	// TSP SupSample: the 4x super-sampling texture filter
	u32 filterPolygons[4] {};		// textured polygons by TSP FilterMode: 0 point, 1 bilinear, 2 trilinear A, 3 trilinear B
	u32 dAdjustPolygons = 0;		// textured polygons with a D-adjust other than 1.0 (TSP MipMapD not 4 or 0)
	u32 fogTablePolygons = 0;

	// YUV converter
	bool yuvUsed = false;		// started since the game began
	bool yuvMulti = false;		// TA_YUV_TEX_CTRL.yuv_tex: 16x16 textures one after the other
	bool yuvForm422 = false;	// TA_YUV_TEX_CTRL.yuv_form: YUV422 input blocks (else YUV420)
};

// The facts of the running game. False until a game has rendered its first frame.
bool pvrGameFacts(PvrFacts& facts);

// Renderer side: called with each parsed frame (screen or render to texture), after ta_parse
void pvrFactsCollect(const rend_context& ctx);

// Whether the YUV converter has been started since the last reset (hw/pvr/pvr_mem.cpp)
bool YUV_active();
