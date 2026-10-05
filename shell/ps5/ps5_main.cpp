/*
	PSFlyCast - the title's entry point.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	Start-up, in order:
	  1. The crash report (shell/ps5/ps5_diag.cpp) and a notification.
	  2. The front end's options (/app0/frontend.cfg). With "USB drives" on,
	     the title asks elfldr to run its helper (shell/ps5/elevation, from
	     ps5-native-app-boilerplate), which lets this process out of its
	     sandbox: /mnt/usb0-7 and /data become readable. Nothing is asked of
	     the console otherwise.
	  3. The root folder: the title's own folder, as PS5 RetroArch keeps
	     everything in its own. In the sandbox that is /app0; outside it,
	     /data/homebrew/PPSA99247. Every file Flycast reads or writes lives
	     under it (USB game folders aside).
	  4. flycast-boot.log (each step, written as it happens) and
	     logs/flycast.log (Flycast's own log) in it.
	  5. A first-run emu.cfg with the console's defaults; the folders in it are
	     set again at every start, since the root can differ between runs.
	  6. flycast_init, then Flycast's own main loop until the user quits; the
	     shell then closes the title (a title must not exit()).
*/
#include "types.h"
#include "emulator.h"
#include "log/LogManager.h"
#include "oslib/oslib.h"
#include "oslib/i18n.h"
#include "ui/mainui.h"
#include "stdclass.h"
#include "ps5_build.h"
#include "ps5_diag.h"
#include "ps5_frontend.h"
#include "cfg/option.h"

#include <algorithm>
#include <cctype>
#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

void common_linux_setup();	// core/linux/common.cpp

extern "C"
{
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char *path, const char *const *argv);
int sceSystemServiceParamGetInt(int id, int *value);
}

namespace ps5
{
void pipelineWarmInit();	// ps5_pipelines.cpp
unsigned elevateFilesystem(const char *helperPath);	// elevation/ps5_elevate.cpp
std::string rootDir;
bool dataJailbroken;
bool elevated;
bool variableRefresh;
bool restartOnExit;
std::vector<std::string> usbDirs;

namespace
{
Options currentOptions;
std::string optionsFile;
}

Options& options()
{
	return currentOptions;
}

void loadOptions(const std::string& dir)
{
	optionsFile = dir + "frontend.cfg";
	FILE *f = fopen(optionsFile.c_str(), "r");
	if (f == nullptr)
		return;
	char key[64];
	int value;
	while (fscanf(f, " %63[^= ] = %d", key, &value) == 2)
	{
		if (!strcmp(key, "view"))
			currentOptions.view = std::clamp(value, 0, 2);
		else if (!strcmp(key, "covers"))
			currentOptions.covers = value != 0;
		else if (!strcmp(key, "usb"))
			currentOptions.usb = value != 0;
		else if (!strcmp(key, "ram_cache"))
			currentOptions.ramCache = value != 0;
		else if (!strcmp(key, "source"))
			currentOptions.source = std::clamp(value, 0, 2);
		else if (!strcmp(key, "defaults"))
			currentOptions.defaults = value;
		else if (!strcmp(key, "hz120"))
			currentOptions.hz120 = value != 0;
		else if (!strcmp(key, "group_discs"))
			currentOptions.groupDiscs = value != 0;
		else if (!strcmp(key, "splash"))
			currentOptions.splash = value != 0;
		else if (!strcmp(key, "splash_sound"))
			currentOptions.splashSound = value != 0;
		else if (!strcmp(key, "update_check"))
			currentOptions.updateCheck = value != 0;
		else if (!strcmp(key, "menu_sounds"))
			currentOptions.menuSounds = value != 0;
		else if (!strcmp(key, "light_bar"))
			currentOptions.lightBar = value != 0;
		else if (!strcmp(key, "usb_input"))
			currentOptions.usbInput = value != 0;
		else if (!strcmp(key, "vrr"))
			currentOptions.vrr = value != 0;
		else if (!strcmp(key, "notifications"))
			currentOptions.notifications = value != 0;
		else if (!strcmp(key, "skin"))
			currentOptions.skin = std::max(0, value);
		else if (!strcmp(key, "accent"))
			currentOptions.accent = std::max(0, value);
		else if (!strcmp(key, "backdrop"))
			currentOptions.backdrop = std::max(0, value);
		else if (!strcmp(key, "motion"))
			currentOptions.motion = std::clamp(value, 0, 2);
	}
	fclose(f);
}

void saveOptions()
{
	if (optionsFile.empty())
		return;
	FILE *f = fopen(optionsFile.c_str(), "w");
	if (f == nullptr)
		return;
	fprintf(f, "view = %d\ncovers = %d\nusb = %d\nram_cache = %d\nsource = %d\ndefaults = %d\nhz120 = %d\n"
			"group_discs = %d\nskin = %d\naccent = %d\nbackdrop = %d\nmotion = %d\nsplash = %d\nsplash_sound = %d\n"
			"update_check = %d\nmenu_sounds = %d\nlight_bar = %d\nusb_input = %d\nvrr = %d\nnotifications = %d\n",
			currentOptions.view, (int)currentOptions.covers, (int)currentOptions.usb, (int)currentOptions.ramCache,
			currentOptions.source, currentOptions.defaults, (int)currentOptions.hz120, (int)currentOptions.groupDiscs,
			currentOptions.skin, currentOptions.accent, currentOptions.backdrop, currentOptions.motion,
			(int)currentOptions.splash, (int)currentOptions.splashSound, (int)currentOptions.updateCheck,
			(int)currentOptions.menuSounds, (int)currentOptions.lightBar, (int)currentOptions.usbInput,
			(int)currentOptions.vrr, (int)currentOptions.notifications);
	fclose(f);
}

std::string shownRoot()
{
	if (rootDir == "/app0/")
		return "/data/homebrew/PPSA99247/";
	return rootDir;
}
}

