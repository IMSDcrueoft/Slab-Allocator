/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
 *
 *
 * slab — small-object slab allocator core (≤256B only)
 */
#include "arena_slab.h"
#include "palloc_os.h"
#include "bits.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

 /* ---- Per-class arena layouts (documentation + static asserts; computed by arenaInit at runtime)----
 * The 32B header occupies whole slots; claims bump forward from headerSlots and freed
 * slots re-enter via the LIFO list — no bitmap, no hint region, no threading pass. */

// 16KB / 256B = 64 slots
typedef struct {
	ArenaHead head;              // 32B
	uint8_t   alignment[224];    // -> 256B (1 header slot)
	uint8_t   payload[256 * (64 - 1)]; // 63 slots
} Arena16K_256B;

// 16KB / 128B = 128 slots
typedef struct {
	ArenaHead head;              // 32B
	uint8_t   alignment[96];     // -> 128B (1 header slot)
	uint8_t   payload[128 * (128 - 1)]; // 127 slots
} Arena16K_128B;

// 16KB / 64B = 256 slots
typedef struct {
	ArenaHead head;              // 32B
	uint8_t   alignment[32];     // -> 64B (1 header slot)
	uint8_t   payload[64 * (256 - 1)]; // 255 slots
} Arena16K_64B;

// 16KB / 32B = 512 slots
typedef struct {
	ArenaHead head;              // 32B (1 header slot)
	uint8_t   payload[32 * (512 - 1)]; // 511 slots
} Arena16K_32B;

// 16KB / 16B = 1024 slots
typedef struct {
	ArenaHead head;              // 32B (2 header slots)
	uint8_t   payload[16 * (1024 - 2)]; // 1022 slots
} Arena16K_16B;

// 16KB / 8B = 2048 slots
typedef struct {
	ArenaHead head;              // 32B (4 header slots)
	uint8_t   payload[8 * (2048 - 4)]; // 2044 slots
} Arena16K_8B;

slabStaticAssert(sizeof(ArenaHead) == 32);
slabStaticAssert(sizeof(Arena16K_8B) == ARENA_SIZE_SMALL);
slabStaticAssert(sizeof(Arena16K_256B) == ARENA_SIZE_SMALL);
slabStaticAssert(sizeof(Arena16K_128B) == ARENA_SIZE_SMALL);
slabStaticAssert(sizeof(Arena16K_64B) == ARENA_SIZE_SMALL);
slabStaticAssert(sizeof(Arena16K_32B) == ARENA_SIZE_SMALL);
slabStaticAssert(sizeof(Arena16K_16B) == ARENA_SIZE_SMALL);


/* ---- Internal constants ---- */
static const uint32_t magicArenaSmall = 0x6D413136; /* "mA16" */
static const uint32_t magicArenaSmallDropped = 0x6D413137; /* "mA17": payload pages dropped by trim */
static const uint64_t arenaSlabCookie = 0x6D41534C41423131; /* "mASLAB11": context gate stamped by arenaSlab_init */

/* ---- Statistics helpers (only with -DLOG_MALLOC_STATS)---- */
#ifdef LOG_MALLOC_STATS
static slabLayerStats* segmentStats(Segment* segment) {
	return (slabLayerStats*)segment->ownerStats;
}
#endif

/* ---- Small helpers ---- */
static uintptr_t alignUp(uintptr_t value, uintptr_t alignment) {
	return (value + alignment - 1) & ~(alignment - 1);
}

static uint32_t arenaCapacity(const ArenaHead* arena) {
	return (uint32_t)(ARENA_SIZE_SMALL >> bits_ctz64(arena->classSize)) - arena->headerSlots;
}

static uint32_t arenaKeptBytes(uint32_t pageSize) {
	return pageSize;
}

/* ---- Arena initialization ----
 * O(1) on purpose: the freelist starts empty and claims bump forward from the tail
 * (the bump position is derived from freeSlotCount — see arenaSlotClaim), so carving
 * or reviving an arena never pays an O(slots) threading pass. */
static void arenaInit(ArenaHead* arena, uint32_t arenaBytes, uint32_t shift, uint32_t magic) {
	assert(shift >= SLOT_CLASS_SHIFT_MIN && shift < SLOT_CLASS_SHIFT_MIN + SLOT_CLASS_COUNT);
	uint32_t classSize = (uint32_t)1 << shift; /* slot classes are powers of two (8..256) */
	uint32_t totalSlots = arenaBytes >> shift;
	uint32_t headerSlots = (uint32_t)(sizeof(ArenaHead) + classSize - 1) >> shift;

	arena->classSize = (uint32_t)classSize;
	arena->headerSlots = (uint16_t)headerSlots;
	arena->freeSlotCount = (uint16_t)(totalSlots - headerSlots);
	arena->magic = magic;
	arena->freeHead = (uint32_t)SLAB_SLOT_NONE;
}

