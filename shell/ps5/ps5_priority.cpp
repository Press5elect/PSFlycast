// Crux2 - which of the title's threads the console runs first: the runtime of ps5_priority.h.
// SPDX-FileCopyrightText: 2026 Press5elect
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see Crux2's NOTICE.
// Ported to PSFlyCast (from Crux2's ps5/frontend/ps5_priority.cpp, Crux2 by Press5elect): the
// setting is a Flycast option ([ps5] ThreadPriorities), the log PSFlyCast's boot log, and the
// two functions are looked up in libkernel by the part of Crux2's PS5SystemModules::FindFunction
// that libkernel needs (by name, then by NID; its module by name, then its fixed handles).

#include "ps5_priority.h"
#include "ps5_diag.h"
#include "cfg/option.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <pthread.h>

extern "C" {
int sceKernelDlsym(int32_t handle, const char* symbol, void** address);
int sceKernelGetModuleList(int32_t* handles, size_t capacity, size_t* count);
int sceKernelGetModuleInfo(int32_t handle, void* info);
}

namespace ps5
{
// Off by default, as in Crux2 ("Sound and controls before the emulator", an expert setting there).
config::Option<bool> ThreadPriorities("ThreadPriorities", false, "ps5");
}

namespace PS5Priority
{
namespace
{
using GetFn = int (*)(pthread_t thread, int* priority);
using SetFn = int (*)(pthread_t thread, int priority);

struct Functions
{
	GetFn get = nullptr;
	SetFn set = nullptr;
};

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

// The kernel's library is libkernel in one process, libkernel_sys or libkernel_web in another.
int32_t FindKernelModule()
{
	std::array<int32_t, 256> handles;
	size_t count = 0;
	if (sceKernelGetModuleList(handles.data(), handles.size(), &count) != 0)
		return -1;
	count = count < handles.size() ? count : handles.size();
	for (const char* wanted : {"libkernel", "libkernel_sys", "libkernel_web"})
	{
		for (size_t i = 0; i < count; i++)
		{
			alignas(8) uint8_t info[MODULE_INFO_SIZE] = {};
			const uint64_t size = sizeof(info);
			std::memcpy(info, &size, sizeof(size));
			if (sceKernelGetModuleInfo(handles[i], info) != 0)
				continue;
			char name[MODULE_NAME_MAX + 1] = {};
			std::memcpy(name, info + MODULE_NAME_OFFSET, MODULE_NAME_MAX);
			if (NameMatches(name, wanted))
				return handles[i];
		}
	}
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

void* FindKernelFunction(const char* function, const char* nid)
{
	if (void* address = FindIn(FindKernelModule(), function, nid))
		return address;
	// The kernel's library is not always in the list a title sees: its fixed handles.
	for (const int32_t handle : {int32_t{1}, int32_t{0x2001}})
	{
		if (void* address = FindIn(handle, function, nid))
			return address;
	}
	return nullptr;
}

const Functions& Find()
{
	static const Functions functions = [] {
		Functions f;
		f.get = reinterpret_cast<GetFn>(FindKernelFunction("scePthreadGetprio", "1tKyG7RlMJo"));
		f.set = reinterpret_cast<SetFn>(FindKernelFunction("scePthreadSetprio", "W0Hpm2X0uPE"));
		ps5::diag::mark("priority: scePthreadSetprio %s", f.set ? "found" : "NOT found: every thread keeps the title's priority");
		return f;
	}();
	return functions;
}

int PriorityOf(Role role)
{
	switch (role)
	{
		case Role::Sound: return 690;
		case Role::Input: return 692;
		case Role::Interface: return 695;
		case Role::Emulation: return 700;
		case Role::Background: return 730;
	}
	return 700;
}

const char* NameOf(Role role)
{
	switch (role)
	{
		case Role::Sound: return "sound";
		case Role::Input: return "controllers";
		case Role::Interface: return "interface";
		case Role::Emulation: return "emulation";
		case Role::Background: return "background job";
	}
	return "?";
}

std::atomic<unsigned> s_said{0};
} // namespace

void SetCurrentThread(Role role)
{
	if (!ps5::ThreadPriorities)
		return;
	const Functions& f = Find();
	if (!f.set)
		return;
	const int wanted = PriorityOf(role);
	int before = -1;
	if (f.get)
		f.get(pthread_self(), &before);
	const int result = f.set(pthread_self(), wanted);
	const unsigned bit = 1u << static_cast<unsigned>(role);
	if ((s_said.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
	{
		int after = -1;
		if (f.get)
			f.get(pthread_self(), &after);
		ps5::diag::mark("priority: a %s thread from %d to %d: %#x (now %d)", NameOf(role), before, wanted,
			static_cast<unsigned>(result), after);
	}
}
} // namespace PS5Priority
