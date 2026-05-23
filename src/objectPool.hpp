/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once
#include <cstdint>
#include "slab.h"

namespace slab {
	class FixedAllocator {
	protected:
		SlabAllocator allocator;
	public:
		FixedAllocator(const FixedAllocator&) = delete;
		FixedAllocator& operator=(const FixedAllocator&) = delete;

		FixedAllocator(FixedAllocator&&) = delete;
		FixedAllocator& operator=(FixedAllocator&&) = delete;

		FixedAllocator(size_t size, uint32_t reserved_limit = 4) {
			if (!slab_constructAllocator(&this->allocator, size, reserved_limit)) {
				// disabled exceptions in this project
				std::cerr << "Failed to construct FixedAllocator with size " << size << " and reserved_limit " << reserved_limit << std::endl;
				exit(1);
			}
		}

		~FixedAllocator() {
			slab_destructAllUnits(&this->allocator, [](void* ptr) {});
		}

		// for advanced users who want to manage construction and destruction themselves
		void* allocate() {
			return slab_allocate(&this->allocator);
		}

		// for advanced users who want to manage construction and destruction themselves
		void deallocate(void* ptr) {
			slab_deallocate(&this->allocator, ptr);
		}
	};

	template<typename T>
	class ObjectPool {
	protected:
		SlabAllocator allocator;
	public:
		ObjectPool(const ObjectPool&) = delete;
		ObjectPool& operator=(const ObjectPool&) = delete;

		ObjectPool(ObjectPool&&) = delete;
		ObjectPool& operator=(ObjectPool&&) = delete;

		ObjectPool(uint32_t reserved_limit = 4) {
			slab_constructAllocator(&this->allocator, sizeof(T), reserved_limit);
		}

		~ObjectPool() {
			slab_destructAllUnits(&this->allocator, [](void* ptr) {reinterpret_cast<T*>(ptr)->~T();});
		}

		template<typename... Args>
		T* allocate(Args&&... args) {
			return new (slab_allocate(&this->allocator)) T(std::forward<Args>(args)...);// allocate memory for T using SlabAllocator
		}

		void deallocate(T* ptr) {
			ptr->~T(); // call destructor
			slab_deallocate(&this->allocator, ptr);
		}

		// for advanced users who want to manage construction and destruction themselves
		T* allocate_no_construct() {
			return reinterpret_cast<T*>(slab_allocate(&this->allocator));
		}

		// for advanced users who want to manage construction and destruction themselves
		void deallocate_no_destruct(T* ptr) {
			slab_deallocate(&this->allocator, ptr);
		}
	};
}