void os_DoEvents()
{
}

void os_RunInstance(int argc, const char *argv[])
{
}

namespace
{

bool writableDir(const std::string& dir)
{
	mkdir(dir.c_str(), 0777);
	const std::string probe = dir + "/.write-test";
	FILE *f = fopen(probe.c_str(), "w");
	if (f == nullptr)
		return false;
	fclose(f);
	unlink(probe.c_str());
	return true;
}

void makeDirs(const std::string& root)
{
	// Open to everyone, so the console's FTP server can add and remove files in
	// them (as PS5 RetroArch does for its folders).
	for (const char *sub : { "", "bios", "games", "covers", "cheats", "data", "logs", "data/savestates", "data/mappings",
			"data/cheats", "data/pipelines" })
	{
		const std::string dir = root + sub;
		mkdir(dir.c_str(), 0777);
		chmod(dir.c_str(), 0777);
	}
}

// Everything Flycast keeps in its folder stays reachable over FTP, which is
// another process: folders 0777 and files 0666, whatever the umask gave them
// when they were made (a save, a log, a config written by an earlier run, as
// PS5 RetroArch repairs its folders at launch). Only what differs is changed.
// The games are the user's own files and are left alone, as are the title's.
void repairModes(const std::string& dir, int depth)
{
	DIR *list = opendir(dir.c_str());
	if (list == nullptr)
		return;
	std::vector<std::string> names;
	while (const dirent *entry = readdir(list))
	{
		const std::string name = entry->d_name;
		if (name != "." && name != "..")
			names.push_back(name);
	}
	closedir(list);
	for (const std::string& name : names)
	{
		if (depth == 0 && (name == "games" || name == "sce_sys" || name == "sce_module" || name == "eboot.bin"
				|| name == "sandbox-elevator.elf" || name == "licenses"))
			continue;
		const std::string path = dir + name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
		{
			if ((st.st_mode & 0777) != 0777)
				chmod(path.c_str(), 0777);
			if (depth < 4)
				repairModes(path + "/", depth + 1);
		}
		else if (S_ISREG(st.st_mode) && (st.st_mode & 0666) != 0666)
			chmod(path.c_str(), (st.st_mode & 0777) | 0666);
	}
}

void redirectLogs(const std::string& root)
{
	// One file, each stream opened on it in append mode, each with a 64 KiB
	// buffer a thread of its own writes out every second. A write to the
	// console's storage costs about 0.7 ms a call, and a line-buffered or
	// unbuffered stream paid it on whichever thread logged: RADV's submission
	// line every 10 s, from under its submit lock, was a visible hitch.
	// stderr is opened on the file itself, as it was up to build 9: builds 10
	// to 16 pointed its descriptor at stdout's with dup2, and on the console
	// nothing written to stderr (Flycast's own log lines, the driver's
	// messages) reached the file.
	const std::string log = root + "logs/flycast.log";
	const std::string prev = root + "logs/flycast.prev.log";
	rename(log.c_str(), prev.c_str());
	if (freopen(log.c_str(), "a", stdout) == nullptr)
		return;
	static char outBuffer[64 * 1024];
	static char errBuffer[64 * 1024];
	setvbuf(stdout, outBuffer, _IOFBF, sizeof(outBuffer));
	fflush(stderr);
	if (freopen(log.c_str(), "a", stderr) != nullptr)
		setvbuf(stderr, errBuffer, _IOFBF, sizeof(errBuffer));
	fprintf(stderr, "[stderr] Flycast's log lines and the graphics driver's messages follow in this file\n");
	std::thread([] {
		for (;;)
		{
			sleep(1);
			fflush(stdout);
			fflush(stderr);
		}
	}).detach();
}

// The console's defaults: Vulkan, 3x internal resolution, the HLE BIOS when
// no BIOS is installed, the folders above, and PS5 audio.
void writeDefaultConfig(const std::string& root)
{
	const std::string cfg = root + "emu.cfg";
	struct stat st;
	if (stat(cfg.c_str(), &st) == 0)
		return;
	FILE *f = fopen(cfg.c_str(), "w");
	if (f == nullptr)
		return;
	fprintf(f,
		"[config]\n"
		"pvr.rend = 4\n"
		"rend.Resolution = 1440\n"
		"rend.WideScreen = no\n"
		"rend.WidescreenGameHacks = no\n"
		"rend.ThreadedRendering = yes\n"
		"rend.vsync = yes\n"
		"rend.AnisotropicFiltering = 4\n"
		"rend.ShowFPS = no\n"
		"Dynarec.Enabled = yes\n"
		"Dreamcast.AutoSaveState = no\n"
		"Dreamcast.AutoLoadState = no\n"
		"Dreamcast.ContentPath = %sgames\n"
		"Dreamcast.BiosPath = %sbios\n"
		"Dreamcast.SavestatePath = %sdata/savestates\n"
		"Dreamcast.BoxartPath = %scovers\n"
		"FetchBoxart = yes\n"
		"BoxartDisplayMode = yes\n"
		"UIScaling = 100\n"
		"\n[audio]\n"
		"backend = ps5\n",
		root.c_str(), root.c_str(), root.c_str(), root.c_str());
	fclose(f);
}

bool hasFile(const std::string& path)
{
	struct stat st;
	return stat(path.c_str(), &st) == 0;
}

// The game folders on USB drives: a folder named flycast, dreamcast, dc,
// naomi, atomiswave or arcade (any case) at the top of /mnt/usb0-7. Only
// those are scanned, not whole drives.
void findUsbDirs()
{
	ps5::usbDirs.clear();
	for (int i = 0; i < 8; i++)
	{
		const std::string drive = "/mnt/usb" + std::to_string(i);
		DIR *dir = opendir(drive.c_str());
		if (dir == nullptr)
			continue;
		int entries = 0;
		while (const dirent *entry = readdir(dir))
		{
			if (entry->d_name[0] == '.')
				continue;
			entries++;
			std::string lower = entry->d_name;
			for (char& c : lower)
				c = (char)tolower((unsigned char)c);
			for (const char *wanted : { "flycast", "dreamcast", "dc", "naomi", "atomiswave", "arcade" })
				if (lower == wanted)
					ps5::usbDirs.push_back(drive + "/" + entry->d_name);
		}
		closedir(dir);
		ps5::diag::mark("usb: %s has %d entries", drive.c_str(), entries);
	}
	ps5::diag::mark("usb: %d game folder(s)", (int)ps5::usbDirs.size());
}

// The folders in emu.cfg, for this run's root: the root is /app0 in the
// sandbox and the title's real folder outside it, and USB folders come and go.
// The network folders are not among them: the library scans those with a
// scanner of their own, when the user opens the Network tab, so that starting
// Flycast does not wake a NAS.
void setFolders()
{
	const std::string& root = ps5::rootDir;
	auto ours = [](const std::string& path) {
		for (const char *prefix : { "/app0/", "/download0/", "/data/homebrew/PPSA99247/", "/mnt/sandbox/", "/mnt/usb",
				"smb://" })
			if (path.rfind(prefix, 0) == 0)
				return true;
		return false;
	};
	std::vector<std::string> content{ root + "games" };
	for (const std::string& dir : ps5::usbDirs)
		content.push_back(dir);
	for (const std::string& dir : config::ContentPath.get())
		if (!dir.empty() && !ours(dir + "/"))
			content.push_back(dir);
	config::ContentPath.set(content);
	config::BiosPath.set({ root + "bios" });
	config::SavestatePath.set({ root + "data/savestates" });
	config::BoxartPath.set(root + "covers");
	config::CheatPath.set({ root + "cheats" });
	// Descriptions, release dates and box art come from Flycast's own
	// scraper (TheGamesDB), through the console's HTTP client
	// (ps5_covers.cpp), when downloads are on; covers by file name come from
	// the front end's downloader as well.
	config::FetchBoxart.set(ps5::options().covers);
}

} // namespace

