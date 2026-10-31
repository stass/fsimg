/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_TEST_BASELINE_H
#define FSIMG_TEST_BASELINE_H

#include "digest.h"
#include "fsimg.h"

/* Comparison-baseline measurements. Each --baseline test mode prints one
 * record per named fixture on stdout: "fixture=<name> key=value ...", with
 * keys in a fixed order. test/baseline.sh compares the records with the
 * checked-in test/baseline.txt. It classifies keys by name: keys ending in
 * _sha256, and image_bytes, must match exactly; every other key is a
 * resource counter compared with a tolerance. Name a new key accordingly.
 * A "*_memory_retained" key is the charged memory at the end of its phase,
 * sampled before any teardown, so it shows indexes, plans and caches that
 * stay resident.
 */

/* Create the shared tree as a reproducible image of format, read it back,
 * and print the fixture's write and read measurements.
 */
void baseline_tree(const char *fixture, const char *format);

/* Read exactly length bytes of the reader's current file at offset and
 * absorb them into digest.
 */
void baseline_digest_read(fsimg_reader_t *, uint64_t, uint64_t,
    fsimg_sha256_t *);

/* Run the fixed fragmented-file access script on the reader's current file
 * of the given size, absorbing every returned byte into digest. Range answers
 * are checked for validity but not recorded.
 */
void baseline_read_script(fsimg_reader_t *, uint64_t, fsimg_sha256_t *);

#endif
