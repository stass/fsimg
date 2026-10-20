/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, fsimg contributors
 */
#ifndef FSIMG_DIRECTORY_H
#define FSIMG_DIRECTORY_H

#include "meta.h"

/* Prepared directories and the common walker (specs/design-directories.md).
 * A driver prepares one directory at a time by admitting each child with its
 * displayed name and a private payload. Common code owns everything else:
 * admission limits and charging, namespace validation, traversal order,
 * deferred descent, paths and name matching.
 */

/* A displayed component and its NUL: room for any UTF-8 component of at
 * most 1020 bytes, which includes every component of at most 255 UTF-16
 * units (765 bytes). Each driver bounds its own names.
 */
#define FSIMG_DIRECTORY_NAME_BYTES 1021u
#define FSIMG_DIRECTORY_PAYLOAD_BYTES 64u
#define FSIMG_DIRECTORY_RECORD_BYTES \
	(FSIMG_DIRECTORY_NAME_BYTES + FSIMG_DIRECTORY_PAYLOAD_BYTES)
/* A namespace key: opaque bytes the driver derives from one spelling. Every
 * key buffer handed to a driver holds this many bytes; 1020 fits four bytes
 * for each of a component's at most 255 UTF-16 units.
 */
#define FSIMG_DIRECTORY_KEY_BYTES 1020u
#define FSIMG_DIRECTORY_KEYS 2u

/* The keys one entry answers to, such as a long name and its alias. */
typedef struct fsimg_directory_keys {
	unsigned	 count;
	size_t		 length[FSIMG_DIRECTORY_KEYS];
	unsigned char	 key[FSIMG_DIRECTORY_KEYS][FSIMG_DIRECTORY_KEY_BYTES];
} fsimg_directory_keys_t;

/* One directory under preparation, private to fsimg_reader_ops_t.prepare. */
typedef struct fsimg_directory_build fsimg_directory_build_t;
typedef struct fsimg_walk fsimg_walk_t;

/* Level 0 is the root; a deeper level is the current entry's directory. */
uint32_t fsimg_directory_level(const fsimg_directory_build_t *);
/* Admit one validated child: enforce the depth and path limits, charge the
 * session, spool its record and record its namespace keys.
 */
fsimg_status_t fsimg_directory_admit(fsimg_directory_build_t *, const char *,
    const void *, size_t, fsimg_error_t *);

#endif
