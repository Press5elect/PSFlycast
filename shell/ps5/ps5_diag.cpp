/*
	PSFlyCast - start-up record and crash report.

	Copyright 2026 the PSFlyCast contributors
	SPDX-License-Identifier: GPL-2.0-or-later

	The console only says that a title stopped, not where. This file writes
	what the next run needs to fix it, in the title's own folder:

	- every start-up step, as a line written straight to the file;
	- on a fatal signal, the faulting instruction as an offset in eboot.bin
	  (shell/ps5/symbolize.sh turns it into a function), the registers, the
	  return addresses on the stack and the last step reached - the approach
	  of PS5 RetroArch's src/crash_report.cpp;
	- on std::terminate, the exception that was not caught.

	The machine context is the console's: the payload SDK fork's ucontext_t
	places uc_mcontext where the console does (ps5platform/context.h).
*/
#include "ps5_diag.h"

#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <initializer_list>
#include <typeinfo>

#include <cxxabi.h>
#include <fcntl.h>
#include <pthread.h>
#include <pthread_np.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <ps5platform/context.h>

extern "C"
{
int sceKernelSendNotificationRequest(int, void *, size_t, int);
int sceKernelAvailableFlexibleMemorySize(size_t *size);
// shell/ps5/runtime/ps5-title.ld
extern char __ps5_text_start[];
extern char __ps5_text_end[];
}

