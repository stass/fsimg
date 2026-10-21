/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_TEST_DIGEST_H
#define FSIMG_TEST_DIGEST_H

#include "sha256.h"

/* Finish the digest and write it as lowercase hexadecimal plus terminator,
 * as printed by shasum -a 256, into FSIMG_SHA256_DIGEST_STRING_LENGTH
 * bytes. The digest is consumed.
 */
void sha256_final_hex(fsimg_sha256_t *, char *);

#endif
