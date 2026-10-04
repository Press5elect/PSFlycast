/*
	PSFlyCast - FSR 1 upscaling of the game's picture (ps5_fsr.cpp).

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "rend/vulkan/vulkan.h"

namespace ps5::fsr
{

// Before the swapchain's render pass: upscales the game's picture (view, of
// extent pixels) to the size it has on the screen (region), into an image of
// its own. fresh: the picture is a new one (not a frame presented again).
// Does nothing when the setting is off or the picture is not smaller than
// the region.
void upscale(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent,
		const vk::Rect2D& region);

// In the swapchain's render pass: draws the upscaled picture, sharpened, into
// region, moved by (shiftX, shiftY) pixels. False when there is none of this
// picture: the caller draws it its usual way.
bool present(vk::CommandBuffer commandBuffer, vk::ImageView view, vk::RenderPass renderPass, const vk::Rect2D& region,
		int shiftX, int shiftY);

// Everything is let go: the swapchain is made again, or the device goes.
void reset();

}
