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

 /* ---- Portable static assertions (works on MSVC / GCC / Clang)---- */
#define slabConcat2(a, b) a##b
#define slabConcat(a, b) slabConcat2(a, b)
#define slabStaticAssert(condition) \
    typedef char slabConcat(slabStaticAssert_, __LINE__)[(condition) ? 1 : -1]

/* ---- Per-class arena layouts (documentation + static asserts; computed by arenaInit at runtime)---- */

// 16KB / 256B = 64 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[1];         //  8B -> 32B
	uint8_t   alignment[224];    // -> 256B (1 header slot)
	uint8_t   payload[256 * (64 - 1)]; // 63 slots
} Arena16K_256B;

// 16KB / 128B = 128 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[2];         // 16B -> 40B
	uint8_t   alignment[88];     // -> 128B (1 header slot)
	uint8_t   payload[128 * (128 - 1)]; // 127 slots
} Arena16K_128B;

// 16KB / 64B = 256 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[4];         // 32B -> 56B
	uint8_t   alignment[8];      // -> 64B (1 header slot)
	uint8_t   payload[64 * (256 - 1)]; // 255 slots
} Arena16K_64B;

// 16KB / 32B = 512 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[8];         // 64B -> 88B
	uint8_t   alignment[8];      // -> 96B (3 header slots)
	uint8_t   payload[32 * (512 - 3)]; // 509 slots
} Arena16K_32B;

