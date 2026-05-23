/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#include "bits.h"
#include "slab.h"

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>

typedef struct SlabUnit
{
	uint32_t index;                // only need 0-63
	uint32_t offset;               // the offset to SlabBlock
	char payload[];                // flexible array member for unit data
} SlabUnit;

typedef struct SlabBlock
{
	struct SlabAllocator* allocator; // pointer to the allocator
	struct SlabBlock* next;          // next slab in the list
	struct SlabBlock* prev;          // prev slab in the list
	uint64_t bitMap;                 // bit==1 means free (bitMap != 0)
	char payload[];                  // flexible array member for unit data
} SlabBlock;

#define UNIT_MAX_SIZE 1024
#define slab_malloc malloc
#define slab_free free

static void constructSlabUnit(SlabUnit* _this, const uint32_t index, const uint32_t offset) {
    _this->index = index;
    _this->offset = offset;
}

// get SlabUnit from payload pointer with offsetof
static SlabUnit* getUnitFromPayload(const void* ptr) {
    return (SlabUnit*)((char*)ptr - OFFSET_OF(SlabUnit, payload));
}

static void constructSlabBlock(SlabBlock* _this, const SlabAllocator* allocator) {
    // Calculate the offset of the 'payload' flexible array member in SlabBlock.
    // Allocate memory for the fixed part of the structure plus space for 64 units of metadata.
    // This ensures the flexible array can be used safely without additional allocations.
    const size_t baseOffset = OFFSET_OF(SlabBlock, payload);

    _this->allocator = (SlabAllocator*)allocator; // set allocator pointer
    _this->prev = NULL;
    _this->next = NULL;
    _this->bitMap = UINT64_MAX; // all free

    for (size_t i = 0; i < 64; ++i) {
        const size_t currentOffset = baseOffset + i * allocator->unit_meta_size;
        SlabUnit* unit = (SlabUnit*)((char*)_this + currentOffset);
        constructSlabUnit(unit, (uint32_t)i, (uint32_t)currentOffset);
    }
}

// relase memory of slab block
static void destroySlabBlock(SlabBlock* _this) {
    slab_free(_this);
}

static bool isSlabBlockFull(const SlabBlock* _this) {
    return _this->bitMap == 0;
}

static bool isSlabBlockEmpty(const SlabBlock* _this) {
    return _this->bitMap == UINT64_MAX;
}

static bool isSlabBlockUnitAllocated(const SlabBlock* _this, const uint32_t index) {
    // if bit is 0, it means the unit is allocated
    return bits_get(_this->bitMap, index) == 0;
}

static SlabUnit* getSlabBlockUnitByIndex(const SlabBlock* _this, const size_t unit_meta_size, const uint32_t index) {
    assert(index < 64 && "Index out of bounds in getUnitByIndex");
    return (SlabUnit*)((char*)_this->payload + index * unit_meta_size);
}

static SlabUnit* allocateSlabBlockUnit(SlabBlock* _this, const size_t unit_meta_size) {
    assert(!isSlabBlockFull(_this) && "SlabBlock is full, cannot allocate unit.");

    size_t index = bits_ctz64(_this->bitMap);
    bits_set_zero(_this->bitMap, index);
    return (SlabUnit*)((char*)_this->payload + index * unit_meta_size);
}

static void deallocateSlabBlockUnit(SlabBlock* _this, const uint32_t index) {
    bits_set_one(_this->bitMap, index);
}

static SlabBlock* createSlabBlock(const SlabAllocator* allocator) {
    // Calculate the offset of the 'payload' flexible array member in SlabBlock.
    // Allocate memory for the fixed part of the structure plus space for 64 units of metadata.
    // This ensures the flexible array can be used safely without additional allocations.
    const size_t baseOffset = OFFSET_OF(SlabBlock, payload);

    SlabBlock* _this = (SlabBlock*)slab_malloc(baseOffset + ((size_t)64) * allocator->unit_meta_size);

    if (_this != NULL) {
        constructSlabBlock(_this, allocator);
        return _this;
    }

    fprintf(stderr, "create: memory allocation failed.\n");
    return NULL;
}

static SlabBlock* getSlabBlockFromUnit(const SlabUnit* unit) {
    return (SlabBlock*)((char*)unit - unit->offset);
}

static void printSlabBlockBitmap(uint64_t bitMap) {
    static const char* bins[16] = {
        "####","###_","##_#","##__",
        "#_##","#_#_","#__#","#___",
        "_###","_##_","_#_#","_#__",
        "__##","__#_","___#","____"
    };

    for (uint32_t i = 0; i < 4; ++i) {
        printf("%s%s%s%s\n",
            bins[(bitMap >> 12) & 0xf],
            bins[(bitMap >> 8) & 0xf],
            bins[(bitMap >> 4) & 0xf],
            bins[bitMap & 0xf]);
        bitMap >>= 16;
    }
}
/*
* create a new block and link it to work list
*/
static SlabBlock* createAndLinkSlabBlock(const SlabAllocator* _this) {
    SlabBlock* slab = createSlabBlock(_this);
    if (slab == NULL) {
        fprintf(stderr, "slabAllocator: failed in allocating memory.\n");
        return NULL;
    }

    slab->next = slab; // link as a circle
    slab->prev = slab; // link as a circle
    return slab;
}

