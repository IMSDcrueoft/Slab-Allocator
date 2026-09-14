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

 /* ---- Per-class arena layouts (documentation + static asserts; computed by arenaInit at runtime)---- */

 // 16KB / 256B = 64 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[1];         //  8B -> 32B
	uint32_t  recentFreeSlot;    // hint: most recently freed slot (claim scan start)
	uint8_t   alignment[220];    // -> 256B (1 header slot)
	uint8_t   payload[256 * (64 - 1)]; // 63 slots
} Arena16K_256B;

// 16KB / 128B = 128 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[2];         // 16B -> 40B
	uint32_t  recentFreeSlot;    // hint: most recently freed slot (claim scan start)
	uint8_t   alignment[84];     // -> 128B (1 header slot)
	uint8_t   payload[128 * (128 - 1)]; // 127 slots
} Arena16K_128B;

// 16KB / 64B = 256 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[4];         // 32B -> 56B
	uint32_t  recentFreeSlot;    // hint: most recently freed slot (claim scan start)
	uint8_t   alignment[4];      // -> 64B (1 header slot)
	uint8_t   payload[64 * (256 - 1)]; // 255 slots
} Arena16K_64B;

// 16KB / 32B = 512 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[8];         // 64B -> 88B
	uint32_t  recentFreeSlot;    // hint: most recently freed slot (claim scan start)
	uint8_t   alignment[4];      // -> 96B (3 header slots)
	uint8_t   payload[32 * (512 - 3)]; // 509 slots
} Arena16K_32B;

// 16KB / 16B = 1024 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[16];        // 128B -> 152B
	uint32_t  recentFreeSlot;    // hint: most recently freed slot (claim scan start)
	uint8_t   alignment[4];      // -> 160B (10 header slots)
	uint8_t   payload[16 * (1024 - 10)]; // 1014 slots
} Arena16K_16B;

slabStaticAssert(sizeof(ArenaHead) == 24);
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

static uint64_t* arenaBitmap(ArenaHead* arena) {
	return (uint64_t*)((uint8_t*)arena + sizeof(ArenaHead));
}

/* Recent-free hint: lives in the header region's tail — the rounding slack every class
 * has (>= 8B), inside the always-kept header page, so it survives trim drops. */
static uint32_t* arenaRecentFreeSlot(ArenaHead* arena) {
	return (uint32_t*)((uint8_t*)arena + sizeof(ArenaHead) + (size_t)arena->bitMapCount * sizeof(uint64_t));
}

static uint32_t arenaCapacity(const ArenaHead* arena) {
	return (uint32_t)arena->bitMapCount * 64 - arena->headerSlots;
}

static uint32_t arenaKeptBytes(uint32_t pageSize) {
	return pageSize;
}

/* ---- Bitmap operations ----
 * Convention: bit=1 free, bit=0 used; allocation scans with ctz starting at the
 * recent-free hint word and wraps around the bitmap.
 */

 /* Claim the lowest free bit of one word; -1 if the word is full */
static int64_t bitmapClaimInWord(uint64_t* bitmap, uint32_t wordIndex) {
	uint64_t word = bitmap[wordIndex];
	if (word == 0) return -1;
	uint32_t bitIndex = (uint32_t)bits_ctz64(word);
	bitmap[wordIndex] = word & ~(((uint64_t)1) << bitIndex);
	return (int64_t)(wordIndex * 64 + bitIndex);
}

/* Claim any free bit, return its slot index; -1 if none.
 * Scans from startWord (the recent-free hint) and wraps around. */
static int64_t bitmapClaimSlot(uint64_t* bitmap, uint32_t wordCount, uint32_t startWord) {
	assert(startWord < wordCount); /* internal invariant: the hint is always a valid slot index */
	for (uint32_t wordIndex = startWord; wordIndex < wordCount; wordIndex++) {
		int64_t slotIndex = bitmapClaimInWord(bitmap, wordIndex);
		if (slotIndex >= 0) return slotIndex;
	}
	for (uint32_t wordIndex = 0; wordIndex < startWord; wordIndex++) {
		int64_t slotIndex = bitmapClaimInWord(bitmap, wordIndex);
		if (slotIndex >= 0) return slotIndex;
	}
	return -1;
}