namespace ps5::diag
{
namespace
{
int logFd = -1;
std::string path;
// Marks made before the file is open, written when it opens.
char early[8192];
size_t earlyUsed;
// The last mark, for the crash report.
char lastMark[256] = "(none)";
timespec startTime;

// eboot.bin's load address: .text starts 16 bytes into the image.
uintptr_t imageBase()
{
	return reinterpret_cast<uintptr_t>(__ps5_text_start) - 0x10;
}

bool inText(uintptr_t address)
{
	return address >= reinterpret_cast<uintptr_t>(__ps5_text_start)
			&& address < reinterpret_cast<uintptr_t>(__ps5_text_end);
}

void writeAll(const char *text, size_t length)
{
	if (logFd < 0)
	{
		const size_t room = sizeof(early) - earlyUsed;
		const size_t n = length < room ? length : room;
		memcpy(early + earlyUsed, text, n);
		earlyUsed += n;
		return;
	}
	while (length > 0)
	{
		const ssize_t n = write(logFd, text, length);
		if (n <= 0)
			return;
		text += n;
		length -= n;
	}
}

// Signal-safe enough: snprintf into a stack buffer and write().
void writef(const char *format, ...) __attribute__((format(printf, 1, 2)));
void writef(const char *format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	const int n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n > 0)
		writeAll(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

// Whether notices show in the console's corner; they are lines of the boot
// log either way.
volatile bool notices = false;

void sendNotification(const char *text)
{
	if (!notices)
		return;
	struct
	{
		char useless[45];
		char message[3075];
	} request;
	memset(&request, 0, sizeof(request));
	strncpy(request.message, text, sizeof(request.message) - 1);
	sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

void report(int signal, siginfo_t *info, void *contextPointer)
{
	const ucontext_t *context = static_cast<const ucontext_t *>(contextPointer);
	const mcontext_t& mc = context->uc_mcontext;
	const uintptr_t rip = mc.mc_rip;
	const uintptr_t rsp = mc.mc_rsp;
	const uintptr_t base = imageBase();
	size_t flexible = 0;
	sceKernelAvailableFlexibleMemorySize(&flexible);

	writef("\n=== CRASH: signal %d (%s) ===\n", signal,
			signal == SIGSEGV ? "SIGSEGV" : signal == SIGBUS ? "SIGBUS" : signal == SIGILL ? "SIGILL"
			: signal == SIGFPE ? "SIGFPE" : signal == SIGABRT ? "SIGABRT" : signal == SIGSYS ? "SIGSYS"
			: signal == SIGTRAP ? "SIGTRAP" : "?");
	writef("last step: %s\n", lastMark);
	writef("fault address: 0x%llx\n", (unsigned long long)(uintptr_t)info->si_addr);
	if (inText(rip))
		writef("rip: 0x%llx = eboot+0x%llx\n", (unsigned long long)rip, (unsigned long long)(rip - base));
	else
		writef("rip: 0x%llx (outside eboot.bin; eboot at 0x%llx)\n", (unsigned long long)rip,
				(unsigned long long)base);
	writef("rsp: 0x%llx  flexible memory free: %zu KiB\n", (unsigned long long)rsp, flexible >> 10);
	writef("rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx\n",
			(unsigned long long)mc.mc_rax, (unsigned long long)mc.mc_rbx, (unsigned long long)mc.mc_rcx,
			(unsigned long long)mc.mc_rdx, (unsigned long long)mc.mc_rsi, (unsigned long long)mc.mc_rdi,
			(unsigned long long)mc.mc_rbp);
	writef("r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n",
			(unsigned long long)mc.mc_r8, (unsigned long long)mc.mc_r9, (unsigned long long)mc.mc_r10,
			(unsigned long long)mc.mc_r11, (unsigned long long)mc.mc_r12, (unsigned long long)mc.mc_r13,
			(unsigned long long)mc.mc_r14, (unsigned long long)mc.mc_r15);

	// The return addresses: every stack word from rsp up that points into
	// eboot.bin's code (no frame pointers to follow).
	if (rsp >= 4096 && (rsp & 7) == 0)
	{
		pthread_attr_t attributes;
		void *stack = nullptr;
		size_t stackSize = 0;
		if (pthread_attr_init(&attributes) == 0)
		{
			if (pthread_attr_get_np(pthread_self(), &attributes) == 0)
				pthread_attr_getstack(&attributes, &stack, &stackSize);
			pthread_attr_destroy(&attributes);
		}
		const uintptr_t top = reinterpret_cast<uintptr_t>(stack) + stackSize;
		if (stack != nullptr && rsp < top)
		{
			const uintptr_t end = top - rsp > 65536 ? rsp + 65536 : top;
			writeAll("stack (eboot offsets):", 22);
			int found = 0;
			for (uintptr_t at = rsp; at + 8 <= end && found < 64; at += 8)
			{
				const uintptr_t word = *reinterpret_cast<const uintptr_t *>(at);
				if (!inText(word))
					continue;
				writef(" %llx", (unsigned long long)(word - base));
				found++;
			}
			writeAll("\n", 1);
		}
	}
	writeAll("=== end of crash report ===\n", 29);
	if (logFd >= 0)
		fsync(logFd);

	// What Flycast logged just before, if the thread that crashed is not
	// holding the stream.
	for (FILE *stream : { stdout, stderr })
		if (ftrylockfile(stream) == 0)
		{
			fflush(stream);
			funlockfile(stream);
		}
	sendNotification("PSFlyCast stopped. Please send flycast-boot.log from its folder.");
	std::signal(signal, SIG_DFL);
	std::raise(signal);
}

void onTerminate()
{
	const std::type_info *type = abi::__cxa_current_exception_type();
	if (type == nullptr)
	{
		mark("terminate: called with no exception");
	}
	else
	{
		int status = 0;
		char *name = abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
		const char *what = "";
		try {
			throw;
		} catch (const std::exception& e) {
			what = e.what();
		} catch (...) {
		}
		mark("terminate: uncaught exception %s: %s", status == 0 && name != nullptr ? name : type->name(), what);
		free(name);
	}
	std::abort();
}

void rotate(const std::string& dir)
{
	const std::string a = dir + "/flycast-boot.log";
	const std::string b = dir + "/flycast-boot.1.log";
	const std::string c = dir + "/flycast-boot.2.log";
	rename(b.c_str(), c.c_str());
	rename(a.c_str(), b.c_str());
}

} // namespace

bool open(const std::string& dir)
{
	if (logFd >= 0)
		return true;
	rotate(dir);
	const std::string file = dir + "/flycast-boot.log";
	const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0666);
	if (fd < 0)
		return false;
	fchmod(fd, 0666);
	logFd = fd;
	path = file;
	writeAll(early, earlyUsed);
	earlyUsed = 0;
	fsync(logFd);
	return true;
}

const std::string& logPath()
{
	return path;
}

void mark(const char *format, ...)
{
	if (startTime.tv_sec == 0 && startTime.tv_nsec == 0)
		clock_gettime(CLOCK_MONOTONIC, &startTime);
	char text[sizeof(lastMark)];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	memcpy(lastMark, text, sizeof(lastMark));
	timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	const double seconds = (now.tv_sec - startTime.tv_sec) + (now.tv_nsec - startTime.tv_nsec) / 1e9;
	writef("[%8.3f] %s\n", seconds, text);
	printf("[boot] %s\n", text);
}

void notify(const char *format, ...)
{
	char text[512];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	mark("notice: %s", text);
	sendNotification(text);
}

void showNotifications(bool show)
{
	notices = show;
}

void installCrashHandler()
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_sigaction = report;
	action.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (const int signal : { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP })
		sigaction(signal, &action, nullptr);
	std::set_terminate(onTerminate);
}

} // namespace ps5::diag
