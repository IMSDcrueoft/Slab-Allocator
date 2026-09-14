# Slab Allocator
A high-performance single-thread memory allocator using slab allocation technique, designed for efficient fixed-size memory allocations.

## Features

- **Api**: Provides both C and CPP interfaces
- **Slab-based allocation**: Organizes memory into slabs containing fixed-size units
- **Fast allocation/deallocation**: O(1) average case performance for both operations
- **Memory efficiency**: Minimizes fragmentation through fixed-size blocks
- **Automatic slab management**: Creates new slabs when needed and reclaims empty ones
- **Debug support**: Includes extensive error checking and statistics reporting

## License

MIT License

See [LICENSE](LICENSE) file for full license text.

## Implementation Details

### Key Components

1. **SlabUnit**:
   - Represents an individual allocation unit
   - Contains metadata (index and offset) and payload space
   - Provides conversion between unit and payload pointers

2. **SlabBlock**:
   - Manages a block of 64 units
   - Uses bitmaps to track free/allocated units
   - Implements unit allocation and block creation/destruction
   - Maintains linked list connections

3. **SlabAllocator**:
   - Main allocator class managing multiple slabs
   - Handles memory allocation and deallocation requests
   - Manages slab creation and reclamation
   - Maintains cache optimization

### Technical Specifications

- Fixed unit sizes (configurable at creation, up to 1024 bytes)
- 64 units per slab block
- Bitmap-based free unit tracking (64-bit)
- Automatic slab creation when needed
- Intelligent slab reclamation when empty
- Reduce traversal overhead
- Memory alignment to 8-byte boundaries

## Usage

### Basic Operations

```cpp
#include "./src/objectPool.hpp"
// Create an allocator for 256-byte units
slab::FixedAllocator allocator(256);

// Allocate memory
void* ptr1 = allocator.allocate();
void* ptr2 = allocator.allocate();

// Use the memory
int* data1 = static_cast<int*>(ptr1);
*data1 = 42;

// Deallocate memory
allocator.deallocate(ptr1);
allocator.deallocate(ptr2);

// Get statistics
std::cout << "Total slabs: " << allocator.total() << std::endl;
std::cout << "Reserved slabs: " << allocator.reserved() << std::endl;

// Print detailed statistics
allocator.print_stats();
```

## Performance Considerations

- The allocator maintains a "full" and a "work" list to minimize traversal
- Empty slabs beyond the reserved limit are automatically destroyed

## arenaSlab — Single-Segment Small-Object Allocator

`src/arena_slab.h` / `src/arena_slab.c` implement a second, independent allocator tuned for
small objects (≤ 256 bytes). It sits on top of raw kernel VM primitives
(`src/palloc_os.c`: `mmap`/`mprotect`/`madvise` on POSIX, `VirtualAlloc`/`VirtualFree` on
Windows) — no libc allocator is involved. The bitmap helpers (`src/bits.h`, `src/bits.c`)
are taken from Slab-Allocator (MIT License, Copyright (c) 2026 IMSDcrueoft).

### Design Highlights

- **Single segment**: one virtual address reservation per allocator, sized `1 << exponent`
  (valid range 1MB … 1TB; the default instance uses 4GB). There is **no automatic growth**:
  once the segment is exhausted, `alloc` returns `NULL` until `free`/`trim` reclaim space.
- **5 slot classes** (16/32/64/128/256 bytes) served from 16KB slab arenas that are carved
  and committed on demand. Requests larger than 256 bytes return `NULL` (caller's responsibility).
- **Deterministic O(1) free path**: ownership is resolved via arena headers plus a segment
  range check; allocation scans a bitmap (`bit = 1` free) with `ctz`, lowest slot first.
- **16-byte aligned** returned pointers; `arenaSlab_usable_size` reports the slot class size.
  Size 0 is treated as 16; double frees and foreign pointers are rejected.
- **Lazy return**: `arenaSlab_trim` drops the pages of empty arenas back to the OS
  (metadata and the header page are always kept). It is never called automatically —
  call it from safe points.
- **Single-threaded, lock-free** by design.
- **Optional statistics**: build with `LOG_MALLOC_STATS` to enable counters and
  `arenaSlab_dumpStats` / `arenaSlab_statsReset`.

### Usage

```c
#include "src/arena_slab.h"

/* dedicated instance: reservation size = 1 << exponent */
ArenaSlabAllocator allocator;
if (arenaSlab_init(&allocator, SEGMENT_SIZE_EXPONENT_DEFAULT)) {   /* 4GB */
    void* p = arenaSlab_alloc(&allocator, 200);       /* served from the 256B class */
    size_t usable = arenaSlab_usable_size(&allocator, p);

    p = arenaSlab_realloc(&allocator, p, 128);        /* small-object realloc */
    arenaSlab_free(&allocator, p);
    arenaSlab_trim(&allocator);                       /* return empty arena pages */
    arenaSlab_shutdown(&allocator);                   /* release the reservation */
}

/* or the documented global default instance (4GB) */
arenaSlab_init(&arenaSlabDefault, SEGMENT_SIZE_EXPONENT_DEFAULT);
```

### Trade-offs

The three allocators in this repo make fundamentally different trade-offs and are not
drop-in replacements for one another. `main.cpp` runs them through the same load patterns,
but the results should be read with each design's scope in mind:

- **libc malloc**: general-purpose — any size, thread-safe, reclaims memory on its own
  terms. Pays for that generality on the hot path and always involves the CRT heap.
- **FixedAllocator** (`objectPool.hpp`): one fixed unit size with unbounded growth through
  the heap. Simplest and fastest hot path; the right tool when a single hot object type
  dominates.
- **arenaSlab**: an up-front bounded reservation with **no automatic growth**, ≤256 bytes
  only, kernel-VM only, deterministic O(1) free. Trades virtual address space and manual
  `trim` calls for predictability, full-page reclamation and isolation from the libc
  allocator.