/* Set [startBit, startBit+runLength) to 1 (free) */
static int64_t bitmapFindRun(const uint64_t* bitmap, uint32_t wordCount, uint32_t runLength) {
	uint32_t pendingRun = 0;
	int64_t  pendingStart = 0;
	for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
		uint64_t remaining = bitmap[wordIndex];
		uint32_t consumedBits = 0;
		if (remaining == 0) { pendingRun = 0; continue; }
		while (remaining != 0) {
			uint32_t firstBit = (uint32_t)bits_ctz64(remaining);
			if (firstBit > 0) pendingRun = 0;
			remaining >>= firstBit;
			uint32_t ones = (uint32_t)bits_ctz64(~remaining);
			uint32_t runStartInWord = consumedBits + firstBit;
			if (pendingRun == 0) pendingStart = (int64_t)(wordIndex * 64 + runStartInWord);
			pendingRun += ones;
			if (pendingRun >= runLength) return pendingStart;
			if (runStartInWord + ones == 64) {
				remaining = 0;
			}
			else {
				pendingRun = 0;
				remaining >>= ones;
				consumedBits = runStartInWord + ones;
			}
		}
	}
	return -1;
}

/* ---- Arena initialization ---- */
static void arenaInit(ArenaHead* arena, uint32_t arenaBytes, uint32_t shift, uint32_t magic) {
	assert(shift >= SLOT_CLASS_SHIFT_MIN && shift < SLOT_CLASS_SHIFT_MIN + SLOT_CLASS_COUNT);
	uint32_t classSize = (uint32_t)1 << shift; /* slot classes are powers of two (16..256) */
	uint32_t totalSlots = arenaBytes >> shift;
	uint32_t bitmapWords = totalSlots >> 6;// 64 slots per u64 word
	uint32_t headerSlots = (uint32_t)(sizeof(ArenaHead) + bitmapWords * sizeof(uint64_t) + classSize - 1) >> shift;

	arena->classSize = (uint16_t)classSize;
	arena->headerSlots = (uint16_t)headerSlots;
	arena->freeSlotCount = (uint16_t)(totalSlots - headerSlots);
	arena->bitMapCount = (uint16_t)bitmapWords;
	arena->magic = magic;

	uint64_t* bitmap = arenaBitmap(arena);
	bitmap[0] = ~(((((uint64_t)1) << headerSlots) - 1));
	for (uint32_t wordIndex = 1; wordIndex < bitmapWords; wordIndex++) {
		bitmap[wordIndex] = ~(uint64_t)0;
	}

	*arenaRecentFreeSlot(arena) = 0; /* no free yet: start scanning from slot 0 */
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
	arena->next = segment->partial[chainIndex];
	segment->partial[chainIndex] = (uint64_t)((uint8_t*)arena - segment->base);
}

/* Unlink the chain head: called when its last free slot was just claimed (off chain <=> full) */
static void arenaPopHead(Segment* segment, uint32_t chainIndex, ArenaHead* arena) {
	segment->partial[chainIndex] = arena->next;
	arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
}

/* ---- Small layer ---- */

static uint32_t slotClassIndexOf(size_t size) {
	uint32_t classIndex = SLOT_CLASS_COUNT - 1;
	if (size <= 16) classIndex = 0;
	else if (size <= 32) classIndex = 1;
	else if (size <= 64) classIndex = 2;
	else if (size <= 128) classIndex = 3;
	/* debug guard: the comparison chain must tile the consecutive 2^N classes exactly */
	assert(size <= (((size_t)1) << (classIndex + SLOT_CLASS_SHIFT_MIN)));
	assert(classIndex == 0 || size > (((size_t)1) << (classIndex + SLOT_CLASS_SHIFT_MIN - 1)));
	return classIndex;
}

