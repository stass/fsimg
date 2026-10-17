/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#include "config.h"

#include <string.h>
#include <strings.h>

#include "exfat.h"
#include "host.h"
#include "unicode.h"

static int key_compare(const void *, size_t, const void *, size_t, void *);

fsimg_status_t
exfat_key_append(fsimg_stream_t *keys, uint64_t parent, const uint16_t *units,
    size_t count, const uint16_t *upcase, fsimg_error_t *error)
{
	unsigned char key[518];
	uint16_t mapped;

	/* Big-endian parent IDs and fixed-width UTF-16 units make the complete
	 * namespace key byte-comparable without a host locale or a hash oracle.
	 */
	for (unsigned i = 0; i < 8; i++)
		key[i] = (unsigned char)(parent >> (56 - i * 8));
	for (size_t i = 0; i < count; i++) {
		mapped = upcase == NULL ? (uint16_t)fsimg_unicode_upper(units[i]) :
		    upcase[units[i]];
		key[8 + i * 2] = (unsigned char)(mapped >> 8);
		key[9 + i * 2] = (unsigned char)mapped;
	}
	return (fsimg_stream_append(keys, key, 8 + count * 2, NULL, error));
}

static int
key_compare(const void *a, size_t alen, const void *b, size_t blen,
    void *argument FSIMG_UNUSED)
{
	int comparison;

	comparison = memcmp(a, b, alen < blen ? alen : blen);
	return (comparison != 0 ? comparison : (alen > blen) - (alen < blen));
}

fsimg_status_t
exfat_keys_unique(fsimg_stream_t *keys, bool image, fsimg_error_t *error)
{
	fsimg_stream_t *sorted;
	unsigned char previous[518], current[518];
	uint64_t cursor;
	size_t length, previous_length;
	fsimg_status_t status;

	sorted = NULL;
	status = fsimg_stream_seal(keys, error);
	if (status != FSIMG_OK)
		return (status);
	status = fsimg_stream_sort(keys, key_compare, NULL, NULL, &sorted, error);
	if (status != FSIMG_OK)
		return (status);
	cursor = 0;
	previous_length = 0;
	for (;;) {
		status = fsimg_stream_next(sorted, &cursor, current, sizeof(current),
		    &length, error);
		if (status == FSIMG_DONE) {
			status = FSIMG_OK;
			break;
		}
		if (status != FSIMG_OK)
			break;
		if (length == previous_length && bcmp(previous, current, length) == 0) {
			status = fsimg_error_set(error, image ? FSIMG_ECORRUPT : FSIMG_EINVAL,
			    0, image ? FSIMG_OP_READER : FSIMG_OP_WRITER,
			    "ambiguous exFAT case-insensitive namespace");
			break;
		}
		bcopy(current, previous, length);
		previous_length = length;
	}
	fsimg_stream_free(sorted);
	return (status);
}
