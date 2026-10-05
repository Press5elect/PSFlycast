/*
	PSFlyCast - link-time names for libSceMouse.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The payload SDK has no import library for the console's mouse library.
	ps5-link.sh builds this file into one: the linker and the host tool only
	read the names, which become imports from libSceMouse in the eboot. The
	bodies are never part of the title.
*/
#include <stdint.h>

int32_t sceMouseInit(void) { return -1; }
int32_t sceMouseOpen(int32_t user, int32_t type, int32_t index, const void *parameter)
{
	(void)user; (void)type; (void)index; (void)parameter;
	return -1;
}
int32_t sceMouseRead(int32_t handle, void *data, int32_t count)
{
	(void)handle; (void)data; (void)count;
	return -1;
}
int32_t sceMouseClose(int32_t handle)
{
	(void)handle;
	return -1;
}
