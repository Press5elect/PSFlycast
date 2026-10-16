/*
	PSFlyCast - what the other frame generators (ps5_framegen_ext.cpp) take
	from the title's own (ps5_framegen.cpp): the game's picture copied into
	an image of known layout, and images to put the made pictures in.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "rend/vulkan/vulkan.h"

namespace ps5::framegen::internal
{
struct Picture
{
	vk::Image image;
	vk::ImageView view;
};

// The game's picture, drawn at its size into one of two images (index 0 or
// 1) that stay for the picture's size. It ends in ShaderReadOnlyOptimal. Only
// between show()'s start and end, with the sizes made.
Picture copyFrame(vk::CommandBuffer commandBuffer, vk::ImageView source, int index);

// The same image as the last copyFrame(…, index) wrote (the other of the two when index is the other).
Picture frame(int index);

// One of three images (R8G8B8A8, the picture's size, usable as a transfer
// destination) for a made picture. Its contents are undefined until written;
// after the writer it must be in ShaderReadOnlyOptimal.
Picture madeFrame(int index);
}

namespace ps5::framegen::ext
{
// How the pictures are made: 1 AMD FSR3's optical flow, 2 MihawkGen. (0 is the title's own.)
// Returns false when the method cannot be used (the title's own is used then). Otherwise `shown` is the picture
// for this present: one made, or `view` itself. `pictures` is how many of the game frame's presents
// show a different picture (the last of them the game's own).
bool show(vk::CommandBuffer commandBuffer, bool fresh, vk::ImageView view, const vk::Extent2D& extent, int slot, int slots,
		int pictures, int method, vk::ImageView& shown);

// The next picture has no picture before it.
void forget();

// Everything is let go (the device is, or the swapchain was made again, or the size changed).
void reset();

// Pictures made so far.
unsigned made();
}
