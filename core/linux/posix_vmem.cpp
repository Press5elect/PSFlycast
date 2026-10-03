
// Implementation of the vmem related function for POSIX-like platforms.
// There's some minimal amount of platform specific hacks to support
// Android and OSX since they are slightly different in some areas.
#include "types.h"

#if defined(__PROSPERO__)
// PS5 (jailbroken, homebrew title): guest memory and JIT code live in direct
// memory through the payload SDK fork's platform layer (ps5platform/shm.h and
// ps5platform/exec.h), as in the PS5 RetroArch port's Dolphin and PPSSPP cores.
//
// - An anonymous or shm_open mapping is charged to the title's small flexible
//   memory budget; one direct-memory object can be mapped at every mirror
//   address and is charged only to the direct pool.
// - The address space (FPCB, SH4 context, 512 MB guest window) is a reserved
//   range. The SH4 context and the FPCB are backed by 64 KiB direct-memory
//   units mapped into it: the FPCB's on first access, a page at a time, as
//   upstream does with PROT_NONE pages. (Only the ps5_shm_*, ps5_vrange_reserve
//   and ps5_exec_* functions are used: the released title binds those for its
//   other cores, so this core loads in it without a title rebuild.)
// - Execute permission cannot be given to the core's own .text, so the code
//   caches are allocated (DECLARE_CODE_CACHE is a pointer on this platform)
//   read-write-execute within +/-2 GiB of the core's code, so the JIT's rel32
//   calls reach it.
#include <sys/mman.h>
#include <cerrno>
#include <vector>

#include <ps5platform/exec.h>
#include <ps5platform/shm.h>

#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_if.h"
#include "oslib/virtmem.h"

static_assert(PAGE_SIZE == 16384, "the PS5 kernel page is 16 KiB");

namespace virtmem
{

// The direct-memory allocation unit.
constexpr size_t UNIT = 0x10000;
constexpr size_t FPCB_BYTES = sizeof(((Sh4RCB *)nullptr)->fpcb);
static_assert(FPCB_BYTES % UNIT == 0, "the FPCB must fill whole 64 KiB units");
static_assert(sizeof(Sh4RCB) - FPCB_BYTES == UNIT, "the SH4 context must be one 64 KiB unit");

static ps5_shm guest_ram;
static void *reserved_base;
static size_t reserved_size;

// The FPCB's units, by index, and the SH4 context's. A fixed table: units are
// committed from the fault handler, where nothing may allocate.
static ps5_shm fpcb_units[FPCB_BYTES / UNIT];
static ps5_shm context_unit;
static u8 *fpcb_base;

// Backs one unit at the address, inside the reservation, read-write.
static bool unit_commit(ps5_shm& unit, void *address)
{
	if (ps5_shm_create(UNIT, &unit) != 0)
		return false;
	void *view = nullptr;
	if (ps5_shm_map(&unit, 0, UNIT, address, PS5_SHM_READ | PS5_SHM_WRITE, PS5_SHM_FIXED, &view) != 0)
	{
		ps5_shm_destroy(&unit);
		return false;
	}
	return true;
}

// Gives the unit's memory back; its range stays reserved, with no access.
static void unit_release(ps5_shm& unit, void *address)
{
	if (unit.bytes == 0)
		return;
	ps5_shm_unmap(address, UNIT, PS5_SHM_KEEP_RESERVED);
	ps5_shm_destroy(&unit);
}

struct View
{
	void *address;
	size_t bytes;
};
static std::vector<View> views;

static bool protect(void *start, size_t len, int prot)
{
	const uintptr_t begin = (uintptr_t)start & ~(uintptr_t)PAGE_MASK;
	const uintptr_t end = ((uintptr_t)start + len + PAGE_MASK) & ~(uintptr_t)PAGE_MASK;
	return mprotect((void *)begin, end - begin, prot) == 0;
}

bool region_lock(void *start, size_t len)
{
	if (!protect(start, len, PROT_READ))
		die("mprotect failed...");
	return true;
}

bool region_unlock(void *start, size_t len)
{
	if (!protect(start, len, PROT_READ | PROT_WRITE))
		die("mprotect failed...");
	return true;
}

bool region_set_exec(void *start, size_t len)
{
	// Only the arm64 back ends call this; executable memory comes from
	// prepare_jit_block on this platform.
	WARN_LOG(VMEM, "region_set_exec is not supported on PS5");
	return false;
}

bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)
{
	int rc = ps5_shm_create(ramSize, &guest_ram);
	if (rc != 0)
	{
		WARN_LOG(VMEM, "PS5: direct memory for guest RAM (%zu bytes) failed: %08x", ramSize, (unsigned)rc);
		return false;
	}

	reserved_size = 512_MB + sizeof(Sh4RCB) + ARAM_SIZE_MAX + 0x10000;
	rc = ps5_vrange_reserve(reserved_size, nullptr, 0x10000, &reserved_base);
	if (rc != 0)
	{
		WARN_LOG(VMEM, "PS5: reserving %zu bytes of address space failed: %08x", reserved_size, (unsigned)rc);
		reserved_base = nullptr;
		ps5_shm_destroy(&guest_ram);
		return false;
	}

	uintptr_t ptrint = (uintptr_t)reserved_base;
	ptrint = (ptrint + 0x10000 - 1) & ~(uintptr_t)0xffff;
	*sh4rcb_addr = (void *)ptrint;
	*vmem_base_addr = (void *)(ptrint + sizeof(Sh4RCB));
	fpcb_base = (u8 *)ptrint;

	// The SH4 context, without the FPCB (committed on demand).
	if (!unit_commit(context_unit, fpcb_base + FPCB_BYTES))
	{
		WARN_LOG(VMEM, "PS5: backing the SH4 context failed");
		destroy();
		return false;
	}
	INFO_LOG(VMEM, "PS5: vmem reserved at %p (%zu bytes)", reserved_base, reserved_size);

	return true;
}

