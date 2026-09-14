#include <iostream>
#include <cstdlib>
#include <chrono>
#include <vector>
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
	CHECK(allocator.initialized == false); /* failed attempts must not half-initialize */

	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MIN) == true); /* 1MB */
	arenaSlab_shutdown(&allocator);
	CHECK(allocator.initialized == false);

	/* NULL context must be safe everywhere */
	CHECK(arenaSlab_alloc(NULL, 16) == NULL);
	CHECK(arenaSlab_free(NULL, NULL) == false); /* NULL context is rejected */
	CHECK(arenaSlab_which(NULL, NULL) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(NULL, NULL) == 0);
	CHECK(arenaSlab_segmentBase(NULL) == 0);
	CHECK(arenaSlab_trim(NULL) == 0);
	arenaSlab_shutdown(NULL); /* must not crash */
	arenaSlab_statsReset(NULL);
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

/* ---- slot classes, alignment, usable size, double free ---- */
static void testClassBasics(ArenaSlabAllocator* allocator) {
	static const size_t sizes[] = { 1, 16, 17, 32, 33, 64, 65, 128, 129, 256 };
	static const size_t expectedUsable[] = { 16, 16, 32, 32, 64, 64, 128, 128, 256, 256 };
	const uint32_t count = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
	void* pointers[sizeof(sizes) / sizeof(sizes[0])];

	for (uint32_t index = 0; index < count; index++) {
		pointers[index] = arenaSlab_alloc(allocator, sizes[index]);
		CHECK(pointers[index] != NULL);
		if (pointers[index] == NULL) continue;
		CHECK(((uintptr_t)pointers[index] & 15) == 0); /* always 16B aligned */
		CHECK(arenaSlab_usable_size(allocator, pointers[index]) == expectedUsable[index]);
		CHECK(arenaSlab_which(allocator, pointers[index]) == SLAB_LAYER_SMALL);
		memset(pointers[index], (int)(index + 1), expectedUsable[index]); /* writable */
	}

	/* size 0 -> 16B class; oversize -> NULL */
	void* zero = arenaSlab_alloc(allocator, 0);
	CHECK(zero != NULL);
	CHECK(arenaSlab_usable_size(allocator, zero) == 16);
	CHECK(arenaSlab_free(allocator, zero) == true);
	CHECK(arenaSlab_alloc(allocator, 257) == NULL);
	CHECK(arenaSlab_alloc(allocator, (size_t)-1) == NULL);

	for (uint32_t index = 0; index < count; index++) {
		CHECK(arenaSlab_free(allocator, pointers[index]) == true);
		CHECK(arenaSlab_free(allocator, pointers[index]) == false); /* double free rejected */
	}
}

/* ---- many arenas per class: carve, chain walk, partial reuse ---- */
static void testChainReuse(ArenaSlabAllocator* allocator) {
	section("chain reuse (3 arenas x 32B class)");
	/* 24B lands in the 32B class; one 32B arena holds 509 payload slots, so 1500 spans 3 arenas */
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
	CHECK(arenaSlab_free(allocator, churnPointers[0]) == false); /* everything already free */
}

