/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include <stdio.h>
#include <strings.h>

#include "digest.h"

/* Written from the FIPS 180-4 description: section 4.2.2 constants, 5.1.1
 * padding, 5.3.3 initial hash value and 6.2.2 hash computation.
 */
static const uint32_t round_constants[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static uint32_t rotate_right(uint32_t, unsigned);
static void sha256_block(sha256_t *, const unsigned char *);

static uint32_t
rotate_right(uint32_t value, unsigned bits)
{
	return ((value >> bits) | (value << (32 - bits)));
}

/* Compress one 64-byte big-endian message block into the hash state. */
static void
sha256_block(sha256_t *digest, const unsigned char *block)
{
	uint32_t schedule[64], v[8];
	uint32_t s0, s1, choose, majority, t1, t2;

	for (unsigned t = 0; t < 16; t++)
		schedule[t] = (uint32_t)block[t * 4] << 24 |
		    (uint32_t)block[t * 4 + 1] << 16 |
		    (uint32_t)block[t * 4 + 2] << 8 | block[t * 4 + 3];
	for (unsigned t = 16; t < 64; t++) {
		s0 = rotate_right(schedule[t - 15], 7) ^
		    rotate_right(schedule[t - 15], 18) ^ (schedule[t - 15] >> 3);
		s1 = rotate_right(schedule[t - 2], 17) ^
		    rotate_right(schedule[t - 2], 19) ^ (schedule[t - 2] >> 10);
		schedule[t] = schedule[t - 16] + s0 + schedule[t - 7] + s1;
	}

	/* v[0..7] are the working variables a..h. */
	for (unsigned i = 0; i < 8; i++)
		v[i] = digest->state[i];
	for (unsigned t = 0; t < 64; t++) {
		s1 = rotate_right(v[4], 6) ^ rotate_right(v[4], 11) ^
		    rotate_right(v[4], 25);
		choose = (v[4] & v[5]) ^ (~v[4] & v[6]);
		t1 = v[7] + s1 + choose + round_constants[t] + schedule[t];
		s0 = rotate_right(v[0], 2) ^ rotate_right(v[0], 13) ^
		    rotate_right(v[0], 22);
		majority = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
		t2 = s0 + majority;
		/* h = g, g = f, f = e, e = d + T1, d = c, c = b, b = a,
		 * a = T1 + T2: shift every variable down one place, then
		 * adjust the new e (the old d) and a.
		 */
		for (unsigned i = 7; i > 0; i--)
			v[i] = v[i - 1];
		v[4] += t1;
		v[0] = t1 + t2;
	}
	for (unsigned i = 0; i < 8; i++)
		digest->state[i] += v[i];
}

void
sha256_init(sha256_t *digest)
{
	static const uint32_t initial[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
	};

	bzero(digest, sizeof(*digest));
	for (unsigned i = 0; i < 8; i++)
		digest->state[i] = initial[i];
}

void
sha256_update(sha256_t *digest, const void *data, size_t length)
{
	const unsigned char *bytes = data;
	size_t take;

	digest->message_bytes += length;
	while (length > 0) {
		take = sizeof(digest->block) - digest->block_used;
		if (take > length)
			take = length;
		bcopy(bytes, digest->block + digest->block_used, take);
		digest->block_used += take;
		bytes += take;
		length -= take;
		if (digest->block_used == sizeof(digest->block)) {
			sha256_block(digest, digest->block);
			digest->block_used = 0;
		}
	}
}

/* Pad with 0x80, zeros and the 64-bit big-endian message length in bits,
 * then print the state words big-endian. The digest is consumed.
 */
void
sha256_final_hex(sha256_t *digest, char *hex)
{
	unsigned char length_field[8];
	uint64_t message_bits;
	unsigned char pad;

	message_bits = digest->message_bytes * 8;
	for (unsigned i = 0; i < 8; i++)
		length_field[i] = (unsigned char)(message_bits >> (56 - i * 8));
	pad = 0x80;
	sha256_update(digest, &pad, 1);
	pad = 0;
	while (digest->block_used != sizeof(digest->block) - sizeof(length_field))
		sha256_update(digest, &pad, 1);
	sha256_update(digest, length_field, sizeof(length_field));

	for (unsigned i = 0; i < 8; i++)
		snprintf(hex + i * 8, SHA256_HEX_BYTES - i * 8, "%08lx",
		    (unsigned long)digest->state[i]);
}
