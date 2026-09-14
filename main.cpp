#include <iostream>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <cassert>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "./src/objectPool.hpp"
#include "./src/arena_slab.h"
#include "./src/palloc_os.h"

// Define the maximum number of allocations
#define MAX_ALLOCATIONS 100000

static int testFailures = 0;

#define CHECK(condition) do { \
	if (!(condition)) { \
		testFailures++; \
		printf("FAIL line %d: %s\n", __LINE__, #condition); \
	} \
} while (0)

static void section(const char* title) {
	printf("- %s\n", title);
}

class Xorshift64 {
private:
	uint64_t state;

public:
	explicit Xorshift64(uint64_t s = 123456789) : state(s) {}

	uint64_t next_u64() {
		uint64_t x = state;
		x ^= x << 12;
		x ^= x >> 25;
		x ^= x << 27;
		state = x;
		return x * UINT64_C(2685821657736338717);
	}

	void reseed(uint64_t new_state) {
		state = new_state;
	}
};

void test_allocation_correctness(size_t fixed_size, size_t num_operations) {
	std::vector<void*> ptrs;
	std::vector<bool> allocated;
	ptrs.reserve(MAX_ALLOCATIONS);
	allocated.reserve(MAX_ALLOCATIONS);

	slab::FixedAllocator alloc(fixed_size, 1);

	Xorshift64 rng(123456789);

	for (size_t i = 0; i < num_operations; ++i) {
		if ((rng.next_u64() % 2 == 0 || ptrs.empty()) && ptrs.size() < MAX_ALLOCATIONS) {
			void* ptr = alloc.allocate();
			if (ptr == nullptr) {
				std::cerr << "test_allocation_correctness: allocation failed at operation " << i << std::endl;
				testFailures++;
				goto cleanup;
			}

			// check ptr is not already in our list
			for (size_t j = 0; j < ptrs.size(); ++j) {
				if (ptrs[j] == ptr) {
					std::cerr << "test_allocation_correctness: duplicate allocation detected at operation " << i << std::endl;
					testFailures++;
					goto cleanup;
				}
			}

			// check ptr alignment (should be at least 8-byte aligned)
			if ((reinterpret_cast<uintptr_t>(ptr) & 7) != 0) {
				std::cerr << "test_allocation_correctness: alignment error at operation " << i << std::endl;
				testFailures++;
				goto cleanup;
			}

			ptrs.push_back(ptr);
			allocated.push_back(true);

			// write pattern to detect corruption
			std::memset(ptr, 0xAA, fixed_size);

		}
		else if (!ptrs.empty()) {
			int idx = rng.next_u64() % ptrs.size();

			if (!allocated[idx]) {
				std::cerr << "test_allocation_correctness: double free detected at operation " << i << std::endl;
				testFailures++;
				goto cleanup;
			}

			// verify pattern before free
			uint8_t* bytes = static_cast<uint8_t*>(ptrs[idx]);
			for (size_t j = 0; j < fixed_size; ++j) {
				if (bytes[j] != 0xAA) {
					std::cerr << "test_allocation_correctness: memory corruption detected at operation " << i << ", byte " << j << std::endl;
					testFailures++;
					goto cleanup;
				}
			}

			alloc.deallocate(ptrs[idx]);
			allocated[idx] = false;

			// remove from array by swapping with last
			if (idx != static_cast<int>(ptrs.size() - 1)) {
				ptrs[idx] = ptrs.back();
				allocated[idx] = allocated.back();
			}
			ptrs.pop_back();
			allocated.pop_back();
		}
	}

	// free remaining allocations
	for (size_t i = 0; i < ptrs.size(); ++i) {
		if (allocated[i]) {
			alloc.deallocate(ptrs[i]);
		}
	}

	std::cout << "[Size " << fixed_size << "] test_allocation_correctness: " << num_operations << " operations completed successfully." << std::endl;

	return;

cleanup:
	// free remaining allocations
	for (size_t i = 0; i < ptrs.size(); ++i) {
		if (allocated[i]) {
			alloc.deallocate(ptrs[i]);
		}
	}
}

/* ================================================================== */
/* arenaSlab test suite (merged from test.c)                          */
/* ================================================================== */

/* ---- init parameter validation + NULL-context safety ---- */
static void testInitValidation(void) {
	section("init validation");
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));

	CHECK(arenaSlab_init(NULL, SEGMENT_SIZE_EXPONENT_DEFAULT) == false);
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MIN - 1) == false); /* 1MB - 1 page: rejected */
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MAX + 1) == false); /* > 1TB: rejected */
	CHECK(allocator.cookie == 0); /* failed attempts must not half-initialize */

	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MIN) == true); /* 1MB */
	arenaSlab_shutdown(&allocator);
	CHECK(allocator.cookie == 0);

	/* NULL context must be safe everywhere */
	CHECK(arenaSlab_alloc(NULL, 16) == NULL);
	CHECK(arenaSlab_free(NULL, NULL) == false); /* NULL context is rejected */
	CHECK(arenaSlab_which(NULL, NULL) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(NULL, NULL) == 0);
	CHECK(arenaSlab_segmentBase(NULL) == 0);
	CHECK(arenaSlab_trim(NULL, 0) == 0);
	arenaSlab_shutdown(NULL); /* must not crash */
	arenaSlab_statsReset(NULL);

	/* garbage context (never zeroed, e.g. a forgotten stack init): every API must reject it
	 * by the cookie gate without touching memory — the old code dereferenced garbage in
	 * alloc and released garbage pointers in shutdown */
	ArenaSlabAllocator garbage;
	memset(&garbage, 0xAB, sizeof(garbage));
	CHECK(arenaSlab_alloc(&garbage, 16) == NULL);
	CHECK(arenaSlab_which(&garbage, &garbage) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(&garbage, &garbage) == 0);
	CHECK(arenaSlab_segmentBase(&garbage) == 0);
	CHECK(arenaSlab_trim(&garbage) == 0);
	CHECK(arenaSlab_calloc(&garbage, 1, 8) == NULL);
	CHECK(arenaSlab_free(&garbage, &garbage) == false);
	arenaSlab_shutdown(&garbage); /* must be a no-op, never a release of garbage pointers */
	arenaSlab_statsReset(&garbage);

	/* init rebuilds a garbage context from scratch */
	CHECK(arenaSlab_init(&garbage, SEGMENT_SIZE_EXPONENT_MIN) == true);
	void* garbageSlot = arenaSlab_alloc(&garbage, 16);
	CHECK(garbageSlot != NULL);
	CHECK(arenaSlab_free(&garbage, garbageSlot) == true);
	arenaSlab_shutdown(&garbage);
	CHECK(arenaSlab_init(&garbage, SEGMENT_SIZE_EXPONENT_MIN) == true); /* re-init after shutdown */
	arenaSlab_shutdown(&garbage);
}

