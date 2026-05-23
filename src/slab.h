/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once

#ifdef __cplusplus
#include<cstdint>
extern "C" {
	// hidden
	typedef struct SlabAllocator
	{
		uintptr_t pointers[2];
		uint32_t data[4];
	} SlabAllocator;
#else
#include<stdint.h>
#include<stdbool.h>

typedef struct SlabBlock SlabBlock;
typedef struct SlabAllocator
{
	SlabBlock* work;
	SlabBlock* full;

	uint32_t unit_meta_size;		// sizeof unit payload + meta
	uint32_t total_count;		// total slab count
	uint32_t reserved_count;	// reserved free slab count
	uint32_t reserved_limit;	// reserved free slab limit
} SlabAllocator;

#endif
/*
* placement new for SlabAllocator, the memory block should be at least sizeof(SlabAllocator) bytes
* returns if is success
*/
bool slab_constructAllocator(void* memory, uint32_t unitSize, const uint32_t reserved_limit);
/*
* destruct with callback for each allocated unit, the callback will be called with the pointer to the unit payload
* this will not destruct allocator it self, so you can reuse the allocator after this
*/
void slab_destructAllUnits(SlabAllocator* const _this, void (*destructor)(void*));
/*
* simple construct by malloc
*/
SlabAllocator* slab_createAllocator(uint32_t unitSize, const uint32_t reserved_limit);
/*
* if you use placement new, don't call this
*/
void slab_destroyAllocator(SlabAllocator* const _this);
/*
* get total block count
*/
uint32_t slab_total(SlabAllocator* const _this);
/*
* get reserved free block count
*/
uint32_t slab_reserved(SlabAllocator* const _this);
/*
* get unit payload size (not including meta)
*/
uint32_t slab_unitSize(SlabAllocator* const _this);
/*
* allocate a unit, returns pointer to the unit payload, or NULL if allocation failed
*/
void* slab_allocate(SlabAllocator* const _this);
/*
* deallocate a unit, the pointer should be the one returned by slab_allocate, and the unit should not be deallocated before
*/
void slab_deallocate(SlabAllocator* const _this, void* ptr);
/*
* print the internal state of the allocator, including total blocks, reserved blocks, and the bitmap of each block (for debugging purposes)
*/
void slab_printStats(SlabAllocator* const _this);

#ifdef __cplusplus
}
#endif