/*
	PSFlyCast - swordpdf's ps5-framegen (AMD FSR3 optical flow) beside the
	title's own ps5::framegen.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Both are in a namespace ps5::framegen. The vendored one is built, unmodified,
	with `ps5` renamed ps5fg (-Dps5=ps5fg), and here is read the same way:
	ps5fg::framegen::interpolator.
*/
#pragma once
#ifndef PS5_FRAMEGEN_VULKAN_HEADER
#define PS5_FRAMEGEN_VULKAN_HEADER "ps5_vkc.h"
#endif
#define ps5 ps5fg
#include "third_party/ps5-framegen/src/ps5_framegen.h"
#undef ps5
