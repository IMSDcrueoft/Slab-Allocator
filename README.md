# memAllocator

A single-threaded memory allocator toolkit — not one allocator, but a collection of
independently usable components built around a shared kernel-VM primitive layer. Each
component makes a different trade-off; pick the one that fits the workload instead of
forcing one general-purpose allocator everywhere.

## Components

| Component | Files | Scope | Best for |
|---|---|---|---|
| **slab** (`FixedAllocator` / `ObjectPool<T>`) | `src/slab.c`, `src/slab.h`, `src/objectPool.hpp` | one fixed unit size per instance, heap-backed | a single hot object type |
| **arenaSlab** | `src/arena_slab.c`, `src/arena_slab.h` | ≤256B slot classes inside one bounded VA segment | many small objects, deterministic O(1) free, page-level reclamation |
| **arenaBump** | `src/arenaBump.c`, `src/arenaBump.h` | LIFO scoped scratch space over one VA segment | parsing, compilation, frame-local temporaries |
| **palloc_os** | `src/palloc_os.c`, `src/palloc_os.h` | raw kernel VM primitives | the backing layer of the above — or your own allocators |
| **bits** | `src/bits.c`, `src/bits.h` | ctz/clz/popcnt helpers | shared by the allocators |

All components are single-threaded and lock-free by design. The C components expose
`extern "C"` interfaces. `main.cpp` drives the whole toolkit through one correctness test
suite and one benchmark harness (`memAllocator.exe [--test | --bench]`).

## License

MIT License

See [LICENSE](LICENSE) file for full license text.

---

## slab — FixedAllocator / ObjectPool (C++)

The original slab allocator: memory is organized into slabs of fixed-size units, tracked
with 64-bit bitmaps. The C++ wrapper (`objectPool.hpp`) comes in two flavors:

- `slab::FixedAllocator` — raw fixed-size units, for advanced users who manage
  construction/destruction themselves.
- `slab::ObjectPool<T>` — typed pool that constructs/destructs `T` in place
  (`sizeof(T)` is the unit size).

### Implementation Details

1. **SlabUnit**: an individual allocation unit; carries metadata (index and offset) plus
   payload; converts between unit and payload pointers.
2. **SlabBlock**: manages a block of 64 units via a 64-bit free bitmap; implements unit
   allocation and block creation/destruction; maintains list connections.
3. **SlabAllocator**: the main allocator class; routes alloc/free requests, creates and
   reclaims slabs, keeps a cache of hot blocks.

### Technical Specifications

- Fixed unit sizes (configurable at creation, up to 1024 bytes)
- 64 units per slab block, bitmap-based free tracking (64-bit)
- 8-byte alignment; O(1) average-case alloc/free
- "full" + "work" lists minimize traversal; empty slabs beyond the reserved limit are
  destroyed automatically
- Unbounded growth through the pluggable backend (the C++ wrapper uses the heap)

### Usage

```cpp
#include "./src/objectPool.hpp"

// one allocator per unit size (reserved_limit = warm spare slabs kept after free)
slab::FixedAllocator allocator(256);

void* p1 = allocator.allocate();
void* p2 = allocator.allocate();
allocator.deallocate(p1);
allocator.deallocate(p2);

// typed pool: constructs/destructs T in place
slab::ObjectPool<MyType> pool;
MyType* obj = pool.allocate(1, 2, 3);   /* forwards constructor args */
pool.deallocate(obj);
```

---

## arenaSlab — Single-Segment Small-Object Allocator

`src/arena_slab.h` / `src/arena_slab.c` implement a second, independent allocator tuned for
small objects (≤ 256 bytes). It sits on top of raw kernel VM primitives
(`src/palloc_os.c`: `mmap`/`mprotect`/`madvise` on POSIX, `VirtualAlloc`/`VirtualFree` on
Windows) — no libc allocator is involved. The bit-manipulation helpers (`src/bits.h`,
`src/bits.c`: ctz/clz/popcnt) are shared.

### Design Highlights

- **Single segment**: one virtual address reservation per allocator, sized `1 << exponent`
  (valid range 1MB … 1TB; the default instance uses 4GB). There is **no automatic growth**:
  once the segment is exhausted, `alloc` returns `NULL` until `free`/`trim` reclaim space.
