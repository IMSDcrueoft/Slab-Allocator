/*
* MIT License
* Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
* See LICENSE file in the root directory for full license text.
*
 * slab — one-tier small-object allocator (≤256B slab arenas)
 *
 * Allocates only up to 256 bytes through 16KB slab arenas with 6 slot classes (8/16/32/64/128/256).
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
 * Free-slot convention: every arena allocates bump-style from its never-used tail and
 * threads ONE LIFO free list through freed slots (the next pointer lives in the slot's
 * first 4 bytes, the head in the header). Claim pops the list; when it is empty the bump
 * position is derived (totalSlots - freeSlotCount) — no threading pass, no scan, O(1)
 * always. Free pushes back. Double-free is NOT detected: freed slot memory doubles as
 * freelist state, so writing into a freed slot corrupts the list (use-after-free is
 * undefined).
 * Deterministic latency: each class chain holds allocatable arenas — partial arenas plus
 * up to CLASS_SPARE_KEEP warm empties that free keeps linked (beyond-quota empties park
 * into the cross-class resident ring, whose cold tail trim drops); allocation always
 * claims from the chain head and unlinks the head the moment it fills. Worst case adds
 * one page-reuse syscall or one carve+commit — never a chain walk. Full arenas are
 * off-chain; free finds them by masking.
 * Threading: single-threaded, lock-free; alloc and free are both O(1) deterministic.
*/
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

 /* ---- Portable static assertions (works on MSVC / GCC / Clang, C and C++)---- */
#define slabConcat2(a, b) a##b
#define slabConcat(a, b) slabConcat2(a, b)
#define slabStaticAssert(condition) \
	typedef char slabConcat(slabStaticAssert_, __LINE__)[(condition) ? 1 : -1]

