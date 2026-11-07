/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */

/*
 * CRC-32C, the Castagnoli polynomial 0x1EDC6F41 in its reflected form
 * 0x82F63B78: bits are taken least significant first and the register
 * shifts right.
 *
 * fsimg_crc32c_update() is the bare register update. It applies no
 * inversion on either side, so the caller chooses the seed and whether to
 * complement the result, and a message may be absorbed in any number of
 * pieces: updating with a and then with b gives the same register as one
 * update over a and b concatenated. The standard CRC-32C of a message,
 * whose check value for the ASCII bytes "123456789" is 0xE3069283, is
 *
 *	fsimg_crc32c_update(FSIMG_CRC32C_SEED, message, length) ^
 *	    FSIMG_CRC32C_SEED
 *
 * and formats that store the register without the final complement use
 * the update's result as it is. An empty range may pass NULL. The function
 * neither allocates nor fails.
 */

#ifndef FSIMG_CRC32C_H
#define FSIMG_CRC32C_H

#include <stddef.h>
#include <stdint.h>

/* The all-ones seed, which is also the standard final complement. */
#define FSIMG_CRC32C_SEED	UINT32_C(0xffffffff)

uint32_t	fsimg_crc32c_update(uint32_t, const void *, size_t);

#endif /* !FSIMG_CRC32C_H */
