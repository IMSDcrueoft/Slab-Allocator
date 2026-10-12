/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
 *
 *
 * bumpArena — LIFO bump allocator over palloc_os segments.
 *
 * VA is reserved once and never moves; memory is committed lazily in
 * commitChunkSize steps. Release is tail-only (mark/rewind/shrink). Page trimming
 * is the caller's job: set dropFrom/dropTop + osPagesDrop, the arena re-commits
 * those pages lazily on the next touching allocation.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BUMP_COMMIT_CHUNK = 64 * 1024,  /* default commitChunkSize   */
    BUMP_MIN_ALIGN    = 8           /* floor for alignBytes      */
};

typedef struct BumpArena {
    uint8_t *base;            /* allocation base                          */
    uint8_t *reserveBase;     /* pass to osSegmentRelease                 */
    size_t   reserveSize;     /* pass to osSegmentRelease                 */
    size_t   capacity;        /* usable bytes                             */
    size_t   commitTop;       /* committed watermark, page aligned        */
    size_t   dropFrom;        /* externally dropped tail; empty if >= dropTop */
    size_t   dropTop;         /*                                          */
    size_t   liveTop;         /* bump cursor                              */
    size_t   peak;            /* liveTop high-water mark                  */
    uint32_t page;            /* OS page size                             */
    uint32_t alignBytes;      /* allocation alignment, 2^n, >= BUMP_MIN_ALIGN */
    size_t   commitChunkSize; /* commit step, page multiple               */
} BumpArena;

/*
 * segmentBytes    : reservation size; must be 2^n when aligned
 * aligned         : base aligned to segmentBytes, else page aligned
 * alignBytes      : alignment of every allocation; 2^n, raised to BUMP_MIN_ALIGN
 * commitChunkSize : commit step; 0 => BUMP_COMMIT_CHUNK, rounded up to a page
 *                   multiple, clamped to capacity
 */
bool bumpArena_init(BumpArena *arena, size_t segmentBytes, bool aligned, size_t alignBytes, size_t commitChunkSize);

/* Releases the reservation. */
void bumpArena_destroy(BumpArena *arena);

/* Returns NULL on overflow, capacity exceeded, or commit failure. */
void *bumpArena_alloc(BumpArena *arena, size_t bytes);

/* Save/restore the cursor; rewind is a no-op if mark > liveTop. */
size_t bumpArena_mark(const BumpArena *arena);
void   bumpArena_rewind(BumpArena *arena, size_t mark);

/* Makes ptr the new live top. False (arena unchanged) if ptr is outside
 * [base, base + used] or not alignBytes-aligned. */
bool bumpArena_shrink(BumpArena *arena, const void *ptr);

size_t bumpArena_used(const BumpArena *arena);       /* liveTop   */
size_t bumpArena_capacity(const BumpArena *arena);   /* capacity  */
size_t bumpArena_committed(const BumpArena *arena);  /* commitTop */
bool   bumpArena_owns(const BumpArena *arena, const void *ptr);

#ifdef __cplusplus
}
#endif