void ps5::rescanUsb()
{
	if (!options().usb || !elevated)
		return;
	findUsbDirs();
	setFolders();
}

int main(int argc, char *argv[])
{
	ps5::diag::installCrashHandler();
	ps5::diag::mark("main: PSFlyCast started (build %d)", PS5_BUILD_NUMBER);

	// The options decide whether the sandbox is left; they are read from the
	// title's folder, which the sandbox shows as /app0.
	ps5::loadOptions("/app0/");
	ps5::diag::showNotifications(ps5::options().notifications);
	// The system's modules for a USB keyboard and mouse and for the keyboard on
	// the screen, while the sandbox's paths are still this process's.
	ps5::usb::preload();
	if (ps5::options().usb)
	{
		// Before any other thread exists, as the helper's protocol asks.
		const unsigned status = ps5::elevateFilesystem("/app0/sandbox-elevator.elf");
		ps5::elevated = status == 0;
		ps5::dataJailbroken = ps5::elevated;
		ps5::diag::mark("usb: sandbox elevation %s (status %u)", ps5::elevated ? "granted" : "not granted", status);
	}
	std::vector<std::string> candidates;
	if (ps5::elevated)
	{
		// Outside the sandbox /app0 is no longer a path: the title's folder is
		// where the launcher mounted it from, or under the sandbox's mount.
		candidates.push_back("/data/homebrew/PPSA99247");
		candidates.push_back("/mnt/sandbox/PPSA99247_000/app0");
	}
	candidates.push_back("/app0");
	candidates.push_back("/download0");
	for (const std::string& candidate : candidates)
		if ((candidate == "/download0" || hasFile(candidate + "/eboot.bin")) && writableDir(candidate))
		{
			ps5::rootDir = candidate + "/";
			break;
		}
	if (ps5::rootDir.empty())
		ps5::rootDir = "/app0/";
	ps5::loadOptions(ps5::rootDir);
	ps5::diag::showNotifications(ps5::options().notifications);
	ps5::diag::open(ps5::rootDir);
	ps5::diag::mark("PSFlyCast, build %d; root folder: %s", PS5_BUILD_NUMBER, ps5::rootDir.c_str());
	makeDirs(ps5::rootDir);
	repairModes(ps5::rootDir, 0);
	// RADV's shader cache, in the root whichever path that is this run.
	setenv("MESA_SHADER_CACHE_DIR", (ps5::rootDir + "radv-shader-cache").c_str(), 1);
	if (ps5::options().usb)
	{
		findUsbDirs();
		if (!ps5::elevated)
			ps5::diag::notify("PSFlyCast: USB drives need elfldr running (port 9021). Continuing without them.");
	}
	ps5::smb::loadConfig();
	redirectLogs(ps5::rootDir);
	ps5::diag::mark("the emulator's log: %slogs/flycast.log", ps5::rootDir.c_str());

	writeDefaultConfig(ps5::rootDir);

	ps5::diag::mark("log manager");
	LogManager::Init();
	i18n::init();
	set_user_config_dir(ps5::rootDir);
	set_user_data_dir(ps5::rootDir + "data/");
	add_system_data_dir(ps5::rootDir + "bios/");

	ps5::diag::mark("hide splash screen: %d", sceSystemServiceHideSplashScreen());

	// Flycast's fault handler (guest memory pages on first use, fast memory
	// access rewriting, VRAM protection), chained to the crash report.
	ps5::diag::mark("fault handler");
	common_linux_setup();

	if (!ps5::options().hz120)
	{
		// The Vulkan driver's display code reads this before it tries the
		// 119.88 Hz mode (PS5_Mesa, wsi_common_videoout.c).
		const int set = setenv("PS5_VIDEOOUT_59HZ", "1", 1);
		ps5::diag::mark("display: 59.94 Hz asked for in the Settings (setenv %d)", set);
	}
	else if (ps5::options().vrr)
	{
		// Read by the driver once the 119.88 Hz mode is configured; it then
		// says "on" or "refused" in the same variable (vulkan_context.cpp).
		const int set = setenv("PS5_VIDEOOUT_VRR", "1", 1);
		ps5::diag::mark("display: variable refresh asked for in the Settings (setenv %d)", set);
	}
	ps5::diag::mark("flycast_init");
	if (flycast_init(argc, argv) != 0)
	{
		ps5::diag::mark("flycast_init failed");
		fflush(nullptr);
		return 1;
	}
	setFolders();
	if (ps5::options().defaults < 1)
	{
		// emu.cfg holds every setting, so a default that changes does not
		// reach a console that ran an earlier build: written once here, and
		// the user's to change after that.
		config::NativeDepthInterpolation.set(true);
		SaveSettings();
		ps5::options().defaults = 1;
		ps5::saveOptions();
		ps5::diag::mark("settings: native depth interpolation turned on (this console's graphics chip needs it)");
	}
	if (ps5::options().defaults < 2)
	{
		// Two memory cards were Flycast's default, and no rumble with them.
		if (config::MapleExpansionDevices[0][1] == MDT_SegaVMU)
		{
			config::MapleExpansionDevices[0][1].set(MDT_PurupuruPack);
			SaveSettings();
			ps5::diag::mark("settings: a rumble pack in the controller's second slot, in place of the second memory card");
		}
		ps5::options().defaults = 2;
		ps5::saveOptions();
	}
	if (ps5::options().defaults < 3)
	{
		// Flycast asks the router for a port (UPnP) before netplay by default.
		// Whether that works from a title is not known: off until it is asked for.
		config::EnableUPnP.set(false);
		SaveSettings();
		ps5::options().defaults = 3;
		ps5::saveOptions();
		ps5::diag::mark("settings: UPnP turned off (Settings > Online has it)");
	}
	if (ps5::options().defaults < 4)
	{
		// The Dreamcast's language was English whatever the console's is. Once,
		// and only while it is still English: the console's language, where the
		// Dreamcast has it (system parameter 1: 0 Japanese, 1 and 18 English,
		// 2 and 22 French, 3 and 20 Spanish, 4 German, 5 Italian).
		int language = -1;
		const int rc = sceSystemServiceParamGetInt(1, &language);
		int dreamcast = -1;
		switch (rc >= 0 ? language : -1)
		{
		case 0: dreamcast = 0; break;
		case 4: dreamcast = 2; break;
		case 2: case 22: dreamcast = 3; break;
		case 3: case 20: dreamcast = 4; break;
		case 5: dreamcast = 5; break;
		default: break;
		}
		ps5::diag::mark("settings: the console's language is %d (%x)%s", language, (unsigned)rc,
				dreamcast >= 0 && config::Language == 1 ? ": the Dreamcast's is set to it" : "");
		if (dreamcast >= 0 && config::Language == 1)
			config::Language.set(dreamcast);
		// Saved either way: this build's new options (how a light gun is
		// aimed) then have their entry in emu.cfg, which a game's own value
		// of them is kept against.
		SaveSettings();
		ps5::options().defaults = 4;
		ps5::saveOptions();
	}
	ps5::update::init();
	ps5::pipelineWarmInit();
	ps5::cheats::init();
	ps5::games::init();
	ps5::patches::init();
	ps5::drawdist::init();
	ps5::diag::mark("main loop");
	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ps5::diag::mark("main loop error: %s", e.what());
	} catch (...) {
		ps5::diag::mark("main loop: unknown exception");
	}
	ps5::diag::mark("main loop ended");
	ps5::sound::close();
	flycast_term();
	os_UninstallFaultHandler();
	fflush(nullptr);
	return 0;
}

// _start (shell/ps5/runtime/ps5_crt.cpp) calls this when main returns: the
// kernel's exit() ends a title with SIGSYS, so the shell is asked to close it,
// as PS5 RetroArch does.
extern "C" void catchReturnFromMain(int status)
{
	ps5::diag::mark("quit (status %d)", status);
	fflush(nullptr);
	if (ps5::restartOnExit)
	{
		// The title's own executable in place of this one (after an update).
		// Refused, or nothing in five seconds: closed as usual.
		const int restart = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
		ps5::diag::mark("restart request: %d", restart);
		fflush(nullptr);
		if (restart >= 0)
			usleep(5000000);
	}
	const int result = sceSystemServiceLoadExec("exit", nullptr);
	ps5::diag::mark("close request: %d", result);
	if (result >= 0)
		for (;;)
			usleep(100000);
}