static void destroyListWithUnitDestructor(const SlabAllocator* _this, SlabBlock* begin, void (*destructor)(void*)) {
    SlabBlock* slab = begin;

    do {
        SlabBlock* next = slab->next;
        for (uint32_t i = 0; i < 64; ++i) {
            if (isSlabBlockUnitAllocated(slab, i)) {
                SlabUnit* unit = getSlabBlockUnitByIndex(slab, _this->unit_meta_size, i);
                destructor(unit->payload); // call the provided callback for each allocated unit
            }
        }
        destroySlabBlock(slab);
        slab = next;
    } while (slab != begin);
}

static void destroyList(SlabBlock* begin) {
    SlabBlock* slab = begin;

    do {
        SlabBlock* next = slab->next;
        destroySlabBlock(slab);
        slab = next;
    } while (slab != begin);
}

static void moveSlabBlockFromWorkToFull(SlabAllocator* _this, SlabBlock* slab) {
    // remove head from work
    if (slab->next != slab) {
        _this->work = _this->work->next;
        _this->work->prev = slab->prev;
        slab->prev->next = _this->work;
    }
    else {
        _this->work = NULL;
    }

    // move into full
    if (_this->full != NULL) {
        slab->next = _this->full;
        slab->prev = _this->full->prev;
        slab->next->prev = slab;
        slab->prev->next = slab;

        _this->full = slab;
    }
    else {
        slab->next = slab;
        slab->prev = slab;

        _this->full = slab;
    }
}

static void moveSlabBlockFromFullToWork(SlabAllocator* _this, SlabBlock* slab) {
    // remove from full
    if (slab != _this->full) {
        slab->prev->next = slab->next;
        slab->next->prev = slab->prev;
    }
    else {
        if (slab->next != slab) {
            slab->prev->next = slab->next;
            slab->next->prev = slab->prev;
            _this->full = slab->next;
        }
        else {
            _this->full = NULL;
        }
    }

    // move to work head
    if (_this->work != NULL) {
        slab->next = _this->work;
        slab->prev = _this->work->prev;
        slab->next->prev = slab;
        slab->prev->next = slab;

        _this->work = slab;
    }
    else {
        slab->next = slab;
        slab->prev = slab;

        _this->work = slab;
    }
}

static void removeSlabBlockFromWorkAndDestroy(SlabAllocator* _this, SlabBlock* slab) {
    // remove from work
    if (slab != _this->work) {
        slab->prev->next = slab->next;
        slab->next->prev = slab->prev;
    }
    else {
        // remove head from work
        if (slab->next != slab) {
            slab->prev->next = slab->next;
            slab->next->prev = slab->prev;
            _this->work = slab->next;
        }
        else {
            _this->work = NULL;
        }
    }

    destroySlabBlock(slab);
    assert(_this->total_count > 0 && "Invalid total count.");
    --_this->total_count;
    assert(_this->reserved_count > 0 && "Invalid reserved count.");
    --_this->reserved_count;
}

// ================================ public interfaces ==================================
bool slab_constructAllocator(void* memory, uint32_t unitSize, const uint32_t reserved_limit)
{
    if (unitSize > UNIT_MAX_SIZE) {
        fprintf(stderr, "Invalid unitSize for SlabAllocator\n");
        return false;
    }

    if (memory == NULL) {
        fprintf(stderr, "Invalid Null pointer to construct\n");
        return false;
    }

    unitSize = (unitSize + 7) & ~7; // align to 8

    SlabAllocator* _this = (SlabAllocator*)memory;
    _this->unit_meta_size = (uint32_t)(sizeof(SlabUnit) + unitSize);

    // create node
    SlabBlock* slab = createAndLinkSlabBlock(_this);
    if (slab == NULL) {
        return false;
    }

    _this->work = slab;
    _this->full = NULL;
    _this->total_count = 1;
    _this->reserved_count = 1;
    _this->reserved_limit = (reserved_limit > 1) ? reserved_limit : 1; // ensure that there is at least one free

    return true;
}

void slab_destructAllUnits(SlabAllocator* const _this, void (*destructor)(void*))
{
	if (_this->full != NULL) {
		destroyListWithUnitDestructor(_this, _this->full, destructor);
		_this->full = NULL;
	}

	if (_this->work != NULL) {
		destroyListWithUnitDestructor(_this, _this->work, destructor);
		_this->work = NULL;
	}

    _this->total_count = 0;
    _this->reserved_count = 0;
}

