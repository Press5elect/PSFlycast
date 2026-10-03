/*
	PSFlyCast - the DualSense, read through libScePad.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The calls and the sample layout are the ones two PS5 projects proved on the
	console: scePadOpen for the logged-in user, retried while the pad service
	starts (PS5 RetroArch), scePadReadState, the 120-byte state with the
	connection flag at 0x4c, and scePadSetVibrationMode(2) before rumble
	(PS5SX2, whose testers found a PS5 title's pad starts in haptics mode).
*/
#include "ps5_pad.h"
#include "input/gamepad_device.h"
#include "input/mapping.h"
#include "cfg/option.h"
#include "hw/naomi/naomi_cart.h"
#include "log/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>

extern "C"
{
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
int scePadClose(int32_t handle);
int scePadSetVibration(int32_t handle, const void *param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *userId);
int sceUserServiceGetLoginUserIdList(int32_t *userIds);
int sceKernelUsleep(uint32_t microseconds);
}

namespace ps5::pad
{

config::Option<int> StickAsDpad("StickAsDpad", StickDpadAuto, "ps5");

namespace
{

struct alignas(8) PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
	uint8_t l2, r2;
	uint8_t reserved[66];
	int32_t connected;		// 0x4c
	uint64_t timestamp;		// 0x50
	uint8_t rest[256];		// the state is 120 bytes; room for a larger one
};
static_assert(offsetof(PadData, connected) == 0x4c, "connection flag at 0x4c");
static_assert(offsetof(PadData, timestamp) == 0x50, "timestamp at 0x50");

// Flycast input codes for this device: buttons are their libScePad bit index,
// axes are 0 LX, 1 LY, 2 RX, 3 RY, 4 L2, 5 R2.
enum Axis : u32 { AxisLX, AxisLY, AxisRX, AxisRY, AxisL2, AxisR2 };
constexpr u32 code(u32 bit) { return (u32)__builtin_ctz(bit); }

const struct { u32 bit; const char *name; } buttonNames[] = {
	{ Cross, "Cross" }, { Circle, "Circle" }, { Square, "Square" }, { Triangle, "Triangle" },
	{ L1, "L1" }, { R1, "R1" }, { L2, "L2" }, { R2, "R2" }, { L3, "L3" }, { R3, "R3" },
	{ Options, "OPTIONS" }, { TouchPad, "Touch pad" },
	{ Up, "D-Pad Up" }, { Down, "D-Pad Down" }, { Left, "D-Pad Left" }, { Right, "D-Pad Right" },
};
const char *axisNames[] = { "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y", "L2", "R2" };

// The DualSense's default layout. Console: Cross A, Circle B, Square X,
// Triangle Y, L2/R2 the analog triggers, L1/R1 Z and C (the six-button pad).
// Arcade: Cross Square Circle Triangle R1 L1 are buttons 1 to 6, L3 inserts a
// coin and R3 is the service button. The touch pad opens the quick menu.
template<bool Arcade>
class DualSenseMapping : public InputMapping
{
public:
	DualSenseMapping()
	{
		name = Arcade ? "DualSense (arcade)" : "DualSense";
		if constexpr (Arcade)
		{
			set_button(DC_BTN_A, code(Cross));
			set_button(DC_BTN_B, code(Square));
			set_button(DC_BTN_C, code(Circle));
			set_button(DC_BTN_X, code(Triangle));
			set_button(DC_BTN_Y, code(R1));
			set_button(DC_BTN_Z, code(L1));
			set_button(DC_BTN_D, code(L3));			// coin
			set_button(DC_DPAD2_UP, code(R3));		// service
		}
		else
		{
			set_button(DC_BTN_A, code(Cross));
			set_button(DC_BTN_B, code(Circle));
			set_button(DC_BTN_X, code(Square));
			set_button(DC_BTN_Y, code(Triangle));
			set_button(DC_BTN_Z, code(L1));
			set_button(DC_BTN_C, code(R1));
		}
		set_button(DC_BTN_START, code(Options));
		set_button(DC_DPAD_UP, code(Up));
		set_button(DC_DPAD_DOWN, code(Down));
		set_button(DC_DPAD_LEFT, code(Left));
		set_button(DC_DPAD_RIGHT, code(Right));
		set_button(EMU_BTN_MENU, code(TouchPad));

		set_axis(DC_AXIS_LEFT, AxisLX, false);
		set_axis(DC_AXIS_RIGHT, AxisLX, true);
		set_axis(DC_AXIS_UP, AxisLY, false);
		set_axis(DC_AXIS_DOWN, AxisLY, true);
		set_axis(DC_AXIS2_LEFT, AxisRX, false);
		set_axis(DC_AXIS2_RIGHT, AxisRX, true);
		set_axis(DC_AXIS2_UP, AxisRY, false);
		set_axis(DC_AXIS2_DOWN, AxisRY, true);
		set_axis(DC_AXIS_LT, AxisL2, true);
		set_axis(DC_AXIS_RT, AxisR2, true);
		addTrigger(AxisL2, false);
		addTrigger(AxisR2, false);
		dirty = false;
	}
};

class DualSenseGamepad : public GamepadDevice
{
public:
	DualSenseGamepad(int index, int32_t userId, int32_t handle)
		: GamepadDevice(index, "PS5"), userId(userId), handle(handle)
	{
		_name = "DualSense";
		_unique_id = "ps5_pad_" + std::to_string(index);
		hasAnalogStick = true;
		rumbleEnabled = true;
		loadMapping();
		if (input_mapper && input_mapper->getTriggers().empty())
		{
			input_mapper->addTrigger(AxisL2, false);
			input_mapper->addTrigger(AxisR2, false);
		}
	}