/* ---- Segment operations ---- */

/* Single-segment ownership check: returns the segment when address falls inside the carved
 * region [base, frontier) — every 16KB block there is a committed, initialized arena, so a
 * positive answer makes the arena header safe to read. Addresses inside the reservation but
 * beyond the frontier are PAGE_NOACCESS and must be rejected without dereferencing. */
static Segment* segmentFind(ArenaSlabAllocator* context, uintptr_t address) {
	Segment* segment = &context->segment;
	if (segment->base == NULL) return NULL;
	if (address >= (uintptr_t)segment->base && address < (uintptr_t)segment->frontier) return segment;
	return NULL;
}

static bool segmentEnsureCommitted(Segment* segment, uint8_t* neededEnd) {
	if (neededEnd <= segment->committedEnd) return true;
	uint8_t* commitEnd = (uint8_t*)alignUp((uintptr_t)neededEnd, ARENA_SIZE_SMALL);
	uint8_t* segmentEnd = segment->base + segment->bytes;
	if (commitEnd > segmentEnd) commitEnd = segmentEnd;
	size_t commitLength = (size_t)(commitEnd - segment->committedEnd);
	if (!osPagesCommit(segment->committedEnd, commitLength)) return false;
#ifdef LOG_MALLOC_STATS
	slabLayerStats* stats = segmentStats(segment);
	if (stats != NULL) {
		stats->commitCalls++;
		stats->commitBytes += commitLength;
	}
#endif
	segment->committedEnd = commitEnd;
	return true;
}

static ArenaHead* segmentCarveArena(Segment* segment, uint32_t arenaBytes, uint32_t shift, uint32_t magic) {
	if ((uint64_t)(segment->frontier - segment->base) + arenaBytes > segment->bytes) return NULL;
	if (!segmentEnsureCommitted(segment, segment->frontier + arenaBytes)) return NULL;
	ArenaHead* arena = (ArenaHead*)segment->frontier;
	segment->frontier += arenaBytes;
	arenaInit(arena, arenaBytes, shift, magic);
#ifdef LOG_MALLOC_STATS
	slabLayerStats* stats = segmentStats(segment);
	if (stats != NULL) stats->carveCount++;
#endif
	return arena;
}

static void segmentPushArena(Segment* segment, uint32_t chainIndex, ArenaHead* arena) {
	uint8_t* base = segment->base; /* hoisted: relaxed aliasing forces a reload per access */
	uint64_t self = (uint64_t)((uint8_t*)arena - base);
	uint64_t headOffset = segment->partial[chainIndex];
	arena->prev = (uint64_t)SLAB_OFFSET_NONE; /* pushed at the head: no predecessor */
	arena->next = headOffset;
	if (headOffset != (uint64_t)SLAB_OFFSET_NONE) {
		((ArenaHead*)(base + headOffset))->prev = self;
	}
	segment->partial[chainIndex] = self;
}

/* Unlink the chain head: called when its last free slot was just claimed (off chain <=> full) */
static void arenaPopHead(Segment* segment, uint32_t chainIndex, ArenaHead* arena) {
	uint8_t* base = segment->base; /* hoisted: relaxed aliasing forces a reload per access */
	uint64_t next = arena->next;
	if (next != (uint64_t)SLAB_OFFSET_NONE) {
		((ArenaHead*)(base + next))->prev = (uint64_t)SLAB_OFFSET_NONE; /* successor becomes the head */
	}
	segment->partial[chainIndex] = next;
	arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
	arena->prev = (uint64_t)SLAB_OFFSET_UNLINKED;
}

/* O(1) unlink from an arbitrary class-chain position (prev reaches the predecessor):
 * called when a free just emptied the arena, right before it parks into the resident
 * ring. Head and tail fall out of the sentinel math (prev == none => chain head).
 * Field offsets are hoisted into locals BEFORE any store: with -relaxed-aliasing the
 * compiler must otherwise re-read every header field it stores through. */
static void chainDelink(Segment* segment, uint32_t chainIndex, ArenaHead* arena) {
	uint8_t* base = segment->base; /* hoisted: relaxed aliasing forces a reload per access */
	uint64_t prev = arena->prev;
	uint64_t next = arena->next;
	if (prev != (uint64_t)SLAB_OFFSET_NONE) ((ArenaHead*)(base + prev))->next = next;
	else segment->partial[chainIndex] = next;
	if (next != (uint64_t)SLAB_OFFSET_NONE) ((ArenaHead*)(base + next))->prev = prev;
	arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
	arena->prev = (uint64_t)SLAB_OFFSET_UNLINKED;
}