- **6 slot classes** (8/16/32/64/128/256 bytes) served from 16KB slab arenas that are carved
  and committed on demand. Requests larger than 256 bytes return `NULL` (caller's responsibility).
- **Deterministic O(1) free path**: ownership is resolved via arena headers plus a segment
  range check; each arena serves allocation bump-style from its never-used tail and reuses
  freed slots through a LIFO free list (next pointer stored in the slot itself, head in the
  header) — no scan, no threading pass, O(1) always.
- **Aligned** returned pointers: 8-byte for the 8B class, 16-byte for every other class;
  `arenaSlab_usable_size` reports the slot class size. Size 0 is treated as 8; foreign
  pointers are rejected. Double frees are **not** detected
  (freed slot memory doubles as freelist state — writing into a freed slot is undefined).
- **Lazy return, two-phase**: `free` keeps the first `CLASS_SPARE_KEEP` (2) emptied arenas
  of a class linked as warm spares — the pair pattern re-claims them straight off the chain
  head — and parks every beyond-quota empty into a cross-class ring (O(1), pages stay
  committed, revival is free); `arenaSlab_trim` returns the pages of the ring's coldest
  shells beyond a 4MB water mark to the OS (the header page is always kept). It is never
  called automatically — call it from safe points.
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

---

## arenaBump — LIFO Bump Allocator

`src/arenaBump.h` / `src/arenaBump.c` implement a pointer-bump allocator over a `palloc_os`
segment: allocate by moving a cursor, free in bulk by rewinding it. Ideal for scoped,
short-lived memory where everything dies together (parsers, compilers, per-frame work).

### Design Highlights

- **VA reserved once, never moves**; memory is committed lazily in `commitChunkSize` steps
  (default 16KB, rounded to page size), so capacity costs virtual address space only.
- **O(1) alloc**: alignment (configurable, 2^n, ≥ 8) + capacity check + cursor bump.
- **LIFO release**: `mark`/`rewind` restores a saved cursor; `shrink(ptr)` makes any
  allocation boundary the new top. There is no per-object free by design.
- **Page trimming is the caller's job**: mark the dead tail with
  `osPagesDrop` + `dropFrom/dropTop`; the arena re-commits those pages lazily on the next
  touching allocation.
- **Failure semantics**: `alloc` returns `NULL` on overflow, capacity exceeded, or commit
  failure; `shrink` rejects pointers outside `[base, base+used)` or off the alignment grid
  (arena unchanged).
- Query helpers: `used` / `capacity` / `committed` / `owns`.

### Usage

```c
#include "src/arenaBump.h"

ArenaBumpAllocator arena;
/* 1GB reservation, page-aligned base, 16-byte allocations, default commit chunks */
if (arenaBump_init(&arena, (size_t)1 << 30, false, 16, 0)) {
    size_t mark = arenaBump_mark(&arena);

    void* scratch = arenaBump_alloc(&arena, 4096);
    /* ... use scratch ... */

    arenaBump_rewind(&arena, mark);   /* bulk-free everything since mark */
    /* or: arenaBump_shrink(&arena, ptr); to trim back to an allocation boundary */

    arenaBump_destroy(&arena);        /* release the reservation */
}
```

---

## palloc_os — Kernel VM Primitives

`src/palloc_os.h` / `src/palloc_os.c` expose raw reservation / commit / drop / protect
primitives and nothing else — no allocator logic. Everything above is built on this layer.

- **Segments**: `osSegmentReserve` (power-of-two aligned reservation), `osSegmentReserveEx`
  (plain page-aligned optional, `executable` flag for JIT-ready memory — required at
  reserve time for Apple Silicon `MAP_JIT`), `osSegmentRelease`.
- **Pages**: `osPagesCommit` (RW), `osPagesDrop` (macOS purgeable / Linux `MADV_DONTNEED` /
  Windows `MEM_RESET`), `osPagesReuse`, `osPagesRecommit` (encapsulates the macOS
  reusable/reuse dance), `osPagesProtect` (NONE / RW / RX / RWX).
- **W^X / code pages**: `osPagesProtect` grants EXEC via `VirtualProtect`/`mprotect`; on
  Apple Silicon the per-thread write toggle (`pthread_jit_write_protect_np`) is handled
  inside — write-capable protections release it, read-only ones re-arm it. Permissions are
  not sticky: commit-family calls reset pages to RW, re-apply EXEC afterwards.
- **Standalone pages**: `osPageAlloc` / `osPageFree` (one committed OS page, independent of
  any segment), `osPageSize` (runtime page size: 16KB macOS arm64, 4KB x86-64, Windows
  follows the system).
- Platform backends: macOS/Linux `mmap` + `mprotect` + `madvise`; Windows
  `VirtualAlloc` / `VirtualProtect` / `VirtualFree`.

---

## Trade-offs

The allocators in this repo make fundamentally different trade-offs and are not drop-in
replacements for one another. `main.cpp` runs them through the same load patterns, but the
results should be read with each design's scope in mind:

- **libc malloc**: general-purpose — any size, thread-safe, reclaims memory on its own
  terms. Pays for that generality on the hot path and always involves the CRT heap.
- **FixedAllocator** (`objectPool.hpp`): one fixed unit size with unbounded growth through
  the heap. Simplest and fastest hot path; the right tool when a single hot object type
  dominates.
- **arenaSlab**: an up-front bounded reservation with **no automatic growth**, ≤256 bytes
  only, kernel-VM only, deterministic O(1) free. Trades virtual address space and manual
  `trim` calls for predictability, full-page reclamation and isolation from the libc
  allocator.
- **arenaBump**: the cheapest possible allocation (cursor bump) with **all-or-nothing**
  LIFO lifetime. Per-object frees do not exist; memory is only returned via
  rewind/shrink and explicit page drops. The right tool when allocations share one scope.
