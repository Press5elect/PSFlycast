/*
	PSFlyCast - text from the console's own keyboard.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The system's text entry (libSceImeDialog) is opened over the interface,
	which goes on drawing behind it, and asked each frame whether the user has
	finished: nothing here waits.

	The calls, the 96-byte parameter block and the order (the common-dialog
	library first, then the dialog; its status 1 while it is open and 2 when it
	is done; the result's first field 0 when the text was accepted) are the
	ones ProsperoStore and ProsperoTV (BlackBearReloaded, GPL-3.0-or-later)
	use on the console. The payload SDK has no import for
	sceCommonDialogInitialize: ps5-link.sh makes one (runtime/stubs).
	Every failure is a line of flycast-boot.log ("keyboard: ...") and leaves
	the interface as it was.
*/
#include "ps5_frontend.h"
#include "ps5_diag.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

extern "C"
{
int sceCommonDialogInitialize(void);
int sceImeDialogInit(const void *param, const void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(void *result);
int sceImeDialogTerm(void);
int sceImeDialogAbort(void);
int sceUserServiceGetForegroundUser(int32_t *userId);
int sceUserServiceGetInitialUser(int32_t *userId);
}

namespace ps5::usb
{
int dialogModule();		// ps5_usbinput.cpp: what loading the dialog's module gave
}

namespace ps5::ime
{
namespace
{

struct Param
{
	int32_t userId, type;
	uint64_t supportedLanguages;
	int32_t enterLabel, inputMethod;
	void *filter;
	uint32_t option, maxTextLength;
	uint16_t *inputTextBuffer;
	float posX, posY;
	int32_t horizontalAlignment, verticalAlignment;
	const uint16_t *placeholder, *title;
	int8_t reserved[16];
};
static_assert(sizeof(Param) == 96, "the dialog's parameters are 96 bytes");
static_assert(offsetof(Param, inputTextBuffer) == 40, "the text buffer at 40");
static_assert(offsetof(Param, title) == 72, "the title at 72");

struct Result
{
	int32_t outcome;
	int8_t reserved[12];
};

bool ready, active, secret;
bool openedPlain;		// the keyboard now up was asked for a password and shows what is typed
int closing;		// frames the dialog has refused to be closed for
std::vector<uint16_t> buffer, titleText, placeholderText;
std::string accepted;
std::chrono::steady_clock::time_point openedAt;

// UTF-8 to UTF-16, the characters of the first plane; others become '?'.
std::vector<uint16_t> wide(const std::string& text, size_t capacity)
{
	std::vector<uint16_t> out;
	for (size_t i = 0; i < text.size() && out.size() + 1 < capacity; )
	{
		const unsigned char c = (unsigned char)text[i];
		uint32_t point = '?';
		size_t length = 1;
		if (c < 0x80)
			point = c;
		else if ((c & 0xe0) == 0xc0 && i + 1 < text.size())
		{
			point = ((c & 0x1f) << 6) | (text[i + 1] & 0x3f);
			length = 2;
		}
		else if ((c & 0xf0) == 0xe0 && i + 2 < text.size())
		{
			point = ((c & 0x0f) << 12) | ((text[i + 1] & 0x3f) << 6) | (text[i + 2] & 0x3f);
			length = 3;
		}
		else if ((c & 0xf8) == 0xf0)
			length = 4;
		out.push_back((uint16_t)point);
		i += length;
	}
	out.resize(capacity, 0);
	return out;
}

std::string narrow(const std::vector<uint16_t>& text)
{
	std::string out;
	for (const uint16_t point : text)
	{
		if (point == 0)
			break;
		if (point < 0x80)
			out += (char)point;
		else if (point < 0x800)
		{
			out += (char)(0xc0 | (point >> 6));
			out += (char)(0x80 | (point & 0x3f));
		}
		else if (point >= 0xd800 && point <= 0xdfff)
			out += '?';		// half of a pair: not a character of the first plane
		else
		{
			out += (char)(0xe0 | (point >> 12));
			out += (char)(0x80 | ((point >> 6) & 0x3f));
			out += (char)(0x80 | (point & 0x3f));
		}
	}
	return out;
}

} // namespace

bool open(const std::string& title, const std::string& placeholder, const std::string& value, size_t maxLength, Kind kind)
{
	if (active)
		return false;
	if (usb::dialogModule() < 0)
	{
		// Its calls are not there to make.
		diag::mark("keyboard: its module was not loaded (%x)", (unsigned)usb::dialogModule());
		return false;
	}
	if (!ready)
	{
		const int common = sceCommonDialogInitialize();
		// 0x80b80002: it was initialized already.
		if (common < 0 && (uint32_t)common != 0x80b80002u)
		{
			diag::mark("keyboard: the dialog library did not start (%x; its module: %x)", (unsigned)common,
					(unsigned)usb::dialogModule());
			return false;
		}
		ready = true;
	}
	Param param{};
	if (sceUserServiceGetForegroundUser(&param.userId) < 0 && sceUserServiceGetInitialUser(&param.userId) < 0)
	{
		diag::mark("keyboard: no user to open it for");
		return false;
	}
	maxLength = std::min<size_t>(std::max<size_t>(maxLength, 1), 255);
	buffer = wide(value, maxLength + 1);
	titleText = wide(title, 64);
	placeholderText = wide(placeholder, 64);
	param.enterLabel = 2;			// "Done"
	param.maxTextLength = (uint32_t)maxLength;
	param.inputTextBuffer = buffer.data();
	param.horizontalAlignment = param.verticalAlignment = 1;	// the middle of the screen
	param.placeholder = placeholderText.data();
	param.title = titleText.data();
	// The dialog's options, as the PS4's library names them: 2 no capital
	// put at the start, 4 a password (shown as dots), 0x20 nothing learned
	// from what is typed. Should this console refuse a combination, the next
	// plainer one is tried: a password's dots matter most.
	const uint32_t tries[3] = { kind == Kind::Password ? 0x26u : kind == Kind::Name ? 0x22u : 0u,
			kind == Kind::Password ? 0x04u : 0u, 0u };
	int rc = -1;
	for (int i = 0; i < 3 && rc != 0; i++)
	{
		if (i > 0 && tries[i] == tries[i - 1])
			continue;
		param.option = tries[i];
		rc = sceImeDialogInit(&param, nullptr);
		if (rc != 0 && tries[i] != 0)
			diag::mark("keyboard: it did not open with the options %x (%x)", tries[i], (unsigned)rc);
	}
	openedPlain = rc == 0 && kind == Kind::Password && (param.option & 4) == 0;
	if (openedPlain)
		diag::mark("keyboard: opened without the password option");
	secret = kind == Kind::Password;
	if (rc != 0)
	{
		diag::mark("keyboard: it did not open (%x; its module: %x)", (unsigned)rc, (unsigned)usb::dialogModule());
		return false;
	}
	active = true;
	closing = 0;
	openedAt = std::chrono::steady_clock::now();
	return true;
}

State poll()
{
	if (!active)
		return State::Idle;
	const int status = sceImeDialogGetStatus();
	// 1: open. 0 in its first second: not up yet.
	if (status == 1 || (status == 0 && std::chrono::steady_clock::now() - openedAt < std::chrono::seconds(1)))
		return State::Open;
	State state = State::Failed;
	if (status == 2)
	{
		Result result{};
		if (sceImeDialogGetResult(&result) >= 0)
		{
			state = result.outcome == 0 ? State::Accepted : State::Cancelled;
			if (state == State::Accepted)
				accepted = narrow(buffer);
		}
	}
	else if (closing == 0)
		diag::mark("keyboard: its status is %x", (unsigned)status);
	// The system still owns the dialog until this succeeds. Refused for two
	// seconds, it is given up: the interface must not wait for it for ever.
	if (const int term = sceImeDialogTerm(); term < 0 && ++closing < 240)
		return State::Open;
	else if (term < 0)
	{
		diag::mark("keyboard: it would not close (%x): given up", (unsigned)term);
		sceImeDialogAbort();
		state = State::Failed;
	}
	closing = 0;
	active = false;
	if (secret)
	{
		// What was typed stays only in what text() gives, until forget().
		std::fill(buffer.begin(), buffer.end(), 0);
		secret = false;
	}
	return state;
}

const std::string& text()
{
	return accepted;
}

bool plain()
{
	return openedPlain;
}

void forget()
{
	std::fill(accepted.begin(), accepted.end(), '\0');
	accepted.clear();
	if (!active)
		std::fill(buffer.begin(), buffer.end(), 0);
}

} // namespace ps5::ime
