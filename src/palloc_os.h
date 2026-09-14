/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
 * 
 * 
 * palloc_os — kernel VM syscall primitives (mmap/mprotect/madvise, VirtualAlloc/VirtualFree)
 *
 * This layer knows nothing about any allocator: it exposes raw reservation / commit / drop
 * primitives and nothing else.
 *
 * macOS / Linux: mmap + mprotect + madvise
 * Windows:       VirtualAlloc(MEM_RESERVE/MEM_COMMIT/MEM_RESET) + VirtualFree
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reserve a aligned,alingn-sized, inaccessible (PROT_NONE / PAGE_NOACCESS) address range.
 * On success *outBase is the aligned base; on the Windows over-reservation path
 * *outReserveBase / *outReserveSize return the real base and size (needed for release).
 */
bool osSegmentReserve(uint64_t pallocSegmentBytes, uint8_t **outBase, uint8_t **outReserveBase, size_t *outReserveSize);

/* Release the whole reservation (must pass the reserveBase/reserveSize from osSegmentReserve) */
void osSegmentRelease(uint8_t *reserveBase, size_t reserveSize);

/* Commit [address, address+length) as read-write; repeatable (idempotent) */
bool osPagesCommit(uint8_t *address, size_t length);

/*
 * Lazily drop the contents of [address, address+length):
 *   macOS:   madvise(MADV_FREE_REUSABLE) — pages become purgeable (instantly reclaimable)
 *   Linux:   madvise(MADV_DONTNEED) — pages returned to the kernel immediately
 *   Windows: VirtualAlloc(MEM_RESET) — pages stay accessible, contents become undefined
 * Returns whether the syscall succeeded; contents are treated as garbage.
 * address and length must be aligned to osPageSize().
 */
bool osPagesDrop(uint8_t *address, size_t length);

/*
 * Clears the drop mark set by osPagesDrop (pages become normally resident again).
 * Must be called before handing previously dropped memory to the user; otherwise macOS purgeable
 * pages can still be reclaimed by the kernel after writes, corrupting live data.
 *   macOS:   madvise(MADV_FREE_REUSE)
 *   Linux/Windows: not needed (no-op)
 */
bool osPagesReuse(uint8_t *address, size_t length);

/* Runtime page size (macOS arm64 = 16KB, x86-64 = 4KB, Windows follows the system) */
uint32_t osPageSize(void);

#ifdef __cplusplus
}
#endif