static void unmap_views()
{
	for (const View& view : views)
		ps5_shm_unmap(view.address, view.bytes, PS5_SHM_KEEP_RESERVED);
	views.clear();
}

void destroy()
{
	if (reserved_base != nullptr)
	{
		unmap_views();
		for (size_t i = 0; i < std::size(fpcb_units); i++)
			unit_release(fpcb_units[i], fpcb_base + i * UNIT);
		unit_release(context_unit, fpcb_base + FPCB_BYTES);
		ps5_vrange_release(reserved_base, reserved_size);
		reserved_base = nullptr;
		fpcb_base = nullptr;
	}
	ps5_shm_destroy(&guest_ram);
}

void reset_mem(void *ptr, unsigned size_bytes)
{
	// Only the FPCB is reset: its units go back to reserved, with no access,
	// and are zero when next committed.
	const uintptr_t begin = (uintptr_t)ptr - (uintptr_t)fpcb_base;
	verify(fpcb_base != nullptr && begin % UNIT == 0 && begin + size_bytes <= FPCB_BYTES);
	for (size_t i = begin / UNIT; i < (begin + size_bytes) / UNIT; i++)
		unit_release(fpcb_units[i], fpcb_base + i * UNIT);
}

void ondemand_page(void *address, unsigned size_bytes)
{
	// Called from the fault handler for an FPCB page: back its unit on first
	// use, with the unit's other pages kept inaccessible so that their first
	// access faults and is filled too, then open this page.
	const uintptr_t offset = (uintptr_t)address - (uintptr_t)fpcb_base;
	if (fpcb_base == nullptr || offset >= FPCB_BYTES)
		die("PS5: on-demand page outside the FPCB");
	ps5_shm& unit = fpcb_units[offset / UNIT];
	u8 *const unit_address = fpcb_base + offset / UNIT * UNIT;
	if (unit.bytes == 0)
	{
		if (!unit_commit(unit, unit_address))
			die("PS5: backing an FPCB unit failed");
		if (mprotect(unit_address, UNIT, PROT_NONE) != 0)
			die("PS5: protecting an FPCB unit failed");
	}
	if (!protect(address, size_bytes, PROT_READ | PROT_WRITE))
		die("PS5: opening an FPCB page failed");
}

