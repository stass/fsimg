/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "crc32c.h"

/* The Castagnoli polynomial with its bits reversed, for a right shift. */
#define CRC32C_POLYNOMIAL_REFLECTED	UINT32_C(0x82f63b78)

/*
 * One bit at a time: each byte is folded into the low end of the register,
 * then for each of its eight bits the register shifts right and, when the
 * bit shifted out was 1, takes the polynomial. The callers checksum a
 * superblock or a file name, so a table buys nothing worth its 1 KiB.
 */
uint32_t
fsimg_crc32c_update(uint32_t crc, const void *buf, size_t len)
{
	const uint8_t *p = buf;

	for (size_t i = 0; i < len; i++) {
		crc ^= p[i];
		for (int bit = 0; bit < CHAR_BIT; bit++) {
			if ((crc & 1) != 0)
				crc = (crc >> 1) ^ CRC32C_POLYNOMIAL_REFLECTED;
			else
				crc >>= 1;
		}
	}
	return (crc);
}
