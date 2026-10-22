/*
	PSFlyCast - the DualSense, read through libScePad.

	Copyright 2026 Press5elect and the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The calls and the sample layout are the ones two PS5 projects proved on the
	console: scePadOpen for the logged-in user, retried while the pad service
	starts (PS5 RetroArch), scePadReadState, the 120-byte state with the
	connection flag at 0x4c, and scePadSetVibrationMode(2) before rumble
	(PS5SX2, whose testers found a PS5 title's pad starts in haptics mode).
*/
#include "ps5_pad.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "input/mouse.h"
#include "emulator.h"
#include "input/gamepad_device.h"
#include "input/mapping.h"
#include "cfg/option.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/naomi/naomi_cart.h"
#include "network/ggpo.h"
#include "oslib/oslib.h"
#include "ui/gui.h"
#include "ui/settings.h"
#include "log/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

extern "C"
{
int scePadInit(void);
int scePadOpen(int32_t userId, int32_t type, int32_t index, const void *param);
int scePadGetHandle(int32_t userId, int32_t type, int32_t index);
int scePadReadState(int32_t handle, void *data);
int scePadClose(int32_t handle);
int scePadSetVibration(int32_t handle, const void *param);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetLightBar(int32_t handle, const void *colour);
int scePadResetLightBar(int32_t handle);
int scePadSetMotionSensorState(int32_t handle, bool enabled);
int scePadGetControllerInformation(int32_t handle, void *information);
int sceUserServiceInitialize(const void *params);
int sceUserServiceGetInitialUser(int32_t *userId);
int sceUserServiceGetLoginUserIdList(int32_t *userIds);
int sceKernelUsleep(uint32_t microseconds);
}