/* Shared pool of emptied shells, linked as a CIRCULAR doubly-linked ring (slab.c
 * convention): free parks at the head (hottest), revival pops the head (a plain
 * re-init, no syscall), trim pops the tail (coldest) to return pages. Pages stay
 * committed while parked; head->prev is the ring tail, a singleton links to itself. */
static void segmentRingPush(Segment* segment, ArenaHead* arena) {
	uint8_t* base = segment->base; /* hoisted: relaxed aliasing forces a reload per access */
	uint64_t self = (uint64_t)((uint8_t*)arena - base);
	if (segment->residentCount == 0) {
		arena->next = self; /* singleton ring: self-linked */
		arena->prev = self;
	}
	else {
		uint64_t headOffset = segment->resident;
		uint64_t tailOffset = ((ArenaHead*)(base + headOffset))->prev; /* read once, used twice */
		arena->next = headOffset;
		arena->prev = tailOffset;
		((ArenaHead*)(base + tailOffset))->next = self;
		((ArenaHead*)(base + headOffset))->prev = self;
	}
	segment->resident = self;
	segment->residentCount++;
}

/* Uniform O(1) unlink of a ring member: works for the head, the tail and the singleton
 * (a singleton's next == prev == self, so the relinks degenerate and the head sentinel
 * falls out of the self-check). Mirrors chainDelink's sentinel discipline. Field
 * offsets are hoisted into locals BEFORE any store: with -relaxed-aliasing the compiler
 * must otherwise re-read every header field it stores through. */
static void segmentRingDelink(Segment* segment, ArenaHead* arena) {
	uint8_t* base = segment->base; /* hoisted: relaxed aliasing forces a reload per access */
	uint64_t self = (uint64_t)((uint8_t*)arena - base);
	uint64_t nextOffset = arena->next;
	uint64_t prevOffset = arena->prev;
	((ArenaHead*)(base + nextOffset))->prev = prevOffset;
	((ArenaHead*)(base + prevOffset))->next = nextOffset;
	segment->resident = (nextOffset == self) ? (uint64_t)SLAB_OFFSET_NONE : nextOffset;
	segment->residentCount--;
	arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
	arena->prev = (uint64_t)SLAB_OFFSET_UNLINKED;
}

static ArenaHead* segmentRingPopHead(Segment* segment) {
	if (segment->residentCount == 0) return NULL;
	ArenaHead* arena = (ArenaHead*)(segment->base + segment->resident); /* head = hottest */
	segmentRingDelink(segment, arena);
	return arena;
}

static ArenaHead* segmentRingPopTail(Segment* segment) {
	if (segment->residentCount == 0) return NULL;
	ArenaHead* arena = (ArenaHead*)(segment->base + ((ArenaHead*)(segment->base + segment->resident))->prev); /* tail = coldest */
	segmentRingDelink(segment, arena);
	return arena;
}

static void segmentPushRevive(Segment* segment, ArenaHead* arena) {
	arena->next = segment->revive;
	arena->prev = (uint64_t)SLAB_OFFSET_UNLINKED; /* revive is singly-linked: prev unused */
	segment->revive = (uint64_t)((uint8_t*)arena - segment->base);
}

static ArenaHead* segmentPopRevive(Segment* segment) {
	if (segment->revive == (uint64_t)SLAB_OFFSET_NONE) return NULL;
	ArenaHead* arena = (ArenaHead*)(segment->base + segment->revive);
	segment->revive = arena->next;
	arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
	return arena;
}

/* ---- Small layer ---- */

static uint32_t slotClassIndexOf(size_t size) {
	/* classes tile [8, 256] in powers of two: shrink to 8B units ((size - 1) >> 3) and
	 * the unit count's bit length IS the class index — branchless (lzcnt on the Release
	 * target); sizes below 8 yield unit count 0 -> bit length 0 -> the 8B class, so no
	 * clamp is needed. Callers guarantee size >= 1 (alloc maps 0 to SLOT_SIZE_MIN). */
	/* debug guard: the caller contract — alloc remaps 0 and rejects oversize */
	assert(size >= 1 && size <= SLOT_SIZE_MAX);
	uint32_t classIndex = (uint32_t)(64 - bits_clz64((size - 1) >> 3));
	/* debug guard: the tiling must stay exact */
	assert(size <= (((size_t)1) << (classIndex + SLOT_CLASS_SHIFT_MIN)));
	assert(classIndex == 0 || size > (((size_t)1) << (classIndex + SLOT_CLASS_SHIFT_MIN - 1)));
	return classIndex;
}