static void* arenaSlotClaim(ArenaHead* arena, uint32_t shift) {
	uint64_t* bitmap = arenaBitmap(arena);
	uint32_t startWord = *arenaRecentFreeSlot(arena) >> 6;
	int64_t slotIndex = bitmapClaimSlot(bitmap, arena->bitMapCount, startWord);
	if (slotIndex < 0) return NULL;
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

/* Slow path: the chain head is dropped (revive its pages) or the chain is empty (carve).
 * No walk: every chain member is allocatable by invariant, so the head always serves. */
static void* smallLayerAllocSlow(ArenaSlabAllocator* context, uint32_t classIndex) {
	uint16_t classSize = (uint16_t)(((uint32_t)1) << (classIndex + SLOT_CLASS_SHIFT_MIN)); /* classes are powers of two */
	Segment* segment = &context->segment;

	while (true) {
		uint64_t offset = segment->partial[classIndex];
		if (offset == (uint64_t)SLAB_OFFSET_NONE) break;
		ArenaHead* arena = (ArenaHead*)(segment->base + offset);

		if (arena->magic == magicArenaSmallDropped) {
			if (arena->classSize != classSize) return NULL;
			uint32_t pageSize = osPageSize(); /* one call: kept length and reuse base share it */
			size_t reuseLength = ARENA_SIZE_SMALL - arenaKeptBytes(pageSize);
			if (!osPagesReuse((uint8_t*)arena + arenaKeptBytes(pageSize), reuseLength)) return NULL;
			arena->magic = magicArenaSmall;
#ifdef LOG_MALLOC_STATS
			slabLayerStats* stats = segmentStats(segment);
			if (stats != NULL) { stats->reuseCalls++; stats->reuseBytes += reuseLength; }
#endif
		}
		else if (arena->magic != magicArenaSmall || arena->classSize != classSize) {
			return NULL;
		}

		if (arena->freeSlotCount == 0) {
			/* full arena on the chain: invariant break, self-heal by unlinking the head */
			arenaPopHead(segment, classIndex, arena);
			continue;
		}

		void* slot = arenaSlotClaim(arena, classIndex + SLOT_CLASS_SHIFT_MIN);
		if (slot == NULL) return NULL;
		if (arena->freeSlotCount == 0) {
			arenaPopHead(segment, classIndex, arena);
		}
		return slot;
	}

	ArenaHead* arena = segmentCarveArena(segment, ARENA_SIZE_SMALL, classIndex + SLOT_CLASS_SHIFT_MIN, magicArenaSmall);
	if (arena == NULL) return NULL;
	segmentPushArena(segment, classIndex, arena);
	return arenaSlotClaim(arena, classIndex + SLOT_CLASS_SHIFT_MIN);
}

static bool smallLayerFree(Segment* segment, ArenaHead* arena, void* pointer) {
	uintptr_t address = (uintptr_t)pointer;

	/* slot classes are powers of two in [16, 256]: derive the class from classSize with ctz
	 * (no table walk) and validate the header fields with arenaInit's own formulas */
	uint32_t classSize = arena->classSize;
	if (classSize < SLOT_SIZE_MIN || classSize > SLOT_SIZE_MAX) return false;
	uint32_t shift = (uint32_t)bits_ctz64(classSize);
	if ((((uint32_t)1) << shift) != classSize) return false;
	uint32_t classIndex = shift - SLOT_CLASS_SHIFT_MIN;
	uint32_t bitMapCount = ARENA_SIZE_SMALL >> (shift + 6);
	uint32_t headerSlots = (uint32_t)(sizeof(ArenaHead) + bitMapCount * sizeof(uint64_t) + classSize - 1) >> shift;
	if (arena->bitMapCount != bitMapCount) return false;
	if (arena->headerSlots != headerSlots) return false;

	uint32_t inArenaOffset = (uint32_t)(address - (uintptr_t)arena);
	if (inArenaOffset & (((uint32_t)1 << shift) - 1)) return false;
	uint32_t slotIndex = inArenaOffset >> shift;
	if (slotIndex < arena->headerSlots) return false;
	if (slotIndex >= (uint32_t)arena->bitMapCount * 64) return false;

	uint64_t* bitmap = arenaBitmap(arena);
	uint64_t mask = ((uint64_t)1) << (slotIndex & 63);
	if (bitmap[slotIndex >> 6] & mask) return false;

	bool wasFull = arena->freeSlotCount == 0;
	bitmap[slotIndex >> 6] |= mask;
	arena->freeSlotCount++;
	*arenaRecentFreeSlot(arena) = slotIndex; /* recent-free hint: next claim starts here */
	if (wasFull && arena->next == (uint64_t)SLAB_OFFSET_UNLINKED) {
		/* full arenas are off-chain: back to the head, so the next claim finds it */
		segmentPushArena(segment, classIndex, arena);
	}
	return true;
}

/* ---- Public API ---- */

bool arenaSlab_init(ArenaSlabAllocator* context, uint8_t segmentSizeExponent) {
	if (context == NULL) return false;
	if (context->cookie == arenaSlabCookie) return true;
	/* garbage / foreign memory is never trusted: rebuild the whole context from scratch */
	memset(context, 0, sizeof(*context));
	/* zero is a VALID chain offset (the first arena sits at segment base + 0), so the
	 * empty-chain sentinel must be written explicitly — memset alone would alias it
	 * with "arena at offset 0" and the alloc fast path would deref uncommitted memory */
	for (uint32_t chainIndex = 0; chainIndex < SLOT_CLASS_COUNT; chainIndex++) {
		context->segment.partial[chainIndex] = (uint64_t)SLAB_OFFSET_NONE;
	}
	if (segmentSizeExponent < SEGMENT_SIZE_EXPONENT_MIN || segmentSizeExponent > SEGMENT_SIZE_EXPONENT_MAX) return false;

	uint64_t segmentBytes = ((uint64_t)1 << segmentSizeExponent);
	uint8_t* base = NULL;
	uint8_t* reserveBase = NULL;
	size_t reserveSize = 0;
	if (!osSegmentReserve(segmentBytes, &base, &reserveBase, &reserveSize)) {
		return false; /* all-or-nothing reserve: the context is already all-zero */
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
#endif
	context->cookie = arenaSlabCookie;
	return true;
}

void arenaSlab_shutdown(ArenaSlabAllocator* context) {
	if (context == NULL || context->cookie != arenaSlabCookie) return; /* never-inited / garbage: owns no reservation, nothing to release */
	osSegmentRelease((uint8_t*)context->segment.reserveBase, context->segment.reserveSize);
	memset(context, 0, sizeof(*context)); /* cookie cleared: repeated shutdown is a no-op, re-init is safe */
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
	uintptr_t address = (uintptr_t)pointer;

	/* range check first (pure arithmetic): a bogus pointer must never be dereferenced */
	Segment* segment = segmentFind(context, address);
	if (segment == NULL) return false;
	ArenaHead* arena = (ArenaHead*)(address & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
	if (arena->magic != magicArenaSmall) return false;
#ifdef LOG_MALLOC_STATS
	size_t freedBytes = arena->classSize;
#endif
	bool ok = smallLayerFree(segment, arena, pointer);
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
	uintptr_t address = (uintptr_t)pointer;
	if (segmentFind(context, address) == NULL) return NULL;
	ArenaHead* arena = (ArenaHead*)(address & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
	if (arena->magic != magicArenaSmall) return NULL;
	size_t oldUsable = arena->classSize;
	if (oldUsable >= newSize) return pointer;
	void* newPointer = arenaSlab_alloc(context, newSize);
	if (newPointer == NULL) return NULL;
	memcpy(newPointer, pointer, oldUsable); /* oldUsable < newSize is established above */
	arenaSlab_free(context, pointer);
	return newPointer;
}

size_t arenaSlab_usable_size(ArenaSlabAllocator* context, void* pointer) {
	if (context == NULL || context->cookie != arenaSlabCookie || pointer == NULL) return 0;
	if (arenaSlab_which(context, pointer) != SLAB_LAYER_SMALL) return 0;
	ArenaHead* arena = (ArenaHead*)((uintptr_t)pointer & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
	if (arena->magic != magicArenaSmall) return 0;
	return arena->classSize;
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

/* ---- Lazy return (small only) ---- */
static size_t trimSmallLayer(ArenaSlabAllocator* context) {
	uint32_t pageSize = osPageSize();
	size_t droppedBytes = 0;
	Segment* segment = &context->segment;
	for (uint32_t classIndex = 0; classIndex < SLOT_CLASS_COUNT; classIndex++) {
		uint64_t arenaOffset = segment->partial[classIndex];
		while (arenaOffset != (uint64_t)SLAB_OFFSET_NONE) {
			ArenaHead* arena = (ArenaHead*)(segment->base + arenaOffset);
			if (arena->magic == magicArenaSmallDropped) {
				arenaOffset = arena->next; /* already dropped: skip, later arenas still count */
				continue;
			}
			if (arena->magic != magicArenaSmall) return droppedBytes;
			if (arena->freeSlotCount == arenaCapacity(arena) && pageSize < ARENA_SIZE_SMALL) {
				size_t dropLength = ARENA_SIZE_SMALL - pageSize;
				if (osPagesDrop((uint8_t*)arena + pageSize, dropLength)) {
					droppedBytes += dropLength;
					arena->magic = magicArenaSmallDropped;
#ifdef LOG_MALLOC_STATS
					slabLayerStats* stats = segmentStats(segment);
					if (stats != NULL) { stats->dropCalls++; stats->dropBytes += dropLength; }
#endif
				}
			}
			arenaOffset = arena->next;
		}
	}
	return droppedBytes;
}

size_t arenaSlab_trim(ArenaSlabAllocator* context) {
	if (context == NULL || context->cookie != arenaSlabCookie) return 0;
	return trimSmallLayer(context);
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
		uint64_t arenaTotal = 0, emptyCount = 0, droppedCount = 0;
		uint64_t liveSlots = 0, capacitySlots = 0;
		Segment* segment = &context->segment;
		for (uint32_t classIndex = 0; classIndex < SLOT_CLASS_COUNT; classIndex++) {
			uint64_t arenaOffset = segment->partial[classIndex];
			while (arenaOffset != (uint64_t)SLAB_OFFSET_NONE) {
				ArenaHead* arena = (ArenaHead*)(segment->base + arenaOffset);
				arenaTotal++;
				uint32_t capacity = arenaCapacity(arena);
				capacitySlots += capacity;
				liveSlots += capacity - arena->freeSlotCount;
				if (arena->freeSlotCount == capacity) emptyCount++;
				if (arena->magic == magicArenaSmallDropped) droppedCount++;
				arenaOffset = arena->next;
			}
		}
		/* full arenas are off-chain and thus not walked; dropped is a subset of empty */
		printf("  State: arenas %llu (empty %llu [dropped %llu] / partial %llu), slots live %llu / %llu\n",
			(unsigned long long)arenaTotal, (unsigned long long)emptyCount,
			(unsigned long long)droppedCount,
			(unsigned long long)(arenaTotal - emptyCount),
			(unsigned long long)liveSlots, (unsigned long long)capacitySlots);
		dumpSegmentWatermarks(segment);
	}
#endif
}

/* ---- Test hook (not part of the public API)---- */
int64_t slabDebugFindRun(const uint64_t* bitmap, uint32_t wordCount, uint32_t runLength) {
	return bitmapFindRun(bitmap, wordCount, runLength);
}

/* ---- Default instance ---- */
ArenaSlabAllocator arenaSlabDefault;
