/*
 * MIT License
 * Copyright (c) 2026 IMSDcrueoft (https://github.com/IMSDcrueoft)
 * See LICENSE file in the root directory for full license text.
*/
#pragma once
#include <stdint.h>
uint8_t bits_ceil8(uint8_t x);
uint16_t bits_ceil16(uint16_t x);
uint32_t bits_ceil32(uint32_t x);
uint64_t bits_ceil64(uint64_t x);

#define bits_ceil(x) _Generic((x), \
    uint8_t:  bits_ceil8,  \
    uint16_t: bits_ceil16, \
    uint32_t: bits_ceil32, \
    uint64_t: bits_ceil64  \
)(x)

size_t bits_popcnt64(uint64_t x);
size_t bits_ctz64(uint64_t x);
size_t bits_clz64(uint64_t x);

#define bits_set_one(value, bitIdx) ((value) |= ((size_t)1u << (bitIdx)))
#define bits_set_zero(value, bitIdx) ((value) &= ~((size_t)1u << (bitIdx)))
#define bits_get(value, bitIdx) (((value) >> (bitIdx)) & (size_t)1u)

#if defined(__clang__) || defined(__GNUC__)  
// GCC / Clang / Linux / macOS / iOS / Android  
#define OFFSET_OF(type, member) __builtin_offsetof(type, member)
#else
#define OFFSET_OF(type, member) offsetof(type, member)
#endif