static void* arenaSlotClaim(ArenaHead* arena, uint32_t shift) {
	uint32_t slotIndex = arena->freeHead;
	if (slotIndex != (uint32_t)SLAB_SLOT_NONE) {
		arena->freeHead = *(uint32_t*)((uint8_t*)arena + ((size_t)slotIndex << shift));
	}
	else {
		/* freelist empty: the free range is the untouched bump tail — the invariant
		 * freeSlotCount == totalSlots - bumpTop holds whenever the list is empty
		 * (bump claims move both sides, list claims/frees never touch bumpTop) */
		uint32_t totalSlots = ARENA_SIZE_SMALL >> shift;
		slotIndex = totalSlots - arena->freeSlotCount;
		if (slotIndex >= totalSlots) return NULL; /* count == 0: arena full */
	}
	arena->freeSlotCount--;
	return (uint8_t*)arena + ((size_t)slotIndex << shift);
}

static void* smallLayerAllocSlow(ArenaSlabAllocator* context, uint32_t classIndex);

static void* smallLayerAlloc(ArenaSlabAllocator* context, uint32_t classIndex) {
	Segment* segment = &context->segment;
	uint64_t offset = segment->partial[classIndex];
	if (offset != (uint64_t)SLAB_OFFSET_NONE) {
		ArenaHead* arena = (ArenaHead*)(segment->base + offset);
		if (arena->magic == magicArenaSmall && arena->freeSlotCount > 0) {
			void* slot = arenaSlotClaim(arena, classIndex + SLOT_CLASS_SHIFT_MIN);
			if (slot != NULL && arena->freeSlotCount == 0) {
				arenaPopHead(segment, classIndex, arena);
			}
			return slot;
		}
	}
	return smallLayerAllocSlow(context, classIndex);
}

/* Slow path: the chain head is a live partial arena by invariant (fulls unlink on their
 * last claim, empties park on their last free) — a broken head is rejected, not healed,
 * and a full head degrades to claim's NULL. An exhausted chain revives a shell from the
 * shared pool (any class) or carves. */
static void* smallLayerAllocSlow(ArenaSlabAllocator* context, uint32_t classIndex) {
	uint32_t shift = classIndex + SLOT_CLASS_SHIFT_MIN;
	Segment* segment = &context->segment;

	uint64_t offset = segment->partial[classIndex];
	if (offset != (uint64_t)SLAB_OFFSET_NONE) {
		ArenaHead* arena = (ArenaHead*)(segment->base + offset);
		if (arena->magic != magicArenaSmall || arena->classSize != (((uint32_t)1) << shift)) {
			return NULL;
		}

		void* slot = arenaSlotClaim(arena, shift);
		if (slot == NULL) return NULL;
		if (arena->freeSlotCount == 0) {
			arenaPopHead(segment, classIndex, arena);
		}
		return slot;
	}

	/* class chain empty: revive a shell from the shared pool — the resident ring head
	 * first (a plain re-init, no syscall), then the revive chain (osPagesReuse); fall
	 * back to carving fresh VA */
	ArenaHead* arena = segmentRingPopHead(segment);
	if (arena == NULL) arena = segmentPopRevive(segment);
	if (arena != NULL) {
		if (arena->magic == magicArenaSmallDropped) { /* pages were dropped: re-commit them */
			uint32_t pageSize = osPageSize(); /* one call: kept length and reuse base share it */
			uint32_t keptBytes = arenaKeptBytes(pageSize);
			size_t reuseLength = ARENA_SIZE_SMALL - keptBytes;
			if (!osPagesReuse((uint8_t*)arena + keptBytes, reuseLength)) {
				segmentPushRevive(segment, arena); /* put the shell back; state preserved */
				return NULL;
			}
#ifdef LOG_MALLOC_STATS
			slabLayerStats* stats = segmentStats(segment);
			if (stats != NULL) { stats->reuseCalls++; stats->reuseBytes += reuseLength; }
#endif
		}
		/* always re-init: it is O(1) now, and it sanitizes whatever freelist state a
		 * parked or dropped shell carries (use-after-free writes die here) */
		arenaInit(arena, ARENA_SIZE_SMALL, classIndex + SLOT_CLASS_SHIFT_MIN, magicArenaSmall);
	}
	else {
		arena = segmentCarveArena(segment, ARENA_SIZE_SMALL, classIndex + SLOT_CLASS_SHIFT_MIN, magicArenaSmall);
		if (arena == NULL) return NULL;
	}
	segmentPushArena(segment, classIndex, arena);
	return arenaSlotClaim(arena, classIndex + SLOT_CLASS_SHIFT_MIN);
}