/* ---- trim drops empty arenas; dropped arenas must come back usable ---- */
static void testTrimAndReuse(ArenaSlabAllocator* allocator) {
	section("trim + dropped-arena reuse");
	/* 16B arena: 1024 slots - 10 header slots = 1014 payload slots */
	enum { ARENAS = 5, SLOTS_PER_ARENA = 1014 };
	void* slots[ARENAS * SLOTS_PER_ARENA];
	const uint32_t count = ARENAS * SLOTS_PER_ARENA;

	/* per-arena dropped size: everything past the first page; 0 on 16KB-page systems */
	uint32_t pageSize = osPageSize();
	size_t dropLength = (pageSize < ARENA_SIZE_SMALL) ? (size_t)(ARENA_SIZE_SMALL - pageSize) : 0;

	/* baseline: empty arenas left over from the earlier tests must not skew the count below */
	(void)arenaSlab_trim(allocator);

	for (uint32_t index = 0; index < count; index++) {
		slots[index] = arenaSlab_alloc(allocator, 16);
		CHECK(slots[index] != NULL);
	}
	for (uint32_t index = 0; index < count; index++) {
		CHECK(arenaSlab_free(allocator, slots[index]) == true);
	}

	size_t dropped = arenaSlab_trim(allocator);
	printf("  trim dropped %llu bytes\n", (unsigned long long)dropped);
	CHECK(dropped == (size_t)ARENAS * dropLength); /* exactly our 5 arenas, nothing else */

	/* the dropped arenas must return through the reuse path, fully writable */
	for (uint32_t index = 0; index < count; index++) {
		slots[index] = arenaSlab_alloc(allocator, 16);
		CHECK(slots[index] != NULL);
	}
	for (uint32_t index = 0; index < count; index++) {
		memset(slots[index], 0xCD, 16);
	}
	for (uint32_t index = 0; index < count; index++) {
		CHECK(arenaSlab_free(allocator, slots[index]) == true);
	}
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
	CHECK(arenaSlab_free(allocator, small) == false); /* realloc already freed it */

	CHECK(arenaSlab_realloc(allocator, bigger, 32) == bigger); /* shrink keeps the pointer */

	unsigned char* fresh = static_cast<unsigned char*>(arenaSlab_realloc(allocator, NULL, 32)); /* acts as alloc */
	CHECK(fresh != NULL);
	CHECK(arenaSlab_usable_size(allocator, fresh) == 32);

	CHECK(arenaSlab_realloc(allocator, bigger, 0) == NULL); /* acts as free */
	CHECK(arenaSlab_free(allocator, bigger) == false);

	CHECK(arenaSlab_realloc(allocator, fresh, 4096) == NULL); /* oversize: freed + NULL */
	CHECK(arenaSlab_free(allocator, fresh) == false);
}

/* ---- foreign pointers must be rejected without touching the allocator ---- */
static void testForeignPointers(ArenaSlabAllocator* allocator) {
	section("foreign pointers");
	int stackValue = 0;
	CHECK(arenaSlab_which(allocator, &stackValue) == SLAB_LAYER_NONE);
	CHECK(arenaSlab_usable_size(allocator, &stackValue) == 0);
	CHECK(arenaSlab_free(allocator, &stackValue) == false);
	CHECK(arenaSlab_free(allocator, NULL) == true);
}

/* ---- single-segment semantics: 1MB, no growth, exhaustion, revive ---- */
static void testSingleSegmentCapacity(void) {
	section("1MB segment capacity");
	/* 1MB = 64 arenas of 16KB; 16B arena holds 1014 slots -> 64 * 1014 = 64896 slots max */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_MIN) == true);

	enum { TOTAL_16B_SLOTS = 64 * 1014 };
	static void* slots[TOTAL_16B_SLOTS];
	uint32_t allocated = 0;
	for (;;) {
		void* slot = arenaSlab_alloc(&allocator, 16);
		if (slot == NULL) break;
		slots[allocated++] = slot;
	}
	printf("  1MB segment held %u x 16B slots\n", allocated);
	CHECK(allocated == 64 * 1014);

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
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);
	testClassBasics(&allocator);
	testChainReuse(&allocator);
	testChurn(&allocator);
	testTrimAndReuse(&allocator);
	testRealloc(&allocator);
	testForeignPointers(&allocator);
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
typedef struct BenchTarget {
	void* (*allocate)(void* context, size_t size);
	void (*deallocate)(void* context, void* pointer);
	void* context;
} BenchTarget;

