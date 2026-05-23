/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#include "bits.h"
#if defined(_MSC_VER)
#include <intrin.h> // Ensure this header is included for MSVC intrinsic functions  
#endif

uint8_t bits_ceil8(uint8_t x)
{
    if (x == 0) return 1;

	--x;

	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;

	return x + 1;
}

uint16_t bits_ceil16(uint16_t x)
{
	if (x == 0) return 1;

	--x;

	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;

	return x + 1;
}

uint32_t bits_ceil32(uint32_t x)
{
	if (x == 0) return 1;

	--x;

	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;

	return x + 1;
}

uint64_t bits_ceil64(uint64_t x)
{
	if (x == 0) return 1;

	--x;

	x |= x >> 1;
	x |= x >> 2;
	x |= x >> 4;
	x |= x >> 8;
	x |= x >> 16;
	x |= x >> 32;

	return x + 1;
}

size_t bits_popcnt64(uint64_t x)
{
#if defined(__clang__) || defined(__GNUC__)  
	// GCC / Clang / Linux / macOS / iOS / Android  
	return __builtin_popcountll(x);
#elif defined(_MSC_VER)
	return __popcnt64(x);
#else
	// fallback: portable software implementation  
	x = (x & 0x5555555555555555ULL) + ((x >> 1) & 0x5555555555555555ULL);
	x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
	x = (x & 0x0F0F0F0F0F0F0F0FULL) + ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL);
	x = (x * 0x0101010101010101ULL) >> 56;
	return x;
#endif  
}

size_t bits_ctz64(uint64_t x)
{
#if defined(__clang__) || defined(__GNUC__)  
	return x ? __builtin_ctzll(x) : 64;
#elif defined(_MSC_VER)  
#include <intrin.h>  
	unsigned long index;
	if (_BitScanForward64(&index, x))
		return index;
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

size_t bits_clz64(uint64_t x)
{
#if defined(__clang__) || defined(__GNUC__)
	return x ? __builtin_clzll(x) : 64;
#elif defined(_MSC_VER)
	unsigned long index;
	if (_BitScanReverse64(&index, x))
		return (63 - index);
	else
		return 64;
#else
	if (x == 0) return 64;
	uint8_t n = 0;
	if ((x >> 32) == 0) { n += 32; }
	else { x >>= 32; }
	if ((x >> 16) == 0) { n += 16; }
	else { x >>= 16; }
	if ((x >> 8) == 0) { n += 8; }
	else { x >>= 8; }
	if ((x >> 4) == 0) { n += 4; }
	else { x >>= 4; }
	if ((x >> 2) == 0) { n += 2; }
	else { x >>= 2; }
	if ((x >> 1) == 0) { n += 1; }
	return n;
#endif
}