/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Minimal stand-in for mGBA's mgba-util/common.h. The real one is mostly
 * platform shims for Windows, MSVC and various consoles, none of which the
 * ARM core needs. Only what src/arm/ actually uses is provided here. */

#ifndef COMMON_H
#define COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
#define CXX_GUARD_START extern "C" {
#define CXX_GUARD_END }
#else
#define CXX_GUARD_START
#define CXX_GUARD_END
#endif

#ifndef UNUSED
#define UNUSED(V) (void) (V)
#endif

#ifdef __GNUC__
#define ATTRIBUTE_UNUSED __attribute__((unused))
#define ATTRIBUTE_NOINLINE __attribute__((noinline))
#define LIKELY(X) __builtin_expect(!!(X), 1)
#define UNLIKELY(X) __builtin_expect(!!(X), 0)
#else
#define ATTRIBUTE_UNUSED
#define ATTRIBUTE_NOINLINE
#define LIKELY(X) (X)
#define UNLIKELY(X) (X)

#ifdef _MSC_VER
#include <intrin.h>
#include <stdlib.h>
#define __builtin_bswap32 _byteswap_ulong
#else
static inline uint32_t touchhle_bswap32(uint32_t x) {
	return ((x & 0x000000FFu) << 24) |
	       ((x & 0x0000FF00u) << 8)  |
	       ((x & 0x00FF0000u) >> 8)  |
	       ((x & 0xFF000000u) >> 24);
}
#define __builtin_bswap32 touchhle_bswap32
#endif

static inline int touchhle_clz(unsigned int x) {
#ifdef _MSC_VER
	unsigned long index;
	if (_BitScanReverse(&index, (unsigned long) x)) {
		return 31 - (int) index;
	}
	return 32;
#else
	int n = 0;
	if (x == 0) return 32;
	if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
	if ((x & 0xFF000000u) == 0) { n += 8;  x <<= 8;  }
	if ((x & 0xF0000000u) == 0) { n += 4;  x <<= 4;  }
	if ((x & 0xC0000000u) == 0) { n += 2;  x <<= 2;  }
	if ((x & 0x80000000u) == 0) { n += 1; }
	return n;
#endif
}
#define __builtin_clz touchhle_clz

static inline int touchhle_popcount(unsigned int x) {
	x = x - ((x >> 1) & 0x55555555);
	x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
	return (int) ((((x + (x >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24);
}
#define __builtin_popcount touchhle_popcount
#endif

#define ROR(I, ROTATE)                                                         \
	((((uint32_t) (I)) >> ROTATE) | ((uint32_t) (I) << ((-ROTATE) & 31)))

#ifndef containerof
#define containerof(PTR, TYPE, MEMBER)                                         \
	((TYPE*) ((uintptr_t) (PTR) -offsetof(TYPE, MEMBER)))
#endif

/* Used by the instruction prefetch fast path. Every platform touchHLE
 * targets is little-endian, so these are plain reads; mGBA's own versions
 * additionally handle big-endian and alignment-restricted hosts. */
#define LOAD_32LE(DEST, ADDR, ARR)                                             \
	DEST = *(uint32_t*) ((uintptr_t) (ARR) + (size_t) (ADDR))
#define LOAD_16LE(DEST, ADDR, ARR)                                             \
	DEST = *(uint16_t*) ((uintptr_t) (ARR) + (size_t) (ADDR))
#define STORE_32LE(SRC, ADDR, ARR)                                             \
	*(uint32_t*) ((uintptr_t) (ARR) + (size_t) (ADDR)) = (SRC)
#define STORE_16LE(SRC, ADDR, ARR)                                             \
	*(uint16_t*) ((uintptr_t) (ARR) + (size_t) (ADDR)) = (SRC)

#endif