/* ---- the documented default instance ---- */
static void testDefaultInstance(void) {
	section("default instance (4GB)");
	CHECK(arenaSlab_init(&arenaSlabDefault, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);

	void* pointer = arenaSlab_alloc(&arenaSlabDefault, 200);
	CHECK(pointer != NULL);
	CHECK(((uintptr_t)pointer & 15) == 0);
	CHECK(arenaSlab_which(&arenaSlabDefault, pointer) == SLAB_LAYER_SMALL);
	CHECK(arenaSlab_usable_size(&arenaSlabDefault, pointer) == 256);

	/* segment base: external pointer compression must round-trip through a u32 offset */
	uintptr_t base = arenaSlab_segmentBase(&arenaSlabDefault);
	CHECK(base != 0);
	CHECK((uintptr_t)pointer >= base); /* slots live inside [base, base + segmentBytes) */
	uint32_t compressed = (uint32_t)((uintptr_t)pointer - base);
	CHECK(base + compressed == (uintptr_t)pointer);

	CHECK(arenaSlab_free(&arenaSlabDefault, pointer) == true);

	/* re-init on an already initialized instance is a no-op (stays 4GB) */
	CHECK(arenaSlab_init(&arenaSlabDefault, SEGMENT_SIZE_EXPONENT_MIN) == true);

	arenaSlab_shutdown(&arenaSlabDefault);
	CHECK(arenaSlab_alloc(&arenaSlabDefault, 16) == NULL); /* unusable after shutdown */
	CHECK(arenaSlab_segmentBase(&arenaSlabDefault) == 0);
}

/* ---- slot classes, alignment, usable size ---- */
static void testClassBasics(ArenaSlabAllocator* allocator) {
	static const size_t sizes[] = { 1, 8, 9, 16, 17, 32, 33, 64, 65, 128, 129, 256 };
	static const size_t expectedUsable[] = { 8, 8, 16, 16, 32, 32, 64, 64, 128, 128, 256, 256 };
	const uint32_t count = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
	void* pointers[sizeof(sizes) / sizeof(sizes[0])];

	for (uint32_t index = 0; index < count; index++) {
		pointers[index] = arenaSlab_alloc(allocator, sizes[index]);
		CHECK(pointers[index] != NULL);
		if (pointers[index] == NULL) continue;
		/* the 8B class returns 8B-aligned pointers, every other class 16B */
		uintptr_t alignMask = (expectedUsable[index] == 8) ? 7 : 15;
		CHECK(((uintptr_t)pointers[index] & alignMask) == 0);
		CHECK(arenaSlab_usable_size(allocator, pointers[index]) == expectedUsable[index]);
		CHECK(arenaSlab_which(allocator, pointers[index]) == SLAB_LAYER_SMALL);
		memset(pointers[index], (int)(index + 1), expectedUsable[index]); /* writable */
	}

	/* size 0 -> 8B class; oversize -> NULL */
	void* zero = arenaSlab_alloc(allocator, 0);
	CHECK(zero != NULL);
	CHECK(arenaSlab_usable_size(allocator, zero) == 8);
	CHECK(arenaSlab_free(allocator, zero) == true);
	CHECK(arenaSlab_alloc(allocator, 257) == NULL);
	CHECK(arenaSlab_alloc(allocator, (size_t)-1) == NULL);

	for (uint32_t index = 0; index < count; index++) {
		CHECK(arenaSlab_free(allocator, pointers[index]) == true);
		/* double free is NOT rejected by design: freed slot memory doubles as freelist
		 * state, so a second free here would corrupt the list — never do it */
	}

	/* 8B arena cycle: bump through a whole arena (2044 payload slots), the next slot
	 * opens a second arena; freeing everything parks both back through the ring */
	enum { SLOTS_8B = 2044 };
	void* slots8b[SLOTS_8B + 1];
	for (uint32_t index = 0; index <= SLOTS_8B; index++) {
		slots8b[index] = arenaSlab_alloc(allocator, 8);
		CHECK(slots8b[index] != NULL);
		CHECK(arenaSlab_usable_size(allocator, slots8b[index]) == 8);
		CHECK(((uintptr_t)slots8b[index] & 7) == 0);
		memset(slots8b[index], 0x8B, 8); /* writable */
	}
	for (uint32_t index = 0; index <= SLOTS_8B; index++) {
		CHECK(arenaSlab_free(allocator, slots8b[index]) == true);
	}

	/* revival: both parked shells come back through the ring (O(1) re-init, no syscall)
	 * and the identical bump cycle repeats */
	for (uint32_t index = 0; index <= SLOTS_8B; index++) {
		slots8b[index] = arenaSlab_alloc(allocator, 8);
		CHECK(slots8b[index] != NULL);
		memset(slots8b[index], 0xCD, 8); /* revived payload writable */
	}
	for (uint32_t index = 0; index <= SLOTS_8B; index++) {
		CHECK(arenaSlab_free(allocator, slots8b[index]) == true);
	}
}

/* ---- many arenas per class: carve, chain walk, partial reuse ---- */
static void testChainReuse(ArenaSlabAllocator* allocator) {
	section("chain reuse (3 arenas x 32B class)");
	/* 24B lands in the 32B class; one 32B arena holds 511 payload slots, so 1500 spans 3 arenas */
	enum { COUNT = 1500 };
	static void* slots[COUNT];

	for (uint32_t index = 0; index < COUNT; index++) {
		slots[index] = arenaSlab_alloc(allocator, 24);
		CHECK(slots[index] != NULL);
	}
	for (uint32_t index = 0; index < COUNT; index += 3) {
		CHECK(arenaSlab_free(allocator, slots[index]) == true);
	}
	for (uint32_t index = 0; index < COUNT; index += 3) {
		slots[index] = arenaSlab_alloc(allocator, 24); /* served from the partial chains */
		CHECK(slots[index] != NULL);
	}
	for (uint32_t index = 0; index < COUNT; index++) {
		CHECK(arenaSlab_free(allocator, slots[index]) == true);
	}
}

/* ---- heavy churn across ~99 arenas on the default segment ---- */
enum { CHURN_SLOTS = 100000 };
static void* churnPointers[CHURN_SLOTS];

static void testChurn(ArenaSlabAllocator* allocator) {
	section("churn (100k x 16B)");
	for (uint32_t index = 0; index < CHURN_SLOTS; index++) {
		churnPointers[index] = arenaSlab_alloc(allocator, 16);
		CHECK(churnPointers[index] != NULL);
		if (churnPointers[index] == NULL) break;
	}
	for (uint32_t index = 0; index < CHURN_SLOTS; index += 2) {
		CHECK(arenaSlab_free(allocator, churnPointers[index]) == true);
	}
	for (uint32_t index = 0; index < CHURN_SLOTS; index += 2) {
		churnPointers[index] = arenaSlab_alloc(allocator, 16); /* reuse through the chains */
		CHECK(churnPointers[index] != NULL);
	}
	for (uint32_t index = 0; index < CHURN_SLOTS; index++) {
		CHECK(arenaSlab_free(allocator, churnPointers[index]) == true);
	}
	/* NOTE: churnPointers[0] was already freed above — a second free would corrupt the freelist */
}

/* ---- free parks emptied arenas into the resident ring; the ring revives them for free ---- */
static void testTrimAndReuse(void) {
	section("trim + resident-ring reuse");
	/* dedicated instance so the ring level is fully controlled */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);

	/* 16B arena: 1024 slots - 2 header slots = 1022 payload slots */
	enum { ARENAS = 5, SLOTS_PER_ARENA = 1022 };
	void* slots[ARENAS * SLOTS_PER_ARENA];
	const uint32_t count = ARENAS * SLOTS_PER_ARENA;

	for (uint32_t index = 0; index < count; index++) {
		slots[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(slots[index] != NULL);
	}
	for (uint32_t index = 0; index < count; index++) {
		CHECK(arenaSlab_free(&allocator, slots[index]) == true);
	}

	/* free keeps the first CLASS_SPARE_KEEP empties of the class linked as warm spares;
	 * the rest park into the resident ring — far below the water mark, trim drops
	 * nothing (no syscall, the pages stay committed) */
	CHECK(allocator.segment.residentCount == ARENAS - CLASS_SPARE_KEEP);
	printf("  ring holds %u shells, trim dropped %llu bytes\n",
		(unsigned)(ARENAS - CLASS_SPARE_KEEP), (unsigned long long)arenaSlab_trim(&allocator));
	CHECK(allocator.segment.residentCount == ARENAS - CLASS_SPARE_KEEP);

	/* the parked shells must revive through the ring head, fully writable, with no
	 * page-reuse syscall (revive-chain shells would pay osPagesReuse) */
#ifdef LOG_MALLOC_STATS
	size_t reuseCallsBefore = allocator.stats.reuseCalls;
#endif
	for (uint32_t index = 0; index < count; index++) {
		slots[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(slots[index] != NULL);
	}
#ifdef LOG_MALLOC_STATS
	CHECK(allocator.stats.reuseCalls == reuseCallsBefore); /* ring revival: no reuse */
#endif
	for (uint32_t index = 0; index < count; index++) {
		memset(slots[index], 0xCD, 16);
	}
	for (uint32_t index = 0; index < count; index++) {
		CHECK(arenaSlab_free(&allocator, slots[index]) == true);
	}
	CHECK(allocator.segment.residentCount == ARENAS - CLASS_SPARE_KEEP); /* 2 spares re-linked, rest re-parked */

	arenaSlab_shutdown(&allocator);
}

/* ---- cross-size reuse: shells parked by free must serve another class ---- */
static void testCrossSizeReuse(void) {
	section("cross-size shell reuse (256B shells -> 16B class)");
	/* dedicated instance so the ring state is fully controlled */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);

	/* 256B phase: fill four arenas (4 x 63 slots), free everything — free parks all four
	 * into the resident ring with pages RESIDENT (no trim, no syscall involved) */
	enum { COUNT_256B = 4 * 63 };
	static void* big[COUNT_256B];
	uintptr_t rangeMin = ~(uintptr_t)0;
	uintptr_t rangeMax = 0;
	for (uint32_t index = 0; index < COUNT_256B; index++) {
		big[index] = arenaSlab_alloc(&allocator, 256);
		CHECK(big[index] != NULL);
		if (big[index] != NULL) {
			uintptr_t block = (uintptr_t)big[index] & ~(uintptr_t)(ARENA_SIZE_SMALL - 1);
			if (block < rangeMin) rangeMin = block;
			if (block > rangeMax) rangeMax = block;
		}
	}
	for (uint32_t index = 0; index < COUNT_256B; index++) {
		CHECK(arenaSlab_free(&allocator, big[index]) == true);
	}
	/* two warm spares stay LINKED on the 256B chain; the other two park into the ring */
	CHECK(allocator.segment.residentCount == 4 - CLASS_SPARE_KEEP);

	/* 16B phase: the 16B class chain is empty, so the shells must be revived cross-size
	 * from the ring head — every new slot must land inside the old 256B arena blocks
	 * (a fresh carve would sit beyond the old frontier and fail this check) */
	enum { COUNT_16B = 2 * 1022 };
	static void* small[COUNT_16B];
#ifdef LOG_MALLOC_STATS
	size_t reuseCallsBefore = allocator.stats.reuseCalls;
#endif
	for (uint32_t index = 0; index < COUNT_16B; index++) {
		small[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(small[index] != NULL);
		if (small[index] != NULL) {
			uintptr_t block = (uintptr_t)small[index] & ~(uintptr_t)(ARENA_SIZE_SMALL - 1);
			CHECK(block >= rangeMin && block <= rangeMax);
			memset(small[index], 0xCD, 16); /* revived payload pages must be writable */
		}
	}
#ifdef LOG_MALLOC_STATS
	CHECK(allocator.stats.reuseCalls == reuseCallsBefore); /* ring revival: no reuse */
#endif
	for (uint32_t index = 0; index < COUNT_16B; index++) {
		CHECK(arenaSlab_free(&allocator, small[index]) == true);
	}
	/* the revived arenas become the 16B class's warm spares (staying linked) — the ring
	 * is empty now: the two parked 256B shells were consumed by the revival */
	CHECK(allocator.segment.residentCount == 0);

	arenaSlab_shutdown(&allocator);
}

/* ---- trim water mark: the resident ring keeps at most TRIM_POOL_MAX_RESIDENT shells;
 * revival from the ring head is free, the dropped cold tail pays one reuse each ---- */
static void testTrimHighWater(void) {
	section("trim water mark");
	/* dedicated instance so the ring level is fully controlled */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);

	uint32_t pageSize = osPageSize();
	size_t dropLength = (pageSize < ARENA_SIZE_SMALL) ? (size_t)(ARENA_SIZE_SMALL - pageSize) : 0;

	/* fill 320 arenas, free everything: free keeps the first CLASS_SPARE_KEEP empties
	 * linked as warm spares and parks the rest into the resident ring (no syscall),
	 * then trim drops exactly the coldest shells beyond the water mark */
	enum { ARENA_COUNT = 320, SLOTS = 1022 };
	enum { COUNT = ARENA_COUNT * SLOTS };
	static void* slots[COUNT];
	for (uint32_t index = 0; index < COUNT; index++) {
		slots[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(slots[index] != NULL);
	}
	for (uint32_t index = 0; index < COUNT; index++) {
		CHECK(arenaSlab_free(&allocator, slots[index]) == true);
	}
	CHECK(allocator.segment.residentCount == ARENA_COUNT - CLASS_SPARE_KEEP);
	CHECK(arenaSlab_trim(&allocator) == (size_t)(ARENA_COUNT - CLASS_SPARE_KEEP - TRIM_POOL_MAX_RESIDENT) * dropLength);
	CHECK(allocator.segment.residentCount == TRIM_POOL_MAX_RESIDENT);

	/* idempotent: the second trim drops nothing (the ring sits exactly at the water mark) */
	CHECK(arenaSlab_trim(&allocator) == 0);

	/* revival order, arena-aligned phases: the CLASS_SPARE_KEEP chain spares revive first,
	 * then the 256 ring-head shells — none may pay a reuse; each of the dropped tail
	 * shells pays exactly one reuse when its first slot is claimed */
	enum { SPARE_PHASE = CLASS_SPARE_KEEP * SLOTS };
	enum { RING_PHASE = TRIM_POOL_MAX_RESIDENT * SLOTS };
	enum { DROPPED_COUNT = ARENA_COUNT - CLASS_SPARE_KEEP - TRIM_POOL_MAX_RESIDENT };
	enum { DROPPED_PHASE = DROPPED_COUNT * SLOTS };
#ifdef LOG_MALLOC_STATS
	size_t reuseCallsBefore = allocator.stats.reuseCalls;
	size_t reuseBytesBefore = allocator.stats.reuseBytes;
#endif
	for (uint32_t index = 0; index < SPARE_PHASE; index++) {
		slots[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(slots[index] != NULL);
	}
#ifdef LOG_MALLOC_STATS
	CHECK(allocator.stats.reuseCalls == reuseCallsBefore); /* spare revival: no reuse */
#endif
	for (uint32_t index = SPARE_PHASE; index < SPARE_PHASE + RING_PHASE; index++) {
		slots[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(slots[index] != NULL);
	}
#ifdef LOG_MALLOC_STATS
	CHECK(allocator.stats.reuseCalls == reuseCallsBefore); /* ring revival: no reuse */
#endif
	for (uint32_t index = SPARE_PHASE + RING_PHASE; index < SPARE_PHASE + RING_PHASE + DROPPED_PHASE; index++) {
		slots[index] = arenaSlab_alloc(&allocator, 16);
		CHECK(slots[index] != NULL);
	}
#ifdef LOG_MALLOC_STATS
	CHECK(allocator.stats.reuseCalls == reuseCallsBefore + DROPPED_COUNT);
	CHECK(allocator.stats.reuseBytes == reuseBytesBefore + (size_t)DROPPED_COUNT * dropLength);
#endif
	for (uint32_t index = 0; index < SPARE_PHASE + RING_PHASE + DROPPED_PHASE; index++) {
		CHECK(arenaSlab_free(&allocator, slots[index]) == true);
	}
	CHECK(allocator.segment.residentCount == ARENA_COUNT - CLASS_SPARE_KEEP); /* 2 spares re-linked, rest re-parked */

	arenaSlab_shutdown(&allocator);
}

/* ---- realloc semantics ---- */
static void testRealloc(ArenaSlabAllocator* allocator) {
	section("realloc");
	unsigned char* small = static_cast<unsigned char*>(arenaSlab_alloc(allocator, 16));
	CHECK(small != NULL);
	for (int index = 0; index < 16; index++) small[index] = (unsigned char)index;

	unsigned char* same = static_cast<unsigned char*>(arenaSlab_realloc(allocator, small, 16));
	CHECK(same == small); /* fits in the old class: pointer unchanged */

	unsigned char* bigger = static_cast<unsigned char*>(arenaSlab_realloc(allocator, small, 128));
	CHECK(bigger != NULL && bigger != small);
	CHECK(arenaSlab_usable_size(allocator, bigger) == 128);
	for (int index = 0; index < 16; index++) CHECK(bigger[index] == (unsigned char)index);
	/* NOTE: `small` was freed by realloc — a second free would corrupt the freelist */

	CHECK(arenaSlab_realloc(allocator, bigger, 32) == bigger); /* shrink keeps the pointer */

	unsigned char* fresh = static_cast<unsigned char*>(arenaSlab_realloc(allocator, NULL, 32)); /* acts as alloc */
	CHECK(fresh != NULL);
	CHECK(arenaSlab_usable_size(allocator, fresh) == 32);

	CHECK(arenaSlab_realloc(allocator, bigger, 0) == NULL); /* acts as free */

	CHECK(arenaSlab_realloc(allocator, fresh, 4096) == NULL); /* oversize: freed + NULL */

	/* 8B leg: growing crosses the class boundary (pointer moves, content survives);
	 * shrinking back keeps the slot and its 16B usable size (shrink never reclassifies) */
	unsigned char* tiny = static_cast<unsigned char*>(arenaSlab_alloc(allocator, 8));
	CHECK(tiny != NULL);
	for (int index = 0; index < 8; index++) tiny[index] = (unsigned char)(index + 0x40);
	unsigned char* grown = static_cast<unsigned char*>(arenaSlab_realloc(allocator, tiny, 16));
	CHECK(grown != NULL && grown != tiny); /* 8B -> 16B class: must move */
	for (int index = 0; index < 8; index++) CHECK(grown[index] == (unsigned char)(index + 0x40));
	CHECK(arenaSlab_usable_size(allocator, grown) == 16);
	CHECK(arenaSlab_realloc(allocator, grown, 8) == grown); /* shrink keeps the pointer */
	CHECK(arenaSlab_usable_size(allocator, grown) == 16);
	CHECK(arenaSlab_free(allocator, grown) == true);
}

/* ---- calloc: zero-fill, overflow, zero total; the size gate ---- */
static void testCalloc(ArenaSlabAllocator* allocator) {
	section("calloc + permissible_size");

	/* basic zero-fill: whatever slot comes back (fresh bump or reused freelist head),
	 * calloc must hand out zeros */
	unsigned char* written = static_cast<unsigned char*>(arenaSlab_alloc(allocator, 64));
	CHECK(written != NULL);
	memset(written, 0xA7, 64);
	CHECK(arenaSlab_free(allocator, written) == true);
	unsigned char* zeroed = static_cast<unsigned char*>(arenaSlab_calloc(allocator, 1, 64));
	CHECK(zeroed != NULL);
	if (zeroed != NULL) {
		CHECK(arenaSlab_usable_size(allocator, zeroed) == 64);
		for (int index = 0; index < 64; index++) CHECK(zeroed[index] == 0);
	}
	CHECK(arenaSlab_free(allocator, zeroed) == true);

	/* multi-element zero-fill at the exact class limit */
	void* grid = arenaSlab_calloc(allocator, 8, 32); /* 256B exactly */
	CHECK(grid != NULL);
	if (grid != NULL) {
		CHECK(arenaSlab_usable_size(allocator, grid) == 256);
		for (int index = 0; index < 256; index++) CHECK(static_cast<unsigned char*>(grid)[index] == 0);
	}
	CHECK(arenaSlab_free(allocator, grid) == true);

	/* overflow and oversize reject; zero total follows alloc's size-0 rule */
	CHECK(arenaSlab_calloc(allocator, (size_t)-1, 16) == NULL); /* count * size overflows */
	CHECK(arenaSlab_calloc(allocator, 32, 16) == NULL);         /* 512B: over the class limit */
	CHECK(arenaSlab_calloc(allocator, 0, 0) != NULL);           /* zero total: alloc's size-0 rule */
	CHECK(arenaSlab_calloc(allocator, 16, 0) != NULL);          /* ditto */

	/* the size gate: alloc's and realloc's shared domain */
	CHECK(arenaSlab_permissible_size(0) == true);
	CHECK(arenaSlab_permissible_size(1) == true);
	CHECK(arenaSlab_permissible_size(256) == true);
	CHECK(arenaSlab_permissible_size(257) == false);
	CHECK(arenaSlab_permissible_size((size_t)-1) == false);

	/* foreign / NULL contexts stay rejected through calloc */
	CHECK(arenaSlab_calloc(NULL, 1, 8) == NULL);
}

/* ---- foreign pointers must be rejected without touching the allocator ---- */
static void testForeignPointers(ArenaSlabAllocator* allocator, uint8_t segmentSizeExponent) {
	section("foreign pointers");
	int stackValue = 0;
	CHECK(arenaSlab_which(allocator, &stackValue) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(allocator, &stackValue) == 0);
	CHECK(arenaSlab_free(allocator, &stackValue) == false);
	CHECK(arenaSlab_free(allocator, NULL) == true);

	/* bogus addresses just below the segment and at the reservation tail: unmapped /
	 * PAGE_NOACCESS, must be rejected by pure arithmetic — never dereferenced (the old
	 * code read the arena header first and crashed). The tail derives from the actual
	 * segment size, so this holds for every exponent, not just the 4GB default. */
	uintptr_t base = arenaSlab_segmentBase(allocator);
	void* belowBase = (void*)(base - 16);
	void* reservationTail = (void*)(base + ((uintptr_t)1 << segmentSizeExponent) - 16);
	CHECK(arenaSlab_which(allocator, belowBase) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(allocator, belowBase) == 0);
	CHECK(arenaSlab_free(allocator, belowBase) == false);
	CHECK(arenaSlab_which(allocator, reservationTail) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(allocator, reservationTail) == 0);
	CHECK(arenaSlab_free(allocator, reservationTail) == false);
	CHECK(arenaSlab_realloc(allocator, &stackValue, 32) == NULL); /* foreign: rejected, left untouched */
}

/* ---- in-arena but invalid pointers (header region, misaligned, tail) must be rejected ---- */
static void testInteriorPointerRejection(ArenaSlabAllocator* allocator) {
	section("interior pointer rejection");
	void* slot = arenaSlab_alloc(allocator, 16);
	CHECK(slot != NULL);
	if (slot == NULL) return;
	uintptr_t arenaBase = (uintptr_t)slot & ~(uintptr_t)(ARENA_SIZE_SMALL - 1);

	/* all of these live inside a carved arena (which reports SMALL) but are not freeable
	 * slots: the arena base, header bytes, a header slot, a mid-slot and the arena tail */
	CHECK(arenaSlab_which(allocator, (void*)(arenaBase + 8)) == SLAB_LAYER_SMALL);
	CHECK(arenaSlab_free(allocator, (void*)arenaBase) == false);                          /* header slot 0 */
	CHECK(arenaSlab_free(allocator, (void*)(arenaBase + 8)) == false);                    /* header bytes, misaligned */
	CHECK(arenaSlab_free(allocator, (void*)(arenaBase + 16)) == false);                   /* header slot 1 */
	CHECK(arenaSlab_free(allocator, (void*)((uintptr_t)slot + 8)) == false);              /* mid-slot, misaligned */
	CHECK(arenaSlab_free(allocator, (void*)(arenaBase + ARENA_SIZE_SMALL - 1)) == false); /* arena tail byte */

	CHECK(arenaSlab_free(allocator, slot) == true); /* the real slot still frees */

	/* 8B geometry: the header spans 4 slots (32B) — aligned frees of header slots 1..3
	 * must be rejected by the merged bounds check. Slot 4 is the first payload slot and
	 * is deliberately NOT freed here (freeing an unallocated slot corrupts the freelist) */
	void* slot8 = arenaSlab_alloc(allocator, 8);
	CHECK(slot8 != NULL);
	if (slot8 != NULL) {
		uintptr_t arenaBase8 = (uintptr_t)slot8 & ~(uintptr_t)(ARENA_SIZE_SMALL - 1);
		CHECK(arenaSlab_which(allocator, (void*)(arenaBase8 + 40)) == SLAB_LAYER_SMALL); /* payload range: SMALL by range */
		CHECK(arenaSlab_free(allocator, (void*)(arenaBase8 + 8)) == false);   /* header slot 1 */
		CHECK(arenaSlab_free(allocator, (void*)(arenaBase8 + 16)) == false);  /* header slot 2 */
		CHECK(arenaSlab_free(allocator, (void*)(arenaBase8 + 24)) == false);  /* header slot 3 */
		CHECK(arenaSlab_free(allocator, (void*)(arenaBase8 + 25)) == false);  /* misaligned */
		CHECK(arenaSlab_free(allocator, slot8) == true); /* the real 8B slot frees */
	}
}

/* ---- two-instance isolation: one allocator must never accept the other's pointers ---- */
static void testCrossInstanceIsolation(void) {
	section("two-instance isolation");
	ArenaSlabAllocator a;
	ArenaSlabAllocator b;
	memset(&a, 0, sizeof(a));
	memset(&b, 0, sizeof(b));
	CHECK(arenaSlab_init(&a, SEGMENT_SIZE_EXPONENT_MIN) == true);
	CHECK(arenaSlab_init(&b, SEGMENT_SIZE_EXPONENT_MIN) == true);

	void* slotA = arenaSlab_alloc(&a, 64);
	CHECK(slotA != NULL);

	CHECK(arenaSlab_which(&b, slotA) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(&b, slotA) == 0);
	CHECK(arenaSlab_free(&b, slotA) == false);
	CHECK(arenaSlab_realloc(&b, slotA, 128) == NULL); /* rejected without freeing */

	CHECK(arenaSlab_which(&a, slotA) == SLAB_LAYER_SMALL);
	CHECK(arenaSlab_free(&a, slotA) == true); /* the true owner still frees it */

	arenaSlab_shutdown(&a);
	arenaSlab_shutdown(&b);
}

/* ---- single-segment semantics: 1MB, no growth, exhaustion, revive ---- */
static void testSingleSegmentCapacity(void) {
	section("1MB segment capacity");
	/* 1MB = 64 arenas of 16KB; 16B arena holds 1022 slots -> 64 * 1022 = 65408 slots max */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MIN) == true);

	enum { TOTAL_16B_SLOTS = 64 * 1022 };
	static void* slots[TOTAL_16B_SLOTS];
	uint32_t allocated = 0;
	for (;;) {
		void* slot = arenaSlab_alloc(&allocator, 16);
		if (slot == NULL) break;
		slots[allocated++] = slot;
	}
	printf("  1MB segment held %u x 16B slots\n", allocated);
	CHECK(allocated == 64 * 1022);

	/* exhausted: every other class must fail too (no cross-class, no cross-segment growth) */
	CHECK(arenaSlab_alloc(&allocator, 256) == NULL);

	/* freeing one slot revives the 16B class only */
	CHECK(arenaSlab_free(&allocator, slots[0]) == true);
	void* slot = arenaSlab_alloc(&allocator, 16);
	CHECK(slot != NULL);
	CHECK(arenaSlab_free(&allocator, slot) == true);
	CHECK(arenaSlab_alloc(&allocator, 256) == NULL);

	for (uint32_t index = 1; index < allocated; index++) {
		CHECK(arenaSlab_free(&allocator, slots[index]) == true);
	}
	arenaSlab_shutdown(&allocator);
}

/* ---- extreme exponent (1TB reservation; the OS may refuse it) ---- */
static void testHugeSegment(void) {
	section("1TB segment");
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	if (arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MAX)) {
		void* pointer = arenaSlab_alloc(&allocator, 16);
		CHECK(pointer != NULL);
		CHECK(arenaSlab_free(&allocator, pointer) == true);
		arenaSlab_shutdown(&allocator);
	}
	else {
		printf("  SKIP: 1TB reservation refused by the OS\n");
	}
}

/* ---- runner: all arenaSlab correctness tests ---- */
static void runArenaSlabTests(void) {
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));

	printf("== arenaSlab test suite ==\n");
	testInitValidation();
	testDefaultInstance();

	section("class basics (default 4GB segment)");
	const uint8_t segmentExponent = SEGMENT_SIZE_EXPONENT_DEFAULT;
	CHECK(arenaSlab_init(&allocator, segmentExponent) == true);
	testClassBasics(&allocator);
	testChainReuse(&allocator);
	testChurn(&allocator);
	testTrimAndReuse();
	testCrossSizeReuse();
	testTrimHighWater();
	testRealloc(&allocator);
	testCalloc(&allocator);
	testForeignPointers(&allocator, segmentExponent);
	testInteriorPointerRejection(&allocator);
	testCrossInstanceIsolation();
	arenaSlab_shutdown(&allocator);

	testSingleSegmentCapacity();
	testHugeSegment();
}

