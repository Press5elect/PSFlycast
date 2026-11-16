/*
	PSFlyCast - the title's entry point.

	Copyright 2026 Press5elect and the PSFlyCast contributors
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
#include "ps5_disc.h"
#include "ps5_folders.h"
#include "oslib/storage.h"
#include "ps5_rewind.h"
#include "ps5_timing.h"
#include "ps5_thread_layout.h"
#include "bigpicture.h"
#include "cfg/option.h"
#include <ps5platform/heap.h>

#include <algorithm>
#include <cctype>
#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
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
		else if (!strcmp(key, "disc_drive"))
			currentOptions.discDrive = value != 0;
		else if (!strcmp(key, "disc_speed"))
			currentOptions.discSpeed = std::clamp(value, 0, 2);
		else if (!strcmp(key, "ram_cache"))
			currentOptions.ramCache = value != 0;
		else if (!strcmp(key, "net_buffers"))
			currentOptions.netBuffers = value != 0;
		else if (!strcmp(key, "logs"))
			currentOptions.logs = value != 0;
		else if (!strcmp(key, "log_place"))
			currentOptions.logPlace = std::clamp(value, 0, 1);
		else if (!strcmp(key, "log_sync"))
			currentOptions.logSync = std::clamp(value, 0, 3);
		else if (!strcmp(key, "source"))
			currentOptions.source = std::clamp(value, 0, 4);
		else if (!strcmp(key, "defaults"))
			currentOptions.defaults = value;
		else if (!strcmp(key, "hz120"))
			currentOptions.hz120 = value != 0;
		else if (!strcmp(key, "group_discs"))
			currentOptions.groupDiscs = value != 0;
		else if (!strcmp(key, "show_hidden"))
			currentOptions.showHidden = value != 0;
		else if (!strcmp(key, "rewind"))
			currentOptions.rewind = value != 0;
		else if (!strcmp(key, "splash"))
			currentOptions.splash = std::clamp(value, 0, 5);
		else if (!strcmp(key, "splash_last"))
			currentOptions.splashLast = value;
		else if (!strcmp(key, "splash_sound"))
			currentOptions.splashSound = value != 0;
		else if (!strcmp(key, "update_check"))
			currentOptions.updateCheck = value != 0;
		else if (!strcmp(key, "menu_sounds"))
			currentOptions.menuSounds = value != 0;
		else if (!strcmp(key, "light_bar"))
			currentOptions.lightBar = value != 0;
		else if (!strncmp(key, "quick_", 6) && key[6] >= '0' && key[6] <= '9')
		{
			const int kind = atoi(key + 6);
			if (kind >= 0 && kind < Options::QuickKinds)
				currentOptions.quickPlace[kind] = std::clamp(value, -1, 2 * Options::QuickKinds);
		}
		else if (!strncmp(key, "pin_", 4) && key[4] >= '0' && key[4] <= '9')
		{
			const int slot = atoi(key + 4);
			if (slot >= 0 && slot < Options::Pins)
				currentOptions.pins[slot] = value;
		}
		else if (!strcmp(key, "classic_ui"))
			currentOptions.classicUi = value != 0;
		else if (!strcmp(key, "start_page"))
			currentOptions.startPage = std::clamp(value, 0, 1);
		else if (!strcmp(key, "menu_vibration"))
			currentOptions.menuVibration = value != 0;
		else if (!strcmp(key, "circle_confirms"))
			currentOptions.circleConfirms = value != 0;
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
		else if (!strcmp(key, "ui_scale"))
			currentOptions.uiScale = std::clamp(value, 70, 130);
		else if (!strcmp(key, "high_contrast"))
			currentOptions.highContrast = value != 0;
		else if (!strcmp(key, "colour_blind"))
			currentOptions.colourBlind = value != 0;
		else if (!strcmp(key, "threaded_recording"))
			currentOptions.threadedRecording = value != 0;
		else if (!strcmp(key, "gpu_wait_spin_us"))
			currentOptions.gpuWaitSpinUs = std::clamp(value, 0, 10000);
		else if (!strcmp(key, "gpu_wait_sleep_us"))
			currentOptions.gpuWaitSleepUs = std::clamp(value, 100, 10000);
		else if (!strcmp(key, "short_queue"))
			currentOptions.shortQueue = value != 0;
	}
	fclose(f);
}

void saveOptions()
{
	if (optionsFile.empty())
		return;
	char text[2048];
	int length = snprintf(text, sizeof(text), "view = %d\ncovers = %d\nusb = %d\nram_cache = %d\nsource = %d\ndefaults = %d\nhz120 = %d\n"
			"group_discs = %d\nskin = %d\naccent = %d\nbackdrop = %d\nmotion = %d\nsplash = %d\nsplash_sound = %d\n"
			"update_check = %d\nmenu_sounds = %d\nlight_bar = %d\nusb_input = %d\nvrr = %d\nnotifications = %d\n"
			"show_hidden = %d\nrewind = %d\nsplash_last = %d\nui_scale = %d\nhigh_contrast = %d\ncolour_blind = %d\n"
			"disc_drive = %d\ndisc_speed = %d\nthreaded_recording = %d\ngpu_wait_spin_us = %d\ngpu_wait_sleep_us = %d\n"
			"short_queue = %d\nnet_buffers = %d\nlogs = %d\nlog_place = %d\nlog_sync = %d\n"
			"menu_vibration = %d\ncircle_confirms = %d\nstart_page = %d\nclassic_ui = %d\n",
			currentOptions.view, (int)currentOptions.covers, (int)currentOptions.usb, (int)currentOptions.ramCache,
			currentOptions.source, currentOptions.defaults, (int)currentOptions.hz120, (int)currentOptions.groupDiscs,
			currentOptions.skin, currentOptions.accent, currentOptions.backdrop, currentOptions.motion,
			(int)currentOptions.splash, (int)currentOptions.splashSound, (int)currentOptions.updateCheck,
			(int)currentOptions.menuSounds, (int)currentOptions.lightBar, (int)currentOptions.usbInput,
			(int)currentOptions.vrr, (int)currentOptions.notifications, (int)currentOptions.showHidden,
			(int)currentOptions.rewind, currentOptions.splashLast, currentOptions.uiScale,
			(int)currentOptions.highContrast, (int)currentOptions.colourBlind, (int)currentOptions.discDrive,
			currentOptions.discSpeed, (int)currentOptions.threadedRecording, currentOptions.gpuWaitSpinUs,
			currentOptions.gpuWaitSleepUs, (int)currentOptions.shortQueue, (int)currentOptions.netBuffers,
			(int)currentOptions.logs, currentOptions.logPlace, currentOptions.logSync,
			(int)currentOptions.menuVibration, (int)currentOptions.circleConfirms, currentOptions.startPage, (int)currentOptions.classicUi);
	// The quick menu's arrangement: only the kinds that were moved or hidden.
	int used = length;
	for (int kind = 0; kind < Options::QuickKinds && used > 0 && used < (int)sizeof(text); kind++)
		if (currentOptions.quickPlace[kind] >= 0)
			used += snprintf(text + used, sizeof(text) - used, "quick_%d = %d\n", kind, currentOptions.quickPlace[kind]);
	for (int pin = 0; pin < Options::Pins && used > 0 && used < (int)sizeof(text); pin++)
		if (currentOptions.pins[pin] != 0)
			used += snprintf(text + used, sizeof(text) - used, "pin_%d = %d\n", pin, currentOptions.pins[pin]);
	length = used;
	// Not written again when it reads as it did at the last write (a toggle back, a setting
	// chosen twice): the drive's writes are not spent on a file that stays as it is.
	static std::string lastSaved;
	if (length <= 0 || length >= (int)sizeof(text) || lastSaved.compare(0, std::string::npos, text, length) == 0)
		return;
	FILE *f = fopen(optionsFile.c_str(), "w");
	if (f == nullptr)
		return;
	const bool written = fwrite(text, 1, length, f) == (size_t)length;
	fclose(f);
	if (written)
		lastSaved.assign(text, length);
}

std::string shownRoot()
{
	if (rootDir == "/app0/")
		return "/data/homebrew/PPSA99247/";
	return rootDir;
}
}

// Once a frame, on the interface's thread, whether a game runs or a menu is up.
void os_DoEvents()
{
	// The time a game is played is counted while the emulator runs.
	ps5::library::tick();
	// A game that went back to the console's menu goes back to the library.
	bigpicture::returnedToMenuCheck();
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
			"data/cheats", "data/pipelines", "vmu" })
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

// Settings > System > Logs, "On a USB drive": /mnt/usbN/PSFlyCast/logs/ on the first drive that can
// be written to. Empty when the console is not elevated (USB drives are outside the sandbox) or no
// drive takes a file.
std::string usbLogFolder()
{
	if (!ps5::elevated)
		return "";
	for (int i = 0; i < 8; i++)
	{
		const std::string drive = "/mnt/usb" + std::to_string(i);
		struct stat st;
		if (stat(drive.c_str(), &st) != 0)
			continue;
		mkdir((drive + "/PSFlyCast").c_str(), 0777);
		const std::string logs = drive + "/PSFlyCast/logs";
		if (writableDir(logs))
			return logs + "/";
	}
	return "";
}

void redirectLogs(const std::string& dir)
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
	const std::string log = dir + "flycast.log";
	const std::string prev = dir + "flycast.prev.log";
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

// The title heap (the payload SDK's ps5platform/heap.h, where every allocation of the title and
// the driver goes) hands a segment back to the console only whole, and only when 32 MiB lie
// free at an arena's top: what a burst took (shaders compiled at a game's start, a disc read
// into memory, a save state) stays mapped, out of the direct memory the GPU and the emulator
// share. ps5_heap_trim, every 10 s as the SDK asks, gives it back. A line in the boot log when
// a trim gave back 16 MiB or more, so a console run shows whether it does anything here.
void startHeapTrim()
{
#ifdef PS5_HEAP_TRIM
	std::thread([] {
		for (;;)
		{
			sleep(10);
			struct ps5_heap_stats before{};
			ps5_heap_stats(&before);
			ps5_heap_trim();
			struct ps5_heap_stats after{};
			ps5_heap_stats(&after);
			if (before.mapped_bytes >= after.mapped_bytes + (16u << 20))
				ps5::diag::mark("heap: trimmed %zu MiB to %zu MiB mapped (%zu MiB in use)",
						before.mapped_bytes >> 20, after.mapped_bytes >> 20, after.in_use_bytes >> 20);
		}
	}).detach();
#endif
}

// The console's defaults, as Press5elect chose them: transparency sorted per
// pixel (96 layers) at 4x the Dreamcast's resolution,
// 16x anisotropic and linear texture filtering, the sound DSP, a 64 ms audio
// buffer, the RGB cable, a save state when a game stops; the HLE BIOS only
// when no BIOS is installed (Flycast falls back to it by itself), the folders
// above, and PS5 audio. The rest is the v1.3.3 settings update in main().
// Written only when there is no emu.cfg yet.
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
		"pvr.rend = 5\n"
		"rend.Resolution = 1920\n"
		"rend.WideScreen = no\n"
		"rend.WidescreenGameHacks = no\n"
		"rend.ThreadedRendering = yes\n"
		"rend.vsync = yes\n"
		"rend.AnisotropicFiltering = 16\n"
		"rend.ShowFPS = no\n"
		"aica.DSPEnabled = yes\n"
		"Dynarec.Enabled = yes\n"
		"Dreamcast.AutoSaveState = yes\n"
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
	// The game folders chosen at the last call are ours too: one taken off since must not be kept
	// because ContentPath still has it.
	static std::vector<std::string> appliedGames;
	for (const std::string& dir : config::ContentPath.get())
		if (!dir.empty() && !ours(dir + "/") && std::find(appliedGames.begin(), appliedGames.end(), dir) == appliedGames.end())
			content.push_back(dir);
	// The folders the player added (Settings > Games and saves > Folders): their games join the
	// library where they are. A folder on a drive that is not in is left out until it is.
	for (const std::string& dir : ps5::folders::settings().games)
		if (std::find(content.begin(), content.end(), dir) == content.end())
			content.push_back(dir);
	appliedGames = ps5::folders::settings().games;
	config::ContentPath.set(content);
	// Where each kind of file is, as chosen; without a choice the folder it has always had. A
	// chosen folder that is not there (a drive pulled out) is left out and said in the log.
	ps5::folders::refresh();
	using ps5::folders::Kind;
	const auto kept = [](Kind kind) {
		const ps5::folders::Resolved r = ps5::folders::resolveNow(kind);
		for (const std::string& gone : r.missing)
			ps5::diag::mark("folders: %s: %s is not there%s", ps5::folders::info(kind).label, gone.c_str(),
					r.askFirst ? " (it is where this is written: a game asks before it starts)" : "");
		return r.use;
	};
	config::BiosPath.set(kept(Kind::Bios));
	config::SavestatePath.set(kept(Kind::States));
	config::BoxartPath.set(ps5::folders::writeFolder(Kind::Covers));
	config::CheatPath.set(kept(Kind::Cheats));
	// Texture packs: the chosen folders, or none (Flycast then looks in its own data folder); a
	// folder chosen earlier and saved in emu.cfg does not outlive its removal.
	config::TexturePath.set(ps5::folders::settings().kept[Kind::Textures].empty() ? std::vector<std::string>()
			: kept(Kind::Textures));
	// Descriptions, release dates and box art come from Flycast's own
	// scraper (TheGamesDB), through the console's HTTP client
	// (ps5_covers.cpp), when downloads are on; covers by file name come from
	// the front end's downloader as well.
	config::FetchBoxart.set(ps5::options().covers);
}

} // namespace

void ps5::applyFolders()
{
	setFolders();
}

void ps5::rescanUsb()
{
	if (!options().usb || !elevated)
		return;
	findUsbDirs();
	setFolders();
}

// Where the title could keep what is the user's (games, saves, settings),
// for the boot log: of each folder a title may be given, whether it is there
// and whether a file can be made in it. Nothing is kept there yet; these
// lines are what a later build goes by.
static void probeStorage()
{
	for (const char *dir : { "/app0", "/download0", "/data", "/data/homebrew", "/user/data" })
	{
		struct stat st{};
		if (stat(dir, &st) != 0)
		{
			ps5::diag::mark("storage: %s: not there (errno %d)", dir, errno);
			continue;
		}
		const std::string probe = std::string(dir) + "/.psflycast-probe";
		FILE *f = fopen(probe.c_str(), "wb");
		const int writeError = f == nullptr ? errno : 0;
		if (f != nullptr)
		{
			fclose(f);
			unlink(probe.c_str());
		}
		ps5::diag::mark("storage: %s: device %llx, %s", dir, (unsigned long long)st.st_dev,
				writeError == 0 ? "writable" : ("not writable (errno " + std::to_string(writeError) + ")").c_str());
	}
}

namespace ps5
{
LaunchRequest launchRequest;
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
	if (ps5::options().usb || ps5::options().discDrive)
	{
		// Before any other thread exists, as the helper's protocol asks.
		const unsigned status = ps5::elevateFilesystem("/app0/sandbox-elevator.elf");
		// 0: granted with the root's references counted (the helper trades its own with this
		// process's, as OGBox fixed it); 11: granted the old way, when the helper's own root was
		// not the console's, so this process's exit releases two references on the console's root
		// that it never took, and enough such exits bring the console down.
		ps5::elevated = status == 0 || status == 11;
		ps5::dataJailbroken = ps5::elevated;
		ps5::diag::mark("usb: sandbox elevation %s (status %u)%s", ps5::elevated ? "granted" : "not granted", status,
				status == 11 ? ": the old way, the console's root references unbalanced" : "");
		ps5::diag::mark("ids: uid %d euid %d gid %d egid %d", (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid());
		// The helper zeroes the real and saved ids but leaves the effective
		// group as it was. Mesa only uses its on-disk shader cache for a
		// "normal user" (effective ids equal to the real ones), so with the
		// groups apart RADV's cache silently stays empty. With an effective
		// uid of 0 the process may set its effective group back to the real one.
		if (geteuid() == 0 && getgid() != getegid())
		{
			const int result = setegid(getgid());
			const int error = result == 0 ? 0 : errno;
			ps5::diag::mark("ids: setegid(%d) returned %d (errno %d); uid %d euid %d gid %d egid %d",
					(int)getgid(), result, error, (int)getuid(), (int)geteuid(), (int)getgid(), (int)getegid());
		}
	}
	std::vector<std::string> candidates;
	if (ps5::elevated)
	{
		// Outside the sandbox /app0 is no longer a path: the title's folder is
		// where the launcher mounted it from, or under the sandbox's mount.
		candidates.push_back("/data/homebrew/PPSA99247");
		// Installed on extended storage or a USB drive (a homebrew store can put it there).
		for (const char *drive : { "/mnt/ext0", "/mnt/ext1", "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
				"/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7" })
			candidates.push_back(std::string(drive) + "/homebrew/PPSA99247");
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
	ps5::folders::load(ps5::rootDir);
	ps5::diag::showNotifications(ps5::options().notifications);
	// Logs: off, or in PSFlyCast's folder (the boot log in it, Flycast's own in logs/), or on a USB
	// drive (both in PSFlyCast/logs there), forced onto the drive as often as chosen.
	// Kept in memory (3): no file until something goes wrong, then every 15 s as the default.
	static const int syncSeconds[] = { 2, 15, 0, 15 };
	ps5::diag::configure(ps5::options().logs, syncSeconds[std::clamp(ps5::options().logSync, 0, 3)],
			ps5::options().logSync == 3);
	std::string bootLogDir = ps5::rootDir, emulatorLogDir = ps5::rootDir + "logs/", placeNote;
	if (ps5::options().logs && ps5::options().logPlace == 1)
	{
		const std::string usb = usbLogFolder();
		if (!usb.empty())
			bootLogDir = emulatorLogDir = usb;
		else
			placeNote = ps5::elevated ? "no USB drive that can be written to" : "USB drives need the sandbox left (Settings > Library > USB)";
	}
	ps5::diag::open(bootLogDir);
	ps5::diag::mark("PSFlyCast, build %d; root folder: %s", PS5_BUILD_NUMBER, ps5::rootDir.c_str());
	if (ps5::options().logs)
	{
		ps5::diag::mark("logs: in %s%s", bootLogDir.c_str(),
				placeNote.empty() ? "" : (std::string(" (a USB drive was asked for: ") + placeNote + ")").c_str());
		ps5::diag::mark("logs: forced onto the drive %s; every 2 s for the first 30 s of a start and of each game",
				ps5::options().logSync == 0 ? "every 2 s" : ps5::options().logSync == 1 ? "every 15 s"
				: ps5::options().logSync == 2 ? "only when PSFlyCast ends or crashes"
				: "never: kept in memory, written only after a crash, a game that did not start, or when asked");
	}
	else
		ps5::diag::mark("logs: off (Settings > System > Logs)");
	makeDirs(ps5::rootDir);
	repairModes(ps5::rootDir, 0);
	probeStorage();
	// The driver reads the title's param.json to learn whether 119.88 Hz may
	// be asked for, from /app0: outside the sandbox there is no /app0, and it
	// is told where the file is. (A root that has none, /download0, leaves
	// the driver to /app0.)
	if (hasFile(ps5::rootDir + "sce_sys/param.json"))
		setenv("PS5_VIDEOOUT_PARAM_JSON", (ps5::rootDir + "sce_sys/param.json").c_str(), 1);
	// RADV's shader cache, in the root whichever path that is this run.
	setenv("MESA_SHADER_CACHE_DIR", (ps5::rootDir + "radv-shader-cache").c_str(), 1);
	if (ps5::options().usb)
	{
		findUsbDirs();
		if (!ps5::elevated)
			ps5::diag::notify("PSFlyCast: USB drives need elfldr running (port 9021). Continuing without them.");
	}
	ps5::disc::start();
	ps5::smb::loadConfig();
	// A folder on a network share is listed through the share's own storage (ps5_smb.cpp).
	ps5::folders::setLister([](const std::string& folder, std::vector<ps5::folders::Entry>& out) {
		try {
			for (const hostfs::FileInfo& info : hostfs::storage().listContent(folder))
				out.push_back({ info.name, info.isDirectory });
		} catch (const hostfs::StorageException&) {
			return false;
		}
		return true;
	});
	if (ps5::options().logs)
	{
		redirectLogs(emulatorLogDir);
		ps5::diag::mark("the emulator's log: %sflycast.log", emulatorLogDir.c_str());
	}
	EventManager::listen(Event::Start, [](Event, void *) { ps5::diag::watch(30); });

	writeDefaultConfig(ps5::rootDir);

	ps5::diag::mark("log manager");
	LogManager::Init();
	i18n::init();
	set_user_config_dir(ps5::rootDir);
	set_user_data_dir(ps5::rootDir + "data/");
	add_system_data_dir(ps5::rootDir + "bios/");

	ps5::diag::mark("hide splash screen: %d", sceSystemServiceHideSplashScreen());

	// How late the console's sleeps wake, measured on a thread of its own (ps5_timing.h).
	PS5Timing::StartProbe();

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
	// The graphics driver (Settings > System, PlayStation 5), read by it when
	// the Vulkan instance and device are made, below in flycast_init and the
	// main loop: a change takes effect at the next start. Crux2's lessons on
	// the same driver (PS5_Mesa patches 0009 and 0010): recording on a thread
	// of its own took a heavy scene of Crux2's from 40 to 48 frames a second;
	// off here until PSFlyCast has been measured with it. A driver without
	// the patches ignores the variables.
	if (ps5::options().threadedRecording)
	{
		const int set = setenv("RADV_THREADED_RECORDING", "1", 1);
		ps5::diag::mark("graphics driver: commands recorded on a thread of its own (setenv %d)", set);
	}
	if (ps5::options().gpuWaitSpinUs != 1500 || ps5::options().gpuWaitSleepUs != 1000)
	{
		// Only when changed from the driver's own 1500 us of checking and 1000 us sleeps.
		const int spin = setenv("RADV_PS5_WAIT_SPIN_US", std::to_string(ps5::options().gpuWaitSpinUs).c_str(), 1);
		const int sleep = setenv("RADV_PS5_WAIT_SLEEP_US", std::to_string(ps5::options().gpuWaitSleepUs).c_str(), 1);
		ps5::diag::mark("graphics driver: a wait for the GPU checks for %d us, then sleeps %d us between checks (setenv %d %d)",
				ps5::options().gpuWaitSpinUs, ps5::options().gpuWaitSleepUs, spin, sleep);
	}
	if (ps5::options().shortQueue)
		ps5::diag::mark("display: a short queue of frames for the screen asked for in the Settings");
	ps5::diag::mark("flycast_init");
	// A forwarder's arguments are taken here, and left out of what Flycast reads.
	std::vector<char *> kept;
	for (int i = 0; i < argc; i++)
	{
		if (i > 0 && !strcmp(argv[i], "--rom") && i + 1 < argc)
		{
			ps5::launchRequest.rom = argv[++i];
			continue;
		}
		if (i > 0 && !strncmp(argv[i], "--rom=", 6))
		{
			ps5::launchRequest.rom = argv[i] + 6;
			continue;
		}
		if (i > 0 && !strcmp(argv[i], "--exit-after-game"))
		{
			ps5::launchRequest.exitAfterGame = true;
			continue;
		}
		kept.push_back(argv[i]);
	}
	if (!ps5::launchRequest.rom.empty() || ps5::launchRequest.exitAfterGame)
		ps5::diag::mark("launch: --rom \"%s\"%s", ps5::launchRequest.rom.c_str(),
				ps5::launchRequest.exitAfterGame ? ", --exit-after-game" : "");
	if (flycast_init((int)kept.size(), kept.data()) != 0)
	{
		ps5::diag::mark("flycast_init failed");
		fflush(nullptr);
		return 1;
	}
	setFolders();
	// Achievements are earned with save states, cheats and rewind at hand
	// here: the mode that forbids them is not offered. An emu.cfg that has it
	// on (copied from a PC) is saved with it off, as the settings are read
	// from the file again whenever a game is unloaded.
	if (config::AchievementsHardcoreMode)
	{
		config::AchievementsHardcoreMode.set(false);
		SaveSettings();
		ps5::diag::mark("settings: the achievements' hardcore mode turned off (it is not offered here)");
	}
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
	if (ps5::options().defaults < 5)
	{
		// Build 42's new options (the software renderer) get their entry in
		// emu.cfg: a game's own value of an option is kept only where it
		// differs from the entry the Settings have, and is dropped where they
		// have none.
		SaveSettings();
		ps5::options().defaults = 5;
		ps5::saveOptions();
	}
	if (ps5::options().defaults < 6)
	{
		// The same for frame generation and for the per-pixel layers, which
		// are an option of their own from this build on.
		SaveSettings();
		ps5::options().defaults = 6;
		ps5::saveOptions();
	}
	if (ps5::options().defaults < 7)
	{
		// The same for the picture's new settings (sharpening, film grain and
		// the colours), which a game's own values are kept against.
		SaveSettings();
		ps5::options().defaults = 7;
		ps5::saveOptions();
	}
	if (ps5::options().defaults < 8)
	{
		// And the turbo settings.
		SaveSettings();
		ps5::options().defaults = 8;
		ps5::saveOptions();
	}
	if (ps5::options().defaults < 9)
	{
		// The sound chip's DSP makes the reverb and echo games were mixed
		// with; without it they sound dry. On once, as the console sounds;
		// the user's to turn off after that.
		config::DSPEnabled.set(true);
		SaveSettings();
		ps5::options().defaults = 9;
		ps5::saveOptions();
		ps5::diag::mark("settings: the sound DSP turned on (reverb and echo as on the console)");
	}
	if (ps5::options().defaults < 10)
	{
		// The defaults Press5elect chose for PSFlyCast (v1.3.3): per-pixel
		// sorting at 4x with FSR 1, the console's 120 Hz and variable refresh,
		// the picture's light beside it, VMU beeps in the controller. A
		// setting moves only while it still has the old default, so whatever
		// was changed by hand stays as it is.
		int moved = 0;
		auto move = [&moved](auto& option, auto from, auto to) {
			if (option.get() == from)
			{
				option.set(to);
				moved++;
			}
		};
		move(config::PerPixelLayers, 32, 96);
		move(config::RenderResolution, 480, 1920);
		move(config::TranslucentPolygonDepthMask, false, true);
		move(config::AnisotropicFiltering, 1, 16);
		move(config::TextureFiltering, 0, 2);
		move(config::AccurateMipmaps, true, false);
		move(config::Original16Bit, true, false);
		move(config::FlickerFilter, true, false);
		move(ps5::SidePicture, 0, 1);
		move(ps5::VmuScreenSize, 100, 200);
		move(ps5::Upscaling, (int)ps5::UpscalingOff, (int)ps5::UpscalingFsrSharp);
		move(config::AudioBufferSize, 5644, 2822);
		move(config::VmuSound, false, true);
		move(ps5::VmuSoundOutput, 0, 1);
		move(config::Cable, 3, 2);
		move(config::AutoSaveState, false, true);
		SaveSettings();
		if (!ps5::options().vrr)
		{
			ps5::options().vrr = true;
			moved++;
		}
		ps5::options().defaults = 10;
		ps5::saveOptions();
		ps5::diag::mark("settings: %d moved to PSFlyCast's v1.3.3 defaults (those changed by hand kept)", moved);
	}
	if (ps5::options().defaults < 11)
	{
		// Widescreen game patches came on with v1.3.3's defaults, and with
		// them every patched game's picture is stretched to 16:9 while the
		// Widescreen setting still says 4:3. Off again, once; the player's
		// to turn on, for every game or for one.
		if (config::WidescreenGameHacks)
		{
			config::WidescreenGameHacks.set(false);
			SaveSettings();
			ps5::diag::mark("settings: widescreen game patches off (v1.3.4 default)");
		}
		ps5::options().defaults = 11;
		ps5::saveOptions();
	}
	// What a new install has for each setting, for the Settings' "changed from default".
	bigpicture::captureSettingDefaults();
	ps5::update::init();
	ps5::pipelineWarmInit();
	ps5::cheats::init();
	ps5::games::init();
	ps5::patches::init();
	ps5::mods::init();
	ps5::memsearch::init();
	ps5::speedrun::init();
	ps5::drawdist::init();
	ps5::rewind::init();
	ps5::rewind::setEnabled(ps5::options().rewind);
	// Where the threads run (ps5_thread_layout.h): the main thread to its core now, the others
	// as they start.
	PS5ThreadLayout::init();
	startHeapTrim();
	ps5::diag::mark("main loop");
	try {
		mainui_loop();
	} catch (const std::exception& e) {
		ps5::diag::mark("main loop error: %s", e.what());
	} catch (...) {
		ps5::diag::mark("main loop: unknown exception");
	}
	ps5::diag::mark("main loop ended");
	// A game still running when the title is closed: its time played.
	ps5::library::flush();
	// Closing does not wait for the achievements' pictures still to come.
	ps5::net::gameEnding(true);
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
	ps5::diag::flush();
	if (ps5::restartOnExit)
	{
		// The title's own executable in place of this one (after an update).
		// Refused, or nothing in five seconds: closed as usual.
		const int restart = sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
		ps5::diag::mark("restart request: %d", restart);
		fflush(nullptr);
		ps5::diag::flush();
		if (restart >= 0)
			usleep(5000000);
	}
	const int result = sceSystemServiceLoadExec("exit", nullptr);
	ps5::diag::mark("close request: %d", result);
	ps5::diag::flush();
	if (result >= 0)
		for (;;)
			usleep(100000);
}