static bool smallLayerFree(Segment* segment, ArenaHead* arena, void* pointer) {
	uintptr_t address = (uintptr_t)pointer;

	/* slot classes are powers of two in [8, 256]: derive the class from classSize with ctz
	 * (no table walk); the headerSlots cross-check guards the bounds math below */
	uint32_t classSize = arena->classSize;
	if (classSize < SLOT_SIZE_MIN || classSize > SLOT_SIZE_MAX) return false;
	uint32_t shift = (uint32_t)bits_ctz64(classSize);
	if ((((uint32_t)1) << shift) != classSize) return false;
	uint32_t classIndex = shift - SLOT_CLASS_SHIFT_MIN;
	uint32_t headerSlots = (uint32_t)(sizeof(ArenaHead) + classSize - 1) >> shift;
	if (arena->headerSlots != headerSlots) return false;

	uint32_t inArenaOffset = (uint32_t)(address - (uintptr_t)arena);
	if (inArenaOffset & (((uint32_t)1 << shift) - 1)) return false;
	uint32_t slotIndex = inArenaOffset >> shift;
	uint32_t capacity = ((uint32_t)(ARENA_SIZE_SMALL >> shift)) - headerSlots;
	if ((uint32_t)(slotIndex - headerSlots) >= capacity) return false; /* header slots + tail in one compare */

	/* double-free is NOT detected by design: freed slot memory doubles as freelist state */
	bool wasFull = arena->freeSlotCount == 0;
	*(uint32_t*)pointer = arena->freeHead; /* push onto the arena's LIFO free list */
	arena->freeHead = slotIndex;
	arena->freeSlotCount++;
	if (arena->freeSlotCount == capacity) {
		/* the arena just became empty (capacity >= 63, so this excludes wasFull): the
		 * first CLASS_SPARE_KEEP empties of a class stay LINKED as warm spares — the
		 * pair pattern re-claims them straight off the chain head with zero relink
		 * churn. Beyond the quota the arena parks into the shared resident ring (pages
		 * stay committed, revival is a plain re-init). Corruption guard wraps every
		 * relinking path: never touch a shell we cannot prove is chained. */
		if (arena->next != (uint64_t)SLAB_OFFSET_UNLINKED && arena->prev != (uint64_t)SLAB_OFFSET_UNLINKED) {
			uint64_t self = (uint64_t)((uint8_t*)arena - segment->base);
			uint32_t hit = CLASS_SPARE_KEEP;
			for (uint32_t spareIndex = 0; spareIndex < segment->spareCount[classIndex]; spareIndex++) {
				if (segment->spare[classIndex][spareIndex] == self) { hit = spareIndex; break; }
			}
			if (hit != CLASS_SPARE_KEEP) {
				/* already a recorded spare: stays linked, nothing to do */
			}
			else if (segment->spareCount[classIndex] < CLASS_SPARE_KEEP) {
				segment->spare[classIndex][segment->spareCount[classIndex]++] = self; /* stays linked */
			}
			else {
				chainDelink(segment, classIndex, arena);
				segmentRingPush(segment, arena);
			}
		}
		/* guard failed: leave the shell as-is (degraded, same as before) */
	}
	else if (wasFull && arena->next == (uint64_t)SLAB_OFFSET_UNLINKED) {
		/* full arenas are off-chain: back to the head, so the next claim finds it */
		segmentPushArena(segment, classIndex, arena);
	}
	return true;
}

/* ---- Public API ---- */