/* ================================================================== */
/* arenaSlab benchmarks (merged from test.c)                          */
/*                                                                     */
/* Timing uses std::chrono::steady_clock: wall clock, sub-us          */
/* resolution. Every workload runs for tens of ms so timer            */
/* granularity stays negligible. Results are folded into benchSink    */
/* so the compiler cannot optimize the work away.                     */
/* ================================================================== */

static uintptr_t benchSink = 0; /* anti-optimization checksum */

static double benchNow(void) {
	return std::chrono::duration<double>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void benchReport(const char* name, uint64_t operations, double seconds) {
	printf("  %-36s %9.1f ns/op %10.2f M ops/s\n", name,
		seconds * 1e9 / (double)operations,
		(double)operations / seconds / 1e6);
}

enum {
	BENCH_PATTERN_OPS = 4000000, /* operations per pattern run (pair / mixed) */
	BENCH_CHURN_OPS = 4000000,   /* operations for the churn pattern */
	BENCH_CHURN_SET = 8192,      /* churn working set size (power of two) */
	BENCH_BURST_COUNT = 100000,  /* pointers held during the burst pattern */
	BENCH_CARVE_ARENAS = 512,    /* fresh arenas carved in one bench */
	BENCH_TRIM_CYCLES = 3,       /* alloc/free/trim repetitions */
};

/* ---- uniform dispatch layer: one bench implementation, three backends ---- */

/* Size-routing wrapper over a family of FixedAllocators: one instance per 8-byte granular
 * size (index = (size + 7) >> 3, instance size = index * 8). Routing every request through
 * it makes the fixed backend pay the same per-call size-routing stage as malloc/arenaSlab —
 * handing the bench a pre-picked instance would distort the comparison. */
struct FixedAllocatorRouter {
	enum { kMaxSize = 256, kSlotCount = (kMaxSize + 7) / 8 + 1 }; /* indices 0..32 */

	slab::FixedAllocator* slots[kSlotCount];

	FixedAllocatorRouter() {
		for (size_t index = 0; index < kSlotCount; index++) slots[index] = NULL;
	}

	~FixedAllocatorRouter() {
		for (size_t index = 0; index < kSlotCount; index++) {
			delete slots[index];
		}
	}

	void* allocate(size_t size) {
		return instanceFor(size)->allocate();
	}

	void deallocate(size_t size, void* pointer) {
		instanceFor(size)->deallocate(pointer);
	}

private:
	/* lazy: an instance (and its eager first block) only exists once its size is used */
	slab::FixedAllocator* instanceFor(size_t size) {
		assert(size >= 1 && size <= kMaxSize);
		size_t index = (size + 7) >> 3;
		if (slots[index] == NULL) {
			slots[index] = new slab::FixedAllocator(index * 8, 1);
		}
		return slots[index];
	}
};

typedef struct BenchTarget {
	void* (*allocate)(void* context, size_t size);
	void (*deallocate)(void* context, void* pointer, size_t size);
	void* context;
} BenchTarget;

static void* mallocAllocate(void*, size_t size) { return std::malloc(size); }
static void mallocDeallocate(void*, void* pointer, size_t) { std::free(pointer); }
static void* fixedAllocate(void* context, size_t size) { return static_cast<FixedAllocatorRouter*>(context)->allocate(size); }
static void fixedDeallocate(void* context, void* pointer, size_t size) { static_cast<FixedAllocatorRouter*>(context)->deallocate(size, pointer); }
static void* slabAllocate(void* context, size_t size) { return arenaSlab_alloc(static_cast<ArenaSlabAllocator*>(context), size); }
static void slabDeallocate(void* context, void* pointer, size_t) { arenaSlab_free(static_cast<ArenaSlabAllocator*>(context), pointer); }

/* one table row: three timings, column order libc malloc / FixedAllocator / arenaSlab */
static void benchReport3(const char* name, uint64_t operations, const double* seconds) {
	printf("  %-30s %9.1f %9.1f %9.1f ns/op %8.2f %8.2f %8.2f M ops/s\n",
		name,
		seconds[0] * 1e9 / (double)operations,
		seconds[1] * 1e9 / (double)operations,
		seconds[2] * 1e9 / (double)operations,
		(double)operations / seconds[0] / 1e6,
		(double)operations / seconds[1] / 1e6,
		(double)operations / seconds[2] / 1e6);
}

/* pattern 1: hot path — one slot allocated and freed back to back */
static double benchPatternPair(BenchTarget* target, size_t size, uint64_t operations) {
	uintptr_t sink = 0;
	double start = benchNow();
	for (uint64_t index = 0; index < operations; index++) {
		void* pointer = target->allocate(target->context, size);
		if (pointer != NULL) {
			((unsigned char*)pointer)[0] = 1;
			sink ^= (uintptr_t)pointer;
		}
		target->deallocate(target->context, pointer, size);
	}
	benchSink ^= sink;
	return benchNow() - start;
}

/* pattern 2: burst — fill the whole working set, then drain it in allocation order */
static double benchPatternBurst(BenchTarget* target, size_t size, uint32_t count) {
	std::vector<void*> slots;
	slots.resize(count);
	uintptr_t sink = 0;

	double start = benchNow();
	for (uint32_t index = 0; index < count; index++) {
		void* pointer = target->allocate(target->context, size);
		slots[index] = pointer;
		if (pointer != NULL) {
			((unsigned char*)pointer)[0] = 1;
			sink ^= (uintptr_t)pointer;
		}
	}
	for (uint32_t index = 0; index < count; index++) {
		target->deallocate(target->context, slots[index], size);
	}
	benchSink ^= sink;
	return benchNow() - start;
}

/* pattern 3: churn — random slot working set, ~50/50 free/alloc hits */
static double benchPatternChurn(BenchTarget* target, size_t size, uint64_t operations) {
	static void* workingSet[BENCH_CHURN_SET];
	memset(workingSet, 0, sizeof(workingSet));
	uint64_t state = 0x9E3779B97F4A7C15ULL;
	uintptr_t sink = 0;

	double start = benchNow();
	for (uint64_t op = 0; op < operations; op++) {
		state = state * 6364136223846793005ULL + 1442695040888963407ULL;
		uint32_t slot = (uint32_t)((state >> 33) & (BENCH_CHURN_SET - 1));
		if (workingSet[slot] != NULL) {
			target->deallocate(target->context, workingSet[slot], size);
			workingSet[slot] = NULL;
		}
		else {
			void* pointer = target->allocate(target->context, size);
			if (pointer != NULL) {
				workingSet[slot] = pointer;
				((unsigned char*)pointer)[0] = 1;
				sink ^= (uintptr_t)pointer;
			}
		}
	}
	double seconds = benchNow() - start;

	for (uint32_t slot = 0; slot < BENCH_CHURN_SET; slot++) {
		if (workingSet[slot] != NULL) target->deallocate(target->context, workingSet[slot], size);
	}
	benchSink ^= sink;
	return seconds;
}

/* pattern 4: mixed — random alloc/free with a large live cap (the original objectPool workload) */
static double benchPatternMixed(BenchTarget* target, size_t size, uint64_t operations) {
	std::vector<void*> slots;
	slots.reserve(MAX_ALLOCATIONS);
	Xorshift64 rng(0x9E3779B97F4A7C15ULL); /* fixed seed: identical workload for all three */
	uintptr_t sink = 0;

	double start = benchNow();
	for (uint64_t op = 0; op < operations; op++) {
		if ((rng.next_u64() % 2 == 0 || slots.empty()) && slots.size() < MAX_ALLOCATIONS) {
			void* pointer = target->allocate(target->context, size);
			if (pointer != NULL) {
				slots.push_back(pointer);
				((unsigned char*)pointer)[0] = 1;
				sink ^= (uintptr_t)pointer;
			}
		}
		else {
			size_t index = (size_t)(((rng.next_u64() & 0xffffffffULL) * slots.size()) >> 32);
			target->deallocate(target->context, slots[index], size);
			slots[index] = slots.back();
			slots.pop_back();
		}
	}
	double seconds = benchNow() - start;

	for (size_t index = 0; index < slots.size(); index++) {
		target->deallocate(target->context, slots[index], size);
	}
	benchSink ^= sink;
	return seconds;
}

static void benchCarve(void) {
	/* dedicated instance: isolates the carve+commit growth phase */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	if (!arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT)) {
		printf("  carve bench skipped (init failed)\n");
		return;
	}
	const uint32_t total = BENCH_CARVE_ARENAS * 1022; /* payload slots per 16B arena */
	static void* pointers[BENCH_CARVE_ARENAS * 1022];

	uintptr_t sink = 0;
	double start = benchNow();
	for (uint32_t index = 0; index < total; index++) {
		void* pointer = arenaSlab_alloc(&allocator, 16);
		pointers[index] = pointer;
		if (pointer != NULL) sink ^= (uintptr_t)pointer;
	}
	benchReport("growth: alloc across 512 carves", total, benchNow() - start);

	start = benchNow();
	for (uint32_t index = 0; index < total; index++) {
		arenaSlab_free(&allocator, pointers[index]);
	}
	benchReport("free across 512 fresh arenas", total, benchNow() - start);

	arenaSlab_shutdown(&allocator);
	benchSink ^= sink;
}

