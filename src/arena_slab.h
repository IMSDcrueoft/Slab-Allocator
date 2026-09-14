/*
* MIT License
* Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
* See LICENSE file in the root directory for full license text.
*
* slab — one-tier small-object allocator (≤256B slab arenas)
*
* Allocates only up to 256 bytes through 16KB slab arenas with 5 slot classes (16/32/64/128/256).
* Everything larger is the caller's responsibility (libc malloc, kernel VM, etc.).
*
* Platform:  macOS / Linux / Windows — depends only on kernel VM syscalls
*         (mmap/mprotect/madvise and VirtualAlloc/VirtualFree); no libc allocator involved
* Compilers: MSVC / GCC / Clang (C11)
*
* Memory layout:
*   ONE reserved segment per allocator; its virtual address size (= alignment) is chosen by the
*   caller as 1 << segmentSizeExponent, valid range [1MB, 1TB] (the default instance uses 4GB).
*   Arenas are carved and committed on demand (16KB per arena) inside this single segment.
*   There is NO automatic segment growth: once the segment is exhausted, alloc returns NULL
*   until free/trim reclaim space. Pages of empty arenas can be returned via arenaSlab_trim
*   (the header page is always kept).
*
* Bitmap convention: bit=1 free, bit=0 used; allocation scans with ctz, lowest index first.
* Threading: single-threaded, lock-free; the free path is O(1) deterministic.
*/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

	/* ---- Constants ---- */
	enum {
		ARENA_SIZE_SMALL = 16384,      /* 16KB slab arena */
		SLOT_SIZE_MIN = 16,
		SLOT_SIZE_MAX = 256,
		SLOT_CLASS_COUNT = 5,          /* 16/32/64/128/256 */
		SEGMENT_SIZE_EXPONENT_MIN = 20,     /* smallest segment: 1<<20 = 1MB (keeps the base 16KB-aligned) */
		SEGMENT_SIZE_EXPONENT_MAX = 40,     /* largest segment: 1<<40 = 1TB */
		SEGMENT_SIZE_EXPONENT_DEFAULT = 32, /* arenaSlabDefault: 1<<32 = 4GB */
		SLAB_OFFSET_NONE = -1,         /* segment-relative offset sentinel, (uint64_t)UINT64_MAX */
		SLAB_OFFSET_UNLINKED = -2,     /* arena->next marker: arena is fully unlinked from its chain */
	};

	/* ---- Arena header (24B, at the start of every arena)----
	 * The bitmap follows right after; the header region (head + bitmap) is rounded up to whole slots;
	 * slots [0, headerSlots) are permanently 0 (used) in the bitmap.
	 * headerSlots is a pure function of classSize:
	 *   headerSlots = ceil((24 + bitMapCount*8) / classSize)
	 */
	typedef struct ArenaHead {
		uint16_t classSize;      /* slot size (2^4..2^8) */
		uint16_t headerSlots;    /* slots reserved for the header region */
		uint16_t freeSlotCount;  /* free slots remaining (avoids frequent bitmap scans) */
		uint16_t bitMapCount;    /* number of u64 words in the bitmap (avoids boilerplate bounds checks) */
		uint32_t magic;          /* validation code (checked on free) */
		uint64_t next;           /* segment-relative offset of next arena in the class chain;
									slabOffsetNone = chain tail; slabOffsetUnlinked = off chain
									(u64 because a segment may be larger than 4GB) */
	} ArenaHead;

	/* ---- Segment and context ----
	 * All allocator state lives in this context (BSS static or embedder-provided); the single
	 * segment carries no metadata; free() resolves via arena headers + the segment base range check.
	 */
	typedef struct Segment {
		uint64_t partial[SLOT_CLASS_COUNT]; /* per-class partial arena chain heads (segment-relative offsets) */
		uint8_t* base;         /* segment base, aligned to the segment size */
		uint8_t* frontier;     /* carve frontier (monotonically grows) */
		uint8_t* committedEnd; /* end of the committed range */
		uint64_t bytes;        /* segment virtual address size (= 1 << segmentSizeExponent = alignment) */
		void* reserveBase;  /* actual reservation base (differs from base on the over-alignment path) */
#ifdef LOG_MALLOC_STATS
		void* ownerStats;   /* slabLayerStats of the owning allocator (wired at init) */
#endif
		size_t   reserveSize;  /* actual reservation size */
	} Segment;

	/* ---- Statistics (compile switch: -DLOG_MALLOC_STATS)---- */