bool arenaSlab_init(ArenaSlabAllocator* context, uint8_t segmentSizeExponent) {
	if (context == NULL) return false;
	if (context->cookie == arenaSlabCookie) return true;
	/* no blanket memset (cache pollution): every field the allocator reads is written
	 * below, so garbage / foreign / never-zeroed memory is rebuilt in place either way.
	 * Failure paths only clear the cookie — the gate then rejects everything else. */
	/* zero is a VALID chain offset (the first arena sits at segment base + 0), so the
	 * empty-chain sentinels must be written explicitly — zeroed memory would alias them
	 * with "arena at offset 0" and the alloc paths would deref uncommitted memory */
	for (uint32_t chainIndex = 0; chainIndex < SLOT_CLASS_COUNT; chainIndex++) {
		context->segment.partial[chainIndex] = (uint64_t)SLAB_OFFSET_NONE;
		context->segment.spareCount[chainIndex] = 0; /* spare[] is only read up to count */
	}
	context->segment.resident = (uint64_t)SLAB_OFFSET_NONE;
	context->segment.residentCount = 0;
	context->segment.revive = (uint64_t)SLAB_OFFSET_NONE;
	if (segmentSizeExponent < SEGMENT_SIZE_EXPONENT_MIN || segmentSizeExponent > SEGMENT_SIZE_EXPONENT_MAX) {
		context->cookie = 0; /* failed attempts must not half-initialize: the gate stays shut */
		return false;
	}

	uint64_t segmentBytes = ((uint64_t)1 << segmentSizeExponent);
	uint8_t* base = NULL;
	uint8_t* reserveBase = NULL;
	size_t reserveSize = 0;
	if (!osSegmentReserve(segmentBytes, &base, &reserveBase, &reserveSize)) {
		context->cookie = 0; /* nothing was reserved, nothing leaks; the gate stays shut */
		return false;
	}

	Segment* segment = &context->segment;
	segment->base = base;
	segment->frontier = base;
	segment->committedEnd = base;
	segment->bytes = segmentBytes;
	segment->reserveBase = reserveBase;
	segment->reserveSize = reserveSize;
#ifdef LOG_MALLOC_STATS
	segment->ownerStats = &context->stats;
	memset(&context->stats, 0, sizeof(context->stats)); /* debug-only: counters start clean */
#endif
	context->cookie = arenaSlabCookie;
	return true;
}

void arenaSlab_shutdown(ArenaSlabAllocator* context) {
	if (context == NULL || context->cookie != arenaSlabCookie) return; /* never-inited / foreign: owns no reservation, nothing to release */
	osSegmentRelease((uint8_t*)context->segment.reserveBase, context->segment.reserveSize);
	/* gate shut: repeated shutdown is a no-op, re-init rebuilds in place — one cache-line
	 * touch instead of scrubbing the whole context (stale pointers stay unreachable) */
	context->cookie = 0;
}

/* Range + magic resolution shared by free / realloc / usable_size: cookie gate, segment
 * range check, arena-head mask and the live-magic validation in one pass. Returns NULL
 * for foreign / uninitialized / non-live pointers (the caller decides the verdict).
 * which() deliberately does NOT use this: it is a pure range check by contract. */