#ifdef __cplusplus
extern "C" {
#endif

	/* ---- Constants ---- */
	enum {
		ARENA_SIZE_SMALL = 16384,      /* 16KB slab arena */
		SLOT_CLASS_SHIFT_MIN = 3,      /* log2(SLOT_SIZE_MIN): classSize(i) = 1 << (i + SLOT_CLASS_SHIFT_MIN) */
		SLOT_SIZE_MIN = 1 << SLOT_CLASS_SHIFT_MIN, /* 8 */
		SLOT_SIZE_MAX = 256,
		SLOT_CLASS_COUNT = 6,          /* 8/16/32/64/128/256 */
		TRIM_POOL_MAX_RESIDENT = 256,   /* resident ring water mark (256 x 16KB = 4MB): free only
		                                * parks shells into the ring; trim is the sole dropper and
		                                * returns the pages of the coldest shells beyond this mark */
		CLASS_SPARE_KEEP = 2,          /* warm empties kept LINKED per class chain: the pair pattern
		                                * re-claims them straight off the chain head with zero relink
		                                * churn; empties beyond the quota park into the resident ring
		                                * (spareCount is uint8 — keep this in the 1..255 range) */
		SEGMENT_SIZE_EXPONENT_MIN = 20,     /* smallest segment: 1<<20 = 1MB (keeps the base 16KB-aligned) */
		SEGMENT_SIZE_EXPONENT_MAX = 40,     /* largest segment: 1<<40 = 1TB */
		SEGMENT_SIZE_EXPONENT_DEFAULT = 32, /* arenaSlabDefault: 1<<32 = 4GB */
		SLAB_OFFSET_NONE = -1,         /* segment-relative offset sentinel, (uint64_t)UINT64_MAX */
		SLAB_OFFSET_UNLINKED = -2,     /* arena->next marker: arena is fully unlinked from its chain */
		SLAB_SLOT_NONE = -1,           /* arena->freeHead sentinel, (uint32_t)UINT32_MAX: list empty */
	};

	/* Slot classes are consecutive powers of two tiling [SLOT_SIZE_MIN, SLOT_SIZE_MAX]
	 * (classSize(i) = 1 << (i + SLOT_CLASS_SHIFT_MIN)); arena_slab.c derives all class
	 * geometry from this by ctz/shift, so the chain is pinned at compile time. */
	slabStaticAssert(SLOT_CLASS_SHIFT_MIN >= 3); /* 8B class pointers are 8B aligned; every other class 16B */
	slabStaticAssert((SLOT_SIZE_MIN << (SLOT_CLASS_COUNT - 1)) == SLOT_SIZE_MAX);
	slabStaticAssert((ARENA_SIZE_SMALL & (ARENA_SIZE_SMALL - 1)) == 0); /* arena masking needs 2^N */
	slabStaticAssert((ARENA_SIZE_SMALL % SLOT_SIZE_MAX) == 0); /* whole slots per arena, exact freelist threading */

	/* ---- Arena header (32B, at the start of every arena)----
	 * The header region is the head struct alone; headerSlots covers it in whole slots
	 * and the free list is threaded starting AT headerSlots (header slots never enter
	 * the list, so no bitmap is needed to mark them).
	 * slots [0, headerSlots) are permanently reserved.
	 * headerSlots is a pure function of classSize:
	 * headerSlots = ceil(32 / classSize)   (8B -> 4 slots, 16B -> 2, every other class -> 1)
	 * Capacities: 8B -> 2044 slots, 16B -> 1022, 32B -> 511, 64B -> 255, 128B -> 127, 256B -> 63.
	 */
	typedef struct ArenaHead {
		uint32_t classSize;      /* slot size (2^4..2^8) */
		uint16_t headerSlots;    /* slots reserved for the header region */
		uint16_t freeSlotCount;  /* free slots remaining == free list length */
		uint32_t magic;          /* validation code (checked on free) */
		uint32_t freeHead;       /* free-list head: slot index of the most recently freed
		                         * slot; its first 4 bytes hold the next index;
		                         * slabSlotNone = list empty — the free range is then the
		                         * untouched bump tail [totalSlots - freeSlotCount, totalSlots) */
		uint64_t next;           /* segment-relative offset of the successor node;
									slabOffsetNone = class-chain tail; slabOffsetUnlinked = off chain
									(off chain <=> full: only the chain head is allocated from,
									and it is unlinked when its last free slot is claimed;
									free() pushes a freed full arena back at the chain head;
									on the resident ring a singleton links to itself)
									(u64 because a segment may be larger than 4GB) */
		uint64_t prev;           /* segment-relative offset of the predecessor node — a just-emptied
									arena sits mid-chain and free must unlink it O(1) to park it into
									the resident ring; slabOffsetNone = class-chain head,
									slabOffsetUnlinked = off chain; on the resident ring head->prev
									is the ring TAIL (the coldest shell, trim drops from there)
									(u64 because a segment may be larger than 4GB) */
	} ArenaHead;

	/* ---- Segment and context ----
	 * All allocator state lives in this context (BSS static or embedder-provided); the single
	 * segment carries no metadata; free() resolves via arena headers + the segment base range check.
	 * The context is gated by a 64-bit cookie: APIs reject anything arenaSlab_init has not
	 * stamped (zeroed, garbage or foreign memory), so no pre-zeroing is ever required.
	 */
	typedef struct Segment {
		uint64_t partial[SLOT_CLASS_COUNT]; /* per-class chains: allocatable arenas (partial) plus
		                                     * up to CLASS_SPARE_KEEP warm empties per class; free
		                                     * parks beyond-quota empties into the shared ring below */
		uint8_t* base;         /* segment base, aligned to the segment size */
		uint8_t* frontier;     /* carve frontier (monotonically grows) */
		uint8_t* committedEnd; /* end of the committed range */
		uint64_t bytes;        /* segment virtual address size (= 1 << segmentSizeExponent = alignment) */
		void* reserveBase;  /* actual reservation base (differs from base on the over-alignment path) */
		size_t reserveSize;  /* actual reservation size */
		size_t residentCount; /* len(resident); makes trim's water-mark check O(1) */
		uint64_t resident;     /* resident ring head: empty shells parked by free with RESIDENT pages —
		                        * head->prev is the ring TAIL (coldest); revival pops the head (free) */
		uint64_t revive;       /* singly-linked chain (next only): shells whose pages were returned —
		                        * revival pops here second and pays osPagesReuse */
		uint64_t spare[SLOT_CLASS_COUNT][CLASS_SPARE_KEEP]; /* per-class warm empties, LINKED on
		                        * their chain (they never enter the ring); entries go stale when
		                        * the arena is claimed again and are validated lazily at park */
		uint8_t  spareCount[SLOT_CLASS_COUNT]; /* per-class spare population, <= CLASS_SPARE_KEEP */
#ifdef LOG_MALLOC_STATS
		void* ownerStats;   /* slabLayerStats of the owning allocator (wired at init) */
#endif
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
		uint64_t cookie;       /* gate: set by arenaSlab_init, cleared by arenaSlab_shutdown; 0 = not
		                        * initialized. Every API rejects contexts without it, so embedder-provided
		                        * contexts need no pre-zeroing — garbage memory is rebuilt by init.
		                        * First field on purpose: the gate shares cache line 0 with the hot
		                        * segment fields (partial/base/frontier) instead of touching a second line. */
		Segment segment;       /* the one and only segment; no cross-segment growth by design */
#ifdef LOG_MALLOC_STATS
		slabLayerStats stats; /* small-layer stats only */
#endif
	} ArenaSlabAllocator;

	extern ArenaSlabAllocator arenaSlabDefault; /* BSS zeroed (cookie == 0): every call is safely
	                                             * rejected until arenaSlab_init succeeds */

	/* ---- API ----
	 * Only objects up to 256 bytes are handled; larger requests return NULL.
	 * Use the platform's malloc/free for anything > 256.
	 * Returned pointers are 16-byte aligned, except the 8B class: 8-byte aligned.
	 */

	 /* Factory: reserve the single segment (eager, PROT_NONE) sized 1 << segmentSizeExponent.
	  * segmentSizeExponent must be in [SEGMENT_SIZE_EXPONENT_MIN, SEGMENT_SIZE_EXPONENT_MAX].
	  * The default instance is meant to be built as:
	  *   arenaSlab_init(&arenaSlabDefault, SEGMENT_SIZE_EXPONENT_DEFAULT);
	  * Returns false on an invalid exponent or reservation failure (cookie left 0: every
	  * API stays rejected; the next init rebuilds in place); returns true as a no-op when
	  * the context is already initialized (cookie matches).
	  * Garbage / never-zeroed contexts are detected by the cookie and rebuilt field by
	  * field — no blanket memset, the cookie gate carries the safety. */
	bool arenaSlab_init(ArenaSlabAllocator* context, uint8_t segmentSizeExponent);

	/* Release the reservation; no-op on never-initialized / foreign contexts (cookie gate);
	 * safe to arenaSlab_init again afterwards */
	void arenaSlab_shutdown(ArenaSlabAllocator* context);

	/* Allocate; size 0 is treated as 8; returns NULL on failure or size > 256 */
	void* arenaSlab_alloc(ArenaSlabAllocator* context, size_t size);

	/* Free; returns false for invalid / non-slab pointers (caller owns those) */
	bool arenaSlab_free(ArenaSlabAllocator* context, void* pointer);

	/* Reallocate; works only for small-object pointers; other pointers go through the platform realloc path? */
	void* arenaSlab_realloc(ArenaSlabAllocator* context, void* pointer, size_t newSize);

	/* Allocate count * size bytes, zero-initialized — the only API that touches payload
	 * bytes. count * size overflow returns NULL; a zero total follows alloc's size-0 rule
	 * (an 8B slot with nothing to zero) */
	void* arenaSlab_calloc(ArenaSlabAllocator* context, size_t count, size_t size);

	/* True if the size is valid for alloc / realloc: size <= SLOT_SIZE_MAX — alloc treats
	 * 0 as 8 and realloc treats 0 as free; anything above 256 is the caller's business */
	bool arenaSlab_permissible_size(size_t size);

	/* Usable payload bytes; returns 0 for invalid pointers */
	size_t arenaSlab_usable_size(ArenaSlabAllocator* context, void* pointer);

	/* Which layer owns this pointer: true only inside the carved arenas [base, frontier);
	 * uncarved reservation space is rejected (returns SLAB_LAYER_NONE) */
	typedef enum { SLAB_LAYER_NONE = 0, SLAB_LAYER_SMALL } slabLayer;
	slabLayer arenaSlab_which(ArenaSlabAllocator* context, void* pointer);

	/* Segment base for external pointer compression, returned as an integer on purpose:
	 * treat it only as a numeric base — store heap references as offsets and decode as
	 * base + offset (exact as u32 while the segment is <= 4GB); do not dereference it.
	 * Returns 0 while the context is uninitialized. */
	uintptr_t arenaSlab_segmentBase(ArenaSlabAllocator* context);

	/* Lazy return, two-phase by design: free keeps the first CLASS_SPARE_KEEP empties of a
	 * class LINKED as warm spares and parks every beyond-quota empty into the shared
	 * resident ring (O(1), pages stay committed, revival is a plain re-init); trim is the
	 * only dropper — while the ring exceeds TRIM_POOL_MAX_RESIDENT it returns the pages of
	 * the coldest ring shells and re-homes them to the revive chain. The water-mark shells
	 * stay warm for free revival between trims; the per-class spares never enter the ring
	 * (bounded: CLASS_SPARE_KEEP arenas per class). Returns dropped bytes.
	 * trim is NEVER called automatically by design; call it from safe points. */
	size_t arenaSlab_trim(ArenaSlabAllocator* context);

	/* ---- Statistics (enabled with -DLOG_MALLOC_STATS)---- */
	void arenaSlab_statsReset(ArenaSlabAllocator* context);
	void arenaSlab_dumpStats(ArenaSlabAllocator* context);

#ifdef __cplusplus
}
#endif
