/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_ENDIAN_H
#define FSIMG_ENDIAN_H

#include <limits.h>
#include <stdint.h>

/*
 * Unsigned scalar loads and stores in an explicit byte order. A name states
 * the encoded order, never the host's. Each function accesses exactly 2, 4
 * or 8 bytes at a non-NULL, possibly unaligned pointer: a load needs that
 * many initialized readable bytes and a store that many writable bytes.
 * These are not parsers. The caller proves the span inside its record
 * before forming the pointer, and interprets the value: sentinels, masks,
 * signed meanings and representability before narrowing stay with it.
 *
 * Every byte is unsigned char and is widened to an unsigned type at least
 * as wide as the result before it is shifted, so char signedness and
 * integer promotion cannot reach a sign bit.
 */

_Static_assert(CHAR_BIT == 8, "scalar codecs assume 8-bit bytes");

static inline uint16_t
fsimg_load_le16(const unsigned char *p)
{
	return ((uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8));
}

static inline uint32_t
fsimg_load_le32(const unsigned char *p)
{
	return ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
	    (uint32_t)p[3] << 24);
}

static inline uint64_t
fsimg_load_le64(const unsigned char *p)
{
	return ((uint64_t)fsimg_load_le32(p) |
	    (uint64_t)fsimg_load_le32(p + 4) << 32);
}

static inline uint16_t
fsimg_load_be16(const unsigned char *p)
{
	return ((uint16_t)((uint32_t)p[0] << 8 | (uint32_t)p[1]));
}

static inline uint32_t
fsimg_load_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
	    (uint32_t)p[2] << 8 | (uint32_t)p[3]);
}

static inline uint64_t
fsimg_load_be64(const unsigned char *p)
{
	return ((uint64_t)fsimg_load_be32(p) << 32 |
	    (uint64_t)fsimg_load_be32(p + 4));
}

static inline void
fsimg_store_le16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)(value & 0xff);
	p[1] = (unsigned char)(value >> 8 & 0xff);
}

static inline void
fsimg_store_le32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)(value & 0xff);
	p[1] = (unsigned char)(value >> 8 & 0xff);
	p[2] = (unsigned char)(value >> 16 & 0xff);
	p[3] = (unsigned char)(value >> 24 & 0xff);
}

static inline void
fsimg_store_le64(unsigned char *p, uint64_t value)
{
	fsimg_store_le32(p, (uint32_t)(value & UINT32_MAX));
	fsimg_store_le32(p + 4, (uint32_t)(value >> 32));
}

static inline void
fsimg_store_be16(unsigned char *p, uint16_t value)
{
	p[0] = (unsigned char)(value >> 8 & 0xff);
	p[1] = (unsigned char)(value & 0xff);
}

static inline void
fsimg_store_be32(unsigned char *p, uint32_t value)
{
	p[0] = (unsigned char)(value >> 24 & 0xff);
	p[1] = (unsigned char)(value >> 16 & 0xff);
	p[2] = (unsigned char)(value >> 8 & 0xff);
	p[3] = (unsigned char)(value & 0xff);
}

static inline void
fsimg_store_be64(unsigned char *p, uint64_t value)
{
	fsimg_store_be32(p, (uint32_t)(value >> 32));
	fsimg_store_be32(p + 4, (uint32_t)(value & UINT32_MAX));
}

#endif /* FSIMG_ENDIAN_H */
