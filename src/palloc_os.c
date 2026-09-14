/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
 * 
 * 
 * palloc_os — kernel VM syscall layer implementation
 */
#include "palloc_os.h"

#if defined(_WIN32)
#define OS_WINDOWS 1
#else
#define OS_POSIX 1
#endif

/* ------------------------------------------------------------------ */
/* Windows: VirtualAlloc / VirtualFree                                  */
/* ------------------------------------------------------------------ */
#if defined(OS_WINDOWS)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static size_t osPageSizeCached = 0;

uint32_t osPageSize(void) {
    if (osPageSizeCached == 0) {
        SYSTEM_INFO systemInfo;
        GetSystemInfo(&systemInfo);
        osPageSizeCached = (size_t)systemInfo.dwPageSize;
    }
    return (uint32_t)osPageSizeCached;
}

/*
 * Windows VirtualAlloc has 64KB allocation granularity only, no alignment parameter, and MEM_RELEASE
 * releases a reservation only as a whole. So we over-reserve 8GB (4GB segment + up to 4GB
 * alignment slack) and use the aligned 4GB window inside; the edges stay RESERVED (pure VA
 * waste <=4GB/segment, negligible in the 128TB x64 user space; everything vanishes at process exit).
 */
bool osSegmentReserve(uint64_t pallocSegmentBytes, uint8_t **outBase, uint8_t **outReserveBase, size_t *outReserveSize) {
    const size_t overReserveSize = pallocSegmentBytes * 2;
    uint8_t *rawBase = (uint8_t *)VirtualAlloc(NULL, overReserveSize, MEM_RESERVE, PAGE_NOACCESS);
    if (rawBase == NULL) return false;

    uintptr_t rawAddress = (uintptr_t)rawBase;
    uintptr_t alignedAddress = (rawAddress + ((uintptr_t)pallocSegmentBytes - 1)) & ~((uintptr_t)pallocSegmentBytes - 1);

    *outBase = (uint8_t *)alignedAddress;
    *outReserveBase = rawBase;
    *outReserveSize = overReserveSize;
    return true;
}

void osSegmentRelease(uint8_t *reserveBase, size_t reserveSize) {
    (void)reserveSize;
    if (reserveBase != NULL) VirtualFree(reserveBase, 0, MEM_RELEASE);
}

bool osPagesCommit(uint8_t *address, size_t length) {
    if (length == 0) return true;
    void *result = VirtualAlloc(address, length, MEM_COMMIT, PAGE_READWRITE);
    return result != NULL;
}

bool osPagesDrop(uint8_t *address, size_t length) {
    if (length == 0) return true;
    /* MEM_RESET: contents lazily dropped; pages stay committed and accessible; contents undefined on access */
    return VirtualAlloc(address, length, MEM_RESET, PAGE_READWRITE) != NULL;
}

bool osPagesReuse(uint8_t *address, size_t length) {
    (void)address;
    (void)length;
    return true; /* Windows MEM_RESET pages need no unmarking */
}

/* ------------------------------------------------------------------ */
/* POSIX: mmap / mprotect / madvise (macOS and Linux share one path)           */
/* ------------------------------------------------------------------ */
#else

#include <sys/mman.h>
#include <unistd.h>

static size_t osPageSizeCached = 0;

uint32_t osPageSize(void) {
    if (osPageSizeCached == 0) {
        long value = sysconf(_SC_PAGESIZE);
        osPageSizeCached = (value > 0) ? (size_t)value : 4096;
    }
    return (uint32_t)osPageSizeCached;
}

/*
 * POSIX mmap has no alignment parameter: over-reserve 8GB, then munmap the head and
 * tail outside the aligned 4GB window; the segment ends up exactly 4GB of VA, no waste.
 */
bool osSegmentReserve(uint64_t pallocSegmentBytes, uint8_t **outBase, uint8_t **outReserveBase, size_t *outReserveSize) {
    const size_t overReserveSize = (size_t)pallocSegmentBytes * 2;
    void *rawBase = mmap(NULL, overReserveSize, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (rawBase == MAP_FAILED) return false;

    uintptr_t rawAddress = (uintptr_t)rawBase;
    uintptr_t alignedAddress = (rawAddress + ((uintptr_t)pallocSegmentBytes - 1)) & ~((uintptr_t)pallocSegmentBytes - 1);
    uintptr_t alignedEnd = alignedAddress + (uintptr_t)pallocSegmentBytes;
    uintptr_t rawEnd = rawAddress + overReserveSize;

    if (alignedAddress > rawAddress) {
        munmap(rawBase, (size_t)(alignedAddress - rawAddress));
    }
    if (rawEnd > alignedEnd) {
        munmap((void *)alignedEnd, (size_t)(rawEnd - alignedEnd));
    }

    *outBase = (uint8_t *)alignedAddress;
    *outReserveBase = (uint8_t *)alignedAddress;
    *outReserveSize = (size_t)pallocSegmentBytes;
    return true;
}

void osSegmentRelease(uint8_t *reserveBase, size_t reserveSize) {
    if (reserveBase != NULL && reserveSize > 0) {
        munmap(reserveBase, reserveSize);
    }
}

bool osPagesCommit(uint8_t *address, size_t length) {
    if (length == 0) return true;
    return mprotect(address, length, PROT_READ | PROT_WRITE) == 0;
}

bool osPagesDrop(uint8_t *address, size_t length) {
    if (length == 0) return true;
#if defined(__APPLE__)
    /* macOS MADV_DONTNEED only deactivates pages (resident_size does not drop, measured);
       MADV_FREE_REUSABLE marks them as instantly reclaimable (purgeable) */
    return madvise(address, length, MADV_FREE_REUSABLE) == 0;
#else
    /* Linux: drop contents and return the physical pages; later access yields zero pages */
    return madvise(address, length, MADV_DONTNEED) == 0;
#endif
}

bool osPagesReuse(uint8_t *address, size_t length) {
#if defined(__APPLE__)
    if (length == 0) return true;
    /* Clear the purgeable mark; if left set the kernel may reclaim pages after writes */
    return madvise(address, length, MADV_FREE_REUSE) == 0;
#else
    (void)address;
    (void)length;
    return true;
#endif
}

#endif
