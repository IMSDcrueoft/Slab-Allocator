/*
 * MIT License
 * Copyright (c) 2025 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <cassert>
#include <algorithm>

#include "./bits.hpp"

namespace slab {
#if defined(__clang__) || defined(__GNUC__)  
	// GCC / Clang / Linux / macOS / iOS / Android  
#define OFFSET_OF(type, member) __builtin_offsetof(type, member)
#else
#define OFFSET_OF(type, member) offsetof(type, member)
#endif

	// limit
	static constexpr auto unit_max_size = 4096;
	// dont set too small
	static constexpr auto traverse_threshold = 4;

	static void* (*_malloc)(size_t size) = std::malloc;
	static void (*_free)(void*) = std::free;

	class SlabAllocator {
	protected:
		struct alignas(8) SlabUnit {
			uint32_t index;				// only need 0-63
			uint32_t offset;			// the offset to SlabBlock
			char payload[];

			SlabUnit(const uint32_t index, const uint32_t offset) {
				this->index = index;
				this->offset = offset;
			}

			//no destructor, and no need to free memory

			static SlabUnit* getUnitFromPayload(const void* ptr) {
				return (SlabUnit*)((char*)ptr - OFFSET_OF(SlabUnit, payload));
			}
		};

		struct alignas(8) SlabBlock {
			SlabAllocator* allocator;	// pointer to the allocator
			SlabBlock* next;			// next slab in the list
			SlabBlock* prev;			// prev slab in the list
			uint64_t bitMap;			// bit==1 means free (bitMap != 0)
			char payload[];				// the slices

			SlabBlock(const SlabAllocator* allocator) {
				// Calculate the offset of the 'payload' flexible array member in SlabBlock.
				// Allocate memory for the fixed part of the structure plus space for 64 units of metadata.
				// This ensures the flexible array can be used safely without additional allocations.
				constexpr size_t baseOffset = OFFSET_OF(SlabBlock, payload);

				this->allocator = const_cast<SlabAllocator*>(allocator); // set allocator pointer
				this->prev = nullptr;
				this->next = nullptr;
				this->bitMap = UINT64_MAX; // all free

				for (size_t i = 0; i < 64; ++i) {
					const auto currentOffset = baseOffset + i * allocator->unitMetaSize;
					SlabUnit* unit = (SlabUnit*)(reinterpret_cast<char*>(this) + currentOffset);
					new (unit) SlabUnit(i, currentOffset); // placement new to initialize the unit
				}
			}

			bool isFull() const {
				return this->bitMap == 0;
			}

			bool isEmpty() const {
				return this->bitMap == UINT64_MAX;
			}

			SlabUnit* allocateUnit(const size_t unitMetaSize) {
#if _DEBUG
				if (this->isFull()) {
					std::cerr << "allocateUnit: no unit for allocate." << std::endl;
					return nullptr;
				}
#endif
				uint32_t index = bits::ctz64(this->bitMap);
				bits::set_zero(this->bitMap, index);
				return (SlabUnit*)((char*)this->payload + index * unitMetaSize);
			}

			static SlabBlock* create(const SlabAllocator* allocator) {
				// Calculate the offset of the 'payload' flexible array member in SlabBlock.
				// Allocate memory for the fixed part of the structure plus space for 64 units of metadata.
				// This ensures the flexible array can be used safely without additional allocations.
				constexpr size_t baseOffset = OFFSET_OF(SlabBlock, payload);

				SlabBlock* slab = (SlabBlock*)_malloc(baseOffset + (static_cast<size_t>(64) * allocator->unitMetaSize));

				if (slab != nullptr) {
					return new (slab) SlabBlock(allocator); // placement new to initialize the slab block
				}

				std::cerr << "create: memory allocation failed." << std::endl;
				return nullptr;
			}

			//there is no destructor, so we need to free the memory manually
			static void destroy(SlabBlock* _this) {
				_free(_this);
			}

			static SlabBlock* getBlockFromUnit(const SlabUnit* unit) {
				return (SlabBlock*)((char*)unit - unit->offset);
			}

			static void print_bitMap(uint64_t bitMap) {
				static const char* bins[16] = {
					"####","###_","##_#","##__",
					"#_##","#_#_","#__#","#___",
					"_###","_##_","_#_#","_#__",
					"__##","__#_","___#","____"
				};

				for (uint32_t i = 0; i < 4; ++i) {
					std::cout << bins[(bitMap >> 12) & 0xf] << bins[(bitMap >> 8) & 0xf] << bins[(bitMap >> 4) & 0xf] << bins[bitMap & 0xf] << std::endl;
					bitMap >>= 16;
				}
			}
		};
	protected:
		SlabBlock* head = nullptr;
		SlabBlock* cache = nullptr;

		uint32_t unitMetaSize = 0;		// sizeof unit payload + meta
		uint32_t total_count = 0;		// total slab count
		uint32_t reserved_count;		// reserved free slab count
		uint32_t reserved_limit;		// reserved free slab limit

	public:
		SlabAllocator(uint32_t unitSize, const uint32_t reserved_limit = 4) {
			assert(unitSize <= unit_max_size && "Invalid unitSize for SlabAllocator");

			unitSize = (unitSize + 7) & ~7;// align to 8
			this->unitMetaSize = (sizeof(SlabUnit) + unitSize);

			//create node
			this->head = SlabBlock::create(this);
			this->cache = this->head;
			this->total_count = 1;
			this->reserved_count = 1;
			this->reserved_limit = std::max(reserved_limit, 1u);// ensure that there is at least one free

			if (this->head == nullptr) {
				std::cerr << "slabAllocator: failed in allocating memory." << std::endl;
				exit(1);
			}
		}

		~SlabAllocator() {
			SlabBlock* slab = this->head;
			while (slab != nullptr) {
				SlabBlock* next = slab->next;
				SlabBlock::destroy(slab);
				slab = next;
			}
		}

		uint32_t total() const {
			return this->total_count;
		}

		uint32_t reserved() const {
			return this->reserved_count;
		}

		uint32_t unitSize() const {
			return this->unitMetaSize - sizeof(SlabUnit);
		}

		void* allocate() {
#if _DEBUG
			if (this->head == nullptr || this->cache == nullptr) {
				std::cerr << "allocate: nullptr allocator." << std::endl;
				return nullptr; // allocation failed
			}
#endif
			if (!this->cache->isFull()) {
				if (this->cache->isEmpty()) {
					assert(this->reserved_count > 0 && "Invalid reserved count.");
					--this->reserved_count;
				}
				return this->cache->allocateUnit(this->unitMetaSize)->payload;
			}

			uint32_t traverseCount = 0;
			SlabBlock* current = this->head;

			while (current->isFull()) {
				if (current->next == nullptr) {
					SlabBlock* newSlab = SlabBlock::create(this);
					if (newSlab == nullptr) {
						std::cerr << "allocate: failed in allocating memory." << std::endl;
						return nullptr; // allocation failed
					}

					// insert it at slab_head
					// why? for reducing traversal overhead
					newSlab->next = this->head;
					// should not be nullptr in any case
					if (this->head != nullptr) {
						this->head->prev = newSlab;
					}
					this->head = newSlab;
					this->cache = newSlab;
					++this->total_count;

					return newSlab->allocateUnit(this->unitMetaSize)->payload;
				}

				current = current->next;
				++traverseCount;
			}

			// float forward, one at a time
			if (traverseCount > traverse_threshold) {
				current->prev->next = current->next;
				if (current->next != nullptr) {
					current->next->prev = current->prev;
				}

				current->next = this->head;
				current->prev = nullptr;
				this->head->prev = current;
				this->head = current;
			}

			this->cache = current;
			if (current->isEmpty()) {
				assert(this->reserved_count > 0 && "Invalid reserved count.");
				--this->reserved_count;
			}
			return current->allocateUnit(this->unitMetaSize)->payload;
		}

		void deallocate(void* ptr) {
			if (ptr == nullptr) {
				std::cerr << "deallocate: Invalid pointer nullptr." << std::endl;
				return;
			}

			//getSlabUnitFromPtr
			SlabUnit* unit = SlabUnit::getUnitFromPayload(ptr);

			if (unit->index >= 64) {
				std::cerr << "deallocate: Invalid unit index " << unit->index << std::endl;
				return;
			}

			//getBlockFromUnit
			SlabBlock* slab = SlabBlock::getBlockFromUnit(unit);

			if (slab->allocator != this) {
				std::cerr << "deallocate: Invalid slab allocator." << std::endl;
				return; // invalid slab
			}

			if (bits::get(slab->bitMap, unit->index) == 0) {
				bits::set_one(slab->bitMap, unit->index);

				if (!slab->isEmpty()) return;

				// it's free now
				++this->reserved_count;
				if (this->reserved_count > this->reserved_limit) {
					if (slab == this->head) {
						this->head = this->head->next;
						this->head->prev = nullptr;
					}
					else {
						slab->prev->next = slab->next;
						if (slab->next != nullptr) {
							slab->next->prev = slab->prev;
						}
					}

					SlabBlock::destroy(slab);
					assert(this->total_count > 0 && "Invalid total count.");
					--this->total_count;
					assert(this->reserved_count > 0 && "Invalid reserved count.");
					--this->reserved_count;

					if (slab == this->cache) {
						this->cache = this->head;
					}
				}
			}
			else {
				std::cerr << "deallocate: Unit is already freed in bitMap." << std::endl;
			}
		}

		bool prepare_bulk(const uint8_t count) {
			if (count > 64) {
				std::cerr << "prepare_bulk: count must not be greater than 64." << std::endl;
				return false;
			}

			SlabBlock* slab = this->head;

			while ((slab != nullptr) && bits::popcnt64(slab->bitMap) < count) {
				if (slab->next == nullptr) {
					SlabBlock* newSlab = SlabBlock::create(this);
					if (newSlab == nullptr) {
						std::cerr << "prepare_bulk: can not allocate slab." << std::endl;
						return false; // allocation failed
					}

					// insert it at slab_head
					// why? for reducing traversal overhead
					newSlab->next = this->head;
					this->head->prev = newSlab;

					this->head = newSlab;
					this->cache = newSlab;
					++this->total_count;

					return true;
				}

				slab = slab->next;
			}

			this->cache = slab;
			return true;
		}

		/**
		 * @brief deallocate idle slabs
		 * @return 
		 */
		uint32_t reclaim() {
			if (this->head == nullptr) return 0;

			uint32_t freedCount = 0;

			SlabBlock* prev = this->head;
			SlabBlock* current = prev->next;

			while (current != nullptr) {
				if (current->isEmpty()) {
					prev->next = current->next;

					SlabBlock::destroy(current);
					assert(this->total_count > 0 && "Invalid total count.");
					--this->total_count;
					assert(this->reserved_count > 0 && "Invalid reserved count.");
					--this->reserved_count;
					++freedCount;

					current = prev->next;
					//connect
					if (current != nullptr) {
						current->prev = prev;
					}
				}
				else {
					prev = current;
					current = current->next;
				}
			}

			//remind this
			this->cache = this->head;

			return freedCount;
		}

		void print_stats() {
			std::cout << "print_stats:" << std::endl;

			SlabBlock* slab = this->head;
			uint32_t id = 1;

			while (slab != nullptr) {
				if (slab == this->cache) {
					std::cout << "[Cache]" << std::endl;
				}
				std::cout << "Slab_" << id << " " << 64 - bits::popcnt64(slab->bitMap) << " / 64" << std::endl;
				SlabBlock::print_bitMap(slab->bitMap);
				std::cout << std::endl;
				slab = slab->next;
				++id;
			}

			std::cout << "End" << std::endl;
		}
	};

#undef OFFSET_OF
}