SlabAllocator* slab_createAllocator(uint32_t unitSize, const uint32_t reserved_limit) {
    if (unitSize > UNIT_MAX_SIZE) {
        fprintf(stderr, "Invalid unitSize for SlabAllocator\n");
        return NULL;
    }

    unitSize = (unitSize + 7) & ~7; // align to 8

    SlabAllocator* _this = (SlabAllocator*)slab_malloc(sizeof(SlabAllocator));
    if (_this == NULL) {
        fprintf(stderr, "Failed to allocate SlabAllocator\n");
		return NULL;
    }

    _this->unit_meta_size = (uint32_t)(sizeof(SlabUnit) + unitSize);

    // create node
    SlabBlock* slab = createAndLinkSlabBlock(_this);
	if (slab == NULL) {
        slab_free(_this);
        return NULL;
    }

    _this->work = slab;
    _this->full = NULL;
    _this->total_count = 1;
    _this->reserved_count = 1;
    _this->reserved_limit = (reserved_limit > 1) ? reserved_limit : 1; // ensure that there is at least one free

    return _this;
}

void slab_destroyAllocator(SlabAllocator* const _this) {
    if (_this->full != NULL) {
        destroyList(_this->full);
        _this->full = NULL;
    }

    if (_this->work != NULL) {
        destroyList(_this->work);
        _this->work = NULL;
    }

    slab_free(_this);
}

uint32_t slab_total(SlabAllocator* const _this) {
    return _this->total_count;
}

uint32_t slab_reserved(SlabAllocator* const _this) {
    return _this->reserved_count;
}

uint32_t slab_unitSize(SlabAllocator* const _this) {
    return _this->unit_meta_size - (uint32_t)sizeof(SlabUnit);
}

void* slab_allocate(SlabAllocator* const _this) {
    SlabBlock* slab = _this->work;

    if (slab == NULL) {
        slab = createAndLinkSlabBlock(_this);

        _this->work = slab;
        // set the work slab to the new slab
        ++_this->total_count;
        // no need to modify reserved_count
        return allocateSlabBlockUnit(slab, _this->unit_meta_size)->payload;
    }

    if (isSlabBlockEmpty(slab)) {
        assert(_this->reserved_count > 0 && "Invalid reserved count.");
        --_this->reserved_count;
    }

    void* mem = allocateSlabBlockUnit(slab, _this->unit_meta_size)->payload;

    // move to full
    if (isSlabBlockFull(slab)) {
        moveSlabBlockFromWorkToFull(_this, slab);
    }

    return mem;
}

void slab_deallocate(SlabAllocator* const _this, void* ptr) {
    if (ptr == NULL) {
        fprintf(stderr, "deallocate: Invalid pointer nullptr.\n");
        return;
    }

    // getSlabUnitFromPtr
    SlabUnit* unit = getUnitFromPayload(ptr);

    if (unit->index >= 64) {
        fprintf(stderr, "deallocate: Invalid unit index %u\n", unit->index);
        return;
    }

    // getBlockFromUnit
    SlabBlock* slab = getSlabBlockFromUnit(unit);

    if (slab->allocator != _this) {
        fprintf(stderr, "deallocate: Invalid slab allocator.\n");
        return; // invalid slab
    }

    if (isSlabBlockUnitAllocated(slab, unit->index)) {
        bool isFull = isSlabBlockFull(slab);
        deallocateSlabBlockUnit(slab, unit->index);

        if (isFull) {
            moveSlabBlockFromFullToWork(_this, slab);
            return;
        }

        if (isSlabBlockEmpty(slab)) {
            // it's free now
            ++_this->reserved_count;
            if (_this->reserved_count > _this->reserved_limit) {
                removeSlabBlockFromWorkAndDestroy(_this, slab);
            }
        }
    }
    else {
        fprintf(stderr, "deallocate: Unit is already freed in bitMap.\n");
    }
}

void slab_printStats(SlabAllocator* const _this) {
    printf("print_stats:\n");

    if (_this->full != NULL) {
        uint32_t count = 0;
        SlabBlock* slab = _this->full;

        do {
            slab = slab->next;
            ++count;
        } while (slab != _this->full);

        printf("full count: %u\n\n", count);
    }

    if (_this->work != NULL) {
        uint32_t id = 1;
        SlabBlock* slab = _this->work;

        do {
            printf("slab_%u %zu / 64\n", id, 64 - bits_popcnt64(slab->bitMap));
            printSlabBlockBitmap(slab->bitMap);
            printf("\n");
            slab = slab->next;

            ++id;
        } while (slab != _this->work);
    }

    printf("End\n");
}