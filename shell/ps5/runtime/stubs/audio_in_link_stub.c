/*
	PSFlyCast - link-time names for libSceAudioIn.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The payload SDK has no import library for the console's audio input
	(the DualSense's microphone, shell/ps5/ps5_audio.cpp). ps5-link.sh builds
	this file into one: the linker and the host tool only read the names, which
	become imports from libSceAudioIn in the eboot. The bodies are never part of
	the title.
*/
#include <stdint.h>

int32_t sceAudioInOpen(int32_t user, uint32_t type, uint32_t index, uint32_t len, uint32_t freq, uint32_t param)
{
	(void)user; (void)type; (void)index; (void)len; (void)freq; (void)param;
	return -1;
}
int32_t sceAudioInInput(int32_t handle, void *dest)
{
	(void)handle; (void)dest;
	return -1;
}
int32_t sceAudioInClose(int32_t handle)
{
	(void)handle;
	return -1;
}
