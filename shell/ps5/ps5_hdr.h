/*
	PSFlyCast - HDR output: the picture to an HDR TV as HDR10 (BT.2020 colours, the PQ curve),
	from OGBox 1.0.0 (OGBox by Press5elect): its shader's maths and the driver patch
	(shell/ps5/mesa/0007) as they are.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later
	Additional terms (GPL-3.0 section 7(b)): keep the attribution "OGBox by Press5elect" (its
	hdr.frag is the shader in ps5_hdr.cpp).

	How a frame goes, while HDR is on (the setting, an HDR output, a UNORM swapchain):
	  1. the swapchain's render pass draws into two layers of its own format instead of the
	     framebuffer: the game's picture (with what is beside it), then the menus (the
	     overlays and ImGui), over a clear of nothing, so the menus' cover is their alpha;
	  2. a pass of its own makes HDR10's pixels of both into an A2R10G10B10 image: gamma 2.2,
	     BT.709 to BT.2020, white at the white level, the picture's luminance above 0.5 raised to
	     the highlights' peak, the menus kept at the white;
	  3. that image is copied into the framebuffer, whose pixels the driver has retyped as HDR10
	     (wsi_videoout_set_hdr10): its bits are what VideoOut reads.
	Off (the default), none of this runs and the frame is drawn as before.
*/
#pragma once
#include "rend/vulkan/vulkan.h"
#include "cfg/option.h"
#include <string>

namespace ps5::hdr
{
extern config::Option<bool> Enabled;	// Settings > Video > HDR, off
extern config::Option<int> White;		// the white level, nits (203, BT.2408's reference)
extern config::Option<int> Peak;		// the highlights' peak, nits; 0 for none

// At the start of each frame's render pass: whether this frame is drawn in HDR. Turns the
// framebuffers to HDR10 or back when that changes (after the GPU's work so far).
bool frameStart(vk::Format presentFormat, u32 width, u32 height);

// In HDR, the render pass on the picture's layer (cleared to the border's colour) and then on
// the menus' (cleared to nothing), instead of the framebuffer.
void beginPicture(vk::CommandBuffer cmd, vk::RenderPass renderPass, u32 width, u32 height, const vk::ClearValue& border);
void beginMenus(vk::CommandBuffer cmd, vk::RenderPass renderPass, u32 width, u32 height);
// In HDR, at the frame's end: the menus' pass ended, the HDR10 pixels made and copied into the
// framebuffer, and a pass begun on it that keeps them, for the caller to end as usual.
void finish(vk::CommandBuffer cmd, vk::Image swapImage, vk::ImageView swapView, vk::Format presentFormat, u32 width,
		u32 height);

// Settings > Video > Adjust HDR brightness: 0 none, 1 the highlights' square with an X at value
// nits, 2 the white level's grey scale at value nits.
void setCalibration(int step, int value);

// What HDR is doing, for the Settings ("On", "The console sends SDR", ...).
std::string status();

// The swapchain is made again, or the device goes.
void reset();
}
