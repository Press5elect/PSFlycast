/*
	PSFlyCast - what the Vulkan renderer asks of the graphics chip, counted.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Render passes begun, pipeline barriers and image layout changes, counted
	at the renderer's few places that record them (drawer.cpp, oit/, texture.cpp,
	vulkan_context.cpp). The counts only grow; ps5_perf.cpp reads them at the
	start and the end of each second of play and puts what a frame took in the
	boot log's speed, slow and session lines. No Vulkan in here, so the front
	end reads them without the renderer's headers.
*/
#pragma once
#include <atomic>
#include <cstdint>

namespace vkcount
{
inline std::atomic<uint64_t> renderPasses{0};
inline std::atomic<uint64_t> barriers{0};
inline std::atomic<uint64_t> layouts{0};

inline void pass() { renderPasses.fetch_add(1, std::memory_order_relaxed); }
inline void barrier() { barriers.fetch_add(1, std::memory_order_relaxed); }
// A barrier that also moves an image to another layout.
inline void layout() { barrier(); layouts.fetch_add(1, std::memory_order_relaxed); }

struct Snapshot
{
	uint64_t passes = 0, barriers = 0, layouts = 0;
};
inline Snapshot now()
{
	return { renderPasses.load(std::memory_order_relaxed), barriers.load(std::memory_order_relaxed),
			layouts.load(std::memory_order_relaxed) };
}
}
