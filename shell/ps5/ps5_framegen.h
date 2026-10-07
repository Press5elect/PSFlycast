/*
	PSFlyCast - frame generation: pictures in between the game's own.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A game that draws 30 pictures a second is shown twice each on a 60 Hz
	display (four times at 119.88 Hz), and one that draws 60 is shown twice at
	119.88 Hz. With "Frame generation" on, the presents that would repeat a
	picture show one made in between the last two instead: the way every part
	of the picture moved from one to the next is estimated, and both are moved
	part of that way and mixed. The game's own picture is then shown last of
	its presents, so it reaches the screen later than without: by half a game
	frame when one picture is made, three quarters when three are.

	It is PSFlyCast's own, and experimental: what moves in a way that is not
	found (too fast, or uncovered from behind something) is mixed in place,
	which shows as a ghost.
*/
#pragma once
#ifdef FRAMEGEN_TEST
#include <vulkan/vulkan.hpp>
#else
#include "rend/vulkan/vulkan.h"
#endif

namespace ps5::framegen
{
// "Frame generation" is on in the settings (and nothing here has failed).
bool wanted();

// One of a game frame's presents: `slot` of `slots`, in order, the game's own
// picture `view` (of `extent`) being the one to show last. `fresh` is true
// the first time a picture is given. Records what it needs into the command
// buffer, outside any render pass, and returns the picture to show in this
// present: one made in between, or `view` itself.
vk::ImageView show(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent, int slot,
		int slots);

// Pictures made so far.
unsigned made();

// Everything is let go (the device is, or the swapchain was made again).
void reset();

#ifdef FRAMEGEN_TEST
// For the test on the build machine: the made picture, and the way found.
struct TestImage { vk::Image image; vk::Extent2D extent; vk::Format format; };
TestImage testOutput();
TestImage testFlow();
#endif
}
