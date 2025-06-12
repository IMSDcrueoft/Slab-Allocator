#include <iostream>
#include <cstdlib>
#include <chrono>
#include <vector>
#include "./src/slab.hpp"

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

void test_fixed_size_allocations_and_frees(size_t fixed_size, size_t num_operations) {
    std::vector<void*> malloc_ptrs;
    std::vector<void*> slab_ptrs;
    malloc_ptrs.reserve(MAX_ALLOCATIONS);
    slab_ptrs.reserve(MAX_ALLOCATIONS);
    slab::SlabAllocator slabAlloc(fixed_size, 3);

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
            slab_ptrs.push_back(slabAlloc.allocate());
        }
        else if (!slab_ptrs.empty()) {
            int idx = rng.next_u64() % slab_ptrs.size();
            slabAlloc.deallocate(slab_ptrs[idx]);
            slab_ptrs[idx] = slab_ptrs.back();
            slab_ptrs.pop_back();
        }
    }
    end = std::chrono::high_resolution_clock::now();
    diff = end - start;
    std::cout << "[Size " << fixed_size << "] Slab:   " << diff.count() << "ms, "
        << (diff.count() / (num_operations / 1e6)) << "ms/Mops" << std::endl;

    // slabAlloc.print_stats(); // Enable if needed

    // Cleanup
    // SlabAllocator destructor will automatically release resources
}

int main() {
    size_t sizes[] = { 8, 12, 16, 20, 24, 28, 32, 40, 48, 56, 64, 80, 96, 112, 128, 192, 256, 384, 512, 768, 1024 };
    size_t num_operations = 4e6; // Increase number of operations

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        test_fixed_size_allocations_and_frees(sizes[i], num_operations);
        std::cout << std::endl;
    }

    return 0;
}