void create_mappings(const Mapping *vmem_maps, unsigned nummaps)
{
	unmap_views();
	for (unsigned i = 0; i < nummaps; i++)
	{
		// Ignore unmapped stuff, it is already reserved with no access
		if (!vmem_maps[i].memsize)
			continue;

		u64 address_range_size = vmem_maps[i].end_address - vmem_maps[i].start_address;
		unsigned num_mirrors = (address_range_size) / vmem_maps[i].memsize;
		verify((address_range_size % vmem_maps[i].memsize) == 0 && num_mirrors >= 1);

		const int protection = PS5_SHM_READ | (vmem_maps[i].allow_writes ? PS5_SHM_WRITE : 0);
		for (unsigned j = 0; j < num_mirrors; j++)
		{
			u64 offset = vmem_maps[i].start_address + j * vmem_maps[i].memsize;
			void *view = nullptr;
			int rc = ps5_shm_map(&guest_ram, vmem_maps[i].memoffset, vmem_maps[i].memsize,
					&addrspace::ram_base[offset], protection, PS5_SHM_FIXED, &view);
			if (rc != 0)
				ERROR_LOG(VMEM, "PS5: mapping %llx bytes of guest memory at %p failed: %08x",
						(unsigned long long)vmem_maps[i].memsize, &addrspace::ram_base[offset], (unsigned)rc);
			verify(rc == 0);
			views.push_back({ view, (size_t)vmem_maps[i].memsize });
		}
	}
}

bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)
{
	// code_area is null: DECLARE_CODE_CACHE declares a pointer on this platform.
	void *p = ps5_exec_allocate(size, reinterpret_cast<uintptr_t>(&destroy));
	if (p == nullptr)
	{
		ERROR_LOG(DYNAREC, "PS5: no executable memory for %zu bytes", size);
		return false;
	}
	*code_area_rwx = p;
	return true;
}

void release_jit_block(void *code_area, size_t size)
{
	ps5_exec_release(code_area);
}

bool prepare_jit_block(void *code_area, size_t size, void **code_area_rw, ptrdiff_t *rx_offset)
{
	// FEAT_NO_RWX_PAGES is not used here: the regions are read-write-execute.
	return false;
}

void release_jit_block(void *code_area1, void *code_area2, size_t size)
{
}

} // namespace virtmem

#elif !defined(__SWITCH__)
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <unistd.h>

#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_if.h"
#include "oslib/virtmem.h"

#ifndef MAP_NOSYNC
#define MAP_NOSYNC 0
#endif

#ifdef __ANDROID__
#include <linux/ashmem.h>

// Only available in SDK 26+. Required in SDK 29+ (android 10)
extern "C" int __attribute__((weak)) ASharedMemory_create(const char*, size_t);

// Android specific ashmem-device stuff for creating shared memory regions
static int ashmem_create_region(const char *name, size_t size)
{
	int fd = -1;
	if (ASharedMemory_create != nullptr)
	{
		fd = ASharedMemory_create(name, size);
		if (fd < 0)
			WARN_LOG(VMEM, "ASharedMemory_create failed: errno %d", errno);
	}

	if (fd < 0)
	{
		fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
		if (fd >= 0 && ioctl(fd, ASHMEM_SET_SIZE, size) < 0)
		{
			close(fd);
			fd = -1;
		}
	}

	return fd;
}
#endif  // #ifdef __ANDROID__