static void benchTrimCycle(void) {
	/* dedicated instance: measures drop + reuse around trim; the per-cycle arena count
	 * must exceed TRIM_POOL_MAX_RESIDENT so every trim actually drops the cold tail */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	if (!arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT)) {
		printf("  trim bench skipped (init failed)\n");
		return;
	}
	enum { ARENAS = TRIM_POOL_MAX_RESIDENT + 64, SLOTS = 1022 };
	static void* slots[ARENAS * SLOTS];

	double start = benchNow();
	for (uint32_t cycle = 0; cycle < BENCH_TRIM_CYCLES; cycle++) {
		for (uint32_t index = 0; index < ARENAS * SLOTS; index++) slots[index] = arenaSlab_alloc(&allocator, 16);
		for (uint32_t index = 0; index < ARENAS * SLOTS; index++) arenaSlab_free(&allocator, slots[index]);
		(void)arenaSlab_trim(&allocator);
	}
	uint64_t operations = (uint64_t)BENCH_TRIM_CYCLES * (2 * (uint64_t)(ARENAS * SLOTS) + 1);
	benchReport("trim cycle (alloc/free/trim x3)", operations, benchNow() - start);

	arenaSlab_shutdown(&allocator);
}

/* adversarial pattern: frees cluster at the top of one arena and are re-claimed right away;
 * probes the LIFO free list (the hottest slots sit at the list head, so every claim is a
 * pure pop — the bump tail is never touched) */
