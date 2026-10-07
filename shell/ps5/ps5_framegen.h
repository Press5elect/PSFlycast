/*
	PSFlyCast - frame generation: pictures in between the game's own.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	A game that draws 30 pictures a second is shown twice each on a 60 Hz
	display (four times at 119.88 Hz), and one that draws 60 is shown twice at
	119.88 Hz. With "Frame generation" on, presents that would repeat a
	picture show one made in between the last two instead: the way every part
	of the picture moved from one to the next is estimated, and both are moved
	part of that way and mixed.

	Light makes one picture between two of the game's, whatever the display:
	half of a game frame's presents show it, the other half the game's own.
	Full makes one for every present that would repeat: three in between at
	119.88 Hz for a game of 30. The game's own picture is shown last of its
	presents, so it reaches the screen later than without: by half a game
	frame with Light or with one picture made, by three quarters with three.

	It is PSFlyCast's own, and experimental: what moves in a way that is not
	found (too fast, or uncovered from behind something) is shown from the
	nearer of the game's two pictures as it is, so that part of the picture
	moves no more smoothly than without; where a wrong way is taken for a
	right one, it shows as a smear.
*/
#pragma once
#ifdef FRAMEGEN_TEST
#include <vulkan/vulkan.hpp>
#else
#include "rend/vulkan/vulkan.h"
#endif

namespace ps5::framegen
{
// "Frame generation" is Light or Full in the settings (and nothing here has failed).
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
// For the test on the build machine: the made picture, the way found, and
// how far it is not trusted.
struct TestImage { vk::Image image; vk::Extent2D extent; vk::Format format; };
TestImage testOutput();
TestImage testFlow();
TestImage testTrust();
#endif
}