namespace virtmem
{

bool region_lock(void *start, size_t len)
{
	size_t inpage = (uintptr_t)start & PAGE_MASK;
	if (mprotect((u8*)start - inpage, len + inpage, PROT_READ))
		die("mprotect failed...");
	return true;
}

bool region_unlock(void *start, size_t len)
{
	size_t inpage = (uintptr_t)start & PAGE_MASK;
	if (mprotect((u8*)start - inpage, len + inpage, PROT_READ | PROT_WRITE))
		// Add some way to see why it failed? gdb> info proc mappings
		die("mprotect  failed...");
	return true;
}

bool region_set_exec(void *start, size_t len)
{
	size_t inpage = (uintptr_t)start & PAGE_MASK;
    int protFlags = PROT_READ | PROT_EXEC;
#ifndef TARGET_IPHONE
    protFlags |= PROT_WRITE;
#endif
	if (mprotect((u8*)start - inpage, len + inpage, protFlags))
	{
		WARN_LOG(VMEM, "region_set_exec: mprotect failed. errno %d", errno);
		return false;
	}
	return true;
}

static void *mem_region_reserve(void *start, size_t len)
{
	void *p = mmap(start, len, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (p == MAP_FAILED)
	{
		perror("mmap");
		return NULL;
	}
	else
		return p;
}

static bool mem_region_release(void *start, size_t len)
{
	return munmap(start, len) == 0;
}

static void *mem_region_map_file(void *file_handle, void *dest, size_t len, size_t offset, bool readwrite)
{
	int flags = MAP_SHARED | MAP_NOSYNC | (dest != NULL ? MAP_FIXED : 0);
	void *p = mmap(dest, len, PROT_READ | (readwrite ? PROT_WRITE : 0), flags, (int)(uintptr_t)file_handle, offset);
	if (p == MAP_FAILED)
	{
		perror("mmap");
		return NULL;
	}
	else
		return p;
}

// Allocates memory via a fd on shmem/ahmem or even a file on disk
static int allocate_shared_filemem(unsigned size)
{
	int fd = -1;
#if defined(__ANDROID__)
	// Use Android's specific shmem stuff.
	fd = ashmem_create_region("RAM", size);
#else
	#if !defined(__APPLE__)
		fd = shm_open("/dcnzorz_mem", O_CREAT | O_EXCL | O_RDWR, S_IREAD | S_IWRITE);
		shm_unlink("/dcnzorz_mem");
	#endif

	// if shmem does not work (or using OSX) fallback to a regular file on disk
	if (fd < 0) {
		std::string path = get_writable_data_path("dcnzorz_mem");
		fd = open(path.c_str(), O_CREAT|O_RDWR|O_TRUNC, S_IRWXU|S_IRWXG|S_IRWXO);
		unlink(path.c_str());
	}
	if (fd >= 0)
	{
		// Finally make the file as big as we need!
		if (ftruncate(fd, size)) {
			// Can't get as much memory as needed, fallback.
			close(fd);
			fd = -1;
		}
	}
#endif
	if (fd < 0)
		WARN_LOG(VMEM, "Virtual memory file allocation failed: errno %d", errno);

	return fd;
}

// Implement vmem initialization for RAM, ARAM, VRAM and SH4 context, fpcb etc.

int vmem_fd = -1;
static void *reserved_base;
static size_t reserved_size;

// vmem_base_addr points to an address space of 512MB that can be used for fast memory ops.
// In negative offsets of the pointer (up to FPCB size, usually 65/129MB) the context and jump table
// can be found. If the platform init returns error, the user is responsible for initializing the
// memory using a fallback (that is, regular mallocs and falling back to slow memory JIT).
bool init(void **vmem_base_addr, void **sh4rcb_addr, size_t ramSize)
{
	// Firt let's try to allocate the shm-backed memory
	vmem_fd = allocate_shared_filemem(ramSize);
	if (vmem_fd < 0)
		return false;

	// Now try to allocate a contiguous piece of memory.
	reserved_size = 512_MB + sizeof(Sh4RCB) + ARAM_SIZE_MAX + 0x10000;
	reserved_base = mem_region_reserve(NULL, reserved_size);
	if (!reserved_base) {
		close(vmem_fd);
		return false;
	}

	// Align pointer to 64KB too, some Linaro bug (no idea but let's just be safe I guess).
	uintptr_t ptrint = (uintptr_t)reserved_base;
	ptrint = (ptrint + 0x10000 - 1) & (~0xffff);
	*sh4rcb_addr = (void*)ptrint;
	*vmem_base_addr = (void*)(ptrint + sizeof(Sh4RCB));
	const size_t fpcb_size = sizeof(((Sh4RCB *)NULL)->fpcb);
	void *sh4rcb_base_ptr  = (void*)(ptrint + fpcb_size);

	// Now map the memory for the SH4 context, do not include FPCB on purpose (paged on demand).
	region_unlock(sh4rcb_base_ptr, sizeof(Sh4RCB) - fpcb_size);

	return true;
}

// Just tries to wipe as much as possible in the relevant area.
void destroy()
{
	if (reserved_base != nullptr)
	{
		mem_region_release(reserved_base, reserved_size);
		reserved_base = nullptr;
	}
	if (vmem_fd >= 0)
	{
		close(vmem_fd);
		vmem_fd = -1;
	}
}

// Resets a chunk of memory by deleting its data and setting its protection back.
void reset_mem(void *ptr, unsigned size_bytes) {
	// Mark them as non accessible.
	mprotect(ptr, size_bytes, PROT_NONE);
	// Tell the kernel to flush'em all (FIXME: perhaps unmap+mmap 'd be better?)
	madvise(ptr, size_bytes, MADV_DONTNEED);
	#if defined(MADV_REMOVE)
	madvise(ptr, size_bytes, MADV_REMOVE);
	#elif defined(MADV_FREE)
	madvise(ptr, size_bytes, MADV_FREE);
	#endif
}

// Allocates a bunch of memory (page aligned and page-sized)
void ondemand_page(void *address, unsigned size_bytes) {
	bool rc = region_unlock(address, size_bytes);
	verify(rc);
}

// Creates mappings to the underlying file including mirroring sections
void create_mappings(const Mapping *vmem_maps, unsigned nummaps) {
	for (unsigned i = 0; i < nummaps; i++) {
		// Ignore unmapped stuff, it is already reserved as PROT_NONE
		if (!vmem_maps[i].memsize)
			continue;

		// Calculate the number of mirrors
		u64 address_range_size = vmem_maps[i].end_address - vmem_maps[i].start_address;
		unsigned num_mirrors = (address_range_size) / vmem_maps[i].memsize;
		verify((address_range_size % vmem_maps[i].memsize) == 0 && num_mirrors >= 1);

		for (unsigned j = 0; j < num_mirrors; j++) {
			u64 offset = vmem_maps[i].start_address + j * vmem_maps[i].memsize;
			void *p = mem_region_map_file((void*)(uintptr_t)vmem_fd, &addrspace::ram_base[offset],
					vmem_maps[i].memsize, vmem_maps[i].memoffset, vmem_maps[i].allow_writes);
			verify(p != nullptr);
		}
	}
}

// Prepares the code region for JIT operations, thus marking it as RWX
bool prepare_jit_block(void *code_area, size_t size, void **code_area_rwx)
{
    // Try to map is as RWX, this fails apparently on OSX (and perhaps other systems?)
	if (code_area != nullptr && region_set_exec(code_area, size))
    {
        // Pointer location should be same:
        *code_area_rwx = code_area;
        return true;
    }
#ifndef TARGET_MAC
    void *ret_ptr = MAP_FAILED;
    if (code_area != nullptr)
    {
		// Well it failed, use another approach, unmap the memory area and remap it back.
        munmap(code_area, size);
        ret_ptr = mmap(code_area, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_FIXED | MAP_PRIVATE | MAP_ANON, 0, 0);
    }
    if (ret_ptr == MAP_FAILED)
    {
        // mmap at the requested code_area location failed, so let the OS pick one for us
        ret_ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (ret_ptr == MAP_FAILED)
            return false;
    }
#else
    // MAP_JIT and toggleable write protection is required on modern macOS.
    // Cannot use MAP_FIXED with MAP_JIT
    void *ret_ptr = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if ( ret_ptr == MAP_FAILED )
        return false;
#endif
    *code_area_rwx = ret_ptr;
    return true;
}

void release_jit_block(void *code_area, size_t size)
{
	munmap(code_area, size);
}

// Use two addr spaces: need to remap something twice, therefore use allocate_shared_filemem()
bool prepare_jit_block(void *code_area, size_t size, void **code_area_rw, ptrdiff_t *rx_offset)
{
	int fd = allocate_shared_filemem(size);
	if (fd < 0)
		return false;

	// Need to unmap the section we are about to use (it might be already unmapped but nevertheless...)
	munmap(code_area, size);

	// Map the RX bits on the code_area, for proximity, as usual.
	void *ptr_rx = mmap(code_area, size, PROT_READ | PROT_EXEC,
	                    MAP_SHARED | MAP_NOSYNC | MAP_FIXED, fd, 0);
	if (ptr_rx != code_area)
	{
		close(fd);
		return false;
	}

	// Now remap the same memory as RW in some location we don't really care at all.
	void *ptr_rw = mmap(NULL, size, PROT_READ | PROT_WRITE,
	                    MAP_SHARED | MAP_NOSYNC, fd, 0);

	*code_area_rw = ptr_rw;
	*rx_offset = (char*)ptr_rx - (char*)ptr_rw;
	close(fd);
	INFO_LOG(DYNAREC, "Info: Using NO_RWX mode, rx ptr: %p, rw ptr: %p, offset: %ld", ptr_rx, ptr_rw, (long)*rx_offset);

	return (ptr_rw != MAP_FAILED);
}

void release_jit_block(void *code_area1, void *code_area2, size_t size)
{
	// keep code_area1 (RX) mapped since it's statically allocated
	munmap(code_area2, size);
}

} // namespace virtmem

#endif // !__SWITCH__

namespace virtmem
{

void jit_set_exec(void* code, size_t size, bool enable) {
}

}

// Some OSes restrict cache flushing, cause why not right? :D

#if HOST_CPU == CPU_ARM64

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

namespace virtmem
{

// Code borrowed from Dolphin https://github.com/dolphin-emu/dolphin
static void Arm64_CacheFlush(void* start, void* end) {
	if (start == end)
		return;

#if defined(__APPLE__)
	// Header file says this is equivalent to: sys_icache_invalidate(start, end - start);
	sys_cache_control(kCacheFunctionPrepareForExecution, start, (uintptr_t)end - (uintptr_t)start);
#else
	// Don't rely on GCC's __clear_cache implementation, as it caches
	// icache/dcache cache line sizes, that can vary between cores on
	// big.LITTLE architectures.
	u64 addr, ctr_el0;
	static size_t icache_line_size = 0xffff, dcache_line_size = 0xffff;
	size_t isize, dsize;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr_el0));
	isize = 4 << ((ctr_el0 >> 0) & 0xf);
	dsize = 4 << ((ctr_el0 >> 16) & 0xf);