static void benchRecentFree(void) {
	/* dedicated instance: isolates one 16B arena and its freelist */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	if (!arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT)) {
		printf("  recent-free bench skipped (init failed)\n");
		return;
	}
	enum { ARENA_FILL = 1022, HOT = 100, CYCLES = 200000 };
	static void* slots[ARENA_FILL];

	for (uint32_t index = 0; index < ARENA_FILL; index++) slots[index] = arenaSlab_alloc(&allocator, 16);

	uintptr_t sink = 0;
	double start = benchNow();
	for (uint32_t cycle = 0; cycle < CYCLES; cycle++) {
		for (uint32_t index = ARENA_FILL - HOT; index < ARENA_FILL; index++) {
			arenaSlab_free(&allocator, slots[index]);
		}
		for (uint32_t index = ARENA_FILL - HOT; index < ARENA_FILL; index++) {
			slots[index] = arenaSlab_alloc(&allocator, 16);
			sink ^= (uintptr_t)slots[index];
		}
	}
	uint64_t operations = (uint64_t)CYCLES * 2 * HOT;
	benchReport("recent-free reuse (top-100 of one arena)", operations, benchNow() - start);

	arenaSlab_shutdown(&allocator);
	benchSink ^= sink;
}

/* ---- use-phase bench: the layout impact of memory once it is actually used ----
 * Alloc/free are deliberately excluded (phase separated); only the traversal of live
 * objects is timed. Footprint tiers: 1MB (L2), 32MB (L3, chip-dependent), 512MB (DRAM):
 * layout differences only show up once the footprint leaves the caches. */

