#include <iostream>
#include <cstdlib>
#include <chrono>
#include <vector>
#include "./src/objectPool.hpp"

// Define the maximum number of allocations
#define MAX_ALLOCATIONS 100000

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
				goto cleanup;
			}

			// check ptr is not already in our list
			for (size_t j = 0; j < ptrs.size(); ++j) {
				if (ptrs[j] == ptr) {
					std::cerr << "test_allocation_correctness: duplicate allocation detected at operation " << i << std::endl;
					goto cleanup;
				}
			}

			// check ptr alignment (should be at least 8-byte aligned)
			if ((reinterpret_cast<uintptr_t>(ptr) & 7) != 0) {
				std::cerr << "test_allocation_correctness: alignment error at operation " << i << std::endl;
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
				goto cleanup;
			}

			// verify pattern before free
			uint8_t* bytes = static_cast<uint8_t*>(ptrs[idx]);
			for (size_t j = 0; j < fixed_size; ++j) {
				if (bytes[j] != 0xAA) {
					std::cerr << "test_allocation_correctness: memory corruption detected at operation " << i << ", byte " << j << std::endl;
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

void test_fixed_size_allocations_and_frees(size_t fixed_size, size_t num_operations) {
	std::vector<void*> malloc_ptrs;
	std::vector<void*> slab_ptrs;
	malloc_ptrs.reserve(MAX_ALLOCATIONS);
	slab_ptrs.reserve(MAX_ALLOCATIONS);
	slab::FixedAllocator alloc(fixed_size, 1);

	uint64_t seed = static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
	Xorshift64 rng(seed);

	// Malloc test
	auto start = std::chrono::high_resolution_clock::now();
	for (size_t i = 0; i < num_operations; ++i) {
		if ((rng.next_u64() % 2 == 0 || malloc_ptrs.empty()) && malloc_ptrs.size() < MAX_ALLOCATIONS) {
			malloc_ptrs.push_back(std::malloc(fixed_size));
		}
		else if (!malloc_ptrs.empty()) {
			int idx = rng.next_u64() % malloc_ptrs.size();
			std::free(malloc_ptrs[idx]);
			malloc_ptrs[idx] = malloc_ptrs.back();
			malloc_ptrs.pop_back();
		}
	}
	auto end = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> diff = end - start;
	std::cout << "[Size " << fixed_size << "] Malloc: " << diff.count() << "ms, "
		<< (diff.count() / (num_operations / 1e6)) << "ms/Mops" << std::endl;

	// Cleanup
	for (void* ptr : malloc_ptrs) {
		if (ptr) std::free(ptr);
	}

	// Slab test (using slab.hpp implementation)
	rng.reseed(seed);

	start = std::chrono::high_resolution_clock::now();
	for (size_t i = 0; i < num_operations; ++i) {
		if ((rng.next_u64() % 2 == 0 || slab_ptrs.empty()) && slab_ptrs.size() < MAX_ALLOCATIONS) {
			slab_ptrs.push_back(alloc.allocate());
		}
		else if (!slab_ptrs.empty()) {
			int idx = rng.next_u64() % slab_ptrs.size();
			alloc.deallocate(slab_ptrs[idx]);
			slab_ptrs[idx] = slab_ptrs.back();
			slab_ptrs.pop_back();
		}
	}
	end = std::chrono::high_resolution_clock::now();
	diff = end - start;
	std::cout << "[Size " << fixed_size << "] Slab:   " << diff.count() << "ms, "
		<< (diff.count() / (num_operations / 1e6)) << "ms/Mops" << std::endl;

	// alloc.print_stats(); // Enable if needed
}

int main() {
	size_t sizes[] = { 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 192, 256, 384, 512, 768, 1024 };
	size_t num_operations = 4e6; // Increase number of operations

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		test_allocation_correctness(sizes[i], 1e5);
		std::cout << std::endl;
	}

	for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
		test_fixed_size_allocations_and_frees(sizes[i], num_operations);
		std::cout << std::endl;
	}

	return 0;
}