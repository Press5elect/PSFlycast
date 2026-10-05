/*
	PSFlyCast - a link-time name for libSceCommonDialog.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The payload SDK has no import library for the console's common-dialog
	library, which the keyboard on the screen needs started first.
	ps5-link.sh builds this file into one: the name becomes an import from
	libSceCommonDialog in the eboot. The body is never part of the title.
*/
#include <stdint.h>

int32_t sceCommonDialogInitialize(void) { return -1; }
