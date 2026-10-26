#pragma once

#include "types.h"
#include "hw/flashrom/flashrom.h"

void reios_init();
void reios_set_flash(MemChip* flash);
void reios_reset(u8* rom);
void reios_serialize(Serializer& ser);
void reios_deserialize(Deserializer& deser);
void reios_term();

struct Sh4Context;
void DYNACALL reios_trap(Sh4Context *ctx, u32 op);

void reios_disk_id();

struct ip_meta_t
{
	char hardware_id[16];
	char maker_id[16];
	char ks[5];
	char disk_type[6];
	char disk_num[5];
	char area_symbols[8];
	char ctrl[4];
	char dev;
	char vga;
	char wince;
	char _unk1;
	char product_number[10];
	char product_version[6];
	char release_date[8];
	char _unk2[8];
	char boot_filename[16];
	char software_company[16];
	char software_name[128];

	bool isWindowsCE() { return wince == '1' || memcmp("0WINCEOS.BIN", boot_filename, 12) == 0; }
	bool supportsVGA() { return vga == '1'; }
	bool isGDROM() const { return memcmp("GD-ROM", disk_type, 6) == 0; }
	bool isMILCD() const { return memcmp("MIL CD", disk_type, 6) == 0; }
	// Whether this is a Dreamcast disc's IP.BIN
	bool isDreamcast() const { return memcmp("SEGA SEGAKATANA ", hardware_id, 16) == 0; }
	// The disc's number in its set and the number of discs, from "1/2  ":
	// 0 when not given
	int discNumber() const { return disk_num[0] >= '1' && disk_num[0] <= '9' && disk_num[1] == '/' ? disk_num[0] - '0' : 0; }
	int discCount() const { return disk_num[1] == '/' && disk_num[2] >= '1' && disk_num[2] <= '9' ? disk_num[2] - '0' : 0; }
};
extern ip_meta_t ip_meta;

// The IP.BIN of the disc in the drive. It is read again whenever a disc is
// inserted (Emulator::insertGdrom) or the lid is opened (all zero then).
const ip_meta_t& currentDiscMeta();
// Reads the IP.BIN of a disc image without inserting it, for a frontend
// looking for the other discs of the running game. False if the image can't
// be opened or isn't a Dreamcast disc.
bool readDiscMeta(const std::string& path, ip_meta_t& meta);
// Whether two discs are discs of one game's set: Dreamcast discs with the same
// number of discs (2 or more), and the same product number or name.
bool sameDiscSet(const ip_meta_t& a, const ip_meta_t& b);

#define REIOS_OPCODE 0x085B
