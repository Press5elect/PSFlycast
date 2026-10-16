/*
	PSFlyCast - MihawkGen's trained network, assembled into the title.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-3.0-or-later

	third_party/mihawkgen/model/network.fgw is Mihawk's network (432,308
	parameters, GPL-3.0-or-later, https://github.com/mihawk-99/PS5_MihawkGen);
	MihawkGen's own loader reads it from this blob.
*/
#include <cstddef>

#ifndef MIHAWKGEN_MODEL_PATH
#error "MIHAWKGEN_MODEL_PATH is the network's file (ps5.cmake)"
#endif

__asm__(".section .rodata\n"
		".balign 16\n"
		".global ps5_mihawkgen_model\n"
		"ps5_mihawkgen_model:\n"
		".incbin \"" MIHAWKGEN_MODEL_PATH "\"\n"
		".global ps5_mihawkgen_model_end\n"
		"ps5_mihawkgen_model_end:\n"
		".byte 0\n"
		".previous\n");

extern "C" const unsigned char ps5_mihawkgen_model[];
extern "C" const unsigned char ps5_mihawkgen_model_end[];

namespace ps5::mihawkgen
{
const void *model()
{
	return ps5_mihawkgen_model;
}

size_t modelSize()
{
	return (size_t)(ps5_mihawkgen_model_end - ps5_mihawkgen_model);
}
}
