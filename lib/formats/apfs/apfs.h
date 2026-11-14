/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Stanislav Sedov <stas@deglitch.com>
 */
#ifndef FSIMG_APFS_H
#define FSIMG_APFS_H

#include "internal.h"

/* APFS (specs/design-apfs.md and specs/design-apfs-driver.md). */

/*
 * Names (unicode.c; specs/design-apfs.md, "Directory records and names").
 * A name is the stored bytes without the NUL, counted in UTF-8 bytes. The
 * reader accepts APFS_NAME_MAX_BYTES, the most a hashed key's 10-bit length
 * holds with the NUL; the writer accepts APFS_WRITE_NAME_MAX_BYTES, a limit
 * provisional until E14.
 *
 * A name's comparison form is a sequence of code points derived by the
 * volume's rule. It is at most APFS_NAME_EXPANSION times its name's length
 * in UTF-8 bytes, which the table generator checks for every scalar value,
 * so its UTF-8 encoding, the namespace key, fits APFS_NAME_KEY_BYTES, and
 * the form fits APFS_NAME_FORM_POINTS points, since a point takes at least
 * one UTF-8 byte.
 */
#define APFS_NAME_MAX_BYTES		1022u
#define APFS_WRITE_NAME_MAX_BYTES	255u
#define APFS_NAME_EXPANSION		3u
#define APFS_NAME_FORM_POINTS		(APFS_NAME_MAX_BYTES * APFS_NAME_EXPANSION)
#define APFS_NAME_KEY_BYTES		(APFS_NAME_MAX_BYTES * APFS_NAME_EXPANSION)
#define APFS_NAME_HASH_BITS		22u
#define APFS_NAME_HASH_MASK		((UINT32_C(1) << APFS_NAME_HASH_BITS) - 1)

typedef enum {
	APFS_NAMES_BYTES,	/* the form is the name itself */
	APFS_NAMES_NFD,		/* the NFD of the name */
	APFS_NAMES_CASELESS	/* the NFD of the full case folding of the NFD */
} apfs_name_rule_t;

typedef struct apfs_name_form {
	size_t		count;
	uint32_t	points[APFS_NAME_FORM_POINTS];
} apfs_name_form_t;

/* apfs_name_form(rule, name, length, form) validates a stored name and
 * stores its comparison form under rule. A name that is empty, "." or "..",
 * not strict UTF-8, or holds a NUL or '/' is FSIMG_ECORRUPT; one longer than
 * APFS_NAME_MAX_BYTES is FSIMG_EUNSUPPORTED. A form beyond its capacity,
 * which the tables' expansion bound rules out, is FSIMG_ELIMIT. On failure
 * form is empty. The statuses describe a stored name; a caller converting a
 * selection query maps them to the query's own (FSIMG_EINVAL).
 */
fsimg_status_t	apfs_name_form(apfs_name_rule_t, const char *, size_t,
		    apfs_name_form_t *);
/* The 22-bit name hash of a comparison form: CRC-32C over its points as
 * little-endian 32-bit units, from the register value FSIMG_CRC32C_SEED,
 * without a final complement or a terminating NUL, keeping the low 22 bits.
 * This is the framing macOS uses (E4); test/apfs-unicode.c checks it
 * against test/fixtures/apfs/name-hashes.txt.
 */
uint32_t	apfs_name_hash(const apfs_name_form_t *);
/* apfs_name_key(form, key, capacity, length) stores the UTF-8 encoding of
 * a comparison form, the namespace key, without a NUL. A capacity of
 * APFS_NAME_KEY_BYTES always suffices; a smaller one that does not is
 * FSIMG_ERANGE with nothing stored.
 */
fsimg_status_t	apfs_name_key(const apfs_name_form_t *, char *, size_t,
		    size_t *);

#endif /* !FSIMG_APFS_H */