	// use the global minimum cache line size
	icache_line_size = isize = icache_line_size < isize ? icache_line_size : isize;
	dcache_line_size = dsize = dcache_line_size < dsize ? dcache_line_size : dsize;

	addr = (u64)start & ~(u64)(dsize - 1);
	for (; addr < (u64)end; addr += dsize)
		// use "civac" instead of "cvau", as this is the suggested workaround for
		// Cortex-A53 errata 819472, 826319, 827319 and 824069.
		__asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
	__asm__ volatile("dsb ish" : : : "memory");

	addr = (u64)start & ~(u64)(isize - 1);
	for (; addr < (u64)end; addr += isize)
		__asm__ volatile("ic ivau, %0" : : "r"(addr) : "memory");

	__asm__ volatile("dsb ish" : : : "memory");
	__asm__ volatile("isb" : : : "memory");
#endif
}


void flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end) {
	Arm64_CacheFlush(dcache_start, dcache_end);

	// Dont risk it and flush and invalidate icache&dcache for both ranges just in case.
	if (icache_start != dcache_start)
		Arm64_CacheFlush(icache_start, icache_end);
}

} // namespace virtmem

#elif HOST_CPU == CPU_ARM

#if defined(__APPLE__)

#include <libkern/OSCacheControl.h>