static void* mallocAllocate(void*, size_t size) { return std::malloc(size); }
static void mallocDeallocate(void*, void* pointer) { std::free(pointer); }
static void* fixedAllocate(void* context, size_t) { return static_cast<slab::FixedAllocator*>(context)->allocate(); }
static void fixedDeallocate(void* context, void* pointer) { static_cast<slab::FixedAllocator*>(context)->deallocate(pointer); }
static void* slabAllocate(void* context, size_t size) { return arenaSlab_alloc(static_cast<ArenaSlabAllocator*>(context), size); }
static void slabDeallocate(void* context, void* pointer) { arenaSlab_free(static_cast<ArenaSlabAllocator*>(context), pointer); }

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
		target->deallocate(target->context, pointer);
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
		target->deallocate(target->context, slots[index]);
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
			target->deallocate(target->context, workingSet[slot]);
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
		if (workingSet[slot] != NULL) target->deallocate(target->context, workingSet[slot]);
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
			target->deallocate(target->context, slots[index]);
			slots[index] = slots.back();
			slots.pop_back();
		}
	}
	double seconds = benchNow() - start;

	for (size_t index = 0; index < slots.size(); index++) {
		target->deallocate(target->context, slots[index]);
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
	const uint32_t total = BENCH_CARVE_ARENAS * 1014; /* payload slots per 16B arena */
	static void* pointers[BENCH_CARVE_ARENAS * 1014];

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
	/* dedicated instance: measures drop + reuse around trim */
	ArenaSlabAllocator allocator;
	memset(&allocator, 0, sizeof(allocator));
	if (!arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT)) {
		printf("  trim bench skipped (init failed)\n");
		return;
	}
	enum { SLOTS = 5 * 1014 }; /* 5 arenas of 16B slots */
	void* slots[SLOTS];

	double start = benchNow();
	for (uint32_t cycle = 0; cycle < BENCH_TRIM_CYCLES; cycle++) {
		for (uint32_t index = 0; index < SLOTS; index++) slots[index] = arenaSlab_alloc(&allocator, 16);
		for (uint32_t index = 0; index < SLOTS; index++) arenaSlab_free(&allocator, slots[index]);
		(void)arenaSlab_trim(&allocator);
	}
	uint64_t operations = (uint64_t)BENCH_TRIM_CYCLES * (2 * (uint64_t)SLOTS + 1);
	benchReport("trim cycle (alloc/free/trim x3)", operations, benchNow() - start);

	arenaSlab_shutdown(&allocator);
}

/* ---- unified comparison: three allocators under identical load patterns ---- */
static void runBenchComparison(ArenaSlabAllocator* allocator) {
	/* arenaSlab serves 16/32/64/128/256 only, so those class sizes are the common ground */
	static const size_t sizes[] = { 16, 32, 64, 128, 256 };
	char name[64];
	double seconds[3];

	printf("  %-30s | ns/op: malloc / fixed / arenaSlab | M ops/s: malloc / fixed / arenaSlab\n", "pattern");

	BenchTarget targets[3];
	targets[0].allocate = mallocAllocate; targets[0].deallocate = mallocDeallocate; targets[0].context = NULL;
	targets[2].allocate = slabAllocate;   targets[2].deallocate = slabDeallocate;   targets[2].context = allocator;

	for (size_t index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		const size_t size = sizes[index];
		slab::FixedAllocator fixed(size, 1);
		targets[1].allocate = fixedAllocate;
		targets[1].deallocate = fixedDeallocate;
		targets[1].context = &fixed;

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
	printf("  anti-optimization checksum: %llx\n", (unsigned long long)benchSink);

	/* stats dump: arena_slab.h always defines LOG_MALLOC_STATS, so the stats API is always present */
	section("stats dump");
	CHECK(arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT) == true);
	void* statsPointer = arenaSlab_alloc(&allocator, 64);
	arenaSlab_free(&allocator, statsPointer);
	(void)arenaSlab_trim(&allocator);
	arenaSlab_dumpStats(&allocator);
	arenaSlab_statsReset(&allocator);
	arenaSlab_shutdown(&allocator);
}

int main() {
	size_t sizes[] = { 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 192, 256 };

	// FixedAllocator correctness tests
	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		test_allocation_correctness(sizes[i], 100000);
		std::cout << std::endl;
	}

	// arenaSlab test suite
	runArenaSlabTests();

	// Benchmarks: three allocators under identical load patterns
	runBenches();

	printf("== %s (%d failures) ==\n",
		testFailures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED", testFailures);
	return testFailures == 0 ? 0 : 1;
}