static ArenaHead* segmentResolveArena(ArenaSlabAllocator* context, void* pointer) {
	if (context == NULL || context->cookie != arenaSlabCookie || pointer == NULL) return NULL;
	if (segmentFind(context, (uintptr_t)pointer) == NULL) return NULL;
	ArenaHead* arena = (ArenaHead*)((uintptr_t)pointer & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
	if (arena->magic != magicArenaSmall) return NULL;
	return arena;
}

void* arenaSlab_alloc(ArenaSlabAllocator* context, size_t size) {
	if (context == NULL || context->cookie != arenaSlabCookie) return NULL;
	if (size == 0) size = SLOT_SIZE_MIN;
	if (size > SLOT_SIZE_MAX) return NULL;
	void* pointer = smallLayerAlloc(context, slotClassIndexOf(size));
#ifdef LOG_MALLOC_STATS
	if (pointer != NULL) {
		ArenaHead* arena = (ArenaHead*)((uintptr_t)pointer & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
		context->stats.allocCount++;
		context->stats.allocBytes += arena->classSize;
	}
	else {
		context->stats.allocFailed++;
	}
#endif
	return pointer;
}

bool arenaSlab_free(ArenaSlabAllocator* context, void* pointer) {
	if (context == NULL || context->cookie != arenaSlabCookie) return false;
	if (pointer == NULL) return true;
	/* the resolve is pure arithmetic: a bogus pointer must never be dereferenced beyond
	 * its (range-proven, live-magic) arena header */
	ArenaHead* arena = segmentResolveArena(context, pointer);
	if (arena == NULL) return false;
#ifdef LOG_MALLOC_STATS
	size_t freedBytes = arena->classSize;
#endif
	bool ok = smallLayerFree(&context->segment, arena, pointer);
#ifdef LOG_MALLOC_STATS
	if (ok) { context->stats.freeCount++; context->stats.freeBytes += freedBytes; }
	else { context->stats.freeRejected++; }
#endif
	return ok;
}

void* arenaSlab_realloc(ArenaSlabAllocator* context, void* pointer, size_t newSize) {
	if (context == NULL || context->cookie != arenaSlabCookie) return NULL;
	if (pointer == NULL) return arenaSlab_alloc(context, newSize);
	if (newSize == 0) {
		arenaSlab_free(context, pointer);
		return NULL;
	}
	/* Realloc is only supported for small objects; otherwise caller's problem. */
	if (newSize > SLOT_SIZE_MAX) { arenaSlab_free(context, pointer); return NULL; }
	/* single validation pass (calling which + usable_size would walk the segment twice) */
	ArenaHead* arena = segmentResolveArena(context, pointer);
	if (arena == NULL) return NULL;
	size_t oldUsable = arena->classSize;
	if (oldUsable >= newSize) return pointer;
	void* newPointer = arenaSlab_alloc(context, newSize);
	if (newPointer == NULL) return NULL;
	memcpy(newPointer, pointer, oldUsable); /* oldUsable < newSize is established above */
	arenaSlab_free(context, pointer);
	return newPointer;
}

/* The only API that touches payload bytes: a plain alloc plus a zero fill. A zero total
 * follows alloc's size-0 rule (an 8B slot with nothing to zero). */
void* arenaSlab_calloc(ArenaSlabAllocator* context, size_t count, size_t size) {
	if (size != 0 && count > (size_t)-1 / size) return NULL; /* count * size would overflow */
	size_t total = count * size;
	void* pointer = arenaSlab_alloc(context, total);
	if (pointer != NULL) memset(pointer, 0, total);
	return pointer;
}

/* Size gate for alloc / realloc: everything up to SLOT_SIZE_MAX is permissible — alloc
 * treats 0 as SLOT_SIZE_MIN and realloc treats 0 as free; larger sizes are the caller's
 * business (alloc returns NULL, realloc frees and returns NULL). */
bool arenaSlab_permissible_size(size_t size) {
	return size <= SLOT_SIZE_MAX;
}

size_t arenaSlab_usable_size(ArenaSlabAllocator* context, void* pointer) {
	ArenaHead* arena = segmentResolveArena(context, pointer);
	return (arena != NULL) ? arena->classSize : 0;
}

slabLayer arenaSlab_which(ArenaSlabAllocator* context, void* pointer) {
	if (context == NULL || context->cookie != arenaSlabCookie || pointer == NULL) return SLAB_LAYER_NONE;
	if (segmentFind(context, (uintptr_t)pointer) != NULL) return SLAB_LAYER_SMALL;
	return SLAB_LAYER_NONE;
}

/* Segment base as a plain integer: embedders compress heap references against this numeric
 * base (store offset, decode base + offset) without poking into the struct internals.
 * It is deliberately not a pointer — the API never hands out the segment as an object. */
uintptr_t arenaSlab_segmentBase(ArenaSlabAllocator* context) {
	if (context == NULL || context->cookie != arenaSlabCookie) return 0;
	return (uintptr_t)context->segment.base;
}

/* ---- Lazy return (small only) ----
 * Two-phase by design: free parks emptied arenas into the shared resident ring; trim is
 * the ONLY place that returns pages to the OS — it drops the coldest ring tail while the
 * ring exceeds TRIM_POOL_MAX_RESIDENT, re-homing the shells to the revive chain. The
 * water-mark shells stay warm for free revival between trims. */
static size_t trimResidentRing(ArenaSlabAllocator* context) {
	size_t droppedBytes = 0;
	uint32_t pageSize = osPageSize();
	Segment* segment = &context->segment;
	while (segment->residentCount > TRIM_POOL_MAX_RESIDENT) {
		ArenaHead* arena = segmentRingPopTail(segment);
		if (pageSize < ARENA_SIZE_SMALL) {
			size_t dropLength = ARENA_SIZE_SMALL - pageSize;
			if (osPagesDrop((uint8_t*)arena + pageSize, dropLength)) {
				droppedBytes += dropLength;
				arena->magic = magicArenaSmallDropped; /* revival must pay osPagesReuse */
#ifdef LOG_MALLOC_STATS
				slabLayerStats* stats = segmentStats(segment);
				if (stats != NULL) { stats->dropCalls++; stats->dropBytes += dropLength; }
#endif
			}
			/* drop failed: pages are still resident, but the ring is over the water mark —
			 * the shell must NOT re-enter the ring (infinite loop); it waits on the revive
			 * chain with its magic intact, so revival skips the reuse and stays free */
		}
		segmentPushRevive(segment, arena);
	}
	return droppedBytes;
}

size_t arenaSlab_trim(ArenaSlabAllocator* context) {
	if (context == NULL || context->cookie != arenaSlabCookie) return 0;
	return trimResidentRing(context);
}

/* ---- Statistics ---- */
void arenaSlab_statsReset(ArenaSlabAllocator* context) {
	if (context == NULL || context->cookie != arenaSlabCookie) return;
#ifdef LOG_MALLOC_STATS
	memset(&context->stats, 0, sizeof(context->stats));
#endif
}

#ifdef LOG_MALLOC_STATS
static void dumpLayerEvents(const slabLayerStats* stats) {
	printf("  Events: alloc %llu / %llu B, free %llu / %llu B, failed %llu, rejected %llu\n",
		(unsigned long long)stats->allocCount, (unsigned long long)stats->allocBytes,
		(unsigned long long)stats->freeCount, (unsigned long long)stats->freeBytes,
		(unsigned long long)stats->allocFailed, (unsigned long long)stats->freeRejected);
	printf("  Growth: carve %llu, commit %llu / %llu B | Return: drop %llu / %llu B, reuse %llu / %llu B\n",
		(unsigned long long)stats->carveCount,
		(unsigned long long)stats->commitCalls, (unsigned long long)stats->commitBytes,
		(unsigned long long)stats->dropCalls, (unsigned long long)stats->dropBytes,
		(unsigned long long)stats->reuseCalls, (unsigned long long)stats->reuseBytes);
}

static void dumpSegmentWatermarks(const Segment* segment) {
	printf("  Segment watermark (carve/commit/capacity): %llukB/%llukB/%llukB\n",
		(unsigned long long)((segment->frontier - segment->base) / 1024),
		(unsigned long long)((segment->committedEnd - segment->base) / 1024),
		(unsigned long long)(segment->bytes / 1024));
}
#endif /* LOG_MALLOC_STATS */

void arenaSlab_dumpStats(ArenaSlabAllocator* context) {
#ifndef LOG_MALLOC_STATS
	(void)context;
#else
	if (context == NULL || context->cookie != arenaSlabCookie) {
		printf("slab stats: not initialized\n");
		return;
	}

	printf("== slab stats (page size %u B) ==\n", osPageSize());
	printf("[small]\n");
	dumpLayerEvents(&context->stats);
	{
		uint64_t totalArenas = 0, chainArenas = 0, reviveCount = 0;
		uint64_t liveSlots = 0, capacitySlots = 0;
		Segment* segment = &context->segment;
		/* every 16KB block inside [base, frontier) is an initialized arena (carve advances
		 * in whole-arena units and inits immediately; dropped shells keep their header page
		 * committed) — this walk is the only one that sees FULL arenas, which are off-chain */
		for (uint64_t offset = 0; offset < (uint64_t)(segment->frontier - segment->base); offset += ARENA_SIZE_SMALL) {
			ArenaHead* arena = (ArenaHead*)(segment->base + offset);
			uint32_t capacity = arenaCapacity(arena);
			assert(arena->magic == magicArenaSmall || arena->magic == magicArenaSmallDropped);
			totalArenas++;
			capacitySlots += capacity;
			liveSlots += capacity - arena->freeSlotCount;
		}
		for (uint32_t classIndex = 0; classIndex < SLOT_CLASS_COUNT; classIndex++) {
			uint32_t emptyInClass = 0;
			uint64_t arenaOffset = segment->partial[classIndex];
			while (arenaOffset != (uint64_t)SLAB_OFFSET_NONE) {
				ArenaHead* arena = (ArenaHead*)(segment->base + arenaOffset);
				chainArenas++;
				/* invariant: chains hold partial arenas plus at most CLASS_SPARE_KEEP warm
				 * empties (the recorded spares) — more empties mean broken bookkeeping */
				if (arena->freeSlotCount == arenaCapacity(arena)) emptyInClass++;
				arenaOffset = arena->next;
			}
			assert(emptyInClass <= CLASS_SPARE_KEEP);
		}
		uint64_t residentShells = segment->residentCount; /* O(1): maintained by push/pop */
		uint64_t reviveOffset = segment->revive;
		while (reviveOffset != (uint64_t)SLAB_OFFSET_NONE) {
			ArenaHead* arena = (ArenaHead*)(segment->base + reviveOffset);
			reviveCount++;
			reviveOffset = arena->next;
		}
		/* everything carved that is neither chained, parked in the ring nor waiting in the
		 * revive chain must be a full arena (off-chain by design) */
		uint64_t fullArenas = totalArenas - chainArenas - residentShells - reviveCount;
		printf("  State: arenas %llu (chain %llu partial+spare | full %llu | resident %llu / revive %llu), slots live %llu / %llu\n",
			(unsigned long long)totalArenas, (unsigned long long)chainArenas,
			(unsigned long long)fullArenas,
			(unsigned long long)residentShells,
			(unsigned long long)reviveCount,
			(unsigned long long)liveSlots, (unsigned long long)capacitySlots);
		dumpSegmentWatermarks(segment);
	}
#endif
}

/* ---- Default instance ---- */
ArenaSlabAllocator arenaSlabDefault;
