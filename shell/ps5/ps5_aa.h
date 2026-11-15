/*
	PSFlyCast - anti-aliasing of the game's picture before it is scaled to the screen: SSAA's
	down-sampling and SMAA, as OGBox has them (OGBox by Press5elect) (ps5_aa.cpp).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
	Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect" (its
	ssaa.frag and SMAA passes are the shaders in ps5_aa.cpp).
*/
#pragma once
#include "rend/vulkan/vulkan.h"
#include "cfg/option.h"

namespace ps5::aa
{
// Settings > Video. Down-sampling, for a picture rendered larger than its place on the screen
// (Internal resolution above the screen's): 0 Bilinear (the quad's one tap, as before), 1 Soft
// (SSAA: every texel a screen pixel covers, averaged), 2 Sharp (SSAA with Lanczos-2). SMAA: SMAA
// 1x over the picture at the size it is shown (after the down-sampling), or at its own size
// before FSR stretches it.
extern config::Option<int> Downsampling;
extern config::Option<bool> Smaa;

// Before the swapchain's render pass, with the picture to show (view, of extent pixels; fresh: a
// new one) and its place on the screen (region): the picture to show instead, extent changed to
// its size. view itself when nothing applies (both off, a rotated screen, or a failure).
vk::ImageView apply(vk::CommandBuffer cmd, bool fresh, vk::ImageView view, vk::Extent2D& extent, const vk::Rect2D& region);

// Everything is let go: the swapchain is made again, or the device goes.
void reset();
}
