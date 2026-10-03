/*
	Pipeline warm-up: the pipelines a game has used, remembered per game and
	built again when it next starts, before it draws with them.

	Copyright 2026 the Flycast PS5 port contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A pipeline is built the first time a game draws with a new combination of
	render states: the fragment shader is generated and compiled, then the
	driver compiles the pipeline. That takes milliseconds to tens of
	milliseconds, in the middle of a frame - a hitch. The pipeline managers
	record each combination they build (record) and, when a game starts, build
	the recorded ones in one go at the first draw (snapshot), while the game is
	still booting. The store is the platform's (shell/ps5/ps5_pipelines.cpp).
*/
#pragma once
#include "types.h"
#include "hw/pvr/ta_ctx.h"

#include <tuple>
#include <vector>

namespace pipelinewarm
{

enum Kind : u32 { Main, Rtt, Oit, OitRtt };

struct Record
{
	u32 kind, listType, sort, pass, gpuPalette, dithering;
	u32 tsp, tcw, pcw, isp, tileclip, tsp1, tcw1, naomi2;

	auto key() const {
		return std::tie(kind, listType, sort, pass, gpuPalette, dithering, tsp, tcw, pcw, isp, tileclip, tsp1, tcw1, naomi2);
	}
	bool operator<(const Record& other) const { return key() < other.key(); }
};

inline Record make(Kind kind, u32 listType, bool sort, u32 pass, int gpuPalette, bool dithering, const PolyParam& pp)
{
	return Record{ kind, listType, sort, pass, (u32)gpuPalette, dithering, pp.tsp.full, pp.tcw.full, pp.pcw.full,
		pp.isp.full, pp.tileclip, pp.tsp1.full, pp.tcw1.full, pp.isNaomi2() };
}

inline PolyParam poly(const Record& r)
{
	PolyParam pp;
	pp.init();
	pp.tsp.full = r.tsp;
	pp.tcw.full = r.tcw;
	pp.pcw.full = r.pcw;
	pp.isp.full = r.isp;
	pp.tileclip = r.tileclip;
	pp.tsp1.full = r.tsp1;
	pp.tcw1.full = r.tcw1;
	if (r.naomi2)
		pp.projMatrix = 0;
	return pp;
}

// A pipeline of this description was just built.
void record(const Record& r);
// Changes whenever the recorded set is replaced (a game starts); never 0.
u32 generation();
// The recorded pipelines of one kind.
std::vector<Record> snapshot(Kind kind);
// Warm-up of `count` pipelines took `ms`.
void report(Kind kind, unsigned count, double ms);

}
