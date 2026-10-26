/*
	Created on: Apr 20, 2020

	Copyright 2020 flyinghead

	This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include "stdclass.h"

#include <atomic>
#include <future>
#include <array>
#include <mutex>
#include <utility>
#include <vector>
#include <ctime>

void loadGameSpecificSettings();
void SaveSettings();

int flycast_init(int argc, char* argv[]);
void flycast_term();
void dc_exit();
void dc_savestate(int index = 0, const u8 *pngData = nullptr, u32 pngSize = 0);
void dc_loadstate(int index = 0);
time_t dc_getStateCreationDate(int index);
void dc_getStateScreenshot(int index, std::vector<u8>& pngData);
bool dc_savestateAllowed();

// Decides a broadcast standard (Broadcast 4) or console language (Language 6)
// set to "Follow the game" from the region: the one resolved from the disc's
// IP.BIN, or the flash's when the disc names none. NTSC follows Japan and the
// USA, PAL Europe; the language is Japanese for Japan, English for the USA,
// and for Europe the console's own if it is a European one, else English.
// It overrides the options, so the stored "Follow the game" stays as it is.
// Called when the disc is read and again when the flash is loaded.
void resolveFollowSettings();

// Thrown by the HLE BIOS when the game asks to go back to the console's menu
// (or to the CD player, or to reset) and there is no disc to start again.
// The emulator catches it and stops cleanly: it isn't an error.
class ReturnToBiosException : public FlycastException
{
public:
	ReturnToBiosException() : FlycastException("Return to the console menu") {}
};

// Whether the game asked to go back to the console's menu and the emulator
// stopped for it (the HLE BIOS has no menu to show). The frontend checks it
// when the game stops running, to go back to its own menu instead of showing
// an error. Cleared when a game is loaded or started again.
bool emuReturnedToBios();

enum class Event {
	Start,
	Pause,
	Resume,
	Terminate,
	LoadState,
	VBlank,
	Network,
	DiskChange,
	LocaleChange,
	max = LocaleChange
};

class EventManager
{
public:
	using Callback = void (*)(Event, void *);

	static void listen(Event event, Callback callback, void *param = nullptr) {
		instance().registerEvent(event, callback, param);
	}

	static void unlisten(Event event, Callback callback, void *param = nullptr) {
		instance().unregisterEvent(event, callback, param);
	}

	static void event(Event event) {
		instance().broadcastEvent(event);
	}

private:
	EventManager() = default;
	static EventManager& instance() {
		static EventManager _instance;
		return _instance;
	}

	void registerEvent(Event event, Callback callback, void *param);
	void unregisterEvent(Event event, Callback callback, void *param);
	void broadcastEvent(Event event);

	std::array<std::vector<std::pair<Callback, void *>>, static_cast<size_t>(Event::max) + 1> callbacks;
};

struct LoadProgress
{
	void reset()
	{
		cancelled = false;
		label = nullptr;
		progress = 0.f;
	}
	std::atomic<bool> cancelled;
	std::atomic<const char *> label;
	std::atomic<float> progress;
};

class Sh4Executor;

class Emulator
{
public:
	/**
	 * Initialize the emulator. Does nothing if already initialized.
	 */
	void init();
	/**
	 * Terminate the emulator. After calling this method, the application must be restarted.
	 */
	void term();
	/**
	 * Load the specified game/media. Will call init() first.
	 * May throw if the game cannot be loaded.
	 * If a game is already loaded, or the emulator is in the error state, unloadGame() must be called first.
	 */
	void loadGame(const char *path, LoadProgress *progress = nullptr);
	/**
	 * Reset the emulator in order to load another game. After calling this method, only loadGame() and term() can be called.
	 * Does nothing if no game is loaded.
	 */
	void unloadGame();
	/**
	 * Run the emulator in the calling thread until a frame is rendered. A game must be loaded and start() must be called
	 * prior to calling this method.
	 */
	void run();
	/**
	 * Prepare the emulator to start executing. Starts the execution thread if in multi-thread mode.
	 * A game must be loaded successfully before calling this method.
	 */
	void start();
	/**
	 * Stops the execution thread if in multi-thread mode. The game can be restarted by calling start().
	 * Does nothing if the game isn't running.
	 */
	void stop();
	/**
	 * Execute a single instruction.
	 */
	void step();
	/**
	 * Execute instructions while PC is with range.
	 */
	void stepRange(u32 from, u32 to);
	/**
	 * Return whether the emulator is currently running.
	 */
	bool running() const {
		return state == Running;
	}
	/**
	 * Wait for the next frame and render it. If in single-thread mode, it will run the emulator until a frame is rendered.
	 */
	bool render();
	/**
	 * Set the network state.
	 */
	void setNetworkState(bool online);
	/**
	 * Called internally to reset the emulator.
	 */
	void requestReset();
	/**
	 * Called internally on vblank.
	 */
	void vblank();
	/**
	 * Restart the cpu iff the emu is still running
	 * Returns true if the cpu was started
	 */
	bool restartCpu();
	/*
	 * Load the machine state from the passed deserializer
	 */
	void loadstate(Deserializer& deser);
	/**
	 * Insert a new media in the GD-ROM drive.
	 */
	void insertGdrom(const std::string& path);
	/**
	 * Open the GD-ROM drive lid.
	 */
	void openGdrom();

	Sh4Executor *getSh4Executor();

	void run(std::function<void()> func) {
		runner.runOnThread(func);
	}

	void dc_reset(bool hard); // for tests only

	/**
	 * Whether the game asked to go back to the console's menu and the emulator stopped for it.
	 */
	bool returnedToBios() const {
		return returnToBios;
	}

private:
	bool checkStatus(bool wait = false);
	void runInternal();
	void diskChange();
	void finishReturnToBios();

	enum State {
		Uninitialized = 0,
		Init,
		Loaded,
		Running,
		Error,
		Terminated,
	};
	State state = Uninitialized;
	std::shared_future<void> threadResult;
	bool resetRequested = false;
	bool singleStep = false;
	u64 startTime = 0;
	u32 stepRangeFrom = 0;
	u32 stepRangeTo = 0;
	bool stopRequested = false;
	// The game went back to the console's menu (returnedToBios), and the
	// interface thread has still to finish stopping (finishReturnToBios).
	std::atomic<bool> returnToBios { false };
	std::atomic<bool> returnToBiosPending { false };
	std::mutex mutex;
	Sh4Executor *interpreter = nullptr;
	Sh4Executor *recompiler = nullptr;
	ThreadRunner runner;
};
extern Emulator emu;

int getGamePlatform(const std::string& filename);