	const char *get_button_name(u32 code) override
	{
		for (const auto& b : buttonNames)
			if (pad::code(b.bit) == code)
				return b.name;
		return nullptr;
	}
	const char *get_axis_name(u32 code) override {
		return code < std::size(axisNames) ? axisNames[code] : nullptr;
	}

	void resetMappingToDefault(bool arcade, bool gamepad) override
	{
		if (arcade)
			input_mapper = std::make_shared<DualSenseMapping<true>>();
		else
			input_mapper = std::make_shared<DualSenseMapping<false>>();
	}

	void rumble(float power, float inclination, u32 durationMs) override
	{
		const float scaled = std::clamp(power * rumblePower / 100.f, 0.f, 1.f);
		rumbleLevel = (u8)std::lround(scaled * 255.f);
		rumbleInclination = inclination;
		rumbleUntil = now() + durationMs;
		rumbleStarted = now();
	}

	void update_rumble() override
	{
		u8 level = 0;
		const u64 t = now();
		if (t < rumbleUntil && rumbleLevel > 0)
		{
			float v = rumbleLevel / 255.f;
			if (rumbleInclination != 0.f)
				v = std::clamp(v + rumbleInclination * (float)(t - rumbleStarted) / 1000.f, 0.f, 1.f);
			level = (u8)std::lround(v * 255.f);
		}
		if (level != sentLevel)
		{
			// { large motor, small motor }: the large one carries the effect,
			// the small one a little of it for the DualSense's lighter feel.
			const uint8_t param[2] = { level, (uint8_t)(level / 2) };
			scePadSetVibration(handle, param);
			sentLevel = level;
		}
	}

	void stopRumble()
	{
		rumbleUntil = 0;
		update_rumble();
	}

	// Feeds Flycast what changed since the last poll. Where the left stick is
	// also the d-pad, the directions it is pushed in are d-pad buttons held:
	// they go through the layout like the d-pad's own, so a game with a digital
	// joystick (most arcade games) is played with either.
	void feed(const State& s, const PadData& raw)
	{
		u32 buttons = s.buttons;
		if (stickIsDpad())
			buttons |= stickDirections(raw.lx, raw.ly);
		else
			stickHeld = 0;
		u32 changed = buttons ^ lastButtons;
		while (changed != 0)
		{
			const u32 bit = changed & -changed;
			changed &= ~bit;
			gamepad_btn_input(pad::code(bit), (buttons & bit) != 0);
		}
		lastButtons = buttons;

		const int axes[6] = {
			stick(raw.lx), stick(raw.ly), stick(raw.rx), stick(raw.ry),
			trigger(raw.l2), trigger(raw.r2)
		};
		for (u32 i = 0; i < 6; i++)
			if (axes[i] != lastAxes[i])
			{
				gamepad_axis_input(i, axes[i]);
				lastAxes[i] = axes[i];
			}
	}

	void releaseAll()
	{
		State none;
		PadData raw{};
		raw.lx = raw.ly = raw.rx = raw.ry = 128;
		feed(none, raw);
	}

	int32_t userId;
	int32_t handle;

protected:
	std::shared_ptr<InputMapping> getDefaultMapping() override {
		return std::make_shared<DualSenseMapping<false>>();
	}

private:
	static u64 now() {
		using namespace std::chrono;
		return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
	}
	static int stick(uint8_t v) {
		return std::clamp(((int)v - 128) * 256, -32768, 32767);
	}
	static int trigger(uint8_t v) {
		return (int)v * 65535 / 255 - 32768;
	}

	// Whether the left stick is also the d-pad, for the game that is loaded.
	// Automatic: in an arcade game that reads no analog stick. A light gun
	// game aims with the stick, and a game with a wheel or a flight stick
	// reads it as one.
	static bool stickIsDpad()
	{
		switch (StickAsDpad)
		{
		case StickDpadOn:
			return true;
		case StickDpadOff:
			return false;
		default:
			break;
		}
		if (!::settings.platform.isArcade() || ::settings.input.lightgunGame)
			return false;
		const InputDescriptors *inputs = NaomiGameInputs;
		if (inputs == nullptr)
			return true;
		for (const AxisDescriptor& axis : inputs->axes)
			if (axis.name != nullptr && axis.type == Full && axis.axis <= 1)
				return false;
		return true;
	}