static uint64_t useGcd(uint64_t a, uint64_t b) {
	while (b != 0) { uint64_t t = a % b; a = b; b = t; }
	return a;
}

static double benchUseLinearRead(void** slots, size_t count, uint32_t passes) {
	uint64_t sink = 0;
	double start = benchNow();
	for (uint32_t pass = 0; pass < passes; pass++) {
		for (size_t index = 0; index < count; index++) {
			sink += *static_cast<uint64_t*>(slots[index]);
		}
	}
	benchSink ^= sink;
	return benchNow() - start;
}

static double benchUseLinearRw(void** slots, size_t count, uint32_t passes) {
	uint64_t sink = 0;
	double start = benchNow();
	for (uint32_t pass = 0; pass < passes; pass++) {
		uint64_t delta = (uint64_t)pass + 1;
		for (size_t index = 0; index < count; index++) {
			uint64_t* field = static_cast<uint64_t*>(slots[index]);
			*field += delta;
			sink ^= *field;
		}
	}
	benchSink ^= sink;
	return benchNow() - start;
}

static double benchUseGoldenWalk(void** slots, size_t count, uint32_t passes) {
	/* golden-ratio hop adjusted to be coprime with count: every slot visited exactly
	 * once per pass; no prefetcher can chase this order */
	uint64_t step = 0x9E3779B97F4A7C15ULL % count;
	while (useGcd((uint64_t)count, step) != 1) step++;
	uint64_t sink = 0;
	size_t index = 0;
	double start = benchNow();
	for (uint32_t pass = 0; pass < passes; pass++) {
		for (size_t visit = 0; visit < count; visit++) {
			sink += *static_cast<uint64_t*>(slots[index]);
			index += step;
			if (index >= count) index -= count;
		}
	}
	benchSink ^= sink;
	return benchNow() - start;
}

