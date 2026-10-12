/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
 *
 *
 * bumpArena — LIFO bump allocator implementation (palloc_os backing)
 */
#include "bumpArena.h"
#include "palloc_os.h"

static size_t bumpAlignUp(size_t x, size_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

static bool bumpIsPow2(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

bool bumpArena_init(BumpArena *arena, size_t segmentBytes, bool aligned, size_t alignBytes, size_t commitChunkSize) {
    if (arena == NULL || segmentBytes == 0) return false;
    if (aligned && !bumpIsPow2(segmentBytes)) return false;
    if (alignBytes < BUMP_MIN_ALIGN) alignBytes = BUMP_MIN_ALIGN;
    if (!bumpIsPow2(alignBytes)) return false;
    if (alignBytes > UINT32_MAX) return false;   /* must fit the stored field */

    uint8_t *base = NULL, *reserveBase = NULL;
    size_t reserveSize = 0;
    if (!osSegmentReserveEx((uint64_t)segmentBytes, aligned, false, &base, &reserveBase, &reserveSize)) {
        return false;
    }

    arena->base = base;
    arena->reserveBase = reserveBase;
    arena->reserveSize = reserveSize;
    arena->capacity = segmentBytes;
    arena->commitTop = 0;
    arena->dropFrom = 0;   /* dropFrom >= dropTop => no dropped tail */
    arena->dropTop = 0;
    arena->liveTop = 0;
    arena->peak = 0;
    arena->page = osPageSize();
    if (arena->page == 0) arena->page = 4096;
    arena->alignBytes = (uint32_t)alignBytes;

    if (commitChunkSize == 0) commitChunkSize = BUMP_COMMIT_CHUNK;
    if (commitChunkSize > segmentBytes) commitChunkSize = segmentBytes;
    if (commitChunkSize < (size_t)arena->page) commitChunkSize = (size_t)arena->page;
    commitChunkSize = bumpAlignUp(commitChunkSize, (size_t)arena->page);  /* keep commitTop page aligned */
    if (commitChunkSize > segmentBytes) commitChunkSize = segmentBytes;
    arena->commitChunkSize = commitChunkSize;
    return true;
}

void bumpArena_destroy(BumpArena *arena) {
    if (arena == NULL || arena->base == NULL) return;
    osSegmentRelease(arena->reserveBase, arena->reserveSize);
    arena->base = NULL;
    arena->capacity = arena->commitTop = arena->liveTop = arena->peak = 0;
    arena->dropFrom = arena->dropTop = 0;
}

/* Commit up to `end`, reactivating externally dropped pages along the way. */
static bool bumpEnsure(BumpArena *arena, size_t end) {
    const size_t page = arena->page;
    const size_t want = bumpAlignUp(end, page);

    if (want > arena->commitTop) {
        size_t target = arena->commitTop + arena->commitChunkSize;
        if (target < want) target = want;
        if (target > arena->capacity) target = arena->capacity;
        if (!osPagesRecommit(arena->base + arena->commitTop, target - arena->commitTop)) return false;
        arena->commitTop = target;
    }

    if (arena->dropFrom < arena->dropTop) {
        size_t b = bumpAlignUp(end, page);
        if (b > arena->dropTop) b = arena->dropTop;
        if (b > arena->dropFrom) {
            if (!osPagesRecommit(arena->base + arena->dropFrom, b - arena->dropFrom)) return false;
            arena->dropFrom = b;
            if (arena->dropFrom >= arena->dropTop) arena->dropFrom = arena->dropTop = 0;
        }
    }
    return true;
}

void *bumpArena_alloc(BumpArena *arena, size_t bytes) {
    if (arena == NULL || bytes == 0) return NULL;

    size_t off = bumpAlignUp(arena->liveTop, arena->alignBytes);
    if (off < arena->liveTop) return NULL;    /* overflow */
    size_t end = off + bytes;
    if (end < off) return NULL;               /* overflow */
    if (end > arena->capacity) return NULL;   /* capacity exceeded */

    if (!bumpEnsure(arena, end)) return NULL;

    arena->liveTop = end;
    if (end > arena->peak) arena->peak = end;
    return arena->base + off;
}

size_t bumpArena_mark(const BumpArena *arena) {
    return arena ? arena->liveTop : 0;
}

void bumpArena_rewind(BumpArena *arena, size_t mark) {
    if (arena == NULL) return;
    if (mark <= arena->liveTop) arena->liveTop = mark;
}

bool bumpArena_shrink(BumpArena *arena, const void *ptr) {
    if (arena == NULL || arena->base == NULL || ptr == NULL) return false;
    uintptr_t b = (uintptr_t)arena->base;
    uintptr_t x = (uintptr_t)ptr;

    if (x < b || x > b + arena->liveTop) return false;   /* outside the live range */

    size_t off = (size_t)(x - b);
    if (arena->alignBytes == 0 || (off & (arena->alignBytes - 1)) != 0) return false;  /* not an allocation boundary */

    arena->liveTop = off;
    return true;
}

size_t bumpArena_used(const BumpArena *arena) {
    return arena ? arena->liveTop : 0;
}

size_t bumpArena_capacity(const BumpArena *arena) {
    return arena ? arena->capacity : 0;
}

size_t bumpArena_committed(const BumpArena *arena) {
    return arena ? arena->commitTop : 0;
}

bool bumpArena_owns(const BumpArena *arena, const void *ptr) {
    if (arena == NULL || arena->base == NULL || ptr == NULL) return false;
    uintptr_t x = (uintptr_t)ptr;
    uintptr_t b = (uintptr_t)arena->base;
    return x >= b && x < b + arena->capacity;
}