// 16KB / 16B = 1024 slots
typedef struct {
	ArenaHead head;              // 24B
	uint64_t  bitMap[16];        // 128B -> 152B
	uint8_t   alignment[8];      // -> 160B (10 header slots)
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

static const uint32_t arenaWalkBudget = 4; /* per-call budget for partial-chain walks */

static const uint16_t slotClassTable[SLOT_CLASS_COUNT] = { 16, 32, 64, 128, 256 };
static const uint16_t bitMapWordTable[SLOT_CLASS_COUNT] = { 16, 8, 4, 2, 1 };
static const uint16_t headerSlotTable[SLOT_CLASS_COUNT] = { 10, 3, 1, 1, 1 };

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

static uint32_t arenaCapacity(const ArenaHead* arena) {
	return (uint32_t)arena->bitMapCount * 64 - arena->headerSlots;
}

static uint32_t arenaKeptBytes(uint32_t pageSize) {
	return pageSize;
}

/* ---- Bitmap operations ----
 * Convention: bit=1 free, bit=0 used; allocation scans with ctz, lowest index first
 */

 /* Claim any free bit, return its slot index; -1 if none */
static int64_t bitmapClaimSlot(uint64_t* bitmap, uint32_t wordCount) {
	for (uint32_t wordIndex = 0; wordIndex < wordCount; wordIndex++) {
		uint64_t word = bitmap[wordIndex];
		if (word == 0) continue;
		uint32_t bitIndex = (uint32_t)bits_ctz64(word);
		bitmap[wordIndex] = word & ~(((uint64_t)1) << bitIndex);
		return (int64_t)(wordIndex * 64 + bitIndex);
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
static void arenaInit(ArenaHead* arena, uint32_t arenaBytes, uint16_t classSize, uint32_t magic) {
	uint32_t totalSlots = arenaBytes / classSize;
	uint32_t bitmapWords = totalSlots / 64;
	uint32_t headerSlots = (uint32_t)(sizeof(ArenaHead) + bitmapWords * sizeof(uint64_t) + classSize - 1) / classSize;

	arena->classSize = classSize;
	arena->headerSlots = (uint16_t)headerSlots;
	arena->freeSlotCount = (uint16_t)(totalSlots - headerSlots);
	arena->bitMapCount = (uint16_t)bitmapWords;
	arena->magic = magic;

	uint64_t* bitmap = arenaBitmap(arena);
	bitmap[0] = ~(((((uint64_t)1) << headerSlots) - 1));
	for (uint32_t wordIndex = 1; wordIndex < bitmapWords; wordIndex++) {
		bitmap[wordIndex] = ~(uint64_t)0;
	}
}

/* ---- Segment operations ---- */

/* Single-segment ownership check: returns the segment when address falls inside it */
static Segment* segmentFind(ArenaSlabAllocator* context, uintptr_t address) {
	Segment* segment = &context->segment;
	if (segment->base == NULL) return NULL;
	uintptr_t offset = address - (uintptr_t)segment->base;
	if (offset < segment->bytes) return segment;
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

static ArenaHead* segmentCarveArena(Segment* segment, uint32_t arenaBytes,
	uint16_t classSize, uint32_t magic) {
	if ((uint64_t)(segment->frontier - segment->base) + arenaBytes > segment->bytes) return NULL;
	if (!segmentEnsureCommitted(segment, segment->frontier + arenaBytes)) return NULL;
	ArenaHead* arena = (ArenaHead*)segment->frontier;
	segment->frontier += arenaBytes;
	arenaInit(arena, arenaBytes, classSize, magic);
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

/* ---- Small layer ---- */

static uint32_t slotClassIndexOf(size_t size) {
	if (size <= 16) return 0;
	if (size <= 32) return 1;
	if (size <= 64) return 2;
	if (size <= 128) return 3;
	return 4;
}

static void* arenaSlotClaim(ArenaHead* arena, uint32_t shift) {
	uint64_t* bitmap = arenaBitmap(arena);
	int64_t slotIndex = bitmapClaimSlot(bitmap, arena->bitMapCount);
	if (slotIndex < 0) return NULL;
	arena->freeSlotCount--;
	return (uint8_t*)arena + ((size_t)slotIndex << shift);
}

static void* smallLayerAllocSlow(ArenaSlabAllocator* context, uint32_t classIndex);

static void* smallLayerAlloc(ArenaSlabAllocator* context, uint32_t classIndex) {
	ArenaHead* arena = context->currentSmall[classIndex];
	if (arena != NULL && arena->magic == magicArenaSmall && arena->freeSlotCount > 0) {
		void* slot = arenaSlotClaim(arena, classIndex + 4);
		if (slot == NULL) return NULL;
		if (arena->freeSlotCount == 0) {
			context->currentSmall[classIndex] = NULL;
		}
		return slot;
	}
	return smallLayerAllocSlow(context, classIndex);
}

static void* smallLayerAllocSlow(ArenaSlabAllocator* context, uint32_t classIndex) {
	uint16_t classSize = slotClassTable[classIndex];
	Segment* segment = &context->segment;

	uint64_t* link = &segment->partial[classIndex];
	uint32_t budget = arenaWalkBudget;
	while (*link != (uint64_t)SLAB_OFFSET_NONE && budget > 0) {
		budget--;
		ArenaHead* arena = (ArenaHead*)(segment->base + *link);
		if (arena->magic == magicArenaSmallDropped) {
			size_t reuseLength = ARENA_SIZE_SMALL - arenaKeptBytes(osPageSize());
			if (!osPagesReuse((uint8_t*)arena + arenaKeptBytes(osPageSize()), reuseLength)) return NULL;
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
			*link = arena->next;
			arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
			continue;
		}
		void* slot = arenaSlotClaim(arena, classIndex + 4);
		if (slot == NULL) return NULL;
		context->currentSmall[classIndex] = arena;
		if (arena->freeSlotCount == 0) {
			*link = arena->next;
			arena->next = (uint64_t)SLAB_OFFSET_UNLINKED;
			context->currentSmall[classIndex] = NULL;
		}
		return slot;
	}

	ArenaHead* arena = segmentCarveArena(segment, ARENA_SIZE_SMALL, classSize, magicArenaSmall);
	if (arena == NULL) {
		context->currentSmall[classIndex] = NULL;
		return NULL;
	}
	segmentPushArena(segment, classIndex, arena);
	context->currentSmall[classIndex] = arena;
	return arenaSlotClaim(arena, classIndex + 4);
}

static bool smallLayerFree(ArenaSlabAllocator* context, Segment* segment, ArenaHead* arena, void* pointer) {
	uintptr_t address = (uintptr_t)pointer;

	uint32_t classIndex = SLOT_CLASS_COUNT;
	for (uint32_t index = 0; index < SLOT_CLASS_COUNT; index++) {
		if (slotClassTable[index] == arena->classSize) { classIndex = index; break; }
	}
	if (classIndex == SLOT_CLASS_COUNT) return false;
	uint32_t shift = classIndex + 4;
	if (arena->bitMapCount != bitMapWordTable[classIndex]) return false;
	if (arena->headerSlots != headerSlotTable[classIndex]) return false;

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
	if (wasFull) {
		if (arena->next == (uint64_t)SLAB_OFFSET_UNLINKED) {
			segmentPushArena(segment, classIndex, arena);
		}
		context->currentSmall[classIndex] = arena;
	}
	return true;
}

/* ---- Public API ---- */

bool arenaSlab_init(ArenaSlabAllocator* context, uint8_t segmentSizeExponent) {
	if (context == NULL) return false;
	if (context->initialized) return true;
	if (segmentSizeExponent < SEGMENT_SIZE_EXPONENT_MIN || segmentSizeExponent > SEGMENT_SIZE_EXPONENT_MAX) return false;

	uint64_t segmentBytes = ((uint64_t)1 << segmentSizeExponent);

	Segment* segment = &context->segment;
	segment->base = NULL;
	segment->frontier = NULL;
	segment->committedEnd = NULL;
	segment->bytes = 0;
	for (uint32_t chainIndex = 0; chainIndex < SLOT_CLASS_COUNT; chainIndex++) {
		segment->partial[chainIndex] = (uint64_t)SLAB_OFFSET_NONE;
	}
	segment->reserveBase = NULL;
	segment->reserveSize = 0;
	for (uint32_t classIndex = 0; classIndex < SLOT_CLASS_COUNT; classIndex++) {
		context->currentSmall[classIndex] = NULL;
	}
#ifdef LOG_MALLOC_STATS
	memset(&context->stats, 0, sizeof(context->stats));
	segment->ownerStats = &context->stats;
#endif

	uint8_t* base = NULL;
	uint8_t* reserveBase = NULL;
	size_t reserveSize = 0;
	if (!osSegmentReserve(segmentBytes, &base, &reserveBase, &reserveSize)) {
		arenaSlab_shutdown(context);
		return false;
	}
	segment->base = base;
	segment->frontier = base;
	segment->committedEnd = base;
	segment->bytes = segmentBytes;
	segment->reserveBase = reserveBase;
	segment->reserveSize = reserveSize;

	context->initialized = true;
	return true;
}

void arenaSlab_shutdown(ArenaSlabAllocator* context) {
	if (context == NULL) return;
	for (uint32_t classIndex = 0; classIndex < SLOT_CLASS_COUNT; classIndex++) {
		context->currentSmall[classIndex] = NULL;
	}
	Segment* segment = &context->segment;
	osSegmentRelease((uint8_t*)segment->reserveBase, segment->reserveSize);
	segment->base = NULL;
	segment->frontier = NULL;
	segment->committedEnd = NULL;
	segment->bytes = 0;
	for (uint32_t chainIndex = 0; chainIndex < SLOT_CLASS_COUNT; chainIndex++) {
		segment->partial[chainIndex] = (uint64_t)SLAB_OFFSET_NONE;
	}
	segment->reserveBase = NULL;
	segment->reserveSize = 0;
#ifdef LOG_MALLOC_STATS
	memset(&context->stats, 0, sizeof(context->stats));
#endif
	context->initialized = false;
}

void* arenaSlab_alloc(ArenaSlabAllocator* context, size_t size) {
	if (context == NULL || !context->initialized) return NULL;
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
	if (context == NULL || !context->initialized) return false;
	if (pointer == NULL) return true;
	uintptr_t address = (uintptr_t)pointer;

	ArenaHead* arena = (ArenaHead*)(address & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
	if (arena->magic == magicArenaSmall) {
		Segment* segment = segmentFind(context, address);
		if (segment != NULL) {
#ifdef LOG_MALLOC_STATS
			size_t freedBytes = arena->classSize;
#endif
			bool ok = smallLayerFree(context, segment, arena, pointer);
#ifdef LOG_MALLOC_STATS
			if (ok) { context->stats.freeCount++; context->stats.freeBytes += freedBytes; }
			else { context->stats.freeRejected++; }
#endif
			return ok;
		}
	}
	return false;
}

void* arenaSlab_realloc(ArenaSlabAllocator* context, void* pointer, size_t newSize) {
	if (context == NULL || !context->initialized) return NULL;
	if (pointer == NULL) return arenaSlab_alloc(context, newSize);
	if (newSize == 0) {
		arenaSlab_free(context, pointer);
		return NULL;
	}
	/* Realloc is only supported for small objects; otherwise caller's problem. */
	if (newSize > SLOT_SIZE_MAX) { arenaSlab_free(context, pointer); return NULL; }
	slabLayer layer = arenaSlab_which(context, pointer);
	size_t oldUsable = arenaSlab_usable_size(context, pointer);
	if (layer != SLAB_LAYER_SMALL || oldUsable == 0) return NULL;
	if (oldUsable >= newSize) return pointer;
	void* newPointer = arenaSlab_alloc(context, newSize);
	if (newPointer == NULL) return NULL;
	memcpy(newPointer, pointer, oldUsable < newSize ? oldUsable : newSize);
	arenaSlab_free(context, pointer);
	return newPointer;
}

size_t arenaSlab_usable_size(ArenaSlabAllocator* context, void* pointer) {
	if (context == NULL || !context->initialized || pointer == NULL) return 0;
	if (arenaSlab_which(context, pointer) != SLAB_LAYER_SMALL) return 0;
	ArenaHead* arena = (ArenaHead*)((uintptr_t)pointer & ~(uintptr_t)(ARENA_SIZE_SMALL - 1));
	if (arena->magic != magicArenaSmall) return 0;
	return arena->classSize;
}

slabLayer arenaSlab_which(ArenaSlabAllocator* context, void* pointer) {
	if (context == NULL || pointer == NULL) return SLAB_LAYER_NONE;
	if (segmentFind(context, (uintptr_t)pointer) != NULL) return SLAB_LAYER_SMALL;
	return SLAB_LAYER_NONE;
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
	if (context == NULL || !context->initialized) return 0;
	return trimSmallLayer(context);
}

/* ---- Statistics ---- */
void arenaSlab_statsReset(ArenaSlabAllocator* context) {
	if (context == NULL) return;
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
	if (context == NULL || !context->initialized) {
		printf("slab stats: not initialized\n");
		return;
	}

	printf("== slab stats (page size %u B) ==\n", osPageSize());
	printf("[small]\n");
	dumpLayerEvents(&context->stats);
	{
		uint64_t arenaTotal = 0, emptyCount = 0, fullCount = 0, droppedCount = 0;
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
				if (arena->freeSlotCount == 0) fullCount++;
				if (arena->magic == magicArenaSmallDropped) droppedCount++;
				arenaOffset = arena->next;
			}
		}
		printf("  State: arenas %llu (empty %llu / partial %llu / full %llu / dropped %llu), slots live %llu / %llu\n",
			(unsigned long long)arenaTotal, (unsigned long long)emptyCount,
			(unsigned long long)(arenaTotal - emptyCount - fullCount),
			(unsigned long long)fullCount, (unsigned long long)droppedCount,
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