	// The d-pad directions the left stick is pushed in. A direction is held
	// from half way out and let go under three eighths, so a stick resting
	// near the edge of one does not flicker; at the rim that leaves each
	// diagonal 30 degrees and each of up, down, left and right 60.
	u32 stickDirections(uint8_t x, uint8_t y)
	{
		constexpr int In = 64, Out = 48;
		const auto axis = [this](int v, u32 negative, u32 positive) {
			if (v <= -In)
				stickHeld = (stickHeld & ~positive) | negative;
			else if (v >= In)
				stickHeld = (stickHeld & ~negative) | positive;
			else
			{
				if (v > -Out)
					stickHeld &= ~negative;
				if (v < Out)
					stickHeld &= ~positive;
			}
		};
		axis((int)x - 128, Left, Right);
		axis((int)y - 128, Up, Down);
		return stickHeld;
	}

	u32 stickHeld = 0;		// the d-pad directions the left stick holds
	u32 lastButtons = 0;
	int lastAxes[6] = { 0, 0, 0, 0, -32768, -32768 };
	u8 rumbleLevel = 0;
	u8 sentLevel = 0;
	float rumbleInclination = 0.f;
	u64 rumbleUntil = 0;
	u64 rumbleStarted = 0;
};

constexpr int MaxPads = 4;
std::array<std::shared_ptr<DualSenseGamepad>, MaxPads> pads;
std::array<State, MaxPads> states;
State noPad;
bool initialized;

float deadZone(float v)
{
	constexpr float dz = 0.12f;
	if (std::fabs(v) < dz)
		return 0.f;
	return (v - std::copysign(dz, v)) / (1.f - dz);
}

int32_t openPad(int32_t userId)
{
	int32_t handle = -1;
	// The pad service can publish the device after the title starts.
	for (int attempt = 0; attempt < 10 && handle < 0; attempt++)
	{
		handle = scePadOpen(userId, 0, 0, nullptr);
		if (handle < 0)
			handle = scePadGetHandle(userId, 0, 0);
		if (handle < 0)
			sceKernelUsleep(100000);
	}
	return handle;
}

} // namespace

void init()
{
	if (initialized)
		return;
	initialized = true;
	int rc = sceUserServiceInitialize(nullptr);
	INFO_LOG(INPUT, "PS5: sceUserServiceInitialize -> %x", rc);
	rc = scePadInit();
	INFO_LOG(INPUT, "PS5: scePadInit -> %x", rc);

	int32_t users[4] = { -1, -1, -1, -1 };
	int32_t initialUser = -1;
	sceUserServiceGetInitialUser(&initialUser);
	if (sceUserServiceGetLoginUserIdList(users) < 0)
		users[0] = initialUser;
	// Player 1 is the user who started the title.
	for (int i = 1; i < 4; i++)
		if (users[i] == initialUser)
			std::swap(users[0], users[i]);
	if (users[0] == -1)
		users[0] = initialUser;

	for (int i = 0; i < MaxPads; i++)
	{
		if (users[i] == -1)
			continue;
		const int32_t handle = openPad(users[i]);
		INFO_LOG(INPUT, "PS5: pad %d: user %d handle %d", i + 1, users[i], handle);
		if (handle < 0)
			continue;
		const int mode = scePadSetVibrationMode(handle, 2);	// rumble, not haptics
		INFO_LOG(INPUT, "PS5: pad %d vibration mode -> %x", i + 1, mode);
		pads[i] = std::make_shared<DualSenseGamepad>(i, users[i], handle);
		GamepadDevice::Register(pads[i]);
	}
}

void term()
{
	for (auto& pad : pads)
	{
		if (!pad)
			continue;
		pad->stopRumble();
		GamepadDevice::Unregister(pad);
		scePadClose(pad->handle);
		pad.reset();
	}
	initialized = false;
}

void poll()
{
	for (int i = 0; i < MaxPads; i++)
	{
		auto& pad = pads[i];
		State& s = states[i];
		const u32 before = s.buttons;
		if (!pad)
		{
			s = State();
			continue;
		}
		PadData raw{};
		const int rc = scePadReadState(pad->handle, &raw);
		if (rc < 0 || raw.connected == 0 || (raw.buttons & Intercepted) != 0)
		{
			// Disconnected, or the system menu has the pad: nothing is held,
			// so no button sticks down.
			if (s.connected || s.buttons != 0)
				pad->releaseAll();
			s = State();
			s.connected = rc >= 0 && raw.connected != 0;
			s.released = before;
			continue;
		}
		s.connected = true;
		s.buttons = raw.buttons & ~Intercepted;
		s.pressed = s.buttons & ~before;
		s.released = before & ~s.buttons;
		s.lx = deadZone((raw.lx - 128) / 127.5f);
		s.ly = deadZone((raw.ly - 128) / 127.5f);
		s.rx = deadZone((raw.rx - 128) / 127.5f);
		s.ry = deadZone((raw.ry - 128) / 127.5f);
		s.l2 = raw.l2 / 255.f;
		s.r2 = raw.r2 / 255.f;
		pad->feed(s, raw);
		pad->update_rumble();
	}
}

const State& player1()
{
	for (const auto& s : states)
		if (s.connected)
			return s;
	return noPad;
}

int connectedCount()
{
	int n = 0;
	for (const auto& s : states)
		n += s.connected;
	return n;
}

}
