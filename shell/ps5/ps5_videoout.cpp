// PSFlyCast - the display's variable refresh, found while the title runs, and the lookup of a
// system module's function it needs.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast (from Crux2's ps5/frontend/ps5_keyboard.cpp, PS5SystemModules::FindFunction,
// and ps5_host.cpp's ps5_title_videoout_vrr_unpeg, Crux2 by Press5elect): the log is PSFlyCast's
// boot log.
//
// sceVideoOutVrrUnpegFromFixedRate asks the output to leave its fixed rate (variable refresh). Not
// every firmware's libSceVideoOut exports it, and a title that imports what the console does not
// give it does not start: up to v1.3.4 PSFlyCast imported it, through the graphics driver. The
// driver (PS5_Mesa patch 0017, from Crux2) now calls the weak ps5_title_videoout_vrr_unpeg below
// instead; it finds the function by its name, then by its NID (T4ucGB8CsnM), and answers -1 when
// the console has none, so the driver refuses variable refresh and 119.88 Hz stays as configured.
//
// Imports this adds (all in the SDK's stubs): libkernel's sceKernelDlsym, sceKernelGetModuleList
// and sceKernelGetModuleInfo.

#include "ps5_videoout.h"
#include "ps5_diag.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
int sceKernelDlsym(int32_t handle, const char* symbol, void** address);
int sceKernelGetModuleList(int32_t* handles, size_t capacity, size_t* count);
int sceKernelGetModuleInfo(int32_t handle, void* info);
}

namespace ps5::modules
{
namespace
{
// The module list's entry: the size of the structure first, the module's name at 8.
constexpr size_t MODULE_INFO_SIZE = 0x160;
constexpr size_t MODULE_NAME_OFFSET = 8;
constexpr size_t MODULE_NAME_MAX = 256;

bool NameMatches(const char* name, const char* wanted)
{
	const size_t n = std::strlen(wanted);
	if (std::strncmp(name, wanted, n) != 0)
		return false;
	return name[n] == '\0' || name[n] == '.';
}

// The process's modules (handles), as the console lists them.
std::vector<int32_t> ModuleList()
{
	std::array<int32_t, 256> handles;
	size_t count = 0;
	if (sceKernelGetModuleList(handles.data(), handles.size(), &count) != 0)
		return {};
	return std::vector<int32_t>(handles.begin(), handles.begin() + static_cast<std::ptrdiff_t>(std::min(count, handles.size())));
}

// A module's name ("libkernel.sprx"), or empty.
std::string ModuleName(int32_t handle)
{
	alignas(8) uint8_t info[MODULE_INFO_SIZE] = {};
	const uint64_t size = sizeof(info);
	std::memcpy(info, &size, sizeof(size));
	if (sceKernelGetModuleInfo(handle, info) != 0)
		return {};
	char name[MODULE_NAME_MAX + 1] = {};
	std::memcpy(name, info + MODULE_NAME_OFFSET, MODULE_NAME_MAX);
	return name;
}

// The names a module goes by in a process: the kernel's library is libkernel in one process,
// libkernel_sys or libkernel_web in another.
std::vector<const char*> NamesOf(const char* moduleName)
{
	if (std::strcmp(moduleName, "libkernel") == 0)
		return {"libkernel", "libkernel_sys", "libkernel_web"};
	return {moduleName};
}

int32_t FindModule(const char* moduleName)
{
	const std::vector<int32_t> modules = ModuleList();
	for (const char* wanted : NamesOf(moduleName))
		for (const int32_t handle : modules)
			if (NameMatches(ModuleName(handle).c_str(), wanted))
				return handle;
	return -1;
}

void* FindIn(int32_t module, const char* function, const char* nid)
{
	if (module < 0)
		return nullptr;
	void* address = nullptr;
	if (sceKernelDlsym(module, function, &address) == 0 && address)
		return address;
	address = nullptr;
	if (nid && sceKernelDlsym(module, nid, &address) == 0 && address)
		return address;
	return dlsym(reinterpret_cast<void*>(static_cast<intptr_t>(module)), function);
}

std::mutex saidMutex;
bool saidModules = false;
} // namespace

void* findFunction(const char* moduleName, const char* function, const char* nid)
{
	// The module by its name(s) first; then every module of the process, since a function can be
	// given by another module than the one its documentation names.
	const int32_t named = FindModule(moduleName);
	if (void* address = FindIn(named, function, nid))
		return address;
	// The kernel's library is not in the list a title sees: its fixed handles.
	if (std::strcmp(moduleName, "libkernel") == 0)
		for (const int32_t handle : {int32_t{1}, int32_t{0x2001}})
			if (void* address = FindIn(handle, function, nid))
				return address;
	const std::vector<int32_t> modules = ModuleList();
	for (const int32_t handle : modules)
	{
		if (handle == named)
			continue;
		if (void* address = FindIn(handle, function, nid))
		{
			ps5::diag::mark("system modules: %s found in %s (handle %d), not in %s", function, ModuleName(handle).c_str(),
					handle, moduleName);
			return address;
		}
	}
	// The system's modules are not in the list a title sees, but they have handles: the small
	// numbers the loader gives in load order, and the 0x2000 range. Each is asked in turn (an
	// unused handle answers at once).
	for (int32_t handle = 0; handle < 0x2040; handle = (handle == 511) ? 0x2000 : handle + 1)
	{
		if (handle == named || handle == 1 || handle == 0x2001
				|| std::find(modules.begin(), modules.end(), handle) != modules.end())
			continue;
		if (void* address = FindIn(handle, function, nid))
		{
			ps5::diag::mark("system modules: %s found in the module of handle %#x", function, static_cast<unsigned>(handle));
			return address;
		}
	}
	// Not found anywhere: once, what the process has, so the boot log says where to look.
	const std::lock_guard<std::mutex> lock(saidMutex);
	if (!saidModules)
	{
		saidModules = true;
		std::string names;
		for (const int32_t handle : modules)
		{
			names += names.empty() ? "" : ", ";
			names += ModuleName(handle);
		}
		ps5::diag::mark("system modules: %s not found (%s was %s); the process has %zu module(s): %.150s", function,
				moduleName, named >= 0 ? "found" : "not found", modules.size(), names.c_str());
	}
	else
		ps5::diag::mark("system modules: %s not found", function);
	return nullptr;
}
} // namespace ps5::modules

// The graphics driver's variable refresh (PS5_Mesa patch 0017): looked up once, by its NID, since
// not every firmware's libSceVideoOut exports it. -1 when the console has none: the driver then
// refuses variable refresh and keeps 119.88 Hz.
extern "C" int ps5_title_videoout_vrr_unpeg(int32_t handle)
{
	using UnpegFn = int (*)(int32_t);
	static const UnpegFn unpeg = [] {
		const UnpegFn found = reinterpret_cast<UnpegFn>(
				ps5::modules::findFunction("libSceVideoOut", "sceVideoOutVrrUnpegFromFixedRate", "T4ucGB8CsnM"));
		ps5::diag::mark("display: sceVideoOutVrrUnpegFromFixedRate %s",
				found ? "found" : "not on this console: no variable refresh");
		return found;
	}();
	return unpeg ? unpeg(handle) : -1;
}
