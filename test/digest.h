/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_TEST_DIGEST_H
#define FSIMG_TEST_DIGEST_H

#include <stddef.h>
#include <stdint.h>

/* Lowercase hexadecimal digest plus terminator, as printed by shasum -a 256. */
#define SHA256_HEX_BYTES 65

/* Incremental SHA-256 (FIPS 180-4) for test fingerprints. The block buffer
 * holds the pending bytes of an incomplete 64-byte block.
 */
typedef struct sha256 {
	uint64_t	 message_bytes;
	uint32_t	 state[8];
	unsigned char	 block[64];
	size_t		 block_used;
} sha256_t;

void sha256_init(sha256_t *);
void sha256_update(sha256_t *, const void *, size_t);
void sha256_final_hex(sha256_t *, char *);

#endif
