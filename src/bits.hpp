/*
 * MIT License
 * Copyright (c) 2025 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once  
#include <cstdint>  
#include <type_traits> // Ensure this header is included for std::enable_if and std::is_integral

#if defined(_MSC_VER)
#include <intrin.h> // Ensure this header is included for MSVC intrinsic functions  
#endif

namespace bits {
	template <typename T, typename = std::enable_if<std::is_integral_v<T>>>
	T ceil(T x) {
		if (x == 0) return 1;

		--x;

		x |= x >> 1;
		x |= x >> 2;
		x |= x >> 4;

		if constexpr (sizeof(T) > 1) x |= x >> 8;
		if constexpr (sizeof(T) > 2) x |= x >> 16;
		if constexpr (sizeof(T) > 4) x |= x >> 32;

		return x + 1;
	}

	template<typename T>
	static inline void set_one(T& value, uint8_t bitIdx) noexcept {
		value |= (static_cast<T>(1) << bitIdx);
	}

	template<typename T>
	static inline void set_zero(T& value, uint8_t bitIdx) noexcept {
		value &= ~(static_cast<T>(1) << bitIdx);
	}

	template<typename T>
	static inline T get(T& value, uint8_t bitIdx) noexcept {
		return (value >> bitIdx) & static_cast<T>(1);
	}

	static inline uint8_t popcnt64(uint64_t x) {
#if defined(__clang__) || defined(__GNUC__)  
		// GCC / Clang / Linux / macOS / iOS / Android  
		return static_cast<uint8_t>(__builtin_popcountll(x));
#elif defined(_MSC_VER)
		return static_cast<uint8_t>(__popcnt64(x));
#else
		// fallback: portable software implementation  
		x = (x & 0x5555555555555555ULL) + ((x >> 1) & 0x5555555555555555ULL);
		x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
		x = (x & 0x0F0F0F0F0F0F0F0FULL) + ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL);
		x = (x * 0x0101010101010101ULL) >> 56;
		return static_cast<uint8_t>(x);
#endif  
	}

	static inline uint8_t ctz64(uint64_t x) {
#if defined(__clang__) || defined(__GNUC__)  
		return x ? static_cast<uint8_t>(__builtin_ctzll(x)) : 64;
#elif defined(_MSC_VER)  
#include <intrin.h>  
		unsigned long index;
		if (_BitScanForward64(&index, x))
			return static_cast<uint8_t>(index);
		else
			return 64;
#else  
		if (x == 0) return 64;
		uint8_t n = 0;
		if ((x & 0xFFFFFFFF) == 0) { n += 32; x >>= 32; }
		if ((x & 0xFFFF) == 0) { n += 16; x >>= 16; }
		if ((x & 0xFF) == 0) { n += 8; x >>= 8; }
		if ((x & 0xF) == 0) { n += 4; x >>= 4; }
		if ((x & 0x3) == 0) { n += 2; x >>= 2; }
		if ((x & 0x1) == 0) { n += 1; }
		return n;
#endif  
	}
}