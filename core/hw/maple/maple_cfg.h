#pragma once
#include <string>
#include "types.h"
#include <cstring>
#include <memory>

enum MapleDeviceType
{
	MDT_SegaController   =  0,
	MDT_SegaVMU          =  1,
	MDT_Microphone       =  2,
	MDT_PurupuruPack     =  3,
	MDT_AsciiStick       =  4,
	MDT_Keyboard         =  5,
	MDT_Mouse            =  6,
	MDT_LightGun         =  7,
	MDT_TwinStick        =  8,
	MDT_NaomiJamma       =  9,
	MDT_None             = 10,
	MDT_RFIDReaderWriter = 11,
	MDT_MaracasController       = 12,
	MDT_FishingController       = 13,
	MDT_PopnMusicController     = 14,
	MDT_RacingController        = 15,
	MDT_DenshaDeGoController    = 16,
	MDT_Dreameye                = 17,
	MDT_SegaControllerXL        = 18,
	MDT_DreamParaParaController = 19,
	MDT_WccfCamera				= 20,
	MDT_Count
};

enum PlainJoystickAxisId
{
	PJAI_X1 = 0,
	PJAI_Y1 = 1,
	PJAI_X2 = 2,
	PJAI_Y2 = 3,
	PJAI_X3 = 4,
	PJAI_Y3 = 5,
	PJAI_Count = 6
};

enum PlainJoystickTriggerId
{
	PJTI_L = 0,
	PJTI_R = 1,
	PJTI_L2 = 2,
	PJTI_R2 = 3,
	PJTI_Count = 4
};

enum class MaplePortType {
	user_accessible,
	built_in,
};

struct PlainJoystickState
{
	PlainJoystickState()
	{
		u32 i;
		for (i=0; i < PJAI_Count; i++)
			joy[i] = 0x80;
		for (i=0; i < PJTI_Count; i++)
			trigger[i] = 0;
	}

	u32 kcode = ~0;

	u8 joy[PJAI_Count];
	u8 trigger[PJTI_Count];
};

struct maple_device;

class MapleConfigMap
{
public:
	MapleConfigMap(maple_device* dev) : dev(dev) {}
	void SetVibration(float power, float inclination, u32 duration_ms);
	void GetInput(PlainJoystickState* pjs);
	void GetAbsCoordinates(int& x, int& y);
	void GetMouseInput(u8& buttons, int& x, int& y, int& wheel);
	void GetKeyboardInput(u8& shift, u8 keys[6]);
	void SetImage(u8 *img);

	static void (*UpdateVibration)(u32 port, float power, float inclination, u32 duration_ms);
	bool invertMouseY = false;

private:
	u32 playerNum();

	maple_device* dev;
};

struct MapleInputState
{
	MapleInputState() : halfAxes{}, fullAxes{} {
		memset(keyboard.key, 0, sizeof(keyboard.key));
	}

	u32 kcode = ~0;
	u16 halfAxes[PJTI_Count];		// LT, RT, 2, 3
	int16_t fullAxes[PJAI_Count];	// Left X, Y, Right X, Y, Other X, Other Y
	u8 mouseButtons = ~0;
	struct {
		int x = -1;
		int y = -1;
	} absPos;
	struct {
		int16_t x = 0;
		int16_t y = 0;
		int16_t wheel = 0;
	} relPos;
	struct {
		u8 shift = 0;				// modifier keys bitmask
		u8 key[6];					// normal keys pressed
	} keyboard;
};
extern MapleInputState mapleInputState[4];

void mcfg_CreateDevices();
void mcfg_DestroyDevices(bool full = true);
void mcfg_SerializeDevices(Serializer& ser);
void mcfg_DeserializeDevices(Deserializer& deser);

/*
	Pulling a memory card (VMU) or vibration pack out of a controller and putting
	one back while a game runs (Dreamcast only).

	port is the controller port, 0 to 3 (A to D); slot the controller's expansion
	slot, 0 or 1. The controller there must have that slot.

	maple_unplug_expansion takes the device out: the slot reports no device from
	the next Maple frame on, as when the card is pulled. A VMU's file is closed,
	having been written as the game wrote each block. It returns false when the
	slot is empty or not one a controller there has.

	maple_plug_expansion puts a new device of the type given (MDT_SegaVMU or
	MDT_PurupuruPack) into an empty slot. It is attached only once the game has
	sent at least one Maple frame to that port since the slot emptied and a tenth
	of a second has gone by, so the game always sees the slot empty first, even
	when the two calls come together; until then it is pending. A VMU opens the
	file hostfs::getVmuPath() names for that slot at that moment, so a frontend
	swaps cards by changing that file (or the settings that choose it) while the
	slot is empty. The new VMU's clock starts from the host's time. It returns
	false when the slot is taken, the slot or type is not valid, or netplay is on.

	Neither is kept in the configuration: mcfg_CreateDevices (a game starting, a
	device setting changed) builds every port from the configuration again, which
	puts back the cards it lists and drops a pending plug. Loading a save state
	sets the slots to what they were when it was saved and also drops a pending
	plug.

	Threads: call these on the emulator thread (emu.run(), or a hook that runs
	there), or while the emulator is stopped. They change MapleDevices, which the
	Maple DMA on the emulator thread reads without a lock.
*/
bool maple_unplug_expansion(int port, int slot);
bool maple_plug_expansion(int port, int slot, MapleDeviceType type, const std::string& vmuFile = {});
// The card file a VMU plugged in by maple_plug_expansion opens in place of the
// slot's usual one (empty: the usual one). Read by the VMU when it is set up.
std::string maple_vmuFileFor(int port, int slot);
// The type of the device in an expansion slot, or MDT_None when it is empty
// (a pending plug counts as empty).
MapleDeviceType maple_expansion_device(int port, int slot);
// Attaches the pending plugs that are due; maple_vblank calls it.
void maple_handlePendingPlugs();

constexpr int maple_getPortCount(MapleDeviceType type, MaplePortType port)
{
	switch (type)
	{
		case MDT_FishingController:
			return port == MaplePortType::user_accessible ? 0 : 1;
		case MDT_SegaController:
		case MDT_SegaControllerXL:
			return 2;
		case MDT_LightGun:
		case MDT_TwinStick:
		case MDT_AsciiStick:
		case MDT_RacingController:
			return 1;
		default:
			return 0;
	}
}

bool maple_atomiswave_coin_chute(int slot);
void push_vmu_screen(int bus_id, int bus_port, u8* buffer);
void insertRfidCard(int playerNum);
const u8 *getRfidCardData(int playerNum);
void setRfidCardData(int playerNum, u8 *data);

struct MIE;
std::shared_ptr<MIE> getMieDevice();
