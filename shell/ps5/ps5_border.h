/*
	PSFlyCast - what fills the screen beside (or above and below) a game's
	picture that does not fit it: the way PSSwanStation's "Beside the picture"
	does (ps5_border.cpp).

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "rend/vulkan/vulkan.h"

namespace ps5::border
{

// Before the swapchain's render pass, with the picture that is to be shown (shown, of extent
// pixels; fresh: a new one): makes the light of the picture (blurred, small) or loads the
// picture of your own. Does nothing while the setting is Black.
void prepare(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView shown, const vk::Extent2D& extent);

// In the swapchain's render pass, before the game is drawn: fills the bars outside `picture`
// (the game's rectangle on a screen of width x height). Does nothing while the setting is Black.
void draw(vk::CommandBuffer commandBuffer, vk::RenderPass renderPass, int width, int height, const vk::Rect2D& picture);

// The interface's accent colour (0xRRGGBB), for the gradient.
void setAccent(unsigned rgb);

// Everything is let go: the swapchain is made again, or the device goes.
void reset();

}
