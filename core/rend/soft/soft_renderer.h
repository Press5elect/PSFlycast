/*
	PSFlyCast - the software "reference" renderer.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A frame Flycast has parsed (rend_context) is cut into 32x32 tiles and each
	tile is rendered by REFSW, skmp's software model of the PowerVR CORE
	(refsw_tile.cpp), on a few threads. The picture goes where the hardware
	puts it: the frame buffer in the emulated VRAM, in the format FB_W_CTRL
	asks for, or a texture's memory for a render to a texture. It is shown by
	the path "full framebuffer emulation" uses, which reads the frame buffer
	back at each vertical blank.
*/
#pragma once
#include "cfg/option.h"

struct rend_context;

namespace ps5
{
// "Software renderer": the game is drawn by the software model instead of the
// graphics processor. A setting, and a game can have its own.
extern config::Option<bool> SoftwareRenderer;

namespace soft
{
// Renders a parsed frame (after ta_parse) into the emulated VRAM. Called on
// the thread the renderer runs on; returns when the frame is in VRAM.
void render(const rend_context& ctx);
// Stops the worker threads and frees the buffers. They come back with the next frame.
void term();

// How many threads render tiles, the calling one included. 0: chosen from
// the processor (the default).
void setThreadCount(int count);
int threadCount();

// What the last frame took, for tests and measurements.
struct Stats
{
	unsigned triangles;			// prepared (not culled), modifier volumes included
	unsigned tiles;
	unsigned tileTriangles;		// triangles rasterised, summed over tiles and passes
	unsigned shadedPixels;
	unsigned peelPasses;		// translucent and punch-through layers, summed over tiles
	double prepareMs;			// triangles set up and put into tiles
	double tilesMs;				// the tiles, wall time
	double writeMs;				// the scaler's writeout, when the scaler is used
	// Processor time, when it is measured (setProfiling): of the preparation
	// and of the tiles (summed over the threads); and, at level 2, of the
	// stages inside the tiles (summed over the threads): 1 ISP opaque, 2 ISP
	// punch-through, 3 ISP translucent, 4 ISP modifier volumes, 5 TSP opaque,
	// 6 TSP punch-through, 7 TSP translucent, 8 pre-sorted translucent (ISP
	// and TSP), 9 writeout. The rest of the tiles' time is buffer clears and copies.
	double prepareCpuMs;
	double tilesCpuMs;
	double profileMs[10];
};
const Stats& lastStats();
// 0: nothing is measured but wall time. 1: the processor time of each thread
// (two clock reads a thread). 2: and of each stage of each tile (many).
void setProfiling(int level);
}
}