static void CacheFlush(void* code, void* pEnd)
{
    sys_dcache_flush(code, (u8*)pEnd - (u8*)code + 1);
    sys_icache_invalidate(code, (u8*)pEnd - (u8*)code + 1);
}

#elif !defined(ARMCC)

#ifdef __ANDROID__
#include <sys/syscall.h>  // for cache flushing.
#endif

static void CacheFlush(void* code, void* pEnd)
{
#if !defined(__ANDROID__)
#ifdef __GNUC__
	__builtin___clear_cache((char *)code, (char *)pEnd);
#else
	__clear_cache((void*)code, pEnd);
#endif
#else // defined(__ANDROID__)
	void* start=code;
	size_t size=(u8*)pEnd-(u8*)start+4;

  // Ideally, we would call
  //   syscall(__ARM_NR_cacheflush, start,
  //           reinterpret_cast<intptr_t>(start) + size, 0);
  // however, syscall(int, ...) is not supported on all platforms, especially
  // not when using EABI, so we call the __ARM_NR_cacheflush syscall directly.

  register uint32_t beg asm("a1") = reinterpret_cast<uint32_t>(start);
  register uint32_t end asm("a2") = reinterpret_cast<uint32_t>(start) + size;
  register uint32_t flg asm("a3") = 0;

  #ifdef __ARM_EABI__
    #if defined (__arm__) && !defined(__thumb__)
      // __arm__ may be defined in thumb mode.
      register uint32_t scno asm("r7") = __ARM_NR_cacheflush;
      asm volatile(
          "svc 0x0"
          : "=r" (beg)
          : "0" (beg), "r" (end), "r" (flg), "r" (scno));
    #else
      // r7 is reserved by the EABI in thumb mode.
      asm volatile(
      "@   Enter ARM Mode  \n\t"
          "adr r3, 1f      \n\t"
          "bx  r3          \n\t"
          ".ALIGN 4        \n\t"
          ".ARM            \n"
      "1:  push {r7}       \n\t"
          "mov r7, %4      \n\t"
          "svc 0x0         \n\t"
          "pop {r7}        \n\t"
      "@   Enter THUMB Mode\n\t"
          "adr r3, 2f+1    \n\t"
          "bx  r3          \n\t"
          ".THUMB          \n"
      "2:                  \n\t"
          : "=r" (beg)
          : "0" (beg), "r" (end), "r" (flg), "r" (__ARM_NR_cacheflush)
          : "r3");
    #endif // !defined (__arm__) || defined(__thumb__)
  #else // ! __ARM_EABI__
    #if defined (__arm__) && !defined(__thumb__)
      // __arm__ may be defined in thumb mode.
      asm volatile(
          "svc %1"
          : "=r" (beg)
          : "i" (__ARM_NR_cacheflush), "0" (beg), "r" (end), "r" (flg));
    #else
      // Do not use the value of __ARM_NR_cacheflush in the inline assembly
      // below, because the thumb mode value would be used, which would be
      // wrong, since we switch to ARM mode before executing the svc instruction
      asm volatile(
      "@   Enter ARM Mode  \n\t"
          "adr r3, 1f      \n\t"
          "bx  r3          \n\t"
          ".ALIGN 4        \n\t"
          ".ARM            \n"
      "1:  svc 0x9f0002    \n"
      "@   Enter THUMB Mode\n\t"
          "adr r3, 2f+1    \n\t"
          "bx  r3          \n\t"
          ".THUMB          \n"
      "2:                  \n\t"
          : "=r" (beg)
          : "0" (beg), "r" (end), "r" (flg)
          : "r3");
    #endif // !defined (__arm__) || defined(__thumb__)
  #endif // !__ARM_EABI__
	#if 0
		const int syscall = 0xf0002;
		__asm __volatile (
			"mov     r0, %0\n"
			"mov     r1, %1\n"
			"mov     r7, %2\n"
			"mov     r2, #0x0\n"
			"svc     0x00000000\n"
			:
			:   "r" (code), "r" (pEnd), "r" (syscall)
			:   "r0", "r1", "r7"
			);
	#endif
#endif // defined(__ANDROID__)
}
#else // defined(ARMCC)
asm static void CacheFlush(void* code, void* pEnd)
{
	ARM
	push {r7}
	//add r1, r1, r0
	mov r7, #0xf0000
	add r7, r7, #0x2
	mov r2, #0x0
	svc #0x0
	pop {r7}
	bx lr
}
#endif

namespace virtmem
{

void flush_cache(void *icache_start, void *icache_end, void *dcache_start, void *dcache_end)
{
	CacheFlush(icache_start, icache_end);
}

} // namespace virtmem

#endif // #if HOST_CPU == CPU_ARM