typedef double (*UsePatternFn)(void** slots, size_t count, uint32_t passes);

/* ---- use-phase cache control ----
 * Eviction sweep between allocators: whichever allocator ran before leaves its footprint
 * resident in L3, so later-tested allocators would start from a dirtier cache. The sweep
 * gives every allocator the same cold-L3 start. The buffer must NOT come from malloc:
 * heap-resident sweep pages would cycle the very physical pages the pointer array and
 * FixedAllocator's blocks live on. It gets a dedicated kernel-VM region instead. */
enum { SWEEP_BYTES = (size_t)512 << 20 }; /* >= 4x the largest current L3 (3D V-Cache) */

static uint8_t* sweepBase = NULL;
static bool sweepFailed = false;
static unsigned sweepPass = 0;

static void evictCaches(void) {
	if (sweepBase == NULL) {
		if (sweepFailed) return;
		uint8_t* base = NULL;
		uint8_t* reserveBase = NULL;
		size_t reserveSize = 0;
		if (osSegmentReserve(SWEEP_BYTES, &base, &reserveBase, &reserveSize) &&
			osPagesCommit(base, SWEEP_BYTES)) {
			sweepBase = base; /* intentionally never released: everything vanishes at process exit */
		}
		else {
			sweepFailed = true;
			printf("  (cache sweep unavailable, benchmark continues unswept)\n");
			return;
		}
	}
	memset(sweepBase, (int)(++sweepPass & 0xFF), SWEEP_BYTES);
}

/* one cell = alloc (untimed) -> timed traversal passes -> free (untimed), per allocator */
static void runUsePhaseCell(BenchTarget* targets, std::vector<void*>& slots, double* seconds,
	UsePatternFn pattern, const char* name, size_t size, size_t count, uint32_t passes) {
	const uint64_t operations = (uint64_t)count * passes;

	evictCaches(); /* same cold-L3 start for every allocator */

	for (int t = 0; t < 3; t++) {		
		BenchTarget* target = &targets[t];
		for (size_t index = 0; index < count; index++) {
			slots[index] = target->allocate(target->context, size);
			/* untimed init touch, like any real program initializing its objects: makes
			 * pages resident so the timed region measures steady-state use */
			*static_cast<uint8_t*>(slots[index]) = (uint8_t)index;
		}
		seconds[t] = pattern(slots.data(), count, passes);
		for (size_t index = 0; index < count; index++) {
			target->deallocate(target->context, slots[index], size);
		}
	}
	benchReport3(name, operations, seconds);
}