#ifdef LOG_MALLOC_STATS
	typedef struct slabLayerStats { /* small only */
		uint64_t allocCount;
		uint64_t allocBytes;
		uint64_t allocFailed;
		uint64_t freeCount;
		uint64_t freeBytes;
		uint64_t freeRejected;
		uint64_t carveCount;
		uint64_t commitCalls;
		uint64_t commitBytes;
		uint64_t dropCalls;
		uint64_t dropBytes;
		uint64_t reuseCalls;
		uint64_t reuseBytes;
	} slabLayerStats;
#endif

	typedef struct ArenaSlabAllocator {
		Segment segment;       /* the one and only segment; no cross-segment growth by design */
		/* Hot-path cache: raw arena pointers, one per class. */
		ArenaHead* currentSmall[SLOT_CLASS_COUNT];
		bool    initialized;
#ifdef LOG_MALLOC_STATS
		slabLayerStats stats; /* small-layer stats only */
#endif
	} ArenaSlabAllocator;

	extern ArenaSlabAllocator arenaSlabDefault; /* BSS static; initialize with arenaSlab_init first */

	/* ---- API ----
	 * Only objects up to 256 bytes are handled; larger requests return NULL.
	 * Use the platform's malloc/free for anything > 256.
	 * Returned pointers are always 16-byte aligned.
	 */

	 /* Factory: reserve the single segment (eager, PROT_NONE) sized 1 << segmentSizeExponent.
	  * segmentSizeExponent must be in [SEGMENT_SIZE_EXPONENT_MIN, SEGMENT_SIZE_EXPONENT_MAX].
	  * The default instance is meant to be built as:
	  *   arenaSlab_init(&arenaSlabDefault, SEGMENT_SIZE_EXPONENT_DEFAULT);
	  * Returns false on an invalid exponent or reservation failure (state rolled back);
	  * returns true as a no-op when the context is already initialized. */
	bool arenaSlab_init(ArenaSlabAllocator* context, uint8_t segmentSizeExponent);

	/* Release the reservation; safe to arenaSlab_init again afterwards */
	void arenaSlab_shutdown(ArenaSlabAllocator* context);

	/* Allocate; size 0 is treated as 16; returns NULL on failure or size > 256 */
	void* arenaSlab_alloc(ArenaSlabAllocator* context, size_t size);

	/* Free; returns false for invalid / non-slab pointers (caller owns those) */
	bool arenaSlab_free(ArenaSlabAllocator* context, void* pointer);

	/* Reallocate; works only for small-object pointers; other pointers go through the platform realloc path? */
	void* arenaSlab_realloc(ArenaSlabAllocator* context, void* pointer, size_t newSize);

	/* Usable payload bytes; returns 0 for invalid pointers */
	size_t arenaSlab_usable_size(ArenaSlabAllocator* context, void* pointer);

	/* Which layer owns this pointer (returns SLAB_LAYER_NONE for non-small pointers) */
	typedef enum { SLAB_LAYER_NONE = 0, SLAB_LAYER_SMALL } slabLayer;
	slabLayer arenaSlab_which(ArenaSlabAllocator* context, void* pointer);

	/* Lazy return: drop page contents of empty arenas (metadata is kept);
	 * never changes allocator state; safe to call anytime; returns dropped bytes.
	 * trim is NEVER called automatically by design; call it from safe points. */
	size_t arenaSlab_trim(ArenaSlabAllocator* context);

	/* ---- Statistics (enabled with -DLOG_MALLOC_STATS)---- */
	void arenaSlab_statsReset(ArenaSlabAllocator* context);
	void arenaSlab_dumpStats(ArenaSlabAllocator* context);

#ifdef __cplusplus
}
#endif