namespace ps5::pad
{

config::Option<int> StickAsDpad("StickAsDpad", StickDpadAuto, "ps5");
config::Option<int> LightGunAim("LightGunAim", AimStick, "ps5");
config::Option<int> MotionAimDirection("MotionAimDirection", 0, "ps5");
config::Option<int> TurboButtons("TurboButtons", 0, "ps5");
config::Option<int> TurboRate("TurboRate", 1, "ps5");
config::Option<int> PadProfile("PadProfile", 0, "ps5");
config::Option<int> PadMouseSpeed("PadMouseSpeed", 4, "ps5");
config::Option<int> PadMouseDeadZone("PadMouseDeadZone", 15, "ps5");
config::Option<int> PadMouseCurve("PadMouseCurve", 1, "ps5");
config::Option<int> PadSmoothing("PadSmoothing", 0, "ps5");
config::Option<int> PadAxisX("PadAxisX", AxisNormal, "ps5");
config::Option<int> PadAxisY("PadAxisY", AxisNormal, "ps5");
config::Option<int> PadType("PadType", PadTypeStandard, "ps5");
config::Option<int> PadRightTurns("PadRightTurns", 0, "ps5");
config::Option<int> PadRightTriggers("PadRightTriggers", 0, "ps5");

namespace
{

// The pad's state, as scePadReadState gives it: the layout PS5 RetroArch and
// PS5SX2 read the buttons, sticks and connection flag from, and ProsperoLight
// (BlackBearReloaded) the motion sensor and the touch pad.
struct alignas(8) PadData
{
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
	uint8_t l2, r2;
	uint8_t pad0[2];
	float orientation[4];			// 0x0c
	float acceleration[3];			// 0x1c, in g
	float angularVelocity[3];		// 0x28, in radians a second
	uint8_t touchCount;				// 0x34
	uint8_t pad1[7];
	struct
	{
		uint16_t x, y;
		uint8_t id;
		uint8_t pad[3];
	} touch[2];						// 0x3c
	int32_t connected;				// 0x4c
	uint64_t timestamp;				// 0x50, in microseconds
	uint8_t rest[256];				// the state is 120 bytes; room for a larger one
};
static_assert(offsetof(PadData, acceleration) == 0x1c, "acceleration at 0x1c");
static_assert(offsetof(PadData, angularVelocity) == 0x28, "angular velocity at 0x28");
static_assert(offsetof(PadData, touchCount) == 0x34, "touch count at 0x34");
static_assert(offsetof(PadData, touch) == 0x3c, "touches at 0x3c");
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
	void feed(const State& s, const PadData& rawIn)
	{
		u32 buttons = s.buttons;
		// What the mouse and keyboard have of this pad is not the controller's as well.
		PadData raw = rawIn;
		if (maple_port() == 0)
		{
			const ps5::usb::Taken taken = ps5::usb::padTaken();
			buttons &= ~taken.buttons;
			if (taken.l2) { raw.l2 = 0; buttons &= ~L2; }
			if (taken.r2) { raw.r2 = 0; buttons &= ~R2; }
			if (taken.lx) raw.lx = 128;
			if (taken.ly) raw.ly = 128;
		}
		// Both stick clicks together start, and then split, a speedrun (the
		// timer is in the quick menu); only the first player's pad does.
		if (maple_port() == 0 && (s.buttons & (L3 | R3)) == (L3 | R3) && (s.pressed & (L3 | R3)) != 0
				&& ps5::speedrun::loaded())
			ps5::speedrun::startOrSplit();
		if (stickIsDpad())
			buttons |= stickDirections(raw.lx, raw.ly);
		else
			stickHeld = 0;
		// Turbo: a held button of the chosen ones is up for every other half
		// period. The clock is the console's, so every button beats together.
		const u32 turbo = (u32)TurboButtons & (Cross | Circle | Square | Triangle | L1 | R1);
		if (turbo != 0 && (buttons & turbo) != 0)
		{
			static const u64 halfMs[3] = { 71, 50, 33 };
			if ((now() / halfMs[std::clamp((int)TurboRate, 0, 2)]) % 2 == 1)
				buttons &= ~turbo;
		}
		u32 changed = buttons ^ lastButtons;
		while (changed != 0)
		{
			const u32 bit = changed & -changed;
			changed &= ~bit;
			gamepad_btn_input(pad::code(bit), (buttons & bit) != 0);
		}
		lastButtons = buttons;

		int axes[6] = {
			stick(raw.lx), stick(raw.ly), stick(raw.rx), stick(raw.ry),
			trigger(raw.l2), trigger(raw.r2)
		};
		// The right stick, as the settings shape it (Settings > Controls): s.px and s.py.
		const float rightX = s.px, rightY = s.py;
		axes[2] = (int)std::lround(std::clamp(rightX, -1.f, 1.f) * 32767.f);
		axes[3] = (int)std::lround(std::clamp(rightY, -1.f, 1.f) * 32767.f);
		// The right stick, left and right, is the Dreamcast's L and R triggers, where the game
		// is asked to (a camera turned with the triggers): as far as the stick is pushed, so a
		// half push is a half press, and L2 and R2 still work (the further pressed counts). Not
		// while the right stick is the mouse.
		const bool asTriggers = PadRightTriggers != 0 && ::settings.platform.isConsole() && !bind::current().mouse;
		if (asTriggers)
		{
			const int left = (int)std::lround(std::clamp(-rightX, 0.f, 1.f) * 65535.f) - 32768;
			const int right = (int)std::lround(std::clamp(rightX, 0.f, 1.f) * 65535.f) - 32768;
			axes[4] = std::max(axes[4], left);
			axes[5] = std::max(axes[5], right);
			axes[2] = 0;	// and not a second stick as well
		}
		// The right stick turns as the left one does, where the game is asked to.
		else if (PadRightTurns != 0 && ::settings.platform.isConsole())
			axes[0] = std::clamp(axes[0] + (int)std::lround(rightX * 32767.f), -32768, 32767);
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

	// A light-gun game's gun follows the touch pad or the pad's motion, when
	// that is chosen (the left stick moves it otherwise, which is Flycast's
	// own). The gun's place is in the game's 640 x 480, as the stick gives it.
	//  - Touch pad: the pad is the screen; the gun stays where the finger left.
	//  - Motion: turning the pad left and right and tilting it up and down
	//    moves the gun, a screen's width for about 40 degrees. It starts in
	//    the middle, and a finger on the touch pad puts it back there.
	// The gun can leave the picture a little, which is how many of these
	// games reload.
	void aim(const PadData& raw)
	{
		const int port = maple_port();
		const bool gunGame = ::settings.input.lightgunGame
				|| (::settings.platform.isConsole() && port >= 0 && port < 4 && config::MapleMainDevices[port] == MDT_LightGun);
		if (LightGunAim == AimStick || port < 0 || port >= 4 || !gunGame)
		{
			aiming = false;
			return;
		}
		if (!aiming)
		{
			aiming = true;
			aimX = 320;
			aimY = 240;
			aimedAt = raw.timestamp;
		}
		if (LightGunAim == AimTouch)
		{
			if (raw.touchCount > 0)
			{
				aimX = raw.touch[0].x * 639.f / std::max(1, touchWidth - 1);
				aimY = raw.touch[0].y * 479.f / std::max(1, touchHeight - 1);
			}
		}
		else
		{
			const float dt = std::clamp((float)((double)(raw.timestamp - aimedAt) / 1e6), 0.f, 0.05f);
			if (raw.touchCount > 0)
			{
				aimX = 320;
				aimY = 240;
			}
			else
			{
				// Turning to the left is a positive turn about the pad's
				// upward axis, tilting its top toward the player one about
				// its rightward axis. 917 pixels a radian: 640 for 40 degrees.
				const float sideways = (MotionAimDirection & 1) != 0 ? 1.f : -1.f;
				const float upward = (MotionAimDirection & 2) != 0 ? 1.f : -1.f;
				aimX += sideways * raw.angularVelocity[1] * dt * 917.f;
				aimY += upward * raw.angularVelocity[0] * dt * 917.f;
			}
		}
		aimedAt = raw.timestamp;
		aimX = std::clamp(aimX, -24.f, 663.f);
		aimY = std::clamp(aimY, -18.f, 497.f);
		mo_x_abs[port] = (s32)std::lround(aimX);
		mo_y_abs[port] = (s32)std::lround(aimY);
	}

	// The light bar in the player's colour, as the console's own games show
	// who is who: blue, red, green, pink. Or the console's own again.
	void lightBar(bool players)
	{
		const int wanted = players ? maple_port() + 1 : 0;
		if (wanted == lightBarShown)
			return;
		lightBarShown = wanted;
		int rc;
		if (wanted >= 1 && wanted <= 4)
		{
			static const uint8_t colours[4][4] = { { 0, 0, 255, 0 }, { 255, 0, 0, 0 }, { 0, 255, 0, 0 }, { 255, 0, 255, 0 } };
			rc = scePadSetLightBar(handle, colours[wanted - 1]);
		}
		else
			rc = scePadResetLightBar(handle);
		if (rc != 0)
			ps5::diag::mark("pads: player %d's light bar: %x", maple_port() + 1, rc);
	}

	// The motion sensor on, and the touch pad's size.
	void sensors()
	{
		const int motion = scePadSetMotionSensorState(handle, true);
		alignas(8) uint8_t information[256] = {};
		const int rc = scePadGetControllerInformation(handle, information);
		uint16_t width = 0, height = 0;
		memcpy(&width, information + 4, 2);
		memcpy(&height, information + 6, 2);
		if (rc >= 0 && width > 1 && height > 1)
		{
			touchWidth = width;
			touchHeight = height;
		}
		ps5::diag::mark("pads: player %d: motion sensor %x, touch pad %d x %d (%x)", maple_port() + 1, motion, touchWidth,
				touchHeight, rc);
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

	bool aiming = false;	// a light gun follows the touch pad or the motion sensor
	float aimX = 320, aimY = 240;
	uint64_t aimedAt = 0;
	int touchWidth = 1920, touchHeight = 1080;
	int lightBarShown = -1;	// 0 the console's own, 1 to 4 a player's colour
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

int32_t openPad(int32_t userId, int attempts)
{
	int32_t handle = -1;
	// The pad service can publish the device after the title starts.
	for (int attempt = 0; attempt < attempts && handle < 0; attempt++)
	{
		handle = scePadOpen(userId, 0, 0, nullptr);
		if (handle < 0)
			handle = scePadGetHandle(userId, 0, 0);
		if (handle < 0 && attempt + 1 < attempts)
			sceKernelUsleep(100000);
	}
	return handle;
}

// Which ports a pad is on, bit by bit: the emulator's thread reads it when it
// makes the emulated controllers (plugPorts).
std::atomic<u32> portsPresent;

const char *const portLetters[MaxPads] = { "A", "B", "C", "D" };

// A signed-in user: their pad's handle, and the port it has once the
// controller is on. A console often has users signed in whose controllers
// are off; they take no port.
struct Seat
{
	int32_t userId = -1;
	int32_t handle = -1;
	int port = -1;
};
std::array<Seat, MaxPads> seats;

// The pad of a user is a port's from now on.
void addPad(Seat& seat, int port)
{
	const int mode = scePadSetVibrationMode(seat.handle, 2);	// rumble, not haptics
	ps5::diag::mark("pads: player %d (port %s): user %d, handle %d, vibration mode %x", port + 1, portLetters[port],
			seat.userId, seat.handle, mode);
	seat.port = port;
	pads[port] = std::make_shared<DualSenseGamepad>(port, seat.userId, seat.handle);
	pads[port]->sensors();
	GamepadDevice::Register(pads[port]);
	portsPresent |= 1u << port;
}

// A user signed out: nothing of theirs stays held, and the port is free.
void removeSeat(Seat& seat)
{
	if (seat.port >= 0)
	{
		auto& pad = pads[seat.port];
		ps5::diag::mark("pads: player %d (port %s) left: user %d signed out", seat.port + 1, portLetters[seat.port],
				seat.userId);
		pad->releaseAll();
		pad->stopRumble();
		GamepadDevice::Unregister(pad);
		pad.reset();
		states[seat.port] = State();
		portsPresent &= ~(1u << seat.port);
	}
	if (seat.handle >= 0)
		scePadClose(seat.handle);
	seat = Seat();
}

bool controllerIsOn(int32_t handle)
{
	PadData raw{};
	return scePadReadState(handle, &raw) >= 0 && raw.connected != 0;
}

// A pad joined on a port the running Dreamcast game has no controller in:
// the game's controllers are made again, as when they are changed in the
// settings, which the emulator has to stand still for. An arcade game reads
// every port as it is.
void plugIntoRunningGame(int port)
{
	if (!game_started || !::settings.platform.isConsole() || MapleDevices[port][5] != nullptr)
		return;
	if (ggpo::active())
	{
		ps5::diag::mark("pads: port %s stays as it is during netplay", portLetters[port]);
		return;
	}
	try {
		if (emu.running())
		{
			emu.stop();
			maple_ReconnectDevices();
			emu.start();
		}
		else
			maple_ReconnectDevices();
		ps5::diag::mark("pads: a controller was plugged into port %s of the running game", portLetters[port]);
	} catch (const std::exception& e) {
		ps5::diag::mark("pads: plugging into the running game failed: %s", e.what());
	}
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

	// Player 1 is the user who started the title: port A, whether or not the
	// controller answers yet, since the menus are theirs. The pad is waited
	// for. Everyone else is found by hotplug(), from the first frame on.
	int32_t initialUser = -1;
	sceUserServiceGetInitialUser(&initialUser);
	if (initialUser == -1)
	{
		int32_t users[4] = { -1, -1, -1, -1 };
		if (sceUserServiceGetLoginUserIdList(users) >= 0)
			initialUser = users[0];
	}
	if (initialUser == -1)
	{
		ps5::diag::mark("pads: no signed-in user was found");
		return;
	}
	const int32_t handle = openPad(initialUser, 10);
	if (handle < 0)
	{
		ps5::diag::mark("pads: no pad yet for user %d (%x)", initialUser, handle);
		return;
	}
	seats[0].userId = initialUser;
	seats[0].handle = handle;
	addPad(seats[0], 0);
}

void term()
{
	for (Seat& seat : seats)
	{
		if (seat.port >= 0)
		{
			pads[seat.port]->stopRumble();
			GamepadDevice::Unregister(pads[seat.port]);
			pads[seat.port].reset();
		}
		if (seat.handle >= 0)
			scePadClose(seat.handle);
		seat = Seat();
	}
	portsPresent = 0;
	initialized = false;
}

void hotplug()
{
	// Not during netplay, whose own thread reads the pads: who plays is
	// settled when it starts.
	if (!initialized || ggpo::active())
		return;
	using clock = std::chrono::steady_clock;
	static clock::time_point next;
	const clock::time_point now = clock::now();
	if (now < next)
		return;
	next = now + std::chrono::seconds(1);

	int32_t users[4] = { -1, -1, -1, -1 };
	if (sceUserServiceGetLoginUserIdList(users) < 0)
		return;
	// Who signed out.
	for (Seat& seat : seats)
		if (seat.userId != -1 && std::find(std::begin(users), std::end(users), seat.userId) == std::end(users))
		{
			const int port = seat.port;
			removeSeat(seat);
			if (port >= 0)
				os_notify(("Player " + std::to_string(port + 1) + " left").c_str(), 3000, nullptr);
		}
	// Who signed in: a seat, and their pad's handle (asked for again each
	// second until the pad service gives it).
	for (const int32_t user : users)
	{
		if (user == -1)
			continue;
		Seat *seat = nullptr, *free = nullptr;
		for (Seat& candidate : seats)
		{
			if (candidate.userId == user)
				seat = &candidate;
			else if (candidate.userId == -1 && free == nullptr)
				free = &candidate;
		}
		if (seat == nullptr)
		{
			if (free == nullptr)
				continue;
			seat = free;
			seat->userId = user;
			ps5::diag::mark("pads: user %d is signed in", user);
		}
		if (seat->handle < 0)
			seat->handle = openPad(user, 1);
	}
	// Whose controller is on, and has no port yet: the lowest free one.
	for (Seat& seat : seats)
	{
		if (seat.handle < 0 || seat.port >= 0 || !controllerIsOn(seat.handle))
			continue;
		int port = -1;
		for (int candidate = MaxPads - 1; candidate >= 0; candidate--)
			if (!pads[candidate])
				port = candidate;
		if (port < 0)
			continue;
		addPad(seat, port);
		os_notify(("Player " + std::to_string(port + 1) + " joined").c_str(), 3000,
				(std::string("Controller port ") + portLetters[port]).c_str());
		if (port > 0)
			plugIntoRunningGame(port);
	}
}

void plugPorts()
{
	u32 present = portsPresent;
	// Netplay's second player is port B's, on both consoles alike.
	if (config::GGPOEnable)
		present |= 1u << 1;
	for (int port = 1; port < MaxPads; port++)
	{
		if ((present & (1u << port)) == 0 || config::MapleMainDevices[port] != MDT_None)
			continue;
		// Not written to the settings: for as long as this game is loaded.
		config::MapleMainDevices[port].override(MDT_SegaController);
		if (config::MapleExpansionDevices[port][0] == MDT_None)
			config::MapleExpansionDevices[port][0].override(MDT_SegaVMU);
		if (config::MapleExpansionDevices[port][1] == MDT_None)
			config::MapleExpansionDevices[port][1].override(MDT_PurupuruPack);
		ps5::diag::mark("pads: port %s gets a controller, a memory card and a rumble pack for player %d",
				portLetters[port], port + 1);
	}
	// The controller with a second stick, where the game is asked to have it.
	if (PadType == PadTypeSecondStick && ::settings.platform.isConsole())
		for (int port = 0; port < MaxPads; port++)
			if ((present & (1u << port)) != 0 || port == 0)
				if (config::MapleMainDevices[port] == MDT_SegaController)
				{
					config::MapleMainDevices[port].override(MDT_SegaControllerXL);
					ps5::diag::mark("pads: port %s has the controller with a second stick", portLetters[port]);
				}
	// A USB keyboard and mouse take the ports after the pads'.
	ps5::usb::plugPorts(present | 1u);
}

std::string portsText()
{
	std::string ports;
	int count = 0;
	for (int port = 0; port < MaxPads; port++)
		if (pads[port] && states[port].connected)
		{
			ports += (count == 0 ? "" : ", ") + std::string(portLetters[port]);
			count++;
		}
	if (count == 0)
		return "None";
	const size_t last = ports.rfind(", ");
	if (last != std::string::npos)
		ports.replace(last, 2, " and ");
	return std::to_string(count) + (count == 1 ? ": port " : ": ports ") + ports;
}

// The right stick for one pad: round the centre its dead zone, then the response curve, then
// smoothing over the frames, then each direction normal, inverted or off. Used by the second
// stick, by turning with it, and by the mouse.
void shapeRight(int pad, State& s)
{
	static float keptX[MaxPads], keptY[MaxPads];
	const float length = std::min(1.f, std::hypot(s.rx, s.ry));
	const float zone = std::clamp((int)PadMouseDeadZone, 0, 60) / 100.f;
	float pushed = length > zone ? (length - zone) / (1.f - zone) : 0.f;
	switch (PadMouseCurve)
	{
	case CurvePrecise:
		pushed *= pushed;
		break;
	case CurveAggressive:
		pushed = std::sqrt(pushed);
		break;
	case CurveS:
		pushed = pushed * pushed * (3.f - 2.f * pushed);
		break;
	default:
		break;
	}
	float x = length > 0.f ? s.rx / length * pushed : 0.f;
	float y = length > 0.f ? s.ry / length * pushed : 0.f;
	const float keep = std::clamp((int)PadSmoothing, 0, 90) / 100.f;
	x = keptX[pad] = keptX[pad] * keep + x * (1.f - keep);
	y = keptY[pad] = keptY[pad] * keep + y * (1.f - keep);
	if (std::fabs(x) < 0.002f) x = keptX[pad] = 0.f;
	if (std::fabs(y) < 0.002f) y = keptY[pad] = 0.f;
	s.px = PadAxisX == AxisOff ? 0.f : PadAxisX == AxisInverted ? -x : x;
	s.py = PadAxisY == AxisOff ? 0.f : PadAxisY == AxisInverted ? -y : y;
}

void poll()
{
	ps5::usb::poll();
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
		shapeRight(i, s);
		s.l2 = raw.l2 / 255.f;
		s.r2 = raw.r2 / 255.f;
		pad->feed(s, raw);
		pad->aim(raw);
		pad->lightBar(ps5::options().lightBar);
		pad->update_rumble();
	}
	// The first pad is also a mouse and a keyboard, when the game asks for it.
	ps5::usb::padInput(player1());
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


int userOfPort(int port)
{
	for (const Seat& seat : seats)
		if (seat.port == port && seat.userId != -1)
			return seat.userId;
	for (const Seat& seat : seats)
		if (seat.port >= 0 && seat.userId != -1)
			return seat.userId;
	return -1;
}

}


// ---- what the buttons do when the pad is a mouse and a keyboard

// ---- the profiles: what the pad is in the Dreamcast's ports beside its controller
namespace ps5::pad::bind
{

namespace
{

// The player's own profiles, in the settings for every game: "m,a0,a1,...,a19".
config::Option<std::string, false> ownText[Own] = {
	{ "PadProfile1", "", "ps5" }, { "PadProfile2", "", "ps5" }, { "PadProfile3", "", "ps5" }, { "PadProfile4", "", "ps5" },
};

// The entries, in the order the bindings page lists them.
struct InputInfo
{
	const char *name;
	uint32_t button;
};
const InputInfo inputs[Inputs] = {
	{ "Cross", Cross }, { "Circle", Circle }, { "Square", Square }, { "Triangle", Triangle },
	{ "L1", L1 }, { "R1", R1 }, { "L2", 0 }, { "R2", 0 }, { "L3", L3 }, { "R3", R3 },
	{ "OPTIONS", ps5::pad::Options }, { "Touch pad", TouchPad },
	{ "D-pad up", Up }, { "D-pad right", Right }, { "D-pad down", Down }, { "D-pad left", Left },
	{ "Left stick up", 0 }, { "Left stick down", 0 }, { "Left stick left", 0 }, { "Left stick right", 0 },
};
enum { InCross = 0, InCircle, InSquare, InTriangle, InL1, InR1, InL2, InR2, InL3, InR3, InOptions, InTouch,
	InUp, InRight, InDown, InLeft, InStickUp, InStickDown, InStickLeft, InStickRight };

const char *const names[Profiles] = {
	"Controller", "Controller and mouse", "Mouse and keyboard, WASD", "Mouse and keyboard, arrows",
	"Custom 1", "Custom 2", "Custom 3", "Custom 4",
};

int key(int usage)
{
	return Key + usage;
}

Profile builtIn(int index)
{
	Profile p;
	if (index <= 0)
		return p;
	p.mouse = true;
	if (index == 1)
		return p;
	// USB usage codes: A 04, D 07, E 08, S 16, W 1a, R 15, C 06, 1 to 4 1e to 21, Escape 29,
	// Space 2c, right, left, down, up arrows 4f to 52, Control e0, Shift e1, Alt e2.
	const bool wasd = index == 2;
	p.action[InR2] = MouseLeft;
	p.action[InL2] = MouseRight;
	p.action[InR3] = MouseMiddle;
	p.action[InR1] = WheelUp;				// the next weapon
	p.action[InL1] = WheelDown;				// the one before
	p.action[InStickUp] = key(wasd ? 0x1a : 0x52);
	p.action[InStickDown] = key(wasd ? 0x16 : 0x51);
	p.action[InStickLeft] = key(wasd ? 0x04 : 0x50);
	p.action[InStickRight] = key(wasd ? 0x07 : 0x4f);
	p.action[InCross] = key(0x2c);							// Space: jump
	p.action[InCircle] = key(wasd ? 0x06 : 0xe0);			// C (Control): crouch
	p.action[InSquare] = key(wasd ? 0x08 : 0xe2);			// E (Alt): use
	p.action[InTriangle] = key(0x15);						// R: reload
	p.action[InL3] = key(0xe1);								// Shift: walk
	p.action[InOptions] = key(0x29);						// Escape
	p.action[InUp] = key(0x1e);								// 1 to 4: the weapons
	p.action[InRight] = key(0x1f);
	p.action[InDown] = key(0x20);
	p.action[InLeft] = key(0x21);
	return p;
}

bool parse(const std::string& text, Profile& out)
{
	Profile p;
	size_t at = 0;
	for (int i = -1; i < Inputs; i++)
	{
		if (at >= text.size())
			return false;
		char *end = nullptr;
		const long v = strtol(text.c_str() + at, &end, 10);
		if (end == text.c_str() + at || v < 0 || v > Key + 255)
			return false;
		if (i < 0)
			p.mouse = v != 0;
		else
			p.action[i] = (int)v;
		at = end - text.c_str();
		if (at < text.size() && text[at] == ',')
			at++;
	}
	out = p;
	return true;
}

std::string unparse(const Profile& p)
{
	std::string text = p.mouse ? "1" : "0";
	for (int i = 0; i < Inputs; i++)
		text += "," + std::to_string(p.action[i]);
	return text;
}

std::vector<KeyName> makeKeys()
{
	std::vector<KeyName> k;
	static const char *const letters[] = { "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
			"N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z" };
	for (int i = 0; i < 26; i++)
		k.push_back({ letters[i], 0x04 + i });
	static const char *const digits[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" };
	for (int i = 0; i < 10; i++)
		k.push_back({ digits[i], 0x1e + i });
	k.push_back({ "Space", 0x2c });
	k.push_back({ "Enter", 0x28 });
	k.push_back({ "Escape", 0x29 });
	k.push_back({ "Tab", 0x2b });
	k.push_back({ "Backspace", 0x2a });
	k.push_back({ "Up arrow", 0x52 });
	k.push_back({ "Down arrow", 0x51 });
	k.push_back({ "Left arrow", 0x50 });
	k.push_back({ "Right arrow", 0x4f });
	k.push_back({ "Left Control", 0xe0 });
	k.push_back({ "Left Shift", 0xe1 });
	k.push_back({ "Left Alt", 0xe2 });
	k.push_back({ "Right Control", 0xe4 });
	k.push_back({ "Right Shift", 0xe5 });
	k.push_back({ "Right Alt", 0xe6 });
	static const char *const fn[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12" };
	for (int i = 0; i < 12; i++)
		k.push_back({ fn[i], 0x3a + i });
	k.push_back({ "Insert", 0x49 });
	k.push_back({ "Delete", 0x4c });
	k.push_back({ "Home", 0x4a });
	k.push_back({ "End", 0x4d });
	k.push_back({ "Page up", 0x4b });
	k.push_back({ "Page down", 0x4e });
	k.push_back({ "Caps Lock", 0x39 });
	k.push_back({ "Minus -", 0x2d });
	k.push_back({ "Equals =", 0x2e });
	k.push_back({ "Left bracket [", 0x2f });
	k.push_back({ "Right bracket ]", 0x30 });
	k.push_back({ "Backslash", 0x31 });
	k.push_back({ "Semicolon ;", 0x33 });
	k.push_back({ "Apostrophe '", 0x34 });
	k.push_back({ "Grave `", 0x35 });
	k.push_back({ "Comma ,", 0x36 });
	k.push_back({ "Period .", 0x37 });
	k.push_back({ "Slash /", 0x38 });
	return k;
}

} // namespace

const char *profileName(int index)
{
	return index >= 0 && index < Profiles ? names[index] : "";
}

bool profileIsOwn(int index)
{
	return index >= BuiltIn && index < Profiles;
}

Profile profile(int index)
{
	if (!profileIsOwn(index))
		return builtIn(std::clamp(index, 0, BuiltIn - 1));
	Profile p;
	if (parse(ownText[index - BuiltIn].get(), p))
		return p;
	return Profile{};
}

void setProfile(int index, const Profile& p)
{
	if (profileIsOwn(index))
		ownText[index - BuiltIn].set(unparse(p));
}

int chosen()
{
	return std::clamp((int)PadProfile, 0, Profiles - 1);
}

Profile current()
{
	return profile(chosen());
}

bool wantsMouse(const Profile& p)
{
	if (p.mouse)
		return true;
	for (int a : p.action)
		if (a >= MouseLeft && a <= WheelDown)
			return true;
	return false;
}

bool wantsKeyboard(const Profile& p)
{
	for (int a : p.action)
		if (a >= Key)
			return true;
	return false;
}

const char *inputName(int input)
{
	return input >= 0 && input < Inputs ? inputs[input].name : "";
}

uint32_t inputButton(int input)
{
	return input >= 0 && input < Inputs ? inputs[input].button : 0;
}

bool inputHeld(int input, const State& s)
{
	switch (input)
	{
	case InL2: return s.l2 > 0.5f;
	case InR2: return s.r2 > 0.5f;
	case InStickUp: return s.ly < -0.5f;
	case InStickDown: return s.ly > 0.5f;
	case InStickLeft: return s.lx < -0.5f;
	case InStickRight: return s.lx > 0.5f;
	default: return input >= 0 && input < Inputs && (s.buttons & inputs[input].button) != 0;
	}
}

const std::vector<KeyName>& keys()
{
	static const std::vector<KeyName> k = makeKeys();
	return k;
}

std::string actionName(int action)
{
	switch (action)
	{
	case Controller: return "Controller";
	case Blocked: return "Nothing";
	case MouseLeft: return "Mouse left button";
	case MouseRight: return "Mouse right button";
	case MouseMiddle: return "Mouse middle button";
	case WheelUp: return "Mouse wheel up";
	case WheelDown: return "Mouse wheel down";
	}
	if (action >= Key)
		for (const KeyName& k : keys())
			if (k.code == action - Key)
				return std::string("Key ") + k.name;
	return "Controller";
}

}