static void runUsePhaseBench(BenchTarget* targets) {
	static const size_t sizes[] = { 8, 16, 64, 256 };
	static const size_t tierBytes[] = { (size_t)1 << 20, (size_t)32 << 20, (size_t)512 << 20 };
	static const uint32_t tierPasses[] = { 1024, 32, 1 };
	static const char* tierNames[] = { "1MB", "32MB", "512MB" };
	char name[64];
	double seconds[3];

	printf("  -- use-phase layout impact (timed traversal only, alloc/free excluded) --\n");

	static std::vector<void*> slots;
	slots.reserve(tierBytes[2] / sizes[0]);

	for (size_t sizeIndex = 0; sizeIndex < sizeof(sizes) / sizeof(sizes[0]); sizeIndex++) {
		const size_t size = sizes[sizeIndex];
		for (size_t tier = 0; tier < sizeof(tierBytes) / sizeof(tierBytes[0]); tier++) {
			const size_t count = tierBytes[tier] / size;
			const uint32_t passes = tierPasses[tier];
			slots.resize(count);

			snprintf(name, sizeof(name), "use read %zuB %s", size, tierNames[tier]);
			runUsePhaseCell(targets, slots, seconds, benchUseLinearRead, name, size, count, passes);
			snprintf(name, sizeof(name), "use rw %zuB %s", size, tierNames[tier]);
			runUsePhaseCell(targets, slots, seconds, benchUseLinearRw, name, size, count, passes);
			snprintf(name, sizeof(name), "use walk %zuB %s", size, tierNames[tier]);
			runUsePhaseCell(targets, slots, seconds, benchUseGoldenWalk, name, size, count, passes);

			/* deliberately no arenaSlab_trim here: malloc and FixedAllocator return freed
			 * memory lazily too, and trim is a manual safe-point operation by design —
			 * calling it every cell would hand arenaSlab a syscall-driven page-return
			 * cost (and a MEM_RESET hangover) the others never pay. Freed arenas stay
			 * cached on their chains as warm spares (plus the ring), which is the
			 * matching deferred-return behavior. */
		}
	}
}

/* ---- unified comparison: three allocators under identical load patterns ---- */
static void runBenchComparison(ArenaSlabAllocator* allocator) {
	/* arenaSlab serves 16/32/64/128/256 only, so those class sizes are the common ground */
	static const size_t sizes[] = { 8, 16, 32, 64, 128, 256 };
	char name[64];
	double seconds[3];

	printf("  %-30s | ns/op: malloc / fixed / arenaSlab | M ops/s: malloc / fixed / arenaSlab\n", "pattern");

	FixedAllocatorRouter fixedRouter; /* one instance per 8B-granular size, serves all bench sizes */

	BenchTarget targets[3];
	targets[0].allocate = mallocAllocate; targets[0].deallocate = mallocDeallocate; targets[0].context = NULL;
	targets[1].allocate = fixedAllocate;  targets[1].deallocate = fixedDeallocate;  targets[1].context = &fixedRouter;
	targets[2].allocate = slabAllocate;   targets[2].deallocate = slabDeallocate;   targets[2].context = allocator;

	for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		const size_t size = sizes[index];

		snprintf(name, sizeof(name), "pair %zuB", size);
		for (int t = 0; t < 3; t++) seconds[t] = benchPatternPair(&targets[t], size, BENCH_PATTERN_OPS);
		benchReport3(name, BENCH_PATTERN_OPS, seconds);

		snprintf(name, sizeof(name), "burst %zuB (fill+drain %uk)", size, (unsigned)(BENCH_BURST_COUNT / 1000));
		for (int t = 0; t < 3; t++) seconds[t] = benchPatternBurst(&targets[t], size, BENCH_BURST_COUNT);
		benchReport3(name, (uint64_t)BENCH_BURST_COUNT * 2, seconds);

		snprintf(name, sizeof(name), "churn %zuB (random 8k slots)", size);
		for (int t = 0; t < 3; t++) seconds[t] = benchPatternChurn(&targets[t], size, BENCH_CHURN_OPS);
		benchReport3(name, BENCH_CHURN_OPS, seconds);

		snprintf(name, sizeof(name), "mixed %zuB (random, 100k cap)", size);
		for (int t = 0; t < 3; t++) seconds[t] = benchPatternMixed(&targets[t], size, BENCH_PATTERN_OPS);
		benchReport3(name, BENCH_PATTERN_OPS, seconds);
	}

	runUsePhaseBench(targets);
}

/* ---- runner: the full benchmark phase ---- */
static void runBenches(void) {
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));

	section("benchmarks: malloc vs FixedAllocator vs arenaSlab (4GB segment)");
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);
	runBenchComparison(&allocator);
	arenaSlab_shutdown(&allocator);

	printf("  -- arenaSlab-specific patterns --\n");
	benchCarve();
	benchTrimCycle();
	benchRecentFree();
	printf("  anti-optimization checksum: %llx\n", (unsigned long long)benchSink);

	/* stats dump: only compiled in with LOG_MALLOC_STATS (Debug builds) — in Release the
	 * dump is a no-op and this section just exercises the gate paths */
	section("stats dump");
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);
	void* statsPointer = arenaSlab_alloc(&allocator, 64);
	arenaSlab_free(&allocator, statsPointer);
	(void)arenaSlab_trim(&allocator, TRIM_KEEP_EMPTY_DEFAULT);
	arenaSlab_dumpStats(&allocator);
	arenaSlab_statsReset(&allocator);

	/* full-arena visibility: 255 x 64B fill one arena exactly (it leaves the chain full),
	 * the next slot opens a second partial arena — State must report full 1 */
	void* fullFill[255];
	for (uint32_t index = 0; index < 255; index++) fullFill[index] = arenaSlab_alloc(&allocator, 64);
	CHECK(fullFill[254] != NULL);
	void* partialSlot = arenaSlab_alloc(&allocator, 64);
	CHECK(partialSlot != NULL);
	arenaSlab_dumpStats(&allocator);
	arenaSlab_statsReset(&allocator);
	for (uint32_t index = 0; index < 255; index++) CHECK(arenaSlab_free(&allocator, fullFill[index]) == true);
	CHECK(arenaSlab_free(&allocator, partialSlot) == true);
	arenaSlab_shutdown(&allocator);
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0); /* crash-safe diagnostics: no lost output on exceptions */
	size_t sizes[] = { 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 192, 256 };

	bool runCorrectness = true;
	bool runThroughput = true;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--test") == 0) runThroughput = false;
		else if (strcmp(argv[i], "--bench") == 0) runCorrectness = false;
		else {
			printf("usage: slabAlloc [--test | --bench]\n"
				"  (no arg)  correctness tests + benchmarks\n"
				"  --test    correctness tests only, no throughput\n"
				"  --bench   benchmarks only\n");
			return 2;
		}
	}

	if (runCorrectness) {
		// FixedAllocator correctness tests
		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
			test_allocation_correctness(sizes[i], 100000);
			std::cout << std::endl;
		}

		// arenaSlab test suite
		runArenaSlabTests();
	}

	if (runThroughput) {
		// Benchmarks: three allocators under identical load patterns
		runBenches();
	}

	printf("== %s (%d failures) ==\n",
		testFailures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", testFailures);
	return testFailures == 0 ? 0 : 1;
}
