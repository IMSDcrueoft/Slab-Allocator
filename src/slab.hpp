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

	// limit size
	constexpr auto unit_max_size = 4096;
	static void* (*_malloc)(size_t size) = std::malloc;
	static void (*_free)(void*) = std::free;

	class SlabAllocator {
	protected:
		struct alignas(8) SlabUnit {
			uint32_t index;				// only need 0-63
			uint32_t offset;			// the offset to SlabBlock
			char payload[];

			SlabUnit() = delete;
			~SlabUnit() = delete;

			static void construct(SlabUnit* _this, const uint32_t index, const uint32_t offset) {
				_this->index = index;
				_this->offset = offset;
			}

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

			SlabBlock() = delete;
			~SlabBlock() = delete;

			static void construct(SlabBlock* _this, const SlabAllocator* allocator) {
				// Calculate the offset of the 'payload' flexible array member in SlabBlock.
				// Allocate memory for the fixed part of the structure plus space for 64 units of metadata.
				// This ensures the flexible array can be used safely without additional allocations.
				constexpr size_t baseOffset = OFFSET_OF(SlabBlock, payload);

				_this->allocator = const_cast<SlabAllocator*>(allocator); // set allocator pointer
				_this->prev = nullptr;
				_this->next = nullptr;
				_this->bitMap = UINT64_MAX; // all free

				for (size_t i = 0; i < 64; ++i) {
					const auto currentOffset = baseOffset + i * allocator->unitMetaSize;
					SlabUnit* unit = (SlabUnit*)(reinterpret_cast<char*>(_this) + currentOffset);
					SlabUnit::construct(unit, i, currentOffset);
				}
			}

			static void destroy(SlabBlock* _this) {
				_free(_this);
			}

			bool isFull() const {
				return this->bitMap == 0;
			}

			bool isEmpty() const {
				return this->bitMap == UINT64_MAX;
			}

			bool isUnitAllocated(const uint32_t index) const {
				// if bit is 0, it means the unit is allocated
				return bits::get(this->bitMap, index) == 0;
			}

			SlabUnit* getUnitByIndex(const size_t unitMetaSize, const uint32_t index) const {
				assert(index < 64 && "Index out of bounds in getUnitByIndex");

				return (SlabUnit*)((char*)this->payload + index * unitMetaSize);
			}

			SlabUnit* allocateUnit(const size_t unitMetaSize) {
				assert(!this->isFull() && "SlabBlock is full, cannot allocate unit.");

				uint32_t index = bits::ctz64(this->bitMap);
				bits::set_zero(this->bitMap, index);
				return (SlabUnit*)((char*)this->payload + index * unitMetaSize);
			}

			void deallocateUnit(const uint32_t index) {
				bits::set_one(this->bitMap, index);
			}

			static SlabBlock* create(const SlabAllocator* allocator) {
				// Calculate the offset of the 'payload' flexible array member in SlabBlock.
				// Allocate memory for the fixed part of the structure plus space for 64 units of metadata.
				// This ensures the flexible array can be used safely without additional allocations.
				constexpr size_t baseOffset = OFFSET_OF(SlabBlock, payload);

				SlabBlock* slab = (SlabBlock*)_malloc(baseOffset + (static_cast<size_t>(64) * allocator->unitMetaSize));

				if (slab != nullptr) {
					SlabBlock::construct(slab, allocator);
					return slab;
				}

				std::cerr << "create: memory allocation failed." << std::endl;
				return nullptr;
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
		SlabBlock* work = nullptr;
		SlabBlock* full = nullptr;

		uint32_t unitMetaSize = 0;		// sizeof unit payload + meta
		uint32_t total_count = 0;		// total slab count
		uint32_t reserved_count;		// reserved free slab count
		uint32_t reserved_limit;		// reserved free slab limit

	protected:
		/**
		 * @brief create a block for work
		 */
		SlabBlock* makeBlock() const {
			SlabBlock* slab = SlabBlock::create(this);
			if (slab == nullptr) {
				std::cerr << "slabAllocator: failed in allocating memory." << std::endl;
				exit(1);
			}

			slab->next = slab; // link as a circle
			slab->prev = slab; // link as a circle
			return slab;
		}

		void destroyList(SlabBlock* begin) {
			SlabBlock* slab = begin;

			do {
				SlabBlock* next = slab->next;
				SlabBlock::destroy(slab);
				slab = next;
			} while (slab != begin);
		}

		void moveFromWorkToFull(SlabBlock* slab) {
			//remove head from work
			if (slab->next != slab) {
				this->work = this->work->next;
				this->work->prev = slab->prev;
				slab->prev->next = this->work;
			}
			else {
				this->work = nullptr;
			}

			//move into full
			if (this->full != nullptr) {
				slab->next = this->full;
				slab->prev = this->full->prev;
				slab->next->prev = slab;
				slab->prev->next = slab;

				this->full = slab;
			}
			else {
				slab->next = slab;
				slab->prev = slab;

				this->full = slab;
			}
		}

		void moveFromFullToWork(SlabBlock* slab) {
			//remove from full
			if (slab != this->full) {
				slab->prev->next = slab->next;
				slab->next->prev = slab->prev;
			}
			else {
				if (slab->next != slab) {
					slab->prev->next = slab->next;
					slab->next->prev = slab->prev;
					this->full = slab->next;
				}
				else {
					this->full = nullptr;
				}
			}
			//move to work head
			if (this->work != nullptr) {
				slab->next = this->work;
				slab->prev = this->work->prev;
				slab->next->prev = slab;
				slab->prev->next = slab;

				this->work = slab;
			}
			else {
				slab->next = slab;
				slab->prev = slab;

				this->work = slab;
			}
		}

		void removeFromWorkAndDestroy(SlabBlock* slab) {
			//remove from work
			if (slab != this->work) {
				slab->prev->next = slab->next;
				slab->next->prev = slab->prev;
			}
			else {
				//remove head from work
				if (slab->next != slab) {
					slab->prev->next = slab->next;
					slab->next->prev = slab->prev;
					this->work = slab->next;
				}
				else {
					this->work = nullptr;
				}
			}

			SlabBlock::destroy(slab);
			assert(this->total_count > 0 && "Invalid total count.");
			--this->total_count;
			assert(this->reserved_count > 0 && "Invalid reserved count.");
			--this->reserved_count;
		}
	public:
		SlabAllocator(const SlabAllocator&) = delete;
		SlabAllocator& operator=(const SlabAllocator&) = delete;

		SlabAllocator(SlabAllocator&&) = delete;
		SlabAllocator& operator=(SlabAllocator&&) = delete;

		SlabAllocator(uint32_t unitSize, const uint32_t reserved_limit = 4) {
			if (unitSize > slab::unit_max_size) {
				std::cerr << "Invalid unitSize for SlabAllocator" << std::endl;
				exit(1);
			}

			unitSize = (unitSize + 7) & ~7;// align to 8
			this->unitMetaSize = (sizeof(SlabUnit) + unitSize);

			//create node
			SlabBlock* slab = this->makeBlock();

			this->work = slab;
			this->total_count = 1;
			this->reserved_count = 1;
			this->reserved_limit = std::max(reserved_limit, 1u);// ensure that there is at least one free
		}

		~SlabAllocator() {
			if (this->full != nullptr) {
				destroyList(this->full);
				this->full = nullptr;
			}

			if (this->work != nullptr) {
				destroyList(this->work);
				this->work = nullptr;
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
			SlabBlock* slab = this->work;

			if (slab == nullptr) {
				slab = this->makeBlock();

				this->work = slab;
				// set the work slab to the new slab
				++this->total_count;
				// no need to modify reserved_count
				return slab->allocateUnit(this->unitMetaSize)->payload;
			}

			if (slab->isEmpty()) {
				assert(this->reserved_count > 0 && "Invalid reserved count.");
				--this->reserved_count;
			}

			void* mem = slab->allocateUnit(this->unitMetaSize)->payload;

			// move to full
			if (slab->isFull()) {
				this->moveFromWorkToFull(slab);
			}

			return mem;
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

			if (slab->isUnitAllocated(unit->index)) {
				bool isFull = slab->isFull();
				slab->deallocateUnit(unit->index);

				if (isFull) {
					this->moveFromFullToWork(slab);
					return;
				}

				if (slab->isEmpty()) {
					// it's free now
					++this->reserved_count;
					if (this->reserved_count > this->reserved_limit) {
						this->removeFromWorkAndDestroy(slab);
					}
				}
			}
			else {
				std::cerr << "deallocate: Unit is already freed in bitMap." << std::endl;
			}
		}

		void print_stats() {
			std::cout << "print_stats:" << std::endl;

			if (this->full != nullptr) {
				uint32_t count = 0;
				SlabBlock* slab = this->full;

				do {
					slab = slab->next;
					++count;
				} while (slab != this->full);

				std::cout << "full count: " << count << std::endl << std::endl;
			}

			if (this->work != nullptr) {
				uint32_t id = 1;
				SlabBlock* slab = this->work;

				do {
					std::cout << "slab_" << id << " " << 64 - bits::popcnt64(slab->bitMap) << " / 64" << std::endl;
					SlabBlock::print_bitMap(slab->bitMap);
					std::cout << std::endl;
					slab = slab->next;

					++id;
				} while (slab != this->work);
			}

			std::cout << "End" << std::endl;
		}
	};

	template<typename T>
	class ObjectPool : protected SlabAllocator {
	protected:
		void destroyList(SlabBlock* begin) {
			SlabBlock* slab = begin;

			do {
				SlabBlock* next = slab->next;

				for (uint32_t i = 0; i < 64; ++i) {
					if (slab->isUnitAllocated(i)) {
						SlabUnit* unit = slab->getUnitByIndex(this->unitMetaSize, i);
						unit->~SlabUnit(); // call destructor for T
					}
				}

				SlabBlock::destroy(slab);
				slab = next;
			} while (slab != begin);
		}
	public:
		ObjectPool(const ObjectPool&) = delete;
		ObjectPool& operator=(const ObjectPool&) = delete;

		ObjectPool(ObjectPool&&) = delete;
		ObjectPool& operator=(ObjectPool&&) = delete;

		ObjectPool(uint32_t reserved_limit = 4) : SlabAllocator(sizeof(T), reserved_limit) {}
		~ObjectPool() {
			if (this->full != nullptr) {
				destroyList(this->full);
				this->full = nullptr;
			}
			if (this->work != nullptr) {
				destroyList(this->work);
				this->work = nullptr;
			}
		}

		template<typename... Args>
		T* allocate(Args&&... args) {
			return new (SlabAllocator::allocate()) T(std::forward<Args>(args)...);// allocate memory for T using SlabAllocator
		}

		void deallocate(T* ptr) {
			ptr->~T(); // call destructor
			SlabAllocator::deallocate(ptr);
		}
	};
#undef OFFSET_OF
}