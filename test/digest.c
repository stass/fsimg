/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#include <stdio.h>

#include "digest.h"

void
sha256_final_hex(fsimg_sha256_t *digest, char *hex)
{
	unsigned char bytes[FSIMG_SHA256_DIGEST_LENGTH];

	fsimg_sha256_final(bytes, digest);
	for (size_t i = 0; i < sizeof(bytes); i++)
		snprintf(hex + i * 2, FSIMG_SHA256_DIGEST_STRING_LENGTH - i * 2,
		    "%02x", bytes[i